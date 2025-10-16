#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/event.h"
#include "../include/array.h"
#include <inttypes.h>

FILE* csv_group_events = NULL;

/* Functions in this file manage events of the simulation; */

struct event* new_event(uint64_t time, enum event_type type, long node_id, struct payment* payment) {
  struct event* e;
  e = malloc(sizeof(struct event));
  e->time = time;
  e->type = type;
  e->node_id = node_id;
  e->payment = payment;
  return e;
}


int compare_event(struct event *e1, struct event *e2) {
  uint64_t time1, time2;
  time1=e1->time;
  time2=e2->time;
  if(time1==time2)
    return 0;
  else if(time1<time2)
    return -1;
  else
    return 1;
}

/* initialize events by creating an event for each payment for which a route has to be found */
struct heap* initialize_events(struct array* payments){
  struct heap* events;
  long i;
  struct event* event;
  struct payment* payment;
  events = heap_initialize(array_len(payments)*10);
  for(i=0; i<array_len(payments); i++){
    payment = array_get(payments, i);
    event = new_event(payment->start_time, FINDPATH, payment->sender, payment);
    events = heap_insert(events, event, compare_event);
  }
  /* events that open new channels during a simulation; currently NOT USED */
  /* payment = array_get(payments, array_len(payments)-1); */
  /* last_payment_time = payment->start_time; */
  /* for(open_channel_time=100; open_channel_time<last_payment_time; open_channel_time+=100){ */
  /*   event = new_event(open_channel_time, OPENCHANNEL, -1, NULL); */
  /*   events = heap_insert(events, event, compare_event); */
  /* } */
  return events;
}

static void ge_write_header_if_needed(void) {
    if (!csv_group_events) return;
    // 既にヘッダを書いたかどうかを判定したいならフラグ管理でもOK（簡略化のため毎回は書かない）
}

void group_events_open(const char* dirpath) {
    if (csv_group_events) return; // 既にオープン済みなら何もしない
    char path[1024];
    snprintf(path, sizeof(path), "%s/group_events.csv", dirpath ? dirpath : ".");
    csv_group_events = fopen(path, "w");
    if (csv_group_events) {
        fprintf(csv_group_events,
            "type,time,group_id,edge_id,reason,size,needed,members,group_cap,min,max\n");
        fflush(csv_group_events);
    }
}

void group_events_close(void) {
    if (csv_group_events) {
        fflush(csv_group_events);
        fclose(csv_group_events);
        csv_group_events = NULL;
    }
}

void ge_construct_begin(uint64_t time, long seed_edge_id) {
    if (!csv_group_events) return;
    //                           type           time               group_id edge_id reason size needed members group_cap min  max
    fprintf(csv_group_events, "construct_begin,%" PRIu64 ",-,"    "%ld"   ",seed,,,,,,\n",  time,       seed_edge_id);
}

void ge_construct_abort(uint64_t time, long seed_edge_id, int size, int needed) {
    if (!csv_group_events) return;
    //                           type           time               group_id edge_id reason size needed members group_cap min max
    fprintf(csv_group_events, "construct_abort,%" PRIu64 ",-,"    "%ld"   ",,%d,%d,,,,\n",  time,       seed_edge_id,     size, needed);
}


void ge_construct_commit(uint64_t time, long group_id,
                         const char* members_dash_joined,
                         uint64_t group_cap, uint64_t min_cap, uint64_t max_cap) {
    if (!csv_group_events) return;
    fprintf(csv_group_events,
        "construct_commit,%" PRIu64 ",%ld,,,%s,%s,%s,%"
        PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
        time, group_id,
        "", "",                     // size,needed は空
        members_dash_joined ? members_dash_joined : "",
        group_cap, min_cap, max_cap);
}

void ge_close(uint64_t time, long group_id, const char* reason,
              const char* members_dash_joined) {
    if (!csv_group_events) return;
    fprintf(csv_group_events,
        "close,%" PRIu64 ",%ld,,%s,,,%s,,,\n",
        time, group_id,
        reason ? reason : "",
        members_dash_joined ? members_dash_joined : "");
}
