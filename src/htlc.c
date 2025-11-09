#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#include <gsl/gsl_math.h>

#include "../include/htlc.h"

#include <inttypes.h>

#include "../include/array.h"
#include "../include/heap.h"
#include "../include/payments.h"
#include "../include/routing.h"
#include "../include/network.h"
#include "../include/event.h"
#include "../include/utils.h"

/* Functions in this file simulate the HTLC mechanism for exchanging payments, as implemented in the Lightning Network.
   They are a (high-level) copy of functions in lnd-v0.9.1-beta (see files `routing/missioncontrol.go`, `htlcswitch/switch.go`, `htlcswitch/link.go`) */


/* AUXILIARY FUNCTIONS */

/* compute the fees to be paid to a hop for forwarding the payment */
uint64_t compute_fee(uint64_t amount_to_forward, struct policy policy) {
  uint64_t fee;
  fee = (policy.fee_proportional*amount_to_forward) / 1000000;
  return policy.fee_base + fee;
}

/* check whether there is sufficient balance in an edge for forwarding the payment; check also that the policies in the edge are respected */
unsigned int check_balance_and_policy(struct edge* edge, struct edge* prev_edge, struct route_hop* prev_hop, struct route_hop* next_hop) {
  uint64_t expected_fee;

  if(next_hop->amount_to_forward > edge->balance)
    return 0;

  if(next_hop->amount_to_forward < edge->policy.min_htlc){
    fprintf(stderr, "ERROR: policy.min_htlc not respected\n");
    exit(-1);
  }

  expected_fee = compute_fee(next_hop->amount_to_forward, edge->policy);
  if(prev_hop->amount_to_forward != next_hop->amount_to_forward + expected_fee){
    fprintf(stderr, "ERROR: policy.fee not respected\n");
    exit(-1);
  }

  if(prev_hop->timelock != next_hop->timelock + prev_edge->policy.timelock){
    fprintf(stderr, "ERROR: policy.timelock not respected\n");
    exit(-1);
  }

  return 1;
}

/* retrieve a hop from a payment route */
struct route_hop *get_route_hop(long node_id, struct array *route_hops, int is_sender) {
  struct route_hop *route_hop;
  long i, index = -1;

  for (i = 0; i < array_len(route_hops); i++) {
    route_hop = array_get(route_hops, i);
    if (is_sender && route_hop->from_node_id == node_id) {
      index = i;
      break;
    }
    if (!is_sender && route_hop->to_node_id == node_id) {
      index = i;
      break;
    }
  }

  if (index == -1)
    return NULL;

  return array_get(route_hops, index);
}


/* FUNCTIONS MANAGING NODE PAIR RESULTS */

/* set the result of a node pair as success: it means that a payment was successfully forwarded in an edge connecting the two nodes of the node pair.
 This information is used by the sender node to find a route that maximizes the possibilities of successfully sending a payment */
void set_node_pair_result_success(struct element** results, long from_node_id, long to_node_id, uint64_t success_amount, uint64_t success_time){
  struct node_pair_result* result;

  result = get_by_key(results[from_node_id], to_node_id, is_equal_key_result);

  if(result == NULL){
    result = malloc(sizeof(struct node_pair_result));
    result->to_node_id = to_node_id;
    result->fail_time = 0;
    result->fail_amount = 0;
    result->success_time = 0;
    result->success_amount = 0;
    results[from_node_id] = push(results[from_node_id], result);
  }

  result->success_time = success_time;
  if(success_amount > result->success_amount)
    result->success_amount = success_amount;
  if(result->fail_time != 0 && result->success_amount > result->fail_amount)
    result->fail_amount = success_amount + 1;
}

/* set the result of a node pair as success: it means that a payment failed when passing through  an edge connecting the two nodes of the node pair.
   This information is used by the sender node to find a route that maximimizes the possibilities of successfully sending a payment */
void set_node_pair_result_fail(struct element** results, long from_node_id, long to_node_id, uint64_t fail_amount, uint64_t fail_time){
  struct node_pair_result* result;

  result = get_by_key(results[from_node_id], to_node_id, is_equal_key_result);

  if(result != NULL)
    if(fail_amount > result->fail_amount && fail_time - result->fail_time < 60000)
      return;

  if(result == NULL){
    result = malloc(sizeof(struct node_pair_result));
    result->to_node_id = to_node_id;
    result->fail_time = 0;
    result->fail_amount = 0;
    result->success_time = 0;
    results[from_node_id] = push(results[from_node_id], result);
  }

  result->fail_amount = fail_amount;
  result->fail_time = fail_time;
  if(fail_amount == 0)
    result->success_amount = 0;
  else if(fail_amount != 0 && fail_amount <= result->success_amount)
    result->success_amount = fail_amount - 1;
}

/* process a payment which succeeded */
void process_success_result(struct node* node, struct payment *payment, uint64_t current_time){
  struct route_hop* hop;
  int i;
  struct array* route_hops;
  route_hops = payment->route->route_hops;
  for(i=0; i<array_len(route_hops); i++){
    hop = array_get(route_hops, i);
    set_node_pair_result_success(node->results, hop->from_node_id, hop->to_node_id, hop->amount_to_forward, current_time);
  }
}

/* process a payment which failed (different processments depending on the error type) */
void process_fail_result(struct node* node, struct payment *payment, uint64_t current_time){
  struct route_hop* hop, *error_hop;
  int i;
  struct array* route_hops;

  error_hop = payment->error.hop;

  if(error_hop->from_node_id == payment->sender) //do nothing if the error was originated by the sender (see `processPaymentOutcomeSelf` in lnd)
    return;

  if(payment->error.type == OFFLINENODE) {
    set_node_pair_result_fail(node->results, error_hop->from_node_id, error_hop->to_node_id, 0, current_time);
    set_node_pair_result_fail(node->results, error_hop->to_node_id, error_hop->from_node_id, 0, current_time);
  }
  else if(payment->error.type == NOBALANCE) {
    route_hops = payment->route->route_hops;
    for(i=0; i<array_len(route_hops); i++){
      hop = array_get(route_hops, i);
      if(hop->edge_id == error_hop->edge_id) {
        set_node_pair_result_fail(node->results, hop->from_node_id, hop->to_node_id, hop->amount_to_forward, current_time);
        break;
      }
      set_node_pair_result_success(node->results, hop->from_node_id, hop->to_node_id, hop->amount_to_forward, current_time);
    }
  }
}


void generate_send_payment_event(struct payment* payment, struct array* path, struct simulation* simulation, struct network* network){
  struct route* route;
  uint64_t next_event_time;
  struct event* send_payment_event;
  route = transform_path_into_route(path, payment->amount, network, simulation->current_time);
  payment->route = route;
  // execute send_payment event immediately
  next_event_time = simulation->current_time;
  send_payment_event = new_event(next_event_time, SENDPAYMENT, payment->sender, payment );
  simulation->events = heap_insert(simulation->events, send_payment_event, compare_event);
}


struct payment* create_payment_shard(long shard_id, uint64_t shard_amount, struct payment* payment){
  struct payment* shard;
  shard = new_payment(shard_id, payment->sender, payment->receiver, shard_amount, payment->start_time, payment->max_fee_limit);
  shard->attempts = 1;
  shard->is_shard = 1;
  return shard;
}

/*HTLC FUNCTIONS*/

/* find a path for a payment (a modified version of dijkstra is used: see `routing.c`) */
void find_path(struct event *event, struct simulation* simulation, struct network* network, struct array** payments, unsigned int mpp, enum routing_method routing_method, struct network_params net_params) {
  struct payment *payment, *shard1, *shard2;
  struct array *path, *shard1_path, *shard2_path;
  uint64_t shard1_amount, shard2_amount;
  enum pathfind_error error;
  long shard1_id, shard2_id;

  payment = event->payment;

  ++(payment->attempts);

  if(net_params.payment_timeout != -1 && simulation->current_time > payment->start_time + net_params.payment_timeout) {
    payment->end_time = simulation->current_time;
    payment->is_timeout = 1;
    return;
  }

  // find path
  if(routing_method == CLOTH_ORIGINAL) {
      if (payment->attempts == 1) {
          path = paths[payment->id];
      }else {
          path = dijkstra(payment->sender, payment->receiver, payment->amount, network, simulation->current_time, 0, &error, net_params.routing_method, NULL, payment->max_fee_limit);
      }
  } else {

      if (payment->attempts == 1) {
          path = paths[payment->id];
          if (path != NULL) {

              // calc path capacity
              uint64_t path_cap = INT64_MAX;
              for (int i = 0; i < array_len(path); i++) {
                  struct route_hop *hop = array_get(path, i);
                  struct edge *edge = array_get(network->edges, hop->edge_id);
                  uint64_t estimated_cap;
                  if (i == 0) {
                      // if first edge of the path (directory connected edge to source node)
                      estimated_cap = edge->balance;
                  } else {
                      estimated_cap = estimate_capacity(edge, network, routing_method);
                  }
                  if (estimated_cap < path_cap) path_cap = estimated_cap;
              }

              // calc total fee
              struct route *route = transform_path_into_route(path, payment->amount, network, simulation->current_time);
              uint64_t fee = route->total_fee;
              free_route(route);

              // if path capacity is not enough to send the payment, find new path
              if (path_cap < payment->amount + fee) {
                  path = dijkstra(payment->sender, payment->receiver, payment->amount, network, simulation->current_time, 0, &error, net_params.routing_method, NULL, payment->max_fee_limit);
              }
          } else {
              path = dijkstra(payment->sender, payment->receiver, payment->amount, network, simulation->current_time, 0, &error, net_params.routing_method, NULL, payment->max_fee_limit);
          }
      } else {

          // exclude edges
          struct element* exclude_edges = NULL;
          for(struct element* iterator = payment->history; iterator != NULL; iterator = iterator->next) {
            struct attempt* a = iterator->data;
            struct edge* exclude_edge = array_get(network->edges, a->error_edge_id);
            exclude_edges = push(exclude_edges, exclude_edge);
          }

          path = dijkstra(payment->sender, payment->receiver, payment->amount, network, simulation->current_time, 0, &error, net_params.routing_method, exclude_edges, payment->max_fee_limit);
      }
  }

  if (path != NULL) {
    generate_send_payment_event(payment, path, simulation, network);
    return;
  }

  //  if a path is not found, try to split the payment in two shards (multi-path payment)
  if(mpp && path == NULL && !(payment->is_shard) && payment->attempts == 1 ){
    shard1_amount = payment->amount/2;
    shard2_amount = payment->amount - shard1_amount;
    shard1_path = dijkstra(payment->sender, payment->receiver, shard1_amount, network, simulation->current_time, 0, &error, net_params.routing_method, NULL, payment->max_fee_limit / 2);
    if(shard1_path == NULL){
      payment->end_time = simulation->current_time;
      return;
    }
    shard2_path = dijkstra(payment->sender, payment->receiver, shard2_amount, network, simulation->current_time, 0, &error, net_params.routing_method, NULL, payment->max_fee_limit / 2);
    if(shard2_path == NULL){
      payment->end_time = simulation->current_time;
      return;
    }
    // if shard1_path and shard2_path is same route, return
    if(routing_method != CLOTH_ORIGINAL) {
        long shard1_path_len = array_len(shard1_path);
        long shard2_path_len = array_len(shard2_path);
        if (shard1_path_len == shard2_path_len) {
            int duplicated = 0;
            for (int i = 0; i < shard1_path_len; i++) {
                struct route_hop *shard1_hop = array_get(shard1_path, i);
                for (int j = 0; j < shard2_path_len; j++) {
                    struct route_hop *shard2_hop = array_get(shard2_path, j);
                    if (shard1_hop->edge_id == shard2_hop->edge_id) duplicated++;
                }
            }
            // all hop of shade1_path is same as shade2_path's, return
            if (duplicated == shard1_path_len && duplicated == shard2_path_len) {
                payment->end_time = simulation->current_time;
                return;
            }
        }
    }
    shard1_id = array_len(*payments);
    shard2_id = array_len(*payments) + 1;
    shard1 = create_payment_shard(shard1_id, shard1_amount, payment);
    shard2 = create_payment_shard(shard2_id, shard2_amount, payment);
    *payments = array_insert(*payments, shard1);
    *payments = array_insert(*payments, shard2);
    payment->is_shard = 1;
    payment->shards_id[0] = shard1_id;
    payment->shards_id[1] = shard2_id;
    generate_send_payment_event(shard1, shard1_path, simulation, network);
    generate_send_payment_event(shard2, shard2_path, simulation, network);
    return;
  }

  // no path
  payment->end_time = simulation->current_time;
}

/* send an HTLC for the payment (behavior of the payment sender) */
void send_payment(struct event* event, struct simulation* simulation, struct network* network, struct network_params net_params){
  struct payment* payment;
  uint64_t next_event_time;
  struct route* route;
  struct route_hop* first_route_hop;
  struct edge* next_edge;
  struct event* next_event;
  enum event_type event_type;
  unsigned long is_next_node_offline;
  struct node* node;

  payment = event->payment;
  route = payment->route;
  node = array_get(network->nodes, event->node_id);
  first_route_hop = array_get(route->route_hops, 0);
  next_edge = array_get(network->edges, first_route_hop->edge_id);

  if(!is_present(next_edge->id, node->open_edges)) {
    printf("ERROR (send_payment): edge %ld is not an edge of node %ld \n", next_edge->id, node->id);
    exit(-1);
  }

  first_route_hop->edges_lock_start_time = simulation->current_time;

  /* simulate the case that the next node in the route is offline */
  is_next_node_offline = gsl_ran_discrete(simulation->random_generator, network->faulty_node_prob);
  if(is_next_node_offline){
    payment->offline_node_count += 1;
    payment->error.type = OFFLINENODE;
    payment->error.hop = first_route_hop;
    next_event_time = simulation->current_time + OFFLINELATENCY;
    next_event = new_event(next_event_time, RECEIVEFAIL, event->node_id, event->payment);
    simulation->events = heap_insert(simulation->events, next_event, compare_event);
    return;
  }

  // fail no balance
  if(first_route_hop->amount_to_forward > next_edge->balance) {
    payment->error.type = NOBALANCE;
    payment->error.hop = first_route_hop;
    payment->no_balance_count += 1;
    next_event_time = simulation->current_time;
    next_event = new_event(next_event_time, RECEIVEFAIL, event->node_id, event->payment);
    simulation->events = heap_insert(simulation->events, next_event, compare_event);
    return;
  }

  // update balance
  uint64_t prev_balance = next_edge->balance;
  (void)prev_balance; /* silence unused warning */
  next_edge->balance -= first_route_hop->amount_to_forward;

  next_edge->tot_flows += 1;

  // success sending
  event_type = first_route_hop->to_node_id == payment->receiver ? RECEIVEPAYMENT : FORWARDPAYMENT;
  next_event_time = simulation->current_time + net_params.average_payment_forward_interval + (long)(fabs(net_params.variance_payment_forward_interval * gsl_ran_ugaussian(simulation->random_generator)));
  next_event = new_event(next_event_time, event_type, first_route_hop->to_node_id, event->payment);
  simulation->events = heap_insert(simulation->events, next_event, compare_event);
}

/* forward an HTLC for the payment (behavior of an intermediate hop node in a route) */
void forward_payment(struct event* event, struct simulation* simulation, struct network* network, struct network_params net_params){
  struct payment* payment;
  struct route* route;
  struct route_hop* next_route_hop, *previous_route_hop;
  long  prev_node_id;
  enum event_type event_type;
  struct event* next_event;
  uint64_t next_event_time;
  unsigned long is_next_node_offline;
  struct node* node;
  unsigned int is_last_hop;
  struct edge *next_edge = NULL, *prev_edge;

  payment = event->payment;
  node = array_get(network->nodes, event->node_id);
  route = payment->route;
  next_route_hop=get_route_hop(node->id, route->route_hops, 1);
  previous_route_hop = get_route_hop(node->id, route->route_hops, 0);
  is_last_hop = next_route_hop->to_node_id == payment->receiver;
  next_route_hop->edges_lock_start_time = simulation->current_time;

  if(!is_present(next_route_hop->edge_id, node->open_edges)) {
    printf("ERROR (forward_payment): edge %ld is not an edge of node %ld \n", next_route_hop->edge_id, node->id);
    exit(-1);
  }

  /* simulate the case that the next node in the route is offline */
  is_next_node_offline = gsl_ran_discrete(simulation->random_generator, network->faulty_node_prob);
  if(is_next_node_offline && !is_last_hop){ //assume that the receiver node is always online
    payment->offline_node_count += 1;
    payment->error.type = OFFLINENODE;
    payment->error.hop = next_route_hop;
    prev_node_id = previous_route_hop->from_node_id;
    event_type = prev_node_id == payment->sender ? RECEIVEFAIL : FORWARDFAIL;
    next_event_time = simulation->current_time + net_params.average_payment_forward_interval + (long)(fabs(net_params.variance_payment_forward_interval * gsl_ran_ugaussian(simulation->random_generator))) + OFFLINELATENCY;
    next_event = new_event(next_event_time, event_type, prev_node_id, event->payment);
    simulation->events = heap_insert(simulation->events, next_event, compare_event);
    return;
  }

  // STRICT FORWARDING
  prev_edge = array_get(network->edges,previous_route_hop->edge_id);
  next_edge = array_get(network->edges, next_route_hop->edge_id);

  // fail no balance
  if(!check_balance_and_policy(next_edge, prev_edge, previous_route_hop, next_route_hop)){
    payment->error.type = NOBALANCE;
    payment->error.hop = next_route_hop;
    payment->no_balance_count += 1;
    prev_node_id = previous_route_hop->from_node_id;
    event_type = prev_node_id == payment->sender ? RECEIVEFAIL : FORWARDFAIL;
    next_event_time = simulation->current_time + net_params.average_payment_forward_interval + (long)(fabs(net_params.variance_payment_forward_interval * gsl_ran_ugaussian(simulation->random_generator)));//prev_channel->latency;
    next_event = new_event(next_event_time, event_type, prev_node_id, event->payment);
    simulation->events = heap_insert(simulation->events, next_event, compare_event);
    return;
  }

  // update balance
  uint64_t prev_balance = next_edge->balance;
  (void)prev_balance;
  next_edge->balance -= next_route_hop->amount_to_forward;

  next_edge->tot_flows += 1;

  // success forwarding
  event_type = is_last_hop  ? RECEIVEPAYMENT : FORWARDPAYMENT;
  // interval for forwarding payment
  next_event_time = simulation->current_time + net_params.average_payment_forward_interval + (long)(fabs(net_params.variance_payment_forward_interval * gsl_ran_ugaussian(simulation->random_generator)));//next_channel->latency;
  next_event = new_event(next_event_time, event_type, next_route_hop->to_node_id, event->payment);
  simulation->events = heap_insert(simulation->events, next_event, compare_event);
}

/* receive a payment (behavior of the payment receiver node) */
void receive_payment(struct event* event, struct simulation* simulation, struct network* network, struct network_params net_params){
  long  prev_node_id;
  struct route* route;
  struct payment* payment;
  struct route_hop* last_route_hop;
  struct edge* forward_edge,*backward_edge;
  struct event* next_event;
  enum event_type event_type;
  uint64_t next_event_time;
  struct node* node;

  payment = event->payment;
  route = payment->route;
  node = array_get(network->nodes, event->node_id);

  last_route_hop = array_get(route->route_hops, array_len(route->route_hops) - 1);
  forward_edge = array_get(network->edges, last_route_hop->edge_id);
  backward_edge = array_get(network->edges, forward_edge->counter_edge_id);

  last_route_hop->edges_lock_end_time = simulation->current_time;

  if(!is_present(backward_edge->id, node->open_edges)) {
    printf("ERROR (receive_payment): edge %ld is not an edge of node %ld \n", backward_edge->id, node->id);
    exit(-1);
  }

  // update balance
  backward_edge->balance += last_route_hop->amount_to_forward;

  payment->is_success = 1;

  prev_node_id = last_route_hop->from_node_id;
  event_type = prev_node_id == payment->sender ? RECEIVESUCCESS : FORWARDSUCCESS;
  next_event_time = simulation->current_time + net_params.average_payment_forward_interval + (long)(fabs(net_params.variance_payment_forward_interval * gsl_ran_ugaussian(simulation->random_generator)));//channel->latency;
  next_event = new_event(next_event_time, event_type, prev_node_id, event->payment);
  simulation->events = heap_insert(simulation->events, next_event, compare_event);
}

/* forward an HTLC success back to the payment sender (behavior of a intermediate hop node in the route) */
void forward_success(struct event* event, struct simulation* simulation, struct network* network, struct network_params net_params){
  struct route_hop* prev_hop;
  struct payment* payment;
  struct edge* forward_edge, * backward_edge;
  long prev_node_id;
  struct event* next_event;
  enum event_type event_type;
  struct node* node;
  uint64_t next_event_time;

  payment = event->payment;
  prev_hop = get_route_hop(event->node_id, payment->route->route_hops, 0);
  forward_edge = array_get(network->edges, prev_hop->edge_id);
  backward_edge = array_get(network->edges, forward_edge->counter_edge_id);
  node = array_get(network->nodes, event->node_id);
  prev_hop->edges_lock_end_time = simulation->current_time;

  if(!is_present(backward_edge->id, node->open_edges)) {
    printf("ERROR (forward_success): edge %ld is not an edge of node %ld \n", backward_edge->id, node->id);
    exit(-1);
  }

  // update balance
  backward_edge->balance += prev_hop->amount_to_forward;

  prev_node_id = prev_hop->from_node_id;
  event_type = prev_node_id == payment->sender ? RECEIVESUCCESS : FORWARDSUCCESS;
  next_event_time = simulation->current_time + net_params.average_payment_forward_interval + (long)(fabs(net_params.variance_payment_forward_interval * gsl_ran_ugaussian(simulation->random_generator)));//prev_channel->latency;
  next_event = new_event(next_event_time, event_type, prev_node_id, event->payment);
  simulation->events = heap_insert(simulation->events, next_event, compare_event);
}

/* receive an HTLC success (behavior of the payment sender node) */
void receive_success(struct event* event, struct simulation* simulation, struct network* network, struct network_params net_params){
  struct node* node;
  struct payment* payment;
  payment = event->payment;
  node = array_get(network->nodes, event->node_id);
  event->payment->end_time = simulation->current_time;

  add_attempt_history(payment, network, simulation->current_time, 1);

  // store edge locked time and balance for statistics
  for(int i = 0; i < array_len(payment->route->route_hops); i++){
      struct route_hop* route_hop = array_get(payment->route->route_hops, i);
      struct edge* edge = array_get(network->edges, route_hop->edge_id);

      struct edge_locked_balance_and_duration* edge_locked_balance_time = malloc(sizeof(struct edge_locked_balance_and_duration));
      edge_locked_balance_time->locked_balance = route_hop->amount_to_forward;
      edge_locked_balance_time->locked_start_time = route_hop->edges_lock_start_time;
      edge_locked_balance_time->locked_end_time = route_hop->edges_lock_end_time;
      if (route_hop->edges_lock_start_time > route_hop->edges_lock_end_time){
          edge_locked_balance_time->locked_end_time = simulation->current_time;
      }
      edge->edge_locked_balance_and_durations = push(edge->edge_locked_balance_and_durations, edge_locked_balance_time);
  }

    // next event
    uint64_t next_event_time = simulation->current_time + net_params.group_broadcast_delay;

    // request_group_update event
    if (net_params.routing_method == GROUP_ROUTING) {
        struct event *next_event = new_event(next_event_time, UPDATEGROUP, event->node_id, event->payment);
        simulation->events = heap_insert(simulation->events, next_event, compare_event);
    }

    // channel update broadcast event
    struct event *channel_update_event = new_event(next_event_time, CHANNELUPDATESUCCESS, node->id, payment);
    simulation->events = heap_insert(simulation->events, channel_update_event, compare_event);
}

/* forward an HTLC fail back to the payment sender (behavior of a intermediate hop node in the route) */
void forward_fail(struct event* event, struct simulation* simulation, struct network* network, struct network_params net_params){
  struct payment* payment;
  struct route_hop* next_hop, *prev_hop;
  struct edge* next_edge;
  long prev_node_id;
  struct event* next_event;
  enum event_type event_type;
  struct node* node;
  uint64_t next_event_time;

  node = array_get(network->nodes, event->node_id);
  payment = event->payment;
  next_hop = get_route_hop(event->node_id, payment->route->route_hops, 1);
  next_edge = array_get(network->edges, next_hop->edge_id);

  if(!is_present(next_edge->id, node->open_edges)) {
    printf("ERROR (forward_fail): edge %ld is not an edge of node %ld \n", next_edge->id, node->id);
    exit(-1);
  }

  next_hop->edges_lock_end_time = simulation->current_time;

  /* since the payment failed, the balance must be brought back to the state before the payment occurred */
  uint64_t prev_balance = next_edge->balance;
  (void)prev_balance;
  next_edge->balance += next_hop->amount_to_forward;

  prev_hop = get_route_hop(event->node_id, payment->route->route_hops, 0);
  prev_node_id = prev_hop->from_node_id;
  event_type = prev_node_id == payment->sender ? RECEIVEFAIL : FORWARDFAIL;
  next_event_time = simulation->current_time + net_params.average_payment_forward_interval + (long)(fabs(net_params.variance_payment_forward_interval * gsl_ran_ugaussian(simulation->random_generator)));//prev_channel->latency;
  next_event = new_event(next_event_time, event_type, prev_node_id, event->payment);
  simulation->events = heap_insert(simulation->events, next_event, compare_event);
}

/* receive an HTLC fail (behavior of the payment sender node) */
void receive_fail(struct event* event, struct simulation* simulation, struct network* network, struct network_params net_params){
  struct payment* payment;
  struct route_hop* first_hop, *error_hop;
  struct edge* next_edge, *error_edge;
  struct event* next_event;
  struct node* node;
  uint64_t next_event_time;

  payment = event->payment;
  node = array_get(network->nodes, event->node_id);

  error_hop = payment->error.hop;
  error_edge = array_get(network->edges, error_hop->edge_id);
  if(error_hop->from_node_id != payment->sender){ // if the error occurred in the first hop, the balance hasn't to be updated, since it was not decreased
    first_hop = array_get(payment->route->route_hops, 0);
    next_edge = array_get(network->edges, first_hop->edge_id);
    if(!is_present(next_edge->id, node->open_edges)) {
      printf("ERROR (receive_fail): edge %ld is not an edge of node %ld \n", next_edge->id, node->id);
      exit(-1);
    }

    uint64_t prev_balance = next_edge->balance;
    (void)prev_balance;
    next_edge->balance += first_hop->amount_to_forward;
  }

  /* record channel_update */
  struct channel_update *channel_update = malloc(sizeof(struct channel_update));
  channel_update->htlc_maximum_msat = payment->amount;
  channel_update->edge_id = error_edge->id;
  channel_update->time = simulation->current_time;
  error_edge->channel_updates = push(error_edge->channel_updates, channel_update);

  add_attempt_history(payment, network, simulation->current_time, 0);

  for(int i = 0; i < array_len(payment->route->route_hops); i++){
      struct route_hop* route_hop = array_get(payment->route->route_hops, i);
      struct edge* edge = array_get(network->edges, route_hop->edge_id);

      struct edge_locked_balance_and_duration* edge_locked_balance_time = malloc(sizeof(struct edge_locked_balance_and_duration));
      edge_locked_balance_time->locked_balance = route_hop->amount_to_forward;
      edge_locked_balance_time->locked_start_time = route_hop->edges_lock_start_time;
      edge_locked_balance_time->locked_end_time = route_hop->edges_lock_end_time;
      if (route_hop->edges_lock_start_time > route_hop->edges_lock_end_time){
          edge_locked_balance_time->locked_end_time = simulation->current_time;
      }
      edge->edge_locked_balance_and_durations = push(edge->edge_locked_balance_and_durations, edge_locked_balance_time);

      if(payment->error.hop->edge_id == edge->id) break;
  }

  next_event_time = simulation->current_time;
  next_event = new_event(next_event_time, FINDPATH, payment->sender, payment);
  simulation->events = heap_insert(simulation->events, next_event, compare_event);

  /* channel update broadcast event */
  struct event *channel_update_event = new_event(simulation->current_time + net_params.group_broadcast_delay, CHANNELUPDATEFAIL, node->id, payment);
  simulation->events = heap_insert(simulation->events, channel_update_event, compare_event);
}

/* ====== UPDATED: request_group_update with leave/rejoin mechanism ====== */
struct element* request_group_update(struct event* event, struct simulation* simulation, struct network* network, struct network_params net_params, struct element* group_add_queue){

    int scheduled_construct = 0; /* whether to schedule CONSTRUCTGROUPS at the end */

    /* parameters for leave decision (MVP defaults) */
    const uint64_t cooldown_ms = (uint64_t)net_params.cooldown_hops
                           * (uint64_t)net_params.average_payment_forward_interval;
    const uint32_t K_used = (uint32_t)net_params.k_used_on_min_edge;
    const uint32_t max_leaves_per_group_tick = (uint32_t)net_params.max_leaves_per_group_tick;

    for(long i = 0; i < array_len(event->payment->route->route_hops); i++){
        struct route_hop* hop = array_get(event->payment->route->route_hops, i);
        struct edge* edge = array_get(network->edges, hop->edge_id);
        struct edge* counter_edge = array_get(network->edges, edge->counter_edge_id);

        /* --- handle group for edge --- */
        if(edge->group != NULL) {
            struct group* group = edge->group;
            int close_flg = update_group(group, net_params, simulation->current_time);
            if (close_flg) {
                // メンバーを外す前に "id-id-..." を作る
                group_close_once(simulation, group, "update_violation");

                /* add all edges to queue and clear membership */
                for(long j = 0; j < array_len(group->edges); j++){
                    struct edge* edge_in_group = array_get(group->edges, j);
                    edge_in_group->group = NULL;
                    edge_in_group->last_leave_time = simulation->current_time; /* mark leave time on close */
                    group_add_queue = list_insert_sorted_position(group_add_queue, edge_in_group, (long (*)(void *)) get_edge_balance);
                }

                /* schedule reconstruction immediately */
                uint64_t next_event_time = simulation->current_time;
                struct event* next_event = new_event(next_event_time, CONSTRUCTGROUPS, event->node_id, event->payment);
                simulation->events = heap_insert(simulation->events, next_event, compare_event);
                scheduled_construct = 1;
            } else {
                /* leave decision (UL/K with cooldown, cap per tick) */
                long m = array_len(group->edges);
                struct array* leave_candidates = array_initialize(m > 0 ? m : 1);
                uint32_t leaves_this_tick = 0;

                for(long j = 0; j < m; j++){
                    struct edge* e = array_get(group->edges, j);
                    if(e == NULL) continue;

                    /* cooldown */
                    if(simulation->current_time >= e->last_leave_time &&
                       (simulation->current_time - e->last_leave_time) < cooldown_ms){
                        continue;
                    }

                    /* UL = max(0, 1 - group_cap / balance) */
                    double UL = 0.0;
                    if(e->balance > 0){
                        UL = 1.0 - ((double)group->group_cap / (double)e->balance);
                        if(UL < 0.0) UL = 0.0;
                        if(UL > 1.0) UL = 1.0;
                    }
                    int is_min_edge = (e->balance == group->group_cap);
                    uint64_t used_since_join = (e->tot_flows >= e->flows_at_join) ? (e->tot_flows - e->flows_at_join) : 0;

                    int divergence = (UL >= e->tolerance_tau);
                    int overuse    = (is_min_edge && (used_since_join >= K_used));

                    if(divergence || overuse){
                        leave_candidates = array_insert(leave_candidates, e);
                    }
                }

                long c = array_len(leave_candidates);
                for(long k = 0; k < c && leaves_this_tick < (long)max_leaves_per_group_tick; k++){
                    struct edge* e = array_get(leave_candidates, k);
                    if (net_params.enable_group_event_csv && csv_group_events) {
                        double UL = 0.0;
                        if (e->balance > 0) {
                            UL = 1.0 - ((double)group->group_cap / (double)e->balance);
                            if (UL < 0.0) UL = 0.0;
                            if (UL > 1.0) UL = 1.0;
                        }
                        uint64_t used_since_join = (e->tot_flows >= e->flows_at_join)
                                                   ? (e->tot_flows - e->flows_at_join) : 0;

                        // 理由の詳細はセミコロン区切りで（CSV列ずれ回避）
                        char reason_buf[128];
                        snprintf(reason_buf, sizeof(reason_buf),
                                 "UL=%.6f;used=%" PRIu64, UL, used_since_join);

                        ge_leave((uint64_t)simulation->current_time,
                                 group->id,
                                 e->id,
                                 reason_buf,
                                 group->group_cap,
                                 group->min_cap,
                                 group->max_cap,
                                 group->seed_edge_id,
                                 group->attempt_id);
                    }

                    /* remove from group and enqueue */
                    remove_edge_from_group(group, e);
                    e->group = NULL;
                    e->last_leave_time = simulation->current_time;

                    group_add_queue = list_insert_sorted_position(group_add_queue, e, (long (*)(void *)) get_edge_balance);

                    leaves_this_tick++;
                    scheduled_construct = 1;
                }

                array_free(leave_candidates);
            }
        }

        /* --- handle group for counter_edge (symmetric) --- */
        if(counter_edge->group != NULL) {
            struct group* group = counter_edge->group;
            int close_flg = update_group(group, net_params, simulation->current_time);

            if (close_flg) {
                if (group->is_closed == 0) {
                    group_close_once(simulation, group, "update_violation");
                }

                for (long j = 0; j < array_len(group->edges); j++) {
                    struct edge* edge_in_group = array_get(group->edges, j);
                    if (edge_in_group) {
                        edge_in_group->group = NULL;
                        edge_in_group->last_leave_time = simulation->current_time;
                        group_add_queue = list_insert_sorted_position(
                            group_add_queue, edge_in_group, (long (*)(void *)) get_edge_balance);
                    }
                }

                uint64_t next_event_time = simulation->current_time;
                struct event* next_event = new_event(next_event_time, CONSTRUCTGROUPS, event->node_id, event->payment);
                simulation->events = heap_insert(simulation->events, next_event, compare_event);
                scheduled_construct = 1;
            } else {
                long m = array_len(group->edges);
                struct array* leave_candidates = array_initialize(m > 0 ? m : 1);
                uint32_t leaves_this_tick = 0;

                for(long j = 0; j < m; j++){
                    struct edge* e = array_get(group->edges, j);
                    if(e == NULL) continue;

                    if(simulation->current_time >= e->last_leave_time &&
                       (simulation->current_time - e->last_leave_time) < cooldown_ms){
                        continue;
                    }

                    double UL = 0.0;
                    if(e->balance > 0){
                        UL = 1.0 - ((double)group->group_cap / (double)e->balance);
                        if(UL < 0.0) UL = 0.0;
                        if(UL > 1.0) UL = 1.0;
                    }
                    int is_min_edge = (e->balance == group->min_cap);
                    uint64_t used_since_join = (e->tot_flows >= e->flows_at_join) ? (e->tot_flows - e->flows_at_join) : 0;

                    int divergence = (UL >= e->tolerance_tau);
                    int overuse    = (is_min_edge && (used_since_join >= K_used));

                    if(divergence || overuse){
                        leave_candidates = array_insert(leave_candidates, e);
                    }
                }

                long c = array_len(leave_candidates);
                for(long k = 0; k < c && leaves_this_tick < (long)max_leaves_per_group_tick; k++){
                    struct edge* e = array_get(leave_candidates, k);
                    remove_edge_from_group(group, e);
                    e->group = NULL;
                    e->last_leave_time = simulation->current_time;

                    group_add_queue = list_insert_sorted_position(group_add_queue, e, (long (*)(void *)) get_edge_balance);

                    leaves_this_tick++;
                    scheduled_construct = 1;
                }

                array_free(leave_candidates);
            }
        }
    }

    /* schedule CONSTRUCTGROUPS once if any leave/close occurred (or rely on above immediate schedule for close) */
    if(scheduled_construct){
        uint64_t next_event_time = simulation->current_time + net_params.group_broadcast_delay;
        struct event* next_event = new_event(next_event_time, CONSTRUCTGROUPS, event->node_id, event->payment);
        simulation->events = heap_insert(simulation->events, next_event, compare_event);
    }

    return group_add_queue;
}

/* ====== UPDATED: construct_groups ====== */
struct element* construct_groups(struct simulation* simulation,
                                 struct element* group_add_queue,
                                 struct network *network,
                                 struct network_params net_params)
{
    if (group_add_queue == NULL) return group_add_queue;

    static uint64_t attempt_counter = 0;  // プロセス全体で単調増加（衝突回避）

    for (struct element* iterator = group_add_queue; iterator != NULL; iterator = iterator->next) {
        int logged_begin = 0;
        int logged_abort = 0;

        struct edge* requesting_edge = iterator->data;
        uint64_t attempt_id = ++attempt_counter; // 1シード試行 = 1 attempt_id

        if (net_params.enable_group_event_csv && csv_group_events && !logged_begin) {
            ge_construct_begin((uint64_t)simulation->current_time,
                               (long)requesting_edge->id,
                               (uint64_t)attempt_id);
            logged_begin = 1;
        }

        // new group
        struct group* group = malloc(sizeof(struct group));
        group->edges = array_initialize(net_params.group_size);
        group->edges = array_insert(group->edges, requesting_edge);
        group->seed_edge_id = requesting_edge->id;
        group->attempt_id   = attempt_id;

        if (net_params.use_conventional_method) {
            group->max_cap_limit = requesting_edge->balance + (uint64_t)((float)requesting_edge->balance * net_params.group_limit_rate);
            group->min_cap_limit = requesting_edge->balance - (uint64_t)((float)requesting_edge->balance * net_params.group_limit_rate);
            if (group->max_cap_limit < requesting_edge->balance) group->max_cap_limit = UINT64_MAX;
            if (group->min_cap_limit > requesting_edge->balance) group->min_cap_limit = 0;
        } else {
            group->max_cap_limit = (uint64_t)((float)requesting_edge->balance * net_params.group_max_cap_ratio);
            group->min_cap_limit = (uint64_t)((float)requesting_edge->balance * net_params.group_min_cap_ratio);
        }

        group->id = -1; /* ★未確定のあいだは -1 のまま */
        group->is_closed = 0;
        group->constructed_time = simulation->current_time;
        group->history = NULL;

        // search the closest balance edge from neighbor
        struct element* bottom = iterator;
        struct element* top = iterator;
        while (bottom != NULL || top != NULL) {

            // both edge are out of group limit, skip this group
            if (top != NULL && bottom != NULL) {
                struct edge* bottom_edge_chk = bottom->data;
                struct edge* top_edge_chk = top->data;
                if (bottom_edge_chk->balance < group->min_cap_limit && top_edge_chk->balance > group->max_cap_limit) {
                    break;
                }
            }

            // join bottom edge to group (if any)
            if (bottom != NULL) {
                struct edge* bottom_edge = bottom->data;
                if (can_join_group(group, bottom_edge)) {
                    group->edges = array_insert(group->edges, bottom_edge);

                    if (array_len(group->edges) == net_params.group_size) break;
                }
                bottom = bottom->prev;
            }

            // join top edge to group (if any)
            if (top != NULL) {
                struct edge* top_edge = top->data;
                if (can_join_group(group, top_edge)) {
                    group->edges = array_insert(group->edges, top_edge);

                    if (array_len(group->edges) == net_params.group_size) break;
                }
                top = top->next;
            }
        }

        // register group
        if (array_len(group->edges) == net_params.group_size) {
            /* ★成功確定の瞬間に初めて ID を採番 */
            group->id = array_len(network->groups);

            // init group_cap/min/max based on real balances
            update_group(group, net_params, simulation->current_time); /* ← network.c 側で group->id>=0 のみログ */

            network->groups = array_insert(network->groups, group);

            // メンバーIDを "id-id-..." に連結
            char members_buf[8192]; members_buf[0] = '\0';
            for (int k = 0; k < array_len(group->edges); k++) {
                struct edge* me = array_get(group->edges, k);
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "%s%ld", (k == 0 ? "" : "-"), me->id);
                strncat(members_buf, tmp, sizeof(members_buf) - strlen(members_buf) - 1);
            }
            group->seed_edge_id = requesting_edge->id;
            group->attempt_id   = attempt_id;
            group->is_closed = 0;
            network->groups = array_insert(network->groups, group);
            if (net_params.enable_group_event_csv && csv_group_events) {
                ge_construct_commit(
                    (uint64_t)simulation->current_time,
                    (long)group->id,
                    members_buf,
                    (uint64_t)group->group_cap,
                    (uint64_t)group->min_cap,
                    (uint64_t)group->max_cap,
                    (long)requesting_edge->id,
                    (uint64_t)attempt_id
                );
            }

            for (int i = 0; i < array_len(group->edges); i++) {
                struct edge* group_member_edge = array_get(group->edges, i);
                group_add_queue = list_delete(
                    group_add_queue, &iterator, group_member_edge,
                    (int (*)(void *, void *)) is_equal_edge
                );
                group_member_edge->group = group;

                /* (re)join メタの初期化 */
                group_member_edge->join_time     = simulation->current_time;
                group_member_edge->flows_at_join = group_member_edge->tot_flows;
                group_member_edge->tolerance_tau = net_params.tau_randomize
                    ? (gsl_rng_uniform(simulation->random_generator) *
                       (net_params.tau_max - net_params.tau_min) + net_params.tau_min)
                    : net_params.tau_default;

                /* join ログ */
                if (net_params.enable_group_event_csv && csv_group_events) {
                    ge_join((uint64_t)simulation->current_time,
                            (long)group->id,
                            (long)group_member_edge->id,
                            "join",
                            (uint64_t)group->group_cap,
                            (uint64_t)group->min_cap,
                            (uint64_t)group->max_cap,
                            (long)requesting_edge->id,
                            (uint64_t)attempt_id);
                }
            }

            if (iterator == NULL) break;

        } else {
            /* 4-4: 失敗クリーンアップ直前（abortは1回だけ） */
            if (!logged_abort && net_params.enable_group_event_csv && csv_group_events) {
                ge_construct_abort((uint64_t)simulation->current_time,
                                   (long)requesting_edge->id,
                                   (int)array_len(group->edges),   // gathered
                                   (int)net_params.group_size,     // needed
                                   (uint64_t)attempt_id);
                logged_abort = 1;
            }
            if (group->id >= 0 && group->is_closed == 0) {
                group_close_once(simulation, group, "cleanup_without_close");
            }

            for (long j = 0; j < array_len(group->edges); j++) {
                struct edge* e = array_get(group->edges, j);
                if (e && e->group == group) e->group = NULL;
            }

            array_free(group->edges);
            free(group);
        }
    }
    return group_add_queue;
}

void channel_update_success(struct event* event, struct simulation* simulation, struct network* network){
    struct node* node = array_get(network->nodes, event->node_id);
    process_success_result(node, event->payment, simulation->current_time);
}

void channel_update_fail(struct event* event, struct simulation* simulation, struct network* network){
    struct node* node = array_get(network->nodes, event->node_id);
    process_fail_result(node, event->payment, simulation->current_time);
}
