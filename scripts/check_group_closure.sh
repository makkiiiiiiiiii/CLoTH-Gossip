#!/usr/bin/env bash
set -euo pipefail

# どこから実行しても同じ動作にするためのパス解決
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/cmake-build-debug"
OUTDIR="$BUILD_DIR/result"
CSV="$OUTDIR/group_events.csv"

# ★ 再構成は基本しない（上書き事故を防ぐ）。
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  echo "ERROR: $BUILD_DIR が未構成です。まず一度だけ以下を実行してください:"
  echo "  cmake -S \"$PROJECT_ROOT\" -B \"$BUILD_DIR\""
  exit 2
fi

# ビルドのみ（再構成しない）
cmake --build "$BUILD_DIR" -j >/dev/null

# 実行は必ずビルドディレクトリをカレントにし、cloth_input.txt はそこで見つかった内容をそのまま使う
pushd "$BUILD_DIR" >/dev/null

# 必須ファイルチェック：ここに書かれた内容で実行する（絶対に書き換えない）
if [[ ! -f cloth_input.txt ]]; then
  echo "ERROR: $BUILD_DIR/cloth_input.txt not found." >&2
  popd >/dev/null
  exit 3
fi

echo "===== EFFECTIVE cloth_input.txt (keys) ====="
grep -E '^(nodes_filename|channels_filename|edges_filename|generate_network_from_file|generate_payments_from_file|payments_filename|group_size|group_limit_rate|group_min_cap_ratio|group_max_cap_ratio)=' cloth_input.txt || true
echo "==========================================="

# 出力先を固定。古いCSVは消す
mkdir -p "$OUTDIR"
rm -f "$CSV"

# 実行（cloth_input.txt の記述どおりに動く）
./CLoTH_Gossip "$OUTDIR" >/dev/null

popd >/dev/null

# 出力確認と集計（CSV/TSV両対応）
if [[ ! -f "$CSV" ]]; then
  echo "ERROR: not found: $CSV" >&2
  exit 4
fi

awk -F '[,\t]' '
  NR>1 {
    if ($1=="construct_commit") C++
    else if ($1=="close") K++
  }
  END {
    if (C!=K) {
      printf("FAIL: commit=%d close=%d diff=%d\n", C, K, K-C)
      exit 1
    } else {
      printf("OK:   commit=%d close=%d diff=%d\n", C, K, K-C)
    }
  }
' "$CSV"

echo "Checked: $CSV"
