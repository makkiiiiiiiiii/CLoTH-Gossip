#include <string.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_rng.h>
#include <inttypes.h>
#include "../include/network.h"
#include "../include/array.h"
#include "../include/utils.h"
#include "../include/event.h"

/* Functions in this file generate a payment-channel network where to simulate the execution of payments */

struct node* new_node(long id) {
  struct node* node = (struct node*)malloc(sizeof(struct node));
  node->id = id;
  node->open_edges = array_initialize(10);
  node->results = NULL;
  node->explored = 0;
  return node;
}

struct channel* new_channel(long id, long direction1, long direction2, long node1, long node2, uint64_t capacity) {
  struct channel* channel = (struct channel*)malloc(sizeof(struct channel));
  channel->id = id;
  channel->edge1 = direction1;
  channel->edge2 = direction2;
  channel->node1 = node1;
  channel->node2 = node2;
  channel->capacity = capacity;
  channel->is_closed = 0;
  return channel;
}

/* one directional edge of a channel */
struct edge* new_edge(long id, long channel_id, long counter_edge_id,
                      long from_node_id, long to_node_id,
                      uint64_t balance, struct policy policy,
                      uint64_t channel_capacity){
  struct edge* edge = (struct edge*)malloc(sizeof(struct edge));
  edge->id = id;
  edge->channel_id = channel_id;
  edge->from_node_id = from_node_id;
  edge->to_node_id = to_node_id;
  edge->counter_edge_id = counter_edge_id;
  edge->policy = policy;
  edge->balance = balance;
  edge->is_closed = 0;
  edge->tot_flows = 0;
  edge->group = NULL;

  struct channel_update* channel_update = (struct channel_update*)malloc(sizeof(struct channel_update));
  channel_update->htlc_maximum_msat = channel_capacity;
  channel_update->edge_id = edge->id;
  channel_update->time = 0;
  edge->channel_updates = push(NULL, channel_update);
  edge->edge_locked_balance_and_durations = NULL;

  /* initialize leave/rejoin related fields */
  edge->join_time       = 0;     /* set when the edge actually joins a group */
  edge->flows_at_join   = 0;     /* snapshot of tot_flows at join_time */
  edge->tolerance_tau   = 0.10;  /* default tolerance */
  edge->last_leave_time = 0;     /* updated when leaving a group */

  /* initialize min-cap usage counter */
  edge->min_cap_use_count = 0;
  edge->in_group_add_queue = 0;

  return edge;
}

/* after generating a network, write it in csv files "nodes.csv" "edges.csv" "channels.csv" */
void write_network_files(struct network* network){
  FILE* nodes_output_file, *edges_output_file, *channels_output_file;
  long i;
  struct node* node;
  struct channel* channel;
  struct edge* edge;

  nodes_output_file = fopen("nodes.csv", "w");
  if(nodes_output_file==NULL) {
    fprintf(stderr, "ERROR: cannot open file <%s>\n", "nodes.csv");
    exit(-1);
  }
  fprintf(nodes_output_file, "id\n");

  channels_output_file = fopen("channels.csv", "w");
  if(channels_output_file==NULL) {
    fprintf(stderr, "ERROR: cannot open file <%s>\n", "channels.csv");
    fclose(nodes_output_file);
    exit(-1);
  }
  fprintf(channels_output_file, "id,edge1_id,edge2_id,node1_id,node2_id,capacity\n");

  edges_output_file = fopen("edges.csv", "w");
  if(edges_output_file==NULL) {
    fprintf(stderr, "ERROR: cannot open file <%s>\n", "edges.csv");
    fclose(nodes_output_file);
    fclose(channels_output_file);
    exit(-1);
  }
  fprintf(edges_output_file, "id,channel_id,counter_edge_id,from_node_id,to_node_id,balance,fee_base,fee_proportional,min_htlc,timelock\n");

  for(i = 0; i < array_len(network->nodes); i++){
    node = array_get(network->nodes, i);
    fprintf(nodes_output_file, "%ld\n", node->id);
  }

  for(i = 0; i < array_len(network->channels); i++){
    channel = array_get(network->channels, i);
    fprintf(channels_output_file, "%ld,%ld,%ld,%ld,%ld,%" PRIu64 "\n",
            channel->id, channel->edge1, channel->edge2, channel->node1, channel->node2,
            (uint64_t)channel->capacity);
  }

  for(i = 0; i < array_len(network->edges); i++){
    edge = array_get(network->edges, i);
    fprintf(edges_output_file, "%ld,%ld,%ld,%ld,%ld,%" PRIu64 ",%ld,%ld,%" PRIu64 ",%d\n",
            edge->id, edge->channel_id, edge->counter_edge_id, edge->from_node_id, edge->to_node_id,
            (uint64_t)edge->balance,
            (long)(edge->policy).fee_base, (long)(edge->policy).fee_proportional,
            (uint64_t)(edge->policy).min_htlc, (int)(edge->policy).timelock);
  }

  fclose(nodes_output_file);
  fclose(edges_output_file);
  fclose(channels_output_file);
}

void update_probability_per_node(double *probability_per_node, int *channels_per_node, long n_nodes, long node1_id, long node2_id, long tot_channels){
  long i;
  channels_per_node[node1_id] += 1;
  channels_per_node[node2_id] += 1;
  for(i = 0; i < n_nodes; i++)
    probability_per_node[i] = ((double)channels_per_node[i]) / tot_channels;
}

/* generate a channel (connecting node1_id and node2_id) with random values */
void generate_random_channel(struct channel channel_data, uint64_t mean_channel_capacity, struct network* network, gsl_rng*random_generator) {
  uint64_t capacity, edge1_balance, edge2_balance;
  struct policy edge1_policy, edge2_policy;
  double min_htlcP[]={0.7, 0.2, 0.05, 0.05}, fraction_capacity;
  gsl_ran_discrete_t* min_htlc_discrete;
  struct channel* channel;
  struct edge* edge1, *edge2;
  struct node* node;

  capacity = (uint64_t)fabs(mean_channel_capacity + gsl_ran_ugaussian(random_generator));
  channel = new_channel(channel_data.id, channel_data.edge1, channel_data.edge2,
                        channel_data.node1, channel_data.node2, capacity*1000);

  fraction_capacity = gsl_rng_uniform(random_generator);
  edge1_balance = (uint64_t)(fraction_capacity * ((double)capacity));
  edge2_balance = capacity - edge1_balance;
  /* convert satoshi to millisatoshi */
  edge1_balance *= 1000;
  edge2_balance *= 1000;

  min_htlc_discrete = gsl_ran_discrete_preproc(4, min_htlcP);

  edge1_policy.fee_base = gsl_rng_uniform_int(random_generator, MAXFEEBASE - MINFEEBASE) + MINFEEBASE;
  edge1_policy.fee_proportional = (gsl_rng_uniform_int(random_generator, MAXFEEPROP - MINFEEPROP) + MINFEEPROP);
  edge1_policy.timelock = gsl_rng_uniform_int(random_generator, MAXTIMELOCK - MINTIMELOCK) + MINTIMELOCK;
  edge1_policy.min_htlc = gsl_pow_int(10, gsl_ran_discrete(random_generator, min_htlc_discrete));
  edge1_policy.min_htlc = edge1_policy.min_htlc == 1 ? 0 : edge1_policy.min_htlc;

  edge2_policy.fee_base = gsl_rng_uniform_int(random_generator, MAXFEEBASE - MINFEEBASE) + MINFEEBASE;
  edge2_policy.fee_proportional = (gsl_rng_uniform_int(random_generator, MAXFEEPROP - MINFEEPROP) + MINFEEPROP);
  edge2_policy.timelock = gsl_rng_uniform_int(random_generator, MAXTIMELOCK - MINTIMELOCK) + MINTIMELOCK;
  edge2_policy.min_htlc = gsl_pow_int(10, gsl_ran_discrete(random_generator, min_htlc_discrete));
  edge2_policy.min_htlc = edge2_policy.min_htlc == 1 ? 0 : edge2_policy.min_htlc;

  edge1 = new_edge(channel_data.edge1, channel_data.id, channel_data.edge2,
                   channel_data.node1, channel_data.node2, edge1_balance, edge1_policy, channel_data.capacity);
  edge2 = new_edge(channel_data.edge2, channel_data.id, channel_data.edge1,
                   channel_data.node2, channel_data.node1, edge2_balance, edge2_policy, channel_data.capacity);

  network->channels = array_insert(network->channels, channel);
  network->edges = array_insert(network->edges, edge1);
  network->edges = array_insert(network->edges, edge2);

  node = array_get(network->nodes, channel_data.node1);
  node->open_edges = array_insert(node->open_edges, &(edge1->id));
  node = array_get(network->nodes, channel_data.node2);
  node->open_edges = array_insert(node->open_edges, &(edge2->id));
}

/* generate a random payment-channel network;
   the model of the network is a snapshot of the Lightning Network (files "nodes_ln.csv", "channels_ln.csv");
   starting from this network, a random network is generated using the scale-free network model */
struct network* generate_random_network(struct network_params net_params, gsl_rng* random_generator){
  FILE* nodes_input_file, *channels_input_file;
  char row[256];
  long node_id_counter=0, id, channel_id_counter=0, tot_nodes, i, tot_channels, node_to_connect_id, edge_id_counter=0, j;
  double *probability_per_node;
  int *channels_per_node;
  struct network* network;
  struct node* node;
  gsl_ran_discrete_t* connection_probability;
  struct channel channel;

  nodes_input_file = fopen("nodes_ln.csv", "r");
  if(nodes_input_file==NULL) {
    fprintf(stderr, "ERROR: cannot open file <%s>\n", "nodes_ln.csv");
    exit(-1);
  }
  channels_input_file = fopen("channels_ln.csv", "r");
  if(channels_input_file==NULL) {
    fprintf(stderr, "ERROR: cannot open file <%s>\n", "channels_ln.csv");
    fclose(nodes_input_file);
    exit(-1);
  }

  network = (struct network*) malloc(sizeof(struct network));
  network->nodes = array_initialize(1000);
  network->channels = array_initialize(1000);
  network->edges = array_initialize(2000);

  fgets(row, 256, nodes_input_file);
  while(fgets(row, 256, nodes_input_file)!=NULL) {
    sscanf(row, "%ld,%*d", &id);
    node = new_node(id);
    network->nodes = array_insert(network->nodes, node);
    node_id_counter++;
  }
  tot_nodes = node_id_counter + net_params.n_nodes;
  if(tot_nodes == 0){
    fprintf(stderr, "ERROR: it is not possible to generate a network with 0 nodes\n");
    fclose(nodes_input_file);
    fclose(channels_input_file);
    exit(-1);
  }

  channels_per_node = (int*)malloc(sizeof(int)*(tot_nodes));
  for(i = 0; i < tot_nodes; i++){
    channels_per_node[i] = 0;
  }

  fgets(row, 256, channels_input_file);
  while(fgets(row, 256, channels_input_file)!=NULL) {
    sscanf(row, "%ld,%ld,%ld,%ld,%ld,%*d,%*d", &(channel.id), &(channel.edge1), &(channel.edge2), &(channel.node1), &(channel.node2));
    generate_random_channel(channel, net_params.capacity_per_channel, network, random_generator);
    channels_per_node[channel.node1] += 1;
    channels_per_node[channel.node2] += 1;
    ++channel_id_counter;
    edge_id_counter+=2;
  }
  tot_channels = channel_id_counter;
  if(tot_channels == 0){
    fprintf(stderr, "ERROR: it is not possible to generate a network with 0 channels\n");
    fclose(nodes_input_file);
    fclose(channels_input_file);
    exit(-1);
  }

  probability_per_node = (double*)malloc(sizeof(double)*tot_nodes);
  for(i = 0; i < tot_nodes; i++){
    probability_per_node[i] = ((double)channels_per_node[i])/tot_channels;
  }

  /* scale-free algorithm that creates a network starting from an existing network;
     the probability of connecting nodes is directly proportional to the number of channels that a node has already open */
  for(i = 0; i < net_params.n_nodes; i++){
    node = new_node(node_id_counter);
    network->nodes = array_insert(network->nodes, node);
    for(j = 0; j < net_params.n_channels; j++){
      connection_probability = gsl_ran_discrete_preproc(node_id_counter, probability_per_node);
      node_to_connect_id = gsl_ran_discrete(random_generator, connection_probability);
      channel.id = channel_id_counter;
      channel.edge1 = edge_id_counter;
      channel.edge2 = edge_id_counter + 1;
      channel.node1 = node->id;
      channel.node2 = node_to_connect_id;
      generate_random_channel(channel, net_params.capacity_per_channel, network, random_generator);
      channel_id_counter++;
      edge_id_counter += 2;
      update_probability_per_node(probability_per_node, channels_per_node, tot_nodes, node->id, node_to_connect_id, channel_id_counter);
    }
    ++node_id_counter;
  }

  fclose(nodes_input_file);
  fclose(channels_input_file);
  free(channels_per_node);
  free(probability_per_node);

  write_network_files(network);

  return network;
}

/* generate a payment-channel network from input files */
struct network* generate_network_from_files(char nodes_filename[256], char channels_filename[256], char edges_filename[256]) {
  char row[2048];
  struct node* node;
  long id, direction1, direction2, node_id1, node_id2, channel_id, other_direction;
  struct policy policy;
  uint64_t capacity, balance;
  struct channel* channel;
  struct edge* edge;
  struct network* network;
  FILE *nodes_file, *channels_file, *edges_file;

  nodes_file = fopen(nodes_filename, "r");
  if(nodes_file==NULL) {
    fprintf(stderr, "ERROR: cannot open file <%s>\n", nodes_filename);
    exit(-1);
  }
  channels_file = fopen(channels_filename, "r");
  if(channels_file==NULL) {
    fprintf(stderr, "ERROR: cannot open file <%s>\n", channels_filename);
    fclose(nodes_file);
    exit(-1);
  }
  edges_file = fopen(edges_filename, "r");
  if(edges_file==NULL) {
    fprintf(stderr, "ERROR: cannot open file <%s>\n", edges_filename);
    fclose(nodes_file);
    fclose(channels_file);
    exit(-1);
  }

  network = (struct network*) malloc(sizeof(struct network));
  network->nodes = array_initialize(1000);
  network->channels = array_initialize(1000);
  network->edges = array_initialize(2000);

  fgets(row, 2048, nodes_file);
  while(fgets(row, 2048, nodes_file)!=NULL) {
    sscanf(row, "%ld", &id);
    node = new_node(id);
    network->nodes = array_insert(network->nodes, node);
  }
  fclose(nodes_file);

  fgets(row, 2048, channels_file);
  while(fgets(row, 2048, channels_file)!=NULL) {
    sscanf(row, "%ld,%ld,%ld,%ld,%ld,%" SCNu64, &id, &direction1, &direction2, &node_id1, &node_id2, &capacity);
    channel = new_channel(id, direction1, direction2, node_id1, node_id2, capacity);
    network->channels = array_insert(network->channels, channel);
  }
  fclose(channels_file);

  fgets(row, 2048, edges_file);
  while(fgets(row, 2048, edges_file)!=NULL) {
    sscanf(row, "%ld,%ld,%ld,%ld,%ld,%" SCNu64 ",%ld,%ld,%" SCNu64 ",%d",
           &id, &channel_id, &other_direction, &node_id1, &node_id2, &balance,
           &policy.fee_base, &policy.fee_proportional, &policy.min_htlc, &policy.timelock);
    channel = array_get(network->channels, channel_id);
    edge = new_edge(id, channel_id, other_direction, node_id1, node_id2, balance, policy, channel->capacity);
    network->edges = array_insert(network->edges, edge);
    node = array_get(network->nodes, node_id1);
    node->open_edges = array_insert(node->open_edges, &(edge->id));
  }
  fclose(edges_file);

  return network;
}

struct network* initialize_network(struct network_params net_params, gsl_rng* random_generator) {
  struct network* network;
  double faulty_prob[2];
  long n_nodes;
  long i, j;
  struct node* node;

  if(net_params.network_from_file)
    network = generate_network_from_files(net_params.nodes_filename, net_params.channels_filename, net_params.edges_filename);
  else
    network = generate_random_network(net_params, random_generator);

  faulty_prob[0] = 1 - net_params.faulty_node_prob;
  faulty_prob[1] = net_params.faulty_node_prob;
  network->faulty_node_prob = gsl_ran_discrete_preproc(2, faulty_prob);

  n_nodes = array_len(network->nodes);
  for(i = 0; i < n_nodes; i++){
    node = array_get(network->nodes, i);
    node->results = (struct element**) malloc(n_nodes * sizeof(struct element*));
    for(j = 0; j < n_nodes; j++)
      node->results[j] = NULL;
  }

  network->groups = array_initialize(1000);

  return network;
}

/* open a new channel during the simulation */
/* currently NOT USED */
void open_channel(struct network* network, gsl_rng* random_generator){
  struct channel channel;
  channel.id = array_len(network->channels);
  channel.edge1 = array_len(network->edges);
  channel.edge2 = array_len(network->edges) + 1;
  channel.node1 = gsl_rng_uniform_int(random_generator, array_len(network->nodes));
  do{
    channel.node2 = gsl_rng_uniform_int(random_generator, array_len(network->nodes));
  } while(channel.node2==channel.node1);
  generate_random_channel(channel, 1000, network, random_generator);
}

int update_group(struct group* group, struct network_params net_params, uint64_t current_time){
  int close_flg = 0;

  long m = array_len(group->edges);

  /* min/max 再計算 */
  uint64_t min = UINT64_MAX;
  uint64_t max = 0;

  /* レンジ逸脱の観測（解散には使わない） */
  long rv_lo = 0;  /* balance < min_cap_limit */
  long rv_hi = 0;  /* balance > max_cap_limit */

  /* 1) min/max とレンジ逸脱数のカウント */
  for (long i = 0; i < m; i++) {
    struct edge* e = array_get(group->edges, i);
    if (!e) { close_flg = 1; continue; }

    if (e->balance < min) min = e->balance;
    if (e->balance > max) max = e->balance;

    if (e->balance < group->min_cap_limit) rv_lo++;
    if (e->balance > group->max_cap_limit) rv_hi++;
  }

  /* 異常系（空など） */
  if (min == UINT64_MAX) {
    min = 0;
    max = 0;
    close_flg = 1;
  }

  /* 2) グループ内の構造整合性（重複・ノード共有）だけは close 扱い */
  for (long i = 0; i < m; i++) {
    struct edge* a = array_get(group->edges, i);
    if (!a) continue;

    for (long j = i + 1; j < m; j++) {
      struct edge* b = array_get(group->edges, j);
      if (!b) continue;

      if (a->id == b->id) {
        close_flg = 1; /* 重複は構造破綻 */
      }

      if (a->to_node_id == b->to_node_id ||
          a->to_node_id == b->from_node_id ||
          a->from_node_id == b->to_node_id ||
          a->from_node_id == b->from_node_id) {
        close_flg = 1; /* ノード共有も構造破綻 */
      }
    }
  }

  /* group_cap 更新 */
  group->max_cap = max;
  group->min_cap = min;
  if (net_params.group_cap_update) {
    group->group_cap = min;
  } else {
    group->group_cap = group->min_cap_limit;
  }

  /* update_group ログ（レンジ逸脱は reason に記録するだけ。close はしない） */
  if (net_params.enable_group_event_csv && csv_group_events && group->id >= 0) {
    char reason_buf[128];
    snprintf(reason_buf, sizeof(reason_buf),
             "update;rv=%ld;lo=%ld;hi=%ld", (rv_lo + rv_hi), rv_lo, rv_hi);

    ge_update_group((uint64_t)current_time, group->id,
                    group->group_cap, group->min_cap, group->max_cap,
                    group->seed_edge_id, group->attempt_id,
                    reason_buf);
  }

  /* history は現状のまま */
  struct group_update* group_update = (struct group_update*)malloc(sizeof(struct group_update));
  group_update->group_cap = group->group_cap;
  group_update->time = current_time;
  group_update->edge_balances = (uint64_t*)malloc(sizeof(uint64_t) * (m > 0 ? m : 1));
  for (long i = 0; i < m; i++) {
    struct edge* e = array_get(group->edges, i);
    group_update->edge_balances[i] = e ? e->balance : 0;
  }
  group->history = push(group->history, group_update);

  return close_flg;
}

long get_edge_balance(struct edge* e){
    return (long)e->balance;
}

/* safely remove an edge from a group's member list */
void remove_edge_from_group(struct group* g, struct edge* e){
    if(g == NULL || g->edges == NULL) return;
    long n = array_len(g->edges);
    struct array* rebuilt = array_initialize(n > 0 ? n : 1);
    for(long i = 0; i < n; i++){
      struct edge* cur = array_get(g->edges, i);
      /* compare by ID to avoid pointer aliasing issues */
      if(cur && e && cur->id == e->id) continue;
      rebuilt = array_insert(rebuilt, cur);
    }
    array_free(g->edges);
    g->edges = rebuilt;
}

struct edge_snapshot* take_edge_snapshot(struct edge* e, uint64_t sent_amt, short is_in_group, uint64_t group_cap) {
    struct edge_snapshot* snapshot = (struct edge_snapshot*)malloc(sizeof(struct edge_snapshot));
    snapshot->id = e->id;
    snapshot->balance = e->balance;
    snapshot->sent_amt = sent_amt;
    snapshot->is_in_group = is_in_group;
    snapshot->group_cap = group_cap;
    if(e->channel_updates != NULL) {
        struct channel_update* cu = e->channel_updates->data;
        snapshot->does_channel_update_exist = 1;
        snapshot->last_channle_update_value = cu->htlc_maximum_msat;
    } else {
        snapshot->does_channel_update_exist = 0;
        snapshot->last_channle_update_value = 0;
    }
    return snapshot;
}

void free_network(struct network* network){
    if (!network) return;

    /* nodes */
    for(uint64_t i = 0; i < (uint64_t)array_len(network->nodes); i++){
        struct node* n = array_get(network->nodes, i);
        if(!n) continue;
        array_free(n->open_edges);
        for(struct element* iterator = (struct element *) n->results; iterator != NULL; iterator = iterator->next){
            list_free(iterator->data);
        }
        free(n);
    }

    /* edges */
    for(uint64_t i = 0; i < (uint64_t)array_len(network->edges); i++){
        struct edge* e = array_get(network->edges, i);
        if(!e) continue;
        list_free(e->channel_updates);
        list_free(e->edge_locked_balance_and_durations);
        free(e);
    }

    /* channels */
    for(uint64_t i = 0; i < (uint64_t)array_len(network->channels); i++){
        struct channel* c = array_get(network->channels, i);
        if(!c) continue;
        free(c);
    }

    /* groups */
    for(uint64_t i = 0; i < (uint64_t)array_len(network->groups); i++){
        struct group* g = array_get(network->groups, i);
        if(!g) continue;
        list_free(g->history);
        free(g);
    }

    /* arrays themselves */
    array_free(network->nodes);
    array_free(network->edges);
    array_free(network->channels);
    array_free(network->groups);

    free(network);
}

void group_close_once(struct simulation* sim,struct group* g,const char* reason){
  if (!g) return;

  /* すでに close 済みなら何もしない（冪等） */
  if (g->is_closed != GROUP_NOT_CLOSED) return;

  /* members を "id-id-..." 形式で組み立て（ログ出力用） */
  char members_buf[8192]; members_buf[0] = '\0';
  for (long j = 0; j < array_len(g->edges); j++) {
    struct edge* ee = array_get(g->edges, j);
    if (!ee) continue;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s%ld", (j == 0 ? "" : "-"), ee->id);
    strncat(members_buf, tmp, sizeof(members_buf) - strlen(members_buf) - 1);
  }

  ge_close((uint64_t)sim->current_time, g->id,
         reason ? reason : "unknown",
         members_buf,
         g->seed_edge_id, g->attempt_id);

  /* 状態を close 済みにマーク（以降の重複発火を抑止） */
  g->is_closed = sim->current_time;
}