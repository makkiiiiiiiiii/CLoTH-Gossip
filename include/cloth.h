#ifndef CLOTH_H
#define CLOTH_H

#include <stdint.h>
#include "heap.h"
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>

enum routing_method{
    CLOTH_ORIGINAL, CHANNEL_UPDATE, GROUP_ROUTING, IDEAL
};

struct network_params{
  long n_nodes;
  long n_channels;
  long capacity_per_channel;
  double faulty_node_prob;
  unsigned int network_from_file;
  char nodes_filename[256];
  char channels_filename[256];
  char edges_filename[256];
  unsigned int payment_timeout; // set -1 to disable payment timeout
  unsigned int average_payment_forward_interval;
  unsigned int variance_payment_forward_interval;
  enum routing_method routing_method;
  unsigned int group_cap_update;
  unsigned int group_broadcast_delay;
  int group_size;
  int group_size_min;
  float group_limit_rate;
  unsigned int use_conventional_method;
  float group_min_cap_ratio;
  float group_max_cap_ratio;
  /* === leave/rejoin control === */
  double   tau_default;              /* UL >= tau で離脱候補 */
  int      k_used_on_min_edge;       /* 最小エッジの加入後使用回数しきい値K */
  int      cooldown_hops;            /* クールダウン長 (平均フォワード間隔×この値) */
  int      max_leaves_per_group_tick;/* 1グループ同tickでの離脱上限 */

  /* === logging === */
  int      enable_group_event_csv;   /* bool: CSVログ有効/無効 */
  char     group_event_csv_filename[256];
  int  enable_group_trace_verbose;

  /* === optional: per-edge tau randomization === */
  int      tau_randomize;            /* bool */
  double   tau_min;
  double   tau_max;
};

struct payments_params{
  double inverse_payment_rate;
  long n_payments;
  double amount_mu; // average_payment_amount [satoshi]
  double amount_sigma; // variance_payment_amount [satoshi]
  unsigned int payments_from_file;
  char payments_filename[256];
  unsigned int mpp;
  double max_fee_limit_mu; // average_max_fee_limit [satoshi]
  double max_fee_limit_sigma; // variance_max_fee_limit [satoshi]
};

struct simulation{
  uint64_t current_time; //milliseconds
  struct heap* events;
  gsl_rng* random_generator;
};

#endif
