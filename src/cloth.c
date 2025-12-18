#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <inttypes.h>
#include <dirent.h>

#include <gsl/gsl_rng.h>
#include <gsl/gsl_cdf.h>

#include "../include/payments.h"
#include "../include/heap.h"
#include "../include/array.h"
#include "../include/routing.h"
#include "../include/htlc.h"
#include "../include/list.h"
#include "../include/cloth.h"
#include "../include/network.h"
#include "../include/event.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>


/* This file contains the main, where the simulation logic is executed;
   additionally, it contains the the initialization functions,
   a function that reads the input and a function that writes the output values in csv files */

static void mkdir_p(const char* path) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char* p = tmp + 1; *p; p++) {
    if (*p == '/') { *p = '\0'; if (mkdir(tmp, 0755) != 0 && errno != EEXIST) break; *p = '/'; }
  }
  mkdir(tmp, 0755);
}
static void ensure_parent_dir(const char* filepath){
  char dir[512]; snprintf(dir, sizeof(dir), "%s", filepath);
  char* slash = strrchr(dir, '/'); if (slash) { *slash = '\0'; mkdir_p(dir); }
}

/* write the final values of nodes, channels, edges and payments in csv files */
/*出力ファイルにノード、チャネル、エッジ、支払いの最終値をcsvファイルに出力*/
void write_output(struct network* network, struct array* payments, char output_dir_name[]) {
  FILE* csv_channel_output, *csv_group_output, *csv_edge_output, *csv_payment_output, *csv_node_output;
  long i,j, *id;
  struct channel* channel;
  struct edge* edge;
  struct payment* payment;
  struct node* node;
  struct route* route;
  struct array* hops;
  struct route_hop* hop;
  DIR* results_dir;
  char output_filename[512];

  results_dir = opendir(output_dir_name);
  if(!results_dir){
    printf("cloth.c: Cannot find the output directory. The output will be stored in the current directory.\n");
    strcpy(output_dir_name, "./");
  }

  strcpy(output_filename, output_dir_name);
  strcat(output_filename, "channels_output.csv"); //チャネルの情報（ID、ノード間の接続、容量など）
  csv_channel_output = fopen(output_filename, "w");
  if(csv_channel_output  == NULL) {
    printf("ERROR cannot open channel_output.csv\n");
    exit(-1);
  }
  fprintf(csv_channel_output, "id,edge1,edge2,node1,node2,capacity,is_closed\n");
  for(i=0; i<array_len(network->channels); i++) {
    channel = array_get(network->channels, i);
    fprintf(csv_channel_output, "%ld,%ld,%ld,%ld,%ld,%ld,%d\n", channel->id, channel->edge1, channel->edge2, channel->node1, channel->node2, channel->capacity, channel->is_closed);
  }
  fclose(csv_channel_output);

  strcpy(output_filename, output_dir_name);
  strcat(output_filename, "groups_output.csv");
  csv_group_output = fopen(output_filename, "w");
  if(csv_group_output  == NULL) {
    printf("ERROR cannot open groups_output.csv\n"); //グループの情報（構成エッジ、容量、閉鎖状態など）
    exit(-1);
  }
  fprintf(csv_group_output, "id,edges,balances,is_closed(closed_time),constructed_time,min_cap_limit,max_cap_limit,max_edge_balance,min_edge_balance,group_capacity,cul\n");
  for(i=0; i<array_len(network->groups); i++) {
    struct group *group = array_get(network->groups, i);
    fprintf(csv_group_output, "%ld,", group->id);
    long n_members = array_len(group->edges);
    for(j=0; j< n_members; j++){
        struct edge* edge_snapshot = array_get(group->edges, j);
        fprintf(csv_group_output, "%ld", edge_snapshot->id);
        if(j < n_members -1){
            fprintf(csv_group_output, "-");
        }else{
            fprintf(csv_group_output, ",");
        }
    }
    for(j=0; j< n_members; j++){
        struct edge* edge_snapshot = array_get(group->edges, j);
        fprintf(csv_group_output, "%lu", edge_snapshot->balance);
        if(j < n_members -1){
            fprintf(csv_group_output, "-");
        }else{
            fprintf(csv_group_output, ",");
        }
    }
    struct group_update* group_update = NULL;
    int group_closed = (group->is_closed != GROUP_NOT_CLOSED);

    if (group->history) {
      if (group_closed && group->history->next) group_update = group->history->next->data;
      else group_update = group->history->data;
    }

    float sum_cul = 0.0f;
    if (group_update && n_members > 0) {
      for (j = 0; j < n_members; j++) {
        uint64_t eb = group_update->edge_balances[j];
        if (eb == 0) continue;
        sum_cul += (1.0f - ((float)group_update->group_cap / (float)eb));
      }
    }

    long long closed_time_out = group_closed ? (long long)group->is_closed : -1LL;

    fprintf(csv_group_output, "%lld,%lu,%lu,%lu,%lu,%lu,%lu,%f\n",
            closed_time_out,
            group->constructed_time,
            group->min_cap_limit,
            group->max_cap_limit,
            group->max_cap,
            group->min_cap,
            group->group_cap,
            (n_members > 0 ? (sum_cul / (float)n_members) : 0.0f));
  }
  fclose(csv_group_output);

  strcpy(output_filename, output_dir_name);
  strcat(output_filename, "edges_output.csv"); //エッジの情報（ID、接続ノード、バランス、手数料など）
  csv_edge_output = fopen(output_filename, "w");
  if(csv_edge_output  == NULL) {
    printf("ERROR cannot open edge_output.csv\n");
    exit(-1);
  }
  fprintf(csv_edge_output, "id,channel_id,counter_edge_id,from_node_id,to_node_id,balance,fee_base,fee_proportional,min_htlc,timelock,is_closed,tot_flows,min_cap_use_count,channel_updates,group,locked_balance_and_duration\n");
  for(i=0; i<array_len(network->edges); i++) {
    edge = array_get(network->edges, i);
    fprintf(csv_edge_output,
        "%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%d,%d,%" PRIu64 ",%" PRIu64 ",",
        edge->id,
        edge->channel_id,
        edge->counter_edge_id,
        edge->from_node_id,
        edge->to_node_id,
        edge->balance,
        edge->policy.fee_base,
        edge->policy.fee_proportional,
        edge->policy.min_htlc,
        edge->policy.timelock,
        edge->is_closed,
        edge->tot_flows,
        edge->min_cap_use_count);
    char channel_updates_text[1000000] = "";
    for (struct element *iterator = edge->channel_updates; iterator != NULL; iterator = iterator->next) {
        struct channel_update *channel_update = iterator->data;
        char temp[1000000];
        int written = 0;
        if(iterator->next != NULL) {
            written = snprintf(temp, sizeof(temp), "-%ld%s", channel_update->htlc_maximum_msat, channel_updates_text);
        }else{
            written = snprintf(temp, sizeof(temp), "%ld%s", channel_update->htlc_maximum_msat, channel_updates_text);
        }
        // Check if the output was truncated
        if (written < 0 || (size_t)written >= sizeof(temp)) {
            fprintf(stderr, "Error: Buffer overflow detected.\n");
            exit(1);
        }
        strncpy(channel_updates_text, temp, sizeof(channel_updates_text) - 1);
    }
    fprintf(csv_edge_output, "%s,", channel_updates_text);
    if(edge->group == NULL){
        fprintf(csv_edge_output, "NULL,");
    }else{
        fprintf(csv_edge_output, "%ld,", edge->group->id);
    }
    for(struct element* iterator = edge->edge_locked_balance_and_durations; iterator != NULL; iterator = iterator->next){
        struct edge_locked_balance_and_duration* edge_locked_balance_time = iterator->data;
        uint64_t locked_time = edge_locked_balance_time->locked_end_time - edge_locked_balance_time->locked_start_time;
        fprintf(csv_edge_output, "%lux%lu", edge_locked_balance_time->locked_balance, locked_time);
        if(iterator->next != NULL){
            fprintf(csv_edge_output, "-");
        }
    }
    fprintf(csv_edge_output, "\n");
  }
  fclose(csv_edge_output);

  strcpy(output_filename, output_dir_name);
  strcat(output_filename, "payments_output.csv"); //支払いの詳細（送信者、受信者、金額、ルート、成功/失敗など）
  csv_payment_output = fopen(output_filename, "w");
  if(csv_payment_output  == NULL) {
    printf("ERROR cannot open payment_output.csv\n");
    exit(-1);
  }
  fprintf(csv_payment_output, "id,sender_id,receiver_id,amount,start_time,max_fee_limit,end_time,mpp,is_success,no_balance_count,offline_node_count,timeout_exp,attempts,route,total_fee,attempts_history\n");
  for(i=0; i<array_len(payments); i++)  {
    payment = array_get(payments, i);
    if (payment->id == -1) continue;
    fprintf(csv_payment_output, "%ld,%ld,%ld,%ld,%ld,%ld,%ld,%u,%u,%d,%d,%u,%d,", payment->id, payment->sender, payment->receiver, payment->amount, payment->start_time, payment->max_fee_limit, payment->end_time, payment->is_shard, payment->is_success, payment->no_balance_count, payment->offline_node_count, payment->is_timeout, payment->attempts);
    route = payment->route;
    if(route==NULL)
      fprintf(csv_payment_output, ",,");
    else {
      hops = route->route_hops;
      for(j=0; j<array_len(hops); j++) {
        hop = array_get(hops, j);
        if(j==array_len(hops)-1)
          fprintf(csv_payment_output,"%ld,",hop->edge_id);
        else
          fprintf(csv_payment_output,"%ld-",hop->edge_id);
      }
      fprintf(csv_payment_output, "%ld,",route->total_fee);
    }
    // build attempts history json
    if(payment->history != NULL) {
        fprintf(csv_payment_output, "\"[");
        for (struct element *iterator = payment->history; iterator != NULL; iterator = iterator->next) {
            struct attempt *attempt = iterator->data;
            fprintf(csv_payment_output, "{\"\"attempts\"\":%d,\"\"is_succeeded\"\":%d,\"\"end_time\"\":%lu,\"\"error_edge\"\":%lu,\"\"error_type\"\":%d,\"\"route\"\":[", attempt->attempts, attempt->is_succeeded, attempt->end_time, attempt->error_edge_id, attempt->error_type);
            for (j = 0; j < array_len(attempt->route); j++) {
                struct edge_snapshot* edge_snapshot = array_get(attempt->route, j);
                edge = array_get(network->edges, edge_snapshot->id);
                channel = array_get(network->channels, edge->channel_id);
                fprintf(csv_payment_output,"{\"\"edge_id\"\":%lu,\"\"from_node_id\"\":%lu,\"\"to_node_id\"\":%lu,\"\"sent_amt\"\":%lu,\"\"edge_cap\"\":%lu,\"\"channel_cap\"\":%lu,", edge_snapshot->id, edge->from_node_id, edge->to_node_id, edge_snapshot->sent_amt, edge_snapshot->balance, channel->capacity);
                if(edge_snapshot->is_in_group) fprintf(csv_payment_output, "\"\"group_cap\"\":%lu,", edge_snapshot->group_cap);
                else fprintf(csv_payment_output,"\"\"group_cap\"\":null,");
                if(edge_snapshot->does_channel_update_exist) fprintf(csv_payment_output,"\"\"channel_update\"\":%lu}", edge_snapshot->last_channle_update_value);
                else fprintf(csv_payment_output,"\"\"channel_update\"\":}");
                if (j != array_len(attempt->route) - 1) fprintf(csv_payment_output, ",");
            }
            fprintf(csv_payment_output, "]}");
            if (iterator->next != NULL) fprintf(csv_payment_output, ",");
            else fprintf(csv_payment_output, "]");
        }
        fprintf(csv_payment_output, "\"");
    }
    fprintf(csv_payment_output, "\n");
  }
  fclose(csv_payment_output);

  strcpy(output_filename, output_dir_name);
  strcat(output_filename, "nodes_output.csv");
  csv_node_output = fopen(output_filename, "w");
  if(csv_node_output  == NULL) {
    printf("ERROR cannot open nodes_output.csv\n"); //ノードの情報（ID、接続エッジなど）
    return;
  }
  fprintf(csv_node_output, "id,open_edges\n");
  for(i=0; i<array_len(network->nodes); i++) {
    node = array_get(network->nodes, i);
    fprintf(csv_node_output, "%ld,", node->id);
    if(array_len(node->open_edges)==0)
      fprintf(csv_node_output, "-1");
    else {
      for(j=0; j<array_len(node->open_edges); j++) {
        id = array_get(node->open_edges, j);
        if(j==array_len(node->open_edges)-1)
          fprintf(csv_node_output,"%ld",*id);
        else
          fprintf(csv_node_output,"%ld-",*id);
      }
    }
    fprintf(csv_node_output,"\n");
  }
  fclose(csv_node_output);
}

/*ネットワークと支払いの初期パラメータを初期化*/
void initialize_input_parameters(struct network_params *net_params, struct payments_params *pay_params) {
  net_params->n_nodes = net_params->n_channels = net_params->capacity_per_channel = 0;
  net_params->faulty_node_prob = 0.0;
  net_params->network_from_file = 0;
  strcpy(net_params->nodes_filename, "\0");
  strcpy(net_params->channels_filename, "\0");
  strcpy(net_params->edges_filename, "\0");
  pay_params->inverse_payment_rate = pay_params->amount_mu = 0.0;
  pay_params->n_payments = 0;
  pay_params->payments_from_file = 0;
  strcpy(pay_params->payments_filename, "\0");
  pay_params->mpp = 0;
  net_params->tau_default               = 0.10;
  net_params->k_used_on_min_edge        = 5;
  net_params->cooldown_hops             = 5;
  net_params->max_leaves_per_group_tick = 1;
  net_params->group_size = -1;        /* target (required for GROUP_ROUTING) */
  net_params->group_size_min = -1;    /* default later to group_size */
  net_params->group_limit_rate = -1;  /* required */
  net_params->group_cap_update = -1;  /* required */
  net_params->group_broadcast_delay = 0; /* or -1 if you validate it */
  net_params->use_conventional_method = 1; /* or 0, depending on your default */
  net_params->group_min_cap_ratio = 0.0f;
  net_params->group_max_cap_ratio = 0.0f;

  /* logging defaults */
  net_params->enable_group_event_csv = 1;
  strncpy(net_params->group_event_csv_filename, "group_events.csv",
          sizeof(net_params->group_event_csv_filename));
  net_params->group_event_csv_filename[sizeof(net_params->group_event_csv_filename)-1] = '\0';

  /* tau randomization defaults (optional) */
  net_params->tau_randomize = 0;
  net_params->tau_min = 0.08;
  net_params->tau_max = 0.15;
}


/* parse the input parameters in "cloth_input.txt" */
/*入力ファイル（cloth_input.txt）を読み取り、シミュレーションの設定を読み込む*/
void read_input(struct network_params* net_params, struct payments_params* pay_params){
  FILE* input_file;
  char *parameter, *value, line[1024];

  initialize_input_parameters(net_params, pay_params);

  input_file = fopen("cloth_input.txt","r");

  if(input_file==NULL){
    fprintf(stderr, "ERROR: cannot open file <cloth_input.txt> in current directory.\n");
    exit(-1);
  }

  while(fgets(line, 1024, input_file)){

    parameter = strtok(line, "=");
    value = strtok(NULL, "=");
    if(parameter==NULL || value==NULL){
      fprintf(stderr, "ERROR: wrong format in file <cloth_input.txt>\n");
      fclose(input_file);
      exit(-1);
    }

    if(value[0]==' ' || parameter[strlen(parameter)-1]==' '){
      fprintf(stderr, "ERROR: no space allowed after/before <=> character in <cloth_input.txt>. Space detected in parameter <%s>\n", parameter);
      fclose(input_file);
      exit(-1);
    }

    value[strlen(value)-1] = '\0';

    if(strcmp(parameter, "generate_network_from_file")==0){
      if(strcmp(value, "true")==0)
        net_params->network_from_file=1;
      else if(strcmp(value, "false")==0)
        net_params->network_from_file=0;
      else{
        fprintf(stderr, "ERROR: wrong value of parameter <%s> in <cloth_input.txt>. Possible values are <true> or <false>\n", parameter);
        fclose(input_file);
        exit(-1);
      }
    }
    else if(strcmp(parameter, "nodes_filename")==0){
      strcpy(net_params->nodes_filename, value);
    }
    else if(strcmp(parameter, "channels_filename")==0){
      strcpy(net_params->channels_filename, value);
    }
    else if(strcmp(parameter, "edges_filename")==0){
      strcpy(net_params->edges_filename, value);
    }
    else if(strcmp(parameter, "n_additional_nodes")==0){
      net_params->n_nodes = strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "n_channels_per_node")==0){
      net_params->n_channels = strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "capacity_per_channel")==0){
      net_params->capacity_per_channel = strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "faulty_node_probability")==0){
      net_params->faulty_node_prob = strtod(value, NULL);
    }
    else if(strcmp(parameter, "generate_payments_from_file")==0){
      if(strcmp(value, "true")==0)
        pay_params->payments_from_file=1;
      else if(strcmp(value, "false")==0)
        pay_params->payments_from_file=0;
      else{
        fprintf(stderr, "ERROR: wrong value of parameter <%s> in <cloth_input.txt>. Possible values are <true> or <false>\n", parameter);
        fclose(input_file);
        exit(-1);
      }
    }
    else if(strcmp(parameter, "payment_timeout")==0) {
        net_params->payment_timeout=strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "average_payment_forward_interval")==0) {
        net_params->average_payment_forward_interval=strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "variance_payment_forward_interval")==0) {
        net_params->variance_payment_forward_interval=strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "group_broadcast_delay")==0) {
        net_params->group_broadcast_delay=strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "routing_method")==0){
      if(strcmp(value, "cloth_original")==0)
        net_params->routing_method=CLOTH_ORIGINAL;
      else if(strcmp(value, "channel_update")==0)
        net_params->routing_method=CHANNEL_UPDATE;
      else if(strcmp(value, "group_routing")==0)
        net_params->routing_method=GROUP_ROUTING;
      else if(strcmp(value, "ideal")==0)
        net_params->routing_method=IDEAL;
      else{
        fprintf(stderr, "ERROR: wrong value of parameter <%s> in <cloth_input.txt>. Possible values are [\"cloth_original\", \"channel_update\", \"group_routing\", \"ideal\"]\n", parameter);
        fclose(input_file);
        exit(-1);
      }
    }
    else if(strcmp(parameter, "group_cap_update")==0){
      if(strcmp(value, "true")==0)
        net_params->group_cap_update=1;
      else if(strcmp(value, "false")==0)
        net_params->group_cap_update=0;
      else
        net_params->group_cap_update=-1;
    }
    else if(strcmp(parameter, "group_size")==0){
        if(strcmp(value, "")==0) net_params->group_size = -1;
        else net_params->group_size = strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "group_size_min")==0){
      if(strcmp(value, "")==0) net_params->group_size_min = -1;
      else net_params->group_size_min = strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "group_limit_rate")==0){
        if(strcmp(value, "")==0) net_params->group_limit_rate = -1;
        else net_params->group_limit_rate = strtof(value, NULL);
    }
    else if (strcmp(parameter,"use_conventional_method")==0) {
        if(strcmp(value, "true")==0)
            net_params->use_conventional_method = 1;
        else if(strcmp(value, "false")==0)
            net_params->use_conventional_method = 0;
        else{
            fprintf(stderr, "ERROR: wrong value of <use_conventional_method>. Use true or false.\n");
            fclose(input_file);
            exit(-1);
        }
    }
    else if(strcmp(parameter, "group_min_cap_ratio")==0){
        net_params->group_min_cap_ratio = strtof(value, NULL);
    }
    else if(strcmp(parameter, "group_max_cap_ratio")==0){
        net_params->group_max_cap_ratio = strtof(value, NULL);
    }
    else if(strcmp(parameter, "payments_filename")==0){
      strcpy(pay_params->payments_filename, value);
    }
    else if(strcmp(parameter, "payment_rate")==0){
      pay_params->inverse_payment_rate = 1.0/strtod(value, NULL);
    }
    else if(strcmp(parameter, "n_payments")==0){
      pay_params->n_payments = strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "average_payment_amount")==0){
      pay_params->amount_mu = strtod(value, NULL);
    }
    else if(strcmp(parameter, "variance_payment_amount")==0){
      pay_params->amount_sigma = strtod(value, NULL);
    }
    else if(strcmp(parameter, "mpp")==0){
      pay_params->mpp = strtoul(value, NULL, 10);
    }
    else if(strcmp(parameter, "average_max_fee_limit")==0){
        pay_params->max_fee_limit_mu = strtod(value, NULL);
    }
    else if(strcmp(parameter, "variance_max_fee_limit")==0){
        pay_params->max_fee_limit_sigma = strtod(value, NULL);
    }
    else if(strcmp(parameter, "tau_default")==0){
      net_params->tau_default = strtod(value, NULL);
    }
    else if(strcmp(parameter, "k_used_on_min_edge")==0){
      net_params->k_used_on_min_edge = (int)strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "cooldown_hops")==0){
      net_params->cooldown_hops = (int)strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "max_leaves_per_group_tick")==0){
      net_params->max_leaves_per_group_tick = (int)strtol(value, NULL, 10);
    }
    else if(strcmp(parameter, "enable_group_event_csv")==0){
      if(strcmp(value, "true")==0)      net_params->enable_group_event_csv = 1;
      else if(strcmp(value, "false")==0)net_params->enable_group_event_csv = 0;
      else{
        fprintf(stderr, "ERROR: wrong value of <enable_group_event_csv>. Use true or false.\n");
        fclose(input_file);
        exit(-1);
      }
    }
    else if(strcmp(parameter, "group_event_csv_filename")==0){
      strncpy(net_params->group_event_csv_filename, value, sizeof(net_params->group_event_csv_filename));
      net_params->group_event_csv_filename[sizeof(net_params->group_event_csv_filename)-1] = '\0';
    }
    else if(strcmp(parameter, "tau_randomize")==0){
      if(strcmp(value, "true")==0)      net_params->tau_randomize = 1;
      else if(strcmp(value, "false")==0)net_params->tau_randomize = 0;
      else{
        fprintf(stderr, "ERROR: wrong value of <tau_randomize>. Use true or false.\n");
        fclose(input_file);
        exit(-1);
      }
    }
    else if(strcmp(parameter, "tau_min")==0){
      net_params->tau_min = strtod(value, NULL);
    }
    else if(strcmp(parameter, "tau_max")==0){
      net_params->tau_max = strtod(value, NULL);
    }
    else{
      fprintf(stderr, "ERROR: unknown parameter <%s>\n", parameter);
      fclose(input_file);
      exit(-1);
    }
  }
  // check invalid group settings
  if(net_params->routing_method == GROUP_ROUTING){
    if(net_params->group_limit_rate < 0 || net_params->group_limit_rate > 1){
      fprintf(stderr, "ERROR: wrong value of parameter <group_limit_rate> in <cloth_input.txt>.\n");
      exit(-1);
    }
    if(net_params->group_size <= 0){
      fprintf(stderr, "ERROR: wrong value of parameter <group_size> in <cloth_input.txt>.\n");
      exit(-1);
    }
    if (net_params->group_size_min < 0) {
      net_params->group_size_min = net_params->group_size; /* min = target by default */
    }
    if(net_params->group_size_min <= 0 || net_params->group_size_min > net_params->group_size){
      fprintf(stderr, "ERROR: group_size_min must satisfy 1 <= group_size_min <= group_size.\n");
      exit(-1);
    }
    if(net_params->group_cap_update == -1){
      fprintf(stderr, "ERROR: wrong value of parameter <group_cap_update> in <cloth_input.txt>.\n");
      exit(-1);
    }
  }
  if(net_params->tau_default < 0.0 || net_params->tau_default > 1.0){
    fprintf(stderr, "ERROR: tau_default must be in [0,1].\n");
    exit(-1);
  }
  if(net_params->tau_randomize){
    if(!(net_params->tau_min >= 0.0 && net_params->tau_max <= 1.0 && net_params->tau_max >= net_params->tau_min)){
      fprintf(stderr, "ERROR: tau_min/tau_max must satisfy 0<=tau_min<=tau_max<=1.\n");
      exit(-1);
    }
  }
  if(net_params->k_used_on_min_edge < 0){
    fprintf(stderr, "ERROR: k_used_on_min_edge must be >= 0.\n");
    exit(-1);
  }
  if(net_params->cooldown_hops < 0){
    fprintf(stderr, "ERROR: cooldown_hops must be >= 0.\n");
    exit(-1);
  }
  if(net_params->max_leaves_per_group_tick <= 0){
    fprintf(stderr, "ERROR: max_leaves_per_group_tick must be >= 1.\n");
    exit(-1);
  }

  fclose(input_file);
}


unsigned int has_shards(struct payment* payment){
  return (payment->shards_id[0] != -1 && payment->shards_id[1] != -1);
}


/* process stats of payments that were split (mpp payments) */
/*支払い（特に分割支払い）の統計を後処理*/
void post_process_payment_stats(struct array* payments){
  long i;
  struct payment* payment, *shard1, *shard2;
  for(i = 0; i < array_len(payments); i++){
    payment = array_get(payments, i);
    if(payment->id == -1) continue;
    if(!has_shards(payment)) continue;
    shard1 = array_get(payments, payment->shards_id[0]);
    shard2 = array_get(payments, payment->shards_id[1]);
    payment->end_time = shard1->end_time > shard2->end_time ? shard1->end_time : shard2->end_time;
    payment->is_success = shard1->is_success && shard2->is_success ? 1 : 0;
    payment->no_balance_count = shard1->no_balance_count + shard2->no_balance_count;
    payment->offline_node_count = shard1->offline_node_count + shard2->offline_node_count;
    payment->is_timeout = shard1->is_timeout || shard2->is_timeout ? 1 : 0;
    payment->attempts = shard1->attempts + shard2->attempts;
    if(shard1->route != NULL && shard2->route != NULL){
      payment->route = array_len(shard1->route->route_hops) > array_len(shard2->route->route_hops) ? shard1->route : shard2->route;
      payment->route->total_fee = shard1->route->total_fee + shard2->route->total_fee;
    }
    else{
      payment->route = NULL;
    }
    //a trick to avoid processing already processed shards
    shard1->id = -1;
    shard2->id = -1;
  }
}

/*ランダムジェネレータを初期化*/
gsl_rng* initialize_random_generator(){
  gsl_rng_env_setup();
  return gsl_rng_alloc (gsl_rng_default);
}


int main(int argc, char *argv[]) {
  struct event* event;
  clock_t  begin, end;
  double time_spent=0.0;
  long time_spent_thread = 0;
  struct network_params net_params;
  struct payments_params pay_params;
  struct timespec start, finish;
  struct network *network;
  long n_nodes, n_edges;
  struct array* payments;
  struct simulation* simulation;
  char output_dir_name[256];

  if(argc != 2) {
    fprintf(stderr, "ERROR cloth.c: please specify the output directory\n");
    return -1;
  }
  strcpy(output_dir_name, argv[1]);

  {
    DIR* results_dir = opendir(output_dir_name);
    if(!results_dir){
      printf("cloth.c: Cannot find the output directory. The output will be stored in the current directory.\n");
      strcpy(output_dir_name, "./");
    } else {
      closedir(results_dir);
    }
  }

  read_input(&net_params, &pay_params); // 入力パラメータの読み込み
  /* パラメータ読込完了後にフラグを見てオープン */
  if (net_params.enable_group_event_csv) {
    group_events_open(output_dir_name);
  }
  simulation = malloc(sizeof(struct simulation));

  simulation->random_generator = initialize_random_generator();
  printf("NETWORK INITIALIZATION\n");
  network = initialize_network(net_params, simulation->random_generator); //ネットワークの初期化
  n_nodes = array_len(network->nodes);
  n_edges = array_len(network->edges);

    // add edge which is not a member of any group to group_add_queue
    struct element* group_add_queue = NULL;
    if(net_params.routing_method == GROUP_ROUTING) {
        for (int i = 0; i < n_edges; i++) {
            group_add_queue = list_insert_sorted_position(group_add_queue, array_get(network->edges, i), (long (*)(void *)) get_edge_balance);
        }
        group_add_queue = construct_groups(simulation, group_add_queue, network, net_params);
    }
    printf("group_cover_rate on init : %f\n", (float)(array_len(network->edges) - list_len(group_add_queue)) / (float)(array_len(network->edges)));

  printf("PAYMENTS INITIALIZATION\n");
  payments = initialize_payments(pay_params,  n_nodes, simulation->random_generator); //支払いイベントの生成

  printf("EVENTS INITIALIZATION\n");
  simulation->events = initialize_events(payments);
  initialize_dijkstra(n_nodes, n_edges, payments);

  printf("INITIAL DIJKSTRA THREADS EXECUTION\n");
  clock_gettime(CLOCK_MONOTONIC, &start);
  run_dijkstra_threads(network, payments, 0, net_params.routing_method);
  clock_gettime(CLOCK_MONOTONIC, &finish);
  time_spent_thread = finish.tv_sec - start.tv_sec;
  printf("Time consumed by initial dijkstra executions: %ld s\n", time_spent_thread);

  printf("EXECUTION OF THE SIMULATION\n");

  /* core of the discrete-event simulation: extract next event, advance simulation time, execute the event */
  begin = clock();
  simulation->current_time = 1;
  long completed_payments = 0;
  while(heap_len(simulation->events) != 0) {
    event = heap_pop(simulation->events, compare_event); //イベントの処理（heap_popでイベントを取得し、対応する処理を実行）

    simulation->current_time = event->time;
    switch(event->type){
    case FINDPATH:
      find_path(event, simulation, network, &payments, pay_params.mpp, net_params.routing_method, net_params);
      break;
    case SENDPAYMENT:
      send_payment(event, simulation, network, net_params);
      break;
    case FORWARDPAYMENT:
      forward_payment(event, simulation, network, net_params);
      break;
    case RECEIVEPAYMENT:
      receive_payment(event, simulation, network, net_params);
      break;
    case FORWARDSUCCESS:
      forward_success(event, simulation, network, net_params);
      break;
    case RECEIVESUCCESS:
      receive_success(event, simulation, network, net_params);
      break;
    case FORWARDFAIL:
      forward_fail(event, simulation, network, net_params);
      break;
    case RECEIVEFAIL:
      receive_fail(event, simulation, network, net_params);
      break;
    case OPENCHANNEL:
      open_channel(network, simulation->random_generator);
      break;
    case CHANNELUPDATEFAIL:
      channel_update_fail(event, simulation, network);
      break;
    case CHANNELUPDATESUCCESS:
      channel_update_success(event, simulation, network);
      break;
    case UPDATEGROUP:
      group_add_queue = request_group_update(event, simulation, network, net_params, group_add_queue);
      break;
    case CONSTRUCTGROUPS:
      group_add_queue = construct_groups(simulation, group_add_queue, network, net_params);
      break;
    default:
      printf("ERROR wrong event type\n");
      exit(-1);
    }

    struct payment* p = array_get(payments, event->payment->id);
    if(p->end_time != 0 && event->type != UPDATEGROUP && event->type != CONSTRUCTGROUPS && event->type != CHANNELUPDATEFAIL && event->type != CHANNELUPDATESUCCESS){
        completed_payments++;
        char progress_filename[512];
        strcpy(progress_filename, output_dir_name);
        strcat(progress_filename, "progress.tmp");
        FILE* progress_file = fopen(progress_filename, "w");
        if(progress_file != NULL){
            fprintf(progress_file, "%f", (float)completed_payments / (float)array_len(payments));
        fclose(progress_file);
        }
    }

    free(event);
  }
  printf("\n");
  end = clock();

  if(pay_params.mpp)
    post_process_payment_stats(payments);

  time_spent = (double) (end - begin)/CLOCKS_PER_SEC;
  printf("Time consumed by simulation events: %lf s\n", time_spent);

  write_output(network, payments, output_dir_name); // シミュレーション結果の出力

  /* ===== finalize: close any still-open groups at simulation end ===== */
  if (net_params.enable_group_event_csv) {
    long gcount = array_len(network->groups);
    for (long i = 0; i < gcount; i++) {
      struct group* g = array_get(network->groups, i);
      if (g && g->is_closed == GROUP_NOT_CLOSED) {
        group_close_once(simulation, g, "simulation_end");
      }
    }

    long ecnt = array_len(network->edges);
    for (long i = 0; i < ecnt; i++) {
      struct edge* e = array_get(network->edges, i);
      if (!e) continue;
      if (e && e->group && e->group->is_closed == GROUP_NOT_CLOSED) {
        group_close_once(simulation, e->group, "simulation_end");
      }
    }
    group_events_close();
  }

  list_free(group_add_queue);
  free(simulation->random_generator);
  heap_free(simulation->events);
  free(simulation);

  // free_network(network);

  return 0;
}
