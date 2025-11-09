#!/usr/bin/env bash
set -euo pipefail

# 出力先をCMake側に統一
OUTDIR="cmake-build-debug/result"
CSV="$OUTDIR/group_events.csv"

# ビルド & 実行（結果CSVを生成）
cmake -S . -B cmake-build-debug >/dev/null
cmake --build cmake-build-debug -j >/dev/null
./cmake-build-debug/CLoTH_Gossip "$OUTDIR" >/dev/null

# 件数チェック（commit と close が一致しないなら失敗）
awk -F, 'NR>1{if($1=="construct_commit")C++; if($1=="close")K++}
         END{
           if(C!=K){
             printf("FAIL: commit=%d close=%d diff=%d\n", C, K, K-C);
             exit 1
           } else {
             printf("OK:   commit=%d close=%d diff=%d\n", C, K, K-C);
           }
         }' "$CSV"
