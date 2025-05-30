if [ $# -lt 3 ]
then
    echo "usage: $0 <seed> <output-directory> [setting_key=setting_value]"
    exit
fi

seed="$1"
environment_dir="$2/environment"
result_dir="$2"

mkdir -p "$environment_dir"
mkdir -p "$result_dir"
mkdir -p "$result_dir/log"

rsync -av -q --exclude='result' --exclude='cmake-build-debug' --exclude='cloth.dSYM' --exclude='.idea' --exclude='.git' --exclude='.cmake' "." "$environment_dir"

for arg in "${@:3}"; do
    key="${arg%=*}"
    value="${arg#*=}"
    sed -i -e "s/$key=.*/$key=$value/" "$environment_dir/cloth_input.txt"
done

cp "$environment_dir/cloth_input.txt" "$2"
cd "$environment_dir"
cmake . > "$result_dir/log/cmake.log" 2>&1
make > "$result_dir/log/make.log" 2>&1

# save all logging for debug
#GSL_RNG_SEED="$seed"  strace -o "$result_dir/log/strace.log" ./CLoTH_Gossip "$result_dir/" > "$result_dir/log/cloth.log" 2>&1
GSL_RNG_SEED="$seed"  ./CLoTH_Gossip "$result_dir/" > "$result_dir/log/cloth.log" 2>&1

cat "$result_dir/output.log"
echo "seed=$seed" >> "$result_dir/cloth_input.txt"
echo "1" > "$result_dir/progress.tmp"

rm -Rf "$environment_dir"
