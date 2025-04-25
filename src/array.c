#include <stdio.h>
#include <stdlib.h>
#include "../include/array.h"

/*配列の容量を2倍に拡張する関数*/
struct array* resize_array(struct array* a) {
  struct array* new;
  long i;

  /*新しい配列newを作成し、元の配列の容量を2倍に設定*/
  new=malloc(sizeof(struct array));
  if (new == NULL) {
      fprintf(stderr, "ERROR: malloc failed for struct array");
      exit(1);

  }
  new->size = a->size*2; //配列の現在の容量（要素数）(size)を2倍にする
  new->index = a->index; //要素数(index)は元の配列と同じ
  new->element = malloc(new->size*sizeof(void*)); //新しい配列の要素を格納するためのメモリを確保
  if (new->element == NULL) {
      fprintf(stderr, "ERROR: malloc failed for array elements");
      free(a);
      exit(1);
  }
  for(i=0; i<new->index; i++) //元の配列の要素を新しい配列にコピー
    new->element[i]=a->element[i];
  for(;i<new->size;i++) //新しい配列の未使用部分をNULLで初期化
    new->element[i] = NULL;
  array_free(a); //元の配列を解放
  return new; //新しい配列を返す
}

/*動的配列を初期化する関数*/
struct array* array_initialize(long size) {
    struct array* a = malloc(sizeof(struct array)); //struct arrayを動的に確保
    if (a == NULL) {
        fprintf(stderr, "ERROR: malloc failed for struct array");
        exit(1);
    }
    a->size = size;
    a->index = 0; //要素数を0に初期化
    a->element = malloc(a->size * sizeof(void*));
    if (a->element == NULL) {
        fprintf(stderr, "ERROR: malloc failed for array elements");
        free(a);
        exit(1);
    }

    return a; //初期化された配列を返す
}

/*配列に新しい要素を挿入する関数*/
struct array* array_insert(struct array* a, void* data) {
  if(a->index >= a->size) //配列が満杯の場合、resize_arrayで容量を拡張
    a = resize_array(a);

  a->element[a->index]=data; //新しい要素をelement配列の末尾に追加
  (a->index)++; //要素数を1増やす

  return a;
}

/*指定したインデックスの要素を取得する関数*/
void* array_get(struct array* a,long i) {
  if(i>=a->size || i>=a->index) return NULL; //インデックスが範囲外の場合はNULLを返す
  return a->element[i]; //指定したインデックスの要素を返す
}

/*配列に格納されている要素数を返す関数*/
long array_len(struct array *a) {
  return a->index; //indexの値(要素数)を返す
}

/*配列の要素を逆順に並べ替える関数*/
void array_reverse(struct array *a) {
  long i, n;
  void*tmp;

  n = array_len(a); //配列の長さを取得

  /*配列の前半と後半をスワップすることで逆順にする*/
  for(i=0; i<n/2; i++) {
    tmp = a->element[i];
    a->element[i] = a->element[n-i-1];
    a->element[n-i-1] = tmp;
  }
}

/*配列の要素を指定したインデックスで削除する関数*/
void delete_element(struct array *a, long element_index) {
  long i;

  (a->index)--; //index(要素数)をデクリメント

  for(i = element_index; i < a->index ; i++)
    a->element[i] = a->element[i+1]; //指定されたインデックス以降の要素を1つ前にシフト
}

/*配列から特定の要素を削除する関数*/
void array_delete(struct array* a, void* element,  int(*is_equal)()) {
  long i;

  for(i = 0; i < array_len(a); i++) {
    if(is_equal(a->element[i], element)) //配列を走査し、指定された条件（is_equal関数）を満たす要素を探す
      delete_element(a, i); //該当する要素をdelete_elementで削除
  }
}

/*配列の全要素を削除する関数*/
void array_delete_all(struct array* a) {
  a->index = 0; //indexを0に設定し、要素を論理的に削除
}

/*配列のメモリを解放する関数*/
void array_free(struct array* a)  {
  free(a->element); //要素を格納しているメモリ(element配列)を解放
  free(a); //struct array自体を解放
}
