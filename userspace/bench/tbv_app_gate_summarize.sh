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
    printf 'mode rep t1_us t2_us wr_retx rnr_retx ack_retry ack64 late_ack dup_ack ack_miss ack_probe ack_probe_fb tx_ack_req tx_ack_req_err rx_ack_req rx_ack_req_reack rx_ack_req_miss rx_ack_req_miss_past rx_ack_req_miss_cur rx_ack_req_miss_cur_active rx_ack_req_miss_cur_reorder rx_ack_req_miss_cur_idle rx_ack_req_miss_fut reord_to reord_retry reord_drop reord_dup_refresh active_to active_retry active_dup_refresh active_merge active_merge_bytes active_merge_complete tx_rnr rx_rnr rnr_exh rnr_exh_cap rnr_exh_close rnr_exh_qperr rnr_wait_notret rnr_wait_retrying rnr_wait_txpend rnr_wait_exh rnr_wait_close rnr_wait_qperr rnr_wait_unknown dv_hard wr_to wr_exh tx_err tx_post tx_comp\n'
    printed=1
  fi

  printf '%s %s %s %s' "$mode" "$rep" "$t1" "$t2"
  for key in \
    data_wr_retransmit \
    data_wr_rnr_retransmit \
    data_rx_ack_match_retried \
    data_rx_ack_match_over_64ms \
    data_rx_late_ack \
    data_rx_duplicate_ack \
    data_rx_ack_miss \
    data_wr_ack_probe \
    data_wr_ack_probe_fallback \
    data_tx_ack_req \
    data_tx_ack_req_send_error \
    data_rx_ack_req \
    data_rx_ack_req_reack \
    data_rx_ack_req_miss \
    data_rx_ack_req_miss_past \
    data_rx_ack_req_miss_current \
    data_rx_ack_req_miss_current_active \
    data_rx_ack_req_miss_current_reorder \
    data_rx_ack_req_miss_current_idle \
    data_rx_ack_req_miss_future \
    data_rx_reorder_timeout \
    data_rx_reorder_retry \
    data_rx_reorder_dropped \
    data_rx_reorder_duplicate_refresh \
    data_rx_active_timeout \
    data_rx_active_retry \
    data_rx_active_duplicate_refresh \
    data_rx_active_write_reorder_merge \
    data_rx_active_write_reorder_merge_bytes \
    data_rx_active_write_reorder_merge_complete \
    data_tx_ack_rnr \
    data_rx_ack_rnr \
    data_wr_rnr_retry_exhausted \
    data_wr_rnr_complete_retry_exhausted \
    data_wr_rnr_complete_closing_qp \
    data_wr_rnr_complete_qp_error \
    data_wr_rnr_wait_not_retryable \
    data_wr_rnr_wait_retrying \
    data_wr_rnr_wait_tx_pending \
    data_wr_rnr_wait_retry_exhausted \
    data_wr_rnr_wait_closing_qp \
    data_wr_rnr_wait_qp_error \
    data_wr_rnr_wait_unknown \
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
