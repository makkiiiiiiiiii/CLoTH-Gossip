#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/cmake-build-debug"
OUTDIR="$BUILD_DIR/result"
CSV="$OUTDIR/group_events.csv"

# --- 初回だけ configure。既に CMakeCache.txt があれば再構成しない ---
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" >/dev/null
fi

# --- ビルドのみ（再構成での上書きリスクを避ける） ---
cmake --build "$BUILD_DIR" -j >/dev/null

# --- toy1 を強制適用：cloth_input.txt を上書き修正 ---
# 1) toy1 CSV をビルドディレクトリに揃える（参照先を単純化）
cp -f "$PROJECT_ROOT/nodes_ln_toy1.csv"    "$BUILD_DIR/" 2>/dev/null || true
cp -f "$PROJECT_ROOT/channels_ln_toy1.csv" "$BUILD_DIR/" 2>/dev/null || true
cp -f "$PROJECT_ROOT/edges_ln_toy1.csv"    "$BUILD_DIR/" 2>/dev/null || true

# 2) cloth_input.txt の参照を toy1 名に固定（ビルドDIR基準）
if [[ ! -f "$BUILD_DIR/cloth_input.txt" ]]; then
  echo "ERROR: $BUILD_DIR/cloth_input.txt not found." >&2
  exit 2
fi

# sed で行置換（存在する行だけを書き換え。無ければ追記）
sed -i -E 's|^nodes_filename=.*$|nodes_filename=nodes_ln_toy1.csv|'       "$BUILD_DIR/cloth_input.txt"
sed -i -E 's|^channels_filename=.*$|channels_filename=channels_ln_toy1.csv|' "$BUILD_DIR/cloth_input.txt"
sed -i -E 's|^edges_filename=.*$|edges_filename=edges_ln_toy1.csv|'       "$BUILD_DIR/cloth_input.txt"

# 念のため表示
grep -E '^(nodes_filename|channels_filename|edges_filename)=' "$BUILD_DIR/cloth_input.txt" || true

# --- 実行（常に cmake-build-debug をカレント） ---
mkdir -p "$OUTDIR"
rm -f "$CSV"

pushd "$BUILD_DIR" >/dev/null
./CLoTH_Gossip "$OUTDIR" >/dev/null
popd >/dev/null

# --- 判定（固定の CSV を毎回参照） ---
if [[ ! -f "$CSV" ]]; then
  echo "ERROR: not found: $CSV" >&2
  exit 3
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
