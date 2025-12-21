#include <stdlib.h>
#include "../include/utils.h"
#include "../include/routing.h"

int is_equal_result(struct node_pair_result *a, struct node_pair_result *b ){
  return a->to_node_id == b->to_node_id;
}

int is_equal_key_result(long key, struct node_pair_result *a){
  return key == a->to_node_id;
}

int is_equal_long(long* a, long* b) {
  return *a==*b;
}

int is_key_equal(struct distance* a, struct distance* b) {
  return a->node == b->node;
}

int is_equal_edge(struct edge* edge1, struct edge* edge2) {
  return edge1->id == edge2->id;
}

int is_present(long element, struct array* long_array) {
  long i, *curr;

  if(long_array==NULL) return 0;

  for(i=0; i<array_len(long_array); i++) {
    curr = array_get(long_array, i);
    if(*curr==element) return 1;
  }

  return 0;
}

int can_join_group(struct group* group, struct edge* edge){

  if (!group || !edge) return 0;
  if (edge->group != NULL) return 0;

  if(edge->balance < group->min_cap_limit || edge->balance > group->max_cap_limit){
    return 0;
  }

  for(int i = 0; i < array_len(group->edges); i++) {
    struct edge *e = array_get(group->edges, i);
    if (edge == e) return 0;
    if (edge->to_node_id == e->to_node_id ||
        edge->to_node_id == e->from_node_id ||
        edge->from_node_id == e->to_node_id ||
        edge->from_node_id == e->from_node_id) {
      return 0;
        }
  }
  return 1;
}

int can_fill_group(struct group* group, struct edge* edge, struct network_params net_params)
{
  if (!group || !edge) return 0;

  /* (c) サイズ：target 未満だけ補充対象（target は group_size で運用） */
  if ((long)array_len(group->edges) >= (long)net_params.group_size) return 0;

  /* 既に所属している edge は不可 */
  if (edge->group != NULL) return 0;

  /* (d) group_cap を下げない：補充で公開最小値が下がるのを抑止 */
  if (edge->balance < group->group_cap) return 0;

  /* (a)(b) レンジ＋構造（ノード共有禁止等）は既存判定を流用 */
  return can_join_group(group, edge);
}