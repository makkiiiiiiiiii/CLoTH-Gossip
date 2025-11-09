#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/event.h"
#include "../include/array.h"
#include <inttypes.h>

FILE* csv_group_events = NULL;

struct event* new_event(uint64_t time, enum event_type type, long node_id, struct payment* payment) {
  struct event* e = (struct event*)malloc(sizeof(struct event));
  e->time = time;
  e->type = type;
  e->node_id = node_id;
  e->payment = payment;
  return e;
}

int compare_event(struct event *e1, struct event *e2) {
  uint64_t time1 = e1->time, time2 = e2->time;
  if (time1 == time2) return 0;
  return (time1 < time2) ? -1 : 1;
}

/* initialize events by creating an event for each payment for which a route has to be found */
struct heap* initialize_events(struct array* payments){
  struct heap* events = heap_initialize(array_len(payments)*10);
  for(long i = 0; i < array_len(payments); i++){
    struct payment* payment = array_get(payments, i);
    struct event* event = new_event(payment->start_time, FINDPATH, payment->sender, payment);
    events = heap_insert(events, event, compare_event);
  }
  return events;
}

static inline const char* dash_if_empty(const char* s) {
    return (s && s[0] != '\0') ? s : "-";
}

static inline const char* gid_or_dash(long gid, char* buf, size_t buflen) {
    if (gid < 0) return "-";            /* 未確定 */
    snprintf(buf, buflen, "%ld", gid);
    return buf;
}

void group_events_open(const char* dirpath) {
    if (csv_group_events) return; // already open
    char path[1024];
    if (dirpath && dirpath[0] != '\0')
        snprintf(path, sizeof(path), "%s/group_events.csv", dirpath);
    else
        snprintf(path, sizeof(path), "group_events.csv");

    csv_group_events = fopen(path, "w");
    if (csv_group_events) {
        fprintf(csv_group_events,
            "type,time,group_id,edge_id,role,seed_id,attempt_id,reason,size,needed,members,group_cap,min,max,C15\n");
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

void ge_construct_begin(uint64_t time, long seed_edge_id, uint64_t attempt_id) {
    if (!csv_group_events) return;
    /* construct_begin,time,-,-,seed,seed_id,attempt_id,-,-,-,-,-,-,-,- */
    fprintf(csv_group_events,
        "construct_begin,%" PRIu64 ",-,-,seed,%ld,%" PRIu64 ",-,-,-,-,-,-,-,-\n",
        (unsigned long long)time, seed_edge_id, (unsigned long long)attempt_id);
}

void ge_construct_abort(uint64_t time, long seed_edge_id, int size, int needed, uint64_t attempt_id) {
    if (!csv_group_events) return;
    fprintf(csv_group_events,
        "construct_abort,%" PRIu64 ",-,-,seed,%ld,%" PRIu64 ",shortage,%d,%d,-,-,-,-,-\n",
        (unsigned long long)time, seed_edge_id, (unsigned long long)attempt_id, size, needed);
}

void ge_construct_commit(uint64_t time, long group_id,
                         const char* members_dash_joined,
                         uint64_t group_cap, uint64_t min_cap, uint64_t max_cap,
                         long seed_edge_id, uint64_t attempt_id) {
    if (!csv_group_events) return;
    fprintf(csv_group_events,
        "construct_commit,%" PRIu64 ",%ld,-,group,%ld,%" PRIu64 ",commit,-,-,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",-\n",
        (unsigned long long)time, group_id,
        seed_edge_id, (unsigned long long)attempt_id,
        dash_if_empty(members_dash_joined),
        (unsigned long long)group_cap, (unsigned long long)min_cap, (unsigned long long)max_cap);
}

void ge_join(uint64_t time, long group_id, long edge_id,
             const char* reason, uint64_t group_cap,
             uint64_t min_cap, uint64_t max_cap,
             long seed_edge_id, uint64_t attempt_id) {
    if (!csv_group_events) return;
    fprintf(csv_group_events,
        "join,%" PRIu64 ",%ld,%ld,join,%ld,%" PRIu64 ",%s,-,-,-,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",-\n",
        (unsigned long long)time, group_id, edge_id,
        seed_edge_id, (unsigned long long)attempt_id, dash_if_empty(reason),
        (unsigned long long)group_cap, (unsigned long long)min_cap, (unsigned long long)max_cap);
}

void ge_leave(uint64_t time, long group_id, long edge_id,
              const char* reason, uint64_t group_cap,
              uint64_t min_cap, uint64_t max_cap,
              long seed_edge_id, uint64_t attempt_id) {
    if (!csv_group_events) return;
    fprintf(csv_group_events,
        "leave,%" PRIu64 ",%ld,%ld,leave,%ld,%" PRIu64 ",%s,-,-,-,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",-\n",
        (unsigned long long)time, group_id, edge_id,
        seed_edge_id, (unsigned long long)attempt_id, dash_if_empty(reason),
        (unsigned long long)group_cap, (unsigned long long)min_cap, (unsigned long long)max_cap);
}

void ge_update_group(uint64_t time, long group_id,
                     uint64_t group_cap, uint64_t min_cap, uint64_t max_cap,
                     long seed_edge_id, uint64_t attempt_id) {
    if (!csv_group_events) return;
    char gidbuf[32];
    const char* gid = gid_or_dash(group_id, gidbuf, sizeof(gidbuf));
    fprintf(csv_group_events,
        "update_group,%" PRIu64 ",%s,-,group,%ld,%" PRIu64 ",update,-,-,-,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",-\n",
        (unsigned long long)time, gid, seed_edge_id, (unsigned long long)attempt_id,
        (unsigned long long)group_cap, (unsigned long long)min_cap, (unsigned long long)max_cap);
}

void ge_close(uint64_t time, long group_id, const char* reason,
              const char* members_dash_joined,
              long seed_edge_id, uint64_t attempt_id) {
    if (!csv_group_events) return;
    fprintf(csv_group_events,
        "close,%" PRIu64 ",%ld,-,group,%ld,%" PRIu64 ",%s,-,-,%s,-,-,-,-\n",
        (unsigned long long)time, group_id,
        seed_edge_id, (unsigned long long)attempt_id, dash_if_empty(reason),
        dash_if_empty(members_dash_joined));
}