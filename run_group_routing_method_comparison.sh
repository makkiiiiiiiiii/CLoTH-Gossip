#!/bin/bash

if [[ "$#" -lt 2 ]]; then
  echo "Usage: ./run_group_routing_comparison.sh <seed> <output_dir>"
  exit 0
fi

seed="$1"
output_dir="$2/$(date "+%Y%m%d%H%M%S")"
mkdir -p "$output_dir"

max_processes=8
queue=()
running_processes=0
total_simulations=0
start_time=$(date +%s)

function enqueue_simulation() {
  queue+=("$@")
  ((total_simulations++))
}

function process_queue() {
  while [ "$running_processes" -lt "$max_processes" ] && [ "${#queue[@]}" -gt 0 ]; do
    eval "${queue[0]}" > /dev/null 2>&1 &
    queue=("${queue[@]:1}")
    ((running_processes++))
  done

  wait -n || true
  ((running_processes--))
}

function display_progress() {
  if [ "$total_simulations" -eq 0 ]; then return 0; fi

  done_simulations=0

  while [ "$done_simulations" -lt "$total_simulations" ]; do
    progress_summary=""
    total_progress=0

    IFS=$'\n' read -r -d '' -a progress_files <<< $(find "$output_dir" -type f -name "progress.tmp")
    done_simulations=0
    for file in "${progress_files[@]}"; do
      progress=$(cat "$file")
      [[ "$progress" = "1" ]] && ((done_simulations++))
      [[ "$progress" = "" ]] && progress="0"
      total_progress=$(printf "%.5f" "$(echo "scale=4; $total_progress + $progress / $total_simulations" | bc)")
      progress_summary+=$(printf "%3d%% %s\n" "$(printf "%.0f" "$(echo "$progress * 100" | bc)")" "$file")
    done

    bar_len=$(printf "%.0f" "$(echo "$total_progress * 50" | bc)")
    progress_bar=$(printf "%-${bar_len}s" "#" | tr ' ' '#')
    if (( $(echo "$total_progress == 0" | bc) )); then
      echo -e "$progress_summary"
      printf "Progress: [%-50s] 0%%\t%d/%d\tTime remaining --:--\n" "" "$done_simulations" "$total_simulations"
    else
      elapsed=$(( $(date +%s) - start_time ))
      remaining=$(python3 -c "print(int($elapsed / $total_progress - $elapsed))")
      min=$((remaining / 60))
      sec=$((remaining % 60))
      echo -e "$progress_summary"
      printf "Progress: [%-50s] %0.1f%%\t%d/%d\tTime remaining %02d:%02d\n" "$progress_bar" "$(echo "$total_progress * 100" | bc)" "$done_simulations" "$total_simulations" "$min" "$sec"
    fi
    sleep 1
  done
}

# === Experiment loop ===
for i in $(seq 1.0 0.2 5.0); do
  avg_amt=$(python3 -c "print('{:.0f}'.format(10**$i))")
  var_amt=$((avg_amt / 10))

  # 提案手法: use_conventional_method=false, group_min_cap_ratio=1.0, group_max_cap_ratio=1.4
  enqueue_simulation "./run-simulation.sh $seed $output_dir/proposed/avg=$avg_amt group_routing alpha=0.2 center=1.2 payment_timeout=-1 n_payments=5000 mpp=0 routing_method=group_routing group_cap_update=true use_conventional_method=false group_min_cap_ratio=1.0 group_max_cap_ratio=1.4 average_payment_amount=$avg_amt variance_payment_amount=$var_amt group_size=10"

  # 比較手法: use_conventional_method=true
  enqueue_simulation "./run-simulation.sh $seed $output_dir/conventional/avg=$avg_amt group_routing_conventional payment_timeout=-1 n_payments=5000 mpp=0 routing_method=group_routing group_cap_update=true use_conventional_method=true group_limit_rate=0.1 average_payment_amount=$avg_amt variance_payment_amount=$var_amt group_size=10"
done

# === Run the queue ===
display_progress &
while [ "${#queue[@]}" -gt 0 ] || [ "$running_processes" -gt 0 ]; do
  process_queue
  sleep 1
done
wait

# === Post processing ===
echo -e "\nAll simulations completed.\nResults saved at: $output_dir"
python3 scripts/analyze_output.py "$output_dir"
end_time=$(date +%s)
echo "START : $(date --date @"$start_time")"
echo "  END : $(date --date @"$end_time")"
echo " TIME : $((end_time - start_time)) seconds"
