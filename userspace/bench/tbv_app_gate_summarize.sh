#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<EOF
Usage: tbv_app_gate_summarize.sh LOG_ROOT

Summarizes PyTorch rep logs from tbv_app_gate.sh. The output is intentionally
flat so it can be pasted into a results note or compared across timeout runs.
EOF
}

if [[ $# -ne 1 || "$1" == "--help" || "$1" == "-h" ]]; then
  usage
  [[ $# -eq 1 && ( "$1" == "--help" || "$1" == "-h" ) ]] && exit 0
  exit 2
fi

root=$1
if [[ ! -d "$root" ]]; then
  echo "log root does not exist: $root" >&2
  exit 2
fi

sum_counter() {
  local file=$1
  local key=$2

  awk -v k="$key" '
    $1 == k && $2 == "sum" {
      gsub(/^\+/, "", $3)
      print $3
      found = 1
    }
    END {
      if (!found)
        print "NA"
    }
  ' "$file"
}

rank0_log_for_rep() {
  local rep_dir=$1
  find "$rep_dir" -maxdepth 1 -type f -name 'rank0.*.log' | sort | head -n 1
}

timing_us() {
  local log=$1
  local bytes=$2

  if [[ -z "$log" || ! -f "$log" ]]; then
    printf 'NA\n'
    return
  fi

  awk -v bytes="$bytes" '
    $0 ~ "all_to_all_single bytes=" bytes ":" {
      print $3
      found = 1
      exit
    }
    END {
      if (!found)
        print "NA"
    }
  ' "$log"
}

printed=0
while IFS= read -r rep_dir; do
  counters=$rep_dir/counters.log
  [[ -f "$counters" ]] || continue

  mode=${rep_dir%/*}
  mode=${mode##*/}
  rep=${rep_dir##*/rep-}
  rank0_log=$(rank0_log_for_rep "$rep_dir")
  t1=$(timing_us "$rank0_log" 1048576)
  t2=$(timing_us "$rank0_log" 2097152)

  if [[ $printed -eq 0 ]]; then
    printf 'mode rep t1_us t2_us wr_retx rnr_retx ack_retry ack64 reord_to reord_retry reord_drop active_to active_retry tx_rnr rx_rnr rnr_exh dv_hard wr_to wr_exh tx_err tx_post tx_comp\n'
    printed=1
  fi

  printf '%s %s %s %s' "$mode" "$rep" "$t1" "$t2"
  for key in \
    data_wr_retransmit \
    data_wr_rnr_retransmit \
    data_rx_ack_match_retried \
    data_rx_ack_match_over_64ms \
    data_rx_reorder_timeout \
    data_rx_reorder_retry \
    data_rx_reorder_dropped \
    data_rx_active_timeout \
    data_rx_active_retry \
    data_tx_ack_rnr \
    data_rx_ack_rnr \
    data_wr_rnr_retry_exhausted \
    dv_hard_error \
    data_wr_timeout \
    data_wr_retry_exhausted \
    data_tx_errors \
    data_tx_posted \
    data_tx_completed; do
    printf ' %s' "$(sum_counter "$counters" "$key")"
  done
  printf '\n'
done < <(find "$root" -path '*/pytorch/*/rep-*' -type d | sort -V)

if [[ $printed -eq 0 ]]; then
  echo "no PyTorch rep counters found under: $root" >&2
  exit 1
fi

if find "$root" -path '*/pytorch/*/rank*.log' -type f -print -quit | grep -q .; then
  printf '\nloaded_collective_lib counts:\n'
  grep -h 'loaded_collective_lib=' $(find "$root" -path '*/pytorch/*/rank*.log' -type f | sort -V) \
    | sed 's/.*loaded_collective_lib=//' \
    | sort \
    | uniq -c
fi
