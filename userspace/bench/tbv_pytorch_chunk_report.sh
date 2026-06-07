#!/usr/bin/env bash
set -euo pipefail

size_filter=${TBV_TORCH_SIZE_FILTER:-}

usage() {
  cat <<EOF
Usage: tbv_pytorch_chunk_report.sh [--size BYTES] ROOT...

Reports PyTorch hoststream chunk-sweep timing, recovery counters, and DV WRITE
timing. ROOT may be a tbv_pytorch_chunk_sweep.sh sweep root or one or more
individual tbv_app_gate.sh log roots.
EOF
}

while (($#)); do
  case "$1" in
    --size) size_filter=$2; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    --) shift; break ;;
    -*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    *) break ;;
  esac
done

if (($# < 1)); then
  usage >&2
  exit 2
fi

chunk_to_bytes() {
  local path=$1
  local base
  local raw
  local unit
  local value

  base=$(basename -- "$path")
  if [[ "$base" =~ [Cc]hunk-?([0-9]+)([kKmMgG]?) ]]; then
    raw=${BASH_REMATCH[1]}
    unit=${BASH_REMATCH[2],,}
    case "$unit" in
      k) value=$((raw * 1024)) ;;
      m) value=$((raw * 1024 * 1024)) ;;
      g) value=$((raw * 1024 * 1024 * 1024)) ;;
      *) value=$raw ;;
    esac
    printf '%s\n' "$value"
  else
    printf 'unknown\n'
  fi
}

safe_name() {
  printf '%s' "$1" | tr -c 'A-Za-z0-9_.-' '_'
}

discover_roots() {
  local root

  for root in "$@"; do
    if [[ -d "$root/pytorch" ]]; then
      printf '%s\n' "$root"
    else
      find "$root" -path '*/pytorch' -type d -printf '%h\n' | sort -V
    fi
  done
}

rank0_logs_for_root() {
  find "$1" -path '*/pytorch/*/rep-*/rank0.*.log' -type f -print0 | sort -z
}

counter_logs_for_root() {
  find "$1" -path '*/pytorch/*/rep-*/counters.log' -type f -print0 | sort -z
}

counter_sums() {
  local root=$1
  shift
  local -a keys=("$@")
  local -a logs=()
  local key_string

  mapfile -d '' -t logs < <(counter_logs_for_root "$root")
  if ((${#logs[@]} == 0)); then
    for _ in "${keys[@]}"; do
      printf '0\n'
    done
    return 0
  fi

  key_string=${keys[*]}
  awk -v keys="$key_string" '
    BEGIN {
      n = split(keys, ordered, " ")
      for (i = 1; i <= n; i++) {
        wanted[ordered[i]] = 1
        total[ordered[i]] = 0
      }
    }
    ($1 in wanted) && $2 == "sum" {
      gsub(/^\+/, "", $3)
      total[$1] += $3
    }
    END {
      for (i = 1; i <= n; i++)
        print total[ordered[i]] + 0
    }
  ' "${logs[@]}"
}

timing_rows() {
  local root=$1
  local -a logs=()

  mapfile -d '' -t logs < <(rank0_logs_for_root "$root")
  if ((${#logs[@]} == 0)); then
    return 0
  fi

  awk -v want_size="$size_filter" '
    match($0, /^([[:alnum:]_]+) bytes=([0-9]+): ([0-9.]+) us\/iter/, m) {
      collective = m[1]
      bytes = m[2]
      if (want_size != "" && bytes != want_size)
        next
      key = collective SUBSEP bytes
      count[key]++
      time = m[3] + 0
      sum_time[key] += time
      if (!(key in min_time) || time < min_time[key])
        min_time[key] = time
      if (time > max_time[key])
        max_time[key] = time
      if (match($0, / gpu=([0-9.]+) us\/iter/, g)) {
        gpu = g[1] + 0
        gpu_count[key]++
        sum_gpu[key] += gpu
      }
      if (match($0, /\(([0-9.]+) Gb\/s logical\/rank\)/, b)) {
        gbps = b[1] + 0
        if (gbps > max_gbps[key])
          max_gbps[key] = gbps
      }
    }
    END {
      for (key in count) {
        split(key, p, SUBSEP)
        gpu_avg = gpu_count[key] ? sprintf("%.1f", sum_gpu[key] / gpu_count[key]) : "NA"
        gbps_max = (key in max_gbps) ? sprintf("%.2f", max_gbps[key]) : "NA"
        printf "%s %s %d %.1f %.1f %.1f %s %s\n",
          p[1], p[2], count[key],
          min_time[key], sum_time[key] / count[key], max_time[key],
          gpu_avg, gbps_max
      }
    }
  ' "${logs[@]}" | sort -V
}

dv_metrics() {
  local root=$1
  local before
  local after

  while IFS= read -r -d '' before; do
    after=${before/.before./.after.}
    [[ -f "$after" ]] || continue
    awk '
      FNR == NR {
        gsub(":", "", $1)
        before[$1] = $2
        next
      }
      {
        gsub(":", "", $1)
        after[$1] = $2
      }
      END {
        for (key in after) {
          if (key !~ /^dv_write_tx_mr_bucket_[0-9]+_count$/)
            continue
          idx = key
          sub(/^dv_write_tx_mr_bucket_/, "", idx)
          sub(/_count$/, "", idx)
          count_key = "dv_write_tx_mr_bucket_" idx "_count"
          ns_key = "dv_write_tx_mr_bucket_" idx "_ns"
          bytes_key = "dv_write_tx_mr_bucket_" idx "_bytes"
          copy_key = "dv_write_copy_mr_bucket_" idx "_ns"
          postcopy_key = "dv_write_postcopy_mr_bucket_" idx "_ns"
          submit_key = "dv_write_submit_mr_bucket_" idx "_ns"
          enqueue_key = "dv_write_enqueue_mr_bucket_" idx "_ns"
          drain_key = "dv_write_drain_mr_bucket_" idx "_ns"
          count = (after[count_key] + 0) - (before[count_key] + 0)
          ns = (after[ns_key] + 0) - (before[ns_key] + 0)
          bytes = (after[bytes_key] + 0) - (before[bytes_key] + 0)
          copy = (after[copy_key] + 0) - (before[copy_key] + 0)
          postcopy = (after[postcopy_key] + 0) - (before[postcopy_key] + 0)
          submit = (after[submit_key] + 0) - (before[submit_key] + 0)
          enqueue = (after[enqueue_key] + 0) - (before[enqueue_key] + 0)
          drain = (after[drain_key] + 0) - (before[drain_key] + 0)
          print count, ns, bytes, copy, postcopy, submit, enqueue, drain
        }
      }
    ' "$before" "$after"
  done < <(find "$root" -path '*/counters/*.before.*.summary' -type f -print0 | sort -z) |
    awk '
      {
        count += $1
        ns += $2
        bytes += $3
        copy += $4
        postcopy += $5
        submit += $6
        enqueue += $7
        drain += $8
      }
      END {
        avg_bytes = count ? bytes / count : 0
        avg_ms = count ? ns / count / 1000000 : 0
        copy_ms = count ? copy / count / 1000000 : 0
        postcopy_ms = count ? postcopy / count / 1000000 : 0
        submit_ms = count ? submit / count / 1000000 : 0
        enqueue_ms = count ? enqueue / count / 1000000 : 0
        drain_ms = count ? drain / count / 1000000 : 0
        total_gbps = ns ? bytes * 8 / ns : 0
        copy_gbps = copy ? bytes * 8 / copy : 0
        postcopy_gbps = postcopy ? bytes * 8 / postcopy : 0
        enqueue_gbps = enqueue ? bytes * 8 / enqueue : 0
        drain_gbps = drain ? bytes * 8 / drain : 0
        printf "%d %d %.0f %.3f %.3f %.3f %.3f %.3f %.3f %.2f %.2f %.2f %.2f %.2f\n",
          count, bytes, avg_bytes, avg_ms, copy_ms, postcopy_ms,
          submit_ms, enqueue_ms, drain_ms,
          total_gbps, copy_gbps, postcopy_gbps, enqueue_gbps, drain_gbps
      }
    '
}

classify_route() {
  local dv_poll=$1
  local dv_writes=$2
  local tx_post=$3
  local tx_comp=$4

  if ((dv_poll > 0 || dv_writes > 0)); then
    printf 'dv\n'
  elif ((tx_post > 0 || tx_comp > 0)); then
    printf 'copied-native\n'
  else
    printf 'no-tbv\n'
  fi
}

print_fields() {
  local -a fields=("$@")

  (IFS=' '; printf '%s\n' "${fields[*]}")
}

COUNTER_KEYS=(
  data_wr_retransmit
  data_wr_rnr_retransmit
  data_rx_ack_match_retried
  data_rx_late_ack
  data_rx_duplicate_ack
  data_rx_write_gap_rnr
  data_tx_ack_rnr
  data_rx_ack_rnr
  dv_hard_error
  dv_poll_wqes
  data_wr_timeout
  data_wr_retry_exhausted
  data_tx_errors
  data_tx_posted
  data_tx_completed
)

HEADER_FIELDS=(
  chunk_bytes size_bytes root reps
  app_avg_us app_min_us app_max_us gpu_avg_us gbps_max
  route dv_poll_wqes
  dv_writes dv_bytes dv_avg_bytes dv_avg_ms
  copy_ms postcopy_ms submit_ms enqueue_ms drain_ms
  total_gbps copy_gbps postcopy_gbps enqueue_gbps drain_gbps
  wr_retx rnr_retx ack_retry late_ack dup_ack write_gap_rnr tx_rnr rx_rnr
  dv_hard wr_to wr_exh tx_err tx_posted tx_completed tx_skew root_name
)

print_fields "${HEADER_FIELDS[@]}"

mapfile -t roots < <(discover_roots "$@")
for root in "${roots[@]}"; do
  [[ -d "$root" ]] || continue
  chunk=$(chunk_to_bytes "$root")
  name=$(safe_name "${root#/mnt/Home/tmp/tbv-app-gate-logs/}")
  dv=$(dv_metrics "$root")
  mapfile -t counters < <(counter_sums "$root" "${COUNTER_KEYS[@]}")
  if ((${#counters[@]} != ${#COUNTER_KEYS[@]})); then
    echo "counter read failed for $root" >&2
    exit 1
  fi
  wr_retx=${counters[0]}
  rnr_retx=${counters[1]}
  ack_retry=${counters[2]}
  late_ack=${counters[3]}
  dup_ack=${counters[4]}
  write_gap_rnr=${counters[5]}
  tx_rnr=${counters[6]}
  rx_rnr=${counters[7]}
  dv_hard=${counters[8]}
  dv_poll=${counters[9]}
  wr_to=${counters[10]}
  wr_exh=${counters[11]}
  tx_err=${counters[12]}
  tx_post=${counters[13]}
  tx_comp=${counters[14]}
  tx_skew=$((tx_post - tx_comp))
  read -r -a dv_fields <<<"$dv"
  route=$(classify_route "$dv_poll" "${dv_fields[0]:-0}" "$tx_post" "$tx_comp")

  while read -r collective size reps min_us avg_us max_us gpu_avg gbps_max; do
    row=(
      "$chunk" "$size" "$root" "$reps" "$avg_us" "$min_us" "$max_us"
      "$gpu_avg" "$gbps_max" "$route" "$dv_poll" "${dv_fields[@]}"
      "$wr_retx" "$rnr_retx" "$ack_retry" "$late_ack" "$dup_ack"
      "$write_gap_rnr" "$tx_rnr" "$rx_rnr"
      "$dv_hard" "$wr_to" "$wr_exh" "$tx_err"
      "$tx_post" "$tx_comp" "$tx_skew" "$name"
    )
    [[ -n "${collective:-}" ]] || continue
    print_fields "${row[@]}"
  done < <(timing_rows "$root")
done
