#ifndef EVENT_H
#define EVENT_H

/*
 * Group Events CSV (A-1/A-2) — unified 15-column schema
 *
 * Header (固定):
 *   type,time,group_id,edge_id,role,seed_id,attempt_id,reason,size,needed,members,group_cap,min,max,C15
 *
 * 各列の意味（常に同じ列へ同じ意味を出力すること）:
 *   - type        : イベント種別（construct_begin / construct_abort / update_group /
 *                   construct_commit / join / leave / close など）
 *   - time        : 発生時刻（例: simulation->current_time）
 *   - group_id    : グループID。未確定の間は "-" を出す（呼び出し側は -1 を渡す）
 *   - edge_id     : 主語のエッジID。join/leave のときのみエッジID、それ以外は "-" を出す
 *   - role        : 文脈ラベル（"seed" / "group" / "join" / "leave" など）
 *   - seed_id     : 構築のシードとなったエッジID（全イベントで同じ列に出す）
 *   - attempt_id  : その seed に対する試行番号（全イベントで同じ列に出す）
 *   - reason      : 理由や補助情報（abort/leave 等で使用。不要時は "-"）
 *   - size        : その時点で集まっている人数（abort で使用。不要時は "-"）
 *   - needed      : 目標人数（典型的に 10。abort 時に使用。不要時は "-"）
 *   - members     : 確定メンバーの dash 連結（"a-b-c-..."）。commit 時のみ、それ以外は "-"
 *   - group_cap   : グループ合計容量（update/commit/join/leave で出す。不要時は "-"）
 *   - min         : 公開最小容量（同上）
 *   - max         : 最大容量（同上）
 *   - C15         : 予備列（未使用なら常に "-"）
 *
 * 出力上のルール:
 *   - 欠損は空欄ではなく必ず "-" を出す
 *   - group_id 未確定時は、呼び出し側は group_id に -1 を渡す（実装側で "-" に変換）
 *   - edge_id は join/leave の「主語」イベントでのみ値を入れ、それ以外は "-" を出す
 *   - seed_id / attempt_id は常に同列・1回だけ出す（edge_id と重複させない）
 */

#include <stdint.h>
#include <stdio.h>

#include "heap.h"
#include "array.h"
#include "payments.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* CSV 出力先ディレクトリを指定してオープン（ヘッダも出力）。dirpath 配下に "group_events.csv" を作成。 */
void group_events_open(const char* dirpath);

/* CSV をクローズ */
void group_events_close(void);

void ge_construct_begin(uint64_t time, long seed_edge_id, uint64_t attempt_id);

void ge_construct_abort(uint64_t time, long seed_edge_id, int size, int needed, uint64_t attempt_id);

void ge_construct_commit(uint64_t time, long group_id,
                         const char* members_dash_joined,
                         uint64_t group_cap, uint64_t min_cap, uint64_t max_cap,
                         long seed_edge_id, uint64_t attempt_id);

void ge_join(uint64_t time, long group_id, long edge_id,
             const char* reason, uint64_t group_cap,
             uint64_t min_cap, uint64_t max_cap,
             long seed_edge_id, uint64_t attempt_id);

void ge_leave(uint64_t time, long group_id, long edge_id,
              const char* reason, uint64_t group_cap,
              uint64_t min_cap, uint64_t max_cap,
              long seed_edge_id, uint64_t attempt_id);

void ge_update_group(uint64_t time, long group_id,
                     uint64_t group_cap, uint64_t min_cap, uint64_t max_cap,
                     long seed_edge_id, uint64_t attempt_id);

void ge_close(uint64_t time, long group_id, const char* reason,
              const char* members_dash_joined,
              long seed_edge_id, uint64_t attempt_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EVENT_H */
