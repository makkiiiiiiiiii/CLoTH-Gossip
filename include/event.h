#ifndef EVENT_H
#define EVENT_H

#include <stdint.h>
#include "heap.h"
#include "array.h"
#include "payments.h"
#include <stdio.h>

enum event_type {
  FINDPATH,
  SENDPAYMENT,
  FORWARDPAYMENT,
  RECEIVEPAYMENT,
  FORWARDSUCCESS,
  FORWARDFAIL,
  RECEIVESUCCESS,
  RECEIVEFAIL,
  OPENCHANNEL,
  CHANNELUPDATEFAIL,
  CHANNELUPDATESUCCESS,
  UPDATEGROUP,
  CONSTRUCTGROUPS,
};

struct event {
  uint64_t time;
  enum event_type type;
  long node_id;
  struct payment *payment;
};

struct event* new_event(uint64_t time, enum event_type type, long node_id, struct payment* payment);

int compare_event(struct event* e1, struct event *e2);

struct heap* initialize_events(struct array* payments);

extern FILE* csv_group_events;

void group_events_open(const char* dirpath);     // 出力先ディレクトリ
void group_events_close(void);

// 各イベント専用の出力関数
void ge_construct_begin(uint64_t time, long seed_edge_id);
void ge_construct_abort(uint64_t time, long seed_edge_id, int size, int needed);
void ge_construct_commit(uint64_t time, long group_id,
                         const char* members_dash_joined,
                         uint64_t group_cap, uint64_t min_cap, uint64_t max_cap);
void ge_close(uint64_t time, long group_id, const char* reason,
              const char* members_dash_joined);

#endif
