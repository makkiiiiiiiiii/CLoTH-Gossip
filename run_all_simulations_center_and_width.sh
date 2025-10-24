#!/bin/bash

if [[ "$#" -lt 2 ]]; then
  echo "./run_all_simulations_center_and_width.sh <seed> <output_base_dir>"
  exit 1
fi

seed="$1"
output_dir="$2/$(date "+%Y%m%d%H%M%S")"
mkdir -p "$output_dir"

max_processes=4
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
        simulation_progress_files=($(find "$output_dir" -type f -name "progress.tmp"))
        done_simulations=0
        total_progress=0
        progress_summary=""
        for file in "${simulation_progress_files[@]}"; do
            progress=$(cat "$file")
            [ "$progress" = "1" ] && ((done_simulations++))
            [ -z "$progress" ] && progress="0"
            total_progress=$(echo "$total_progress + $progress / $total_simulations" | bc -l)
            progress_summary="${progress_summary}$(printf "%3d%% %s\n" "$(echo "$progress * 100" | bc -l | cut -d'.' -f1)" "$file")"
        done
        bar_len=$(printf "%.0f" "$(echo "$total_progress * 50" | bc)")
        progress_bar=$(printf "%-${bar_len}s" "#" | tr ' ' '#')
        percent=$(printf "%.1f" "$(echo "$total_progress * 100" | bc)")
        elapsed=$(( $(date +%s) - start_time ))
        remaining=$(python3 -c "print(int($elapsed / $total_progress - $elapsed))" 2>/dev/null || echo 0)
        printf "Progress: [%-50s] %5s%%\t%d/%d\t Time remaining %02d:%02d\n%s\n" "$progress_bar" "$percent" "$done_simulations" "$total_simulations" "$((remaining/60))" "$((remaining%60))" "$progress_summary"
        sleep 1
    done
}

# sweep center and alpha (±α)
for center in $(seq 0.80 0.05 1.20); do
  for alpha in $(seq 0.01 0.01 0.20); do
    min_cap_ratio=$(python3 -c "print(round($center - $alpha, 4))")
    max_cap_ratio=$(python3 -c "print(round($center + $alpha, 4))")

    if (( $(echo "$min_cap_ratio <= 1.0" | bc -l) )) && (( $(echo "$max_cap_ratio >= 1.0" | bc -l) )); then
      dir="$output_dir/center=${center}_alpha=${alpha}"
      mkdir -p "$dir"

      enqueue_simulation "./run-simulation.sh $seed $dir \
        group_min_cap_ratio=$min_cap_ratio \
        group_max_cap_ratio=$max_cap_ratio \
        average_payment_amount=1000 \
        variance_payment_amount=100 \
        n_payments=20000 \
        payment_timeout=-1 \
        mpp=0 \
        use_conventional_method=false \
        routing_method=group_routing \
        group_cap_update=true \
        group_size=10 \
        group_limit_rate=0.1"
    fi
  done
done

display_progress &
while [ "${#queue[@]}" -gt 0 ] || [ "$running_processes" -gt 0 ]; do
    process_queue
    sleep 1
done
wait

echo -e "\nAll simulations have completed.\nOutputs saved at $output_dir"
python3 scripts/analyze_output.py "$output_dir"

end_time=$(date +%s)
echo "START : $(date --date @$start_time)"
echo "  END : $(date --date @$end_time)"
echo " TIME : $((end_time - start_time)) seconds"
