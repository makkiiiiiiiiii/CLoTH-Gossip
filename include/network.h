#ifndef NETWORK_H
#define NETWORK_H

#include <stdio.h>
#include <stdint.h>
#include "cloth.h"
#include "list.h"

#define MAXMSATOSHI 5E17 //5 millions  bitcoin
#define MAXTIMELOCK 100
#define MINTIMELOCK 10
#define MAXFEEBASE 5000
#define MINFEEBASE 1000
#define MAXFEEPROP 10
#define MINFEEPROP 1
#define MAXLATENCY 100
#define MINLATENCY 10
#define MINBALANCE 1E2
#define MAXBALANCE 1E11

/* a policy that must be respected when forwarding a payment through an edge (see edge below) */
struct policy {
  uint64_t fee_base;
  uint64_t fee_proportional;
  uint64_t min_htlc;
  uint32_t timelock;
};

/* a node of the payment-channel network */
struct node {
  long id;
  struct array* open_edges;
  struct element **results;
  unsigned int explored;
};

/* a bidirectional payment channel of the payment-channel network open between two nodes */
struct channel {
  long id;
  long node1;
  long node2;
  long edge1;
  long edge2;
  uint64_t capacity;
  unsigned int is_closed;
};

/* an edge represents one of the two direction of a payment channel */
struct edge {
  long id;
  long channel_id;
  long from_node_id;
  long to_node_id;
  long counter_edge_id;
  struct policy policy;
  uint64_t balance;
  unsigned int is_closed;
  uint64_t tot_flows;
  struct group* group;
  struct element* channel_updates;
  struct element* edge_locked_balance_and_durations;

  /* === leave/rejoin metadata === */
  double   tolerance_tau;     /* UL threshold */
  uint64_t join_time;         /* time when (re)joined the current group */
  uint64_t flows_at_join;     /* snapshot of tot_flows at join */
  uint64_t last_leave_time;   /* last time this edge left a group */
};

struct edge_locked_balance_and_duration{
    uint64_t locked_balance;
    uint64_t locked_start_time;
    uint64_t locked_end_time;
};

struct edge_snapshot {
  long id;
  uint64_t balance;
  short is_in_group;
  uint64_t group_cap;
  short does_channel_update_exist;
  uint64_t last_channle_update_value;
  uint64_t sent_amt;
};

struct channel_update {
    long edge_id;
    uint64_t time;
    uint64_t htlc_maximum_msat;
};

struct group_update {
    uint64_t time;
    uint64_t group_cap;
    uint64_t* edge_balances;
};

/* A group of edges used for group routing */
struct group {
    long id;                       /* -1 while provisional / >=0 when committed */
    struct array* edges;

    /* join constraints derived from seed edge */
    uint64_t max_cap_limit;
    uint64_t min_cap_limit;

    /* current observed stats */
    uint64_t max_cap;
    uint64_t min_cap;
    uint64_t group_cap;            /* usually = min_cap if group_cap_update=true */

    uint64_t is_closed;            /* if not zero, it describes closed time */
    uint64_t constructed_time;

    struct element* history;       /* list of `struct group_update` */

    /* provenance for logging */
    long     seed_edge_id;
    uint64_t attempt_id;

    /* === logging throttling (③-1) ===
       確定後の update_group を“変化時のみ”に間引くための前回値 */
    uint64_t last_logged_time;
    uint64_t last_logged_group_cap;
    uint64_t last_logged_min_cap;
    uint64_t last_logged_max_cap;
    int      last_logged_valid;
};

struct graph_channel {
  long node1_id;
  long node2_id;
};

struct network {
  struct array* nodes;
  struct array* channels;
  struct array* edges;
  struct array* groups;
  gsl_ran_discrete_t* faulty_node_prob; //the probability that a nodes in the network has a fault and goes offline
};

/* constructors */
struct node* new_node(long id);
struct channel* new_channel(long id, long direction1, long direction2, long node1, long node2, uint64_t capacity);
struct edge* new_edge(long id, long channel_id, long counter_edge_id, long from_node_id, long to_node_id, uint64_t balance, struct policy policy, uint64_t channel_capacity);

/* network lifecycle */
void open_channel(struct network* network, gsl_rng* random_generator);
struct network* initialize_network(struct network_params net_params, gsl_rng* random_generator);
void free_network(struct network* network);

/* group maintenance */
int  update_group(struct group* group, struct network_params net_params, uint64_t current_time);
long get_edge_balance(struct edge* e);
void remove_edge_from_group(struct group* g, struct edge* e);
void group_close_once(struct simulation* sim,struct group* g,const char* reason);

/* stats helpers */
struct edge_snapshot* take_edge_snapshot(struct edge* e, uint64_t sent_amt, short is_in_group, uint64_t group_cap);

#endif
