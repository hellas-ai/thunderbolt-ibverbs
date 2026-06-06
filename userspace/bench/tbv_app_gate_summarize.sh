#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<EOF
Usage: tbv_app_gate_summarize.sh LOG_ROOT

Summarizes RCCL/PyTorch rep logs from tbv_app_gate.sh. The output is
intentionally flat so it can be pasted into a results note or compared across
timeout runs.
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

print_loaded_collective_lib_counts() {
  local -a logs=("$@")

  ((${#logs[@]} > 0)) || return 0
  if grep -h 'loaded_collective_lib=' "${logs[@]}" >/dev/null 2>&1; then
    printf '\nloaded_collective_lib counts:\n'
    grep -h 'loaded_collective_lib=' "${logs[@]}" \
      | sed 's/.*loaded_collective_lib=//' \
      | sort \
      | uniq -c
  fi
}

print_pytorch_timing_aggregates() {
  local -a logs=("$@")

  ((${#logs[@]} > 0)) || return 0
  printf '\npytorch_timing aggregates:\n'
  printf 'mode collective bytes count time_us_min time_us_avg time_us_max gpu_us_min gpu_us_avg gpu_us_max gbps_max\n'
  awk '
    function file_mode(path, parts, n, i) {
      n = split(path, parts, "/")
      for (i = 1; i <= n; i++) {
        if (parts[i] == "pytorch" && i + 1 <= n)
          return parts[i + 1]
      }
      return "unknown"
    }
    FNR == 1 {
      mode = file_mode(FILENAME)
    }
    match($0, /^([[:alnum:]_]+) bytes=([0-9]+): ([0-9.]+) us\/iter/, m) {
      key = mode SUBSEP m[1] SUBSEP m[2]
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
        if (!(key in min_gpu) || gpu < min_gpu[key])
          min_gpu[key] = gpu
        if (gpu > max_gpu[key])
          max_gpu[key] = gpu
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
        gpu_min = gpu_count[key] ? sprintf("%.1f", min_gpu[key]) : "NA"
        gpu_avg = gpu_count[key] ? sprintf("%.1f", sum_gpu[key] / gpu_count[key]) : "NA"
        gpu_max = gpu_count[key] ? sprintf("%.1f", max_gpu[key]) : "NA"
        gbps_max = (key in max_gbps) ? sprintf("%.2f", max_gbps[key]) : "NA"
        printf "%s %s %s %d %.1f %.1f %.1f %s %s %s %s\n",
          p[1], p[2], p[3], count[key],
          min_time[key], sum_time[key] / count[key], max_time[key],
          gpu_min, gpu_avg, gpu_max, gbps_max
      }
    }
  ' "${logs[@]}" | sort -V
}

print_hoststream_phase_aggregates() {
  local -a logs=("$@")

  ((${#logs[@]} > 0)) || return 0
  if ! grep -h 'NCCL WARN RCCL_ROCSHMEM_HOST_STREAM_TIMING' "${logs[@]}" >/dev/null 2>&1; then
    return 0
  fi

  printf '\nhoststream_phase aggregates:\n'
  printf 'app_mode bench_mode msg_size rank_offset sym_id count copyin_avg exchange_avg exchange_p50 exchange_p90 exchange_p95 exchange_p99 exchange_max copyout_avg total_avg total_p90 total_max\n'
  awk '
    function file_mode(path, parts, n, i) {
      n = split(path, parts, "/")
      for (i = 1; i <= n; i++) {
        if (parts[i] == "pytorch" && i + 1 <= n)
          return parts[i + 1]
      }
      return "unknown"
    }
    function metric(name, m) {
      if (match($0, name "=([0-9.]+)", m))
        return m[1] + 0
      return 0
    }
    function percentile(src, len, pct, tmp, i, pos) {
      delete tmp
      for (i = 1; i <= len; i++)
        tmp[i] = src[i]
      asort(tmp)
      pos = int((len * pct) + 0.999999)
      if (pos < 1)
        pos = 1
      if (pos > len)
        pos = len
      return tmp[pos]
    }
    FNR == 1 {
      app_mode = file_mode(FILENAME)
    }
    /NCCL WARN RCCL_ROCSHMEM_HOST_STREAM_TIMING/ {
      bench_mode = metric("mode")
      msg_size = metric("msgSize")
      rank_offset = metric("rankOffset")
      sym_id = metric("symId")
      copy_in = metric("copyInMs")
      exchange = metric("exchangeMs")
      copy_out = metric("copyOutMs")
      total = metric("totalMs")
      key = app_mode SUBSEP bench_mode SUBSEP msg_size SUBSEP rank_offset SUBSEP sym_id
      idx = ++count[key]
      copy_in_v[key, idx] = copy_in
      exchange_v[key, idx] = exchange
      copy_out_v[key, idx] = copy_out
      total_v[key, idx] = total
      copy_in_sum[key] += copy_in
      exchange_sum[key] += exchange
      copy_out_sum[key] += copy_out
      total_sum[key] += total
      if (exchange > exchange_max[key])
        exchange_max[key] = exchange
      if (total > total_max[key])
        total_max[key] = total
    }
    END {
      for (key in count) {
        split(key, p, SUBSEP)
        len = count[key]
        for (i = 1; i <= len; i++) {
          ex[i] = exchange_v[key, i]
          tot[i] = total_v[key, i]
        }
        printf "%s %s %s %s %s %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",
          p[1], p[2], p[3], p[4], p[5], len,
          copy_in_sum[key] / len,
          exchange_sum[key] / len,
          percentile(ex, len, 0.50),
          percentile(ex, len, 0.90),
          percentile(ex, len, 0.95),
          percentile(ex, len, 0.99),
          exchange_max[key],
          copy_out_sum[key] / len,
          total_sum[key] / len,
          percentile(tot, len, 0.90),
          total_max[key]
        delete ex
        delete tot
      }
    }
  ' "${logs[@]}" | sort -V
}

print_hoststream_addr_layouts() {
  local -a logs=("$@")

  ((${#logs[@]} > 0)) || return 0
  if ! grep -h 'NCCL WARN RCCL_ROCSHMEM_HOST_STREAM_ADDR' "${logs[@]}" >/dev/null 2>&1; then
    return 0
  fi

  printf '\nhoststream_addr layouts:\n'
  printf 'app_mode rank bench_mode msg_size rank_offset sym_id fixed_sym_id count source_backing dest_backing num_sym_buf buf_threshold slot_offset source_base source_slot dest_base dest_slot\n'
  awk '
    function file_mode(path, parts, n, i) {
      n = split(path, parts, "/")
      for (i = 1; i <= n; i++) {
        if (parts[i] == "pytorch" && i + 1 <= n)
          return parts[i + 1]
      }
      return "unknown"
    }
    function field(name, m) {
      if (match($0, name "=([^[:space:]]+)", m))
        return m[1]
      return "NA"
    }
    FNR == 1 {
      app_mode = file_mode(FILENAME)
    }
    /NCCL WARN RCCL_ROCSHMEM_HOST_STREAM_ADDR/ {
      rank = field("rank")
      bench_mode = field("mode")
      msg_size = field("msgSize")
      rank_offset = field("rankOffset")
      sym_id = field("symId")
      fixed_sym_id = field("fixedSymId")
      num_sym_buf = field("numSymBuf")
      buf_threshold = field("bufThreshold")
      slot_offset = field("slotOffset")
      source_backing = field("sourceBacking")
      source_base = field("sourceBase")
      source_slot = field("sourceSlot")
      dest_backing = field("destBacking")
      dest_base = field("destBase")
      dest_slot = field("destSlot")
      key = app_mode SUBSEP rank SUBSEP bench_mode SUBSEP msg_size SUBSEP rank_offset SUBSEP sym_id SUBSEP fixed_sym_id \
        SUBSEP source_backing SUBSEP dest_backing SUBSEP num_sym_buf SUBSEP buf_threshold SUBSEP slot_offset \
        SUBSEP source_base SUBSEP source_slot SUBSEP dest_base SUBSEP dest_slot
      count[key]++
    }
    END {
      for (key in count) {
        split(key, p, SUBSEP)
        printf "%s %s %s %s %s %s %s %d %s %s %s %s %s %s %s %s %s\n",
          p[1], p[2], p[3], p[4], p[5], p[6], p[7], count[key],
          p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], p[16]
      }
    }
  ' "${logs[@]}" | sort -V
}

print_usb4_a2a_post_layouts() {
  local -a logs=("$@")

  ((${#logs[@]} > 0)) || return 0
  if ! grep -h 'USB4_GDA_A2A_POST' "${logs[@]}" >/dev/null 2>&1; then
    return 0
  fi

  printf '\nusb4_a2a_post layouts:\n'
  printf 'app_mode my_pe team_my_pe pe_size seq dest_pe qpn bytes count src_heap src_sync dst_heap dst_sync sync_remote_sync src_heap_off src_sync_off dst_heap_off dst_sync_off sync_remote_off src_lkey dst_rkey heap_lkey heap_rkey sync_lkey sync_rkey src dst sync_remote base_delta\n'
  awk '
    function file_mode(path, parts, n, i) {
      n = split(path, parts, "/")
      for (i = 1; i <= n; i++) {
        if (parts[i] == "pytorch" && i + 1 <= n)
          return parts[i + 1]
      }
      return "unknown"
    }
    function field(name, m) {
      if (match($0, name "=([^[:space:]]+)", m))
        return m[1]
      return "NA"
    }
    FNR == 1 {
      app_mode = file_mode(FILENAME)
    }
    /USB4_GDA_A2A_POST/ {
      my_pe = field("my_pe")
      team_my_pe = field("team_my_pe")
      pe_size = field("pe_size")
      seq = field("seq")
      dest_pe = field("dest_pe")
      qpn = field("qpn")
      bytes = field("bytes")
      src_heap = field("src_heap")
      src_sync = field("src_sync")
      dst_heap = field("dst_heap")
      dst_sync = field("dst_sync")
      sync_remote_sync = field("sync_remote_sync")
      src_heap_off = field("src_heap_off")
      src_sync_off = field("src_sync_off")
      dst_heap_off = field("dst_heap_off")
      dst_sync_off = field("dst_sync_off")
      sync_remote_off = field("sync_remote_off")
      src_lkey = field("src_lkey")
      dst_rkey = field("dst_rkey")
      heap_lkey = field("heap_lkey")
      heap_rkey = field("heap_rkey")
      sync_lkey = field("sync_lkey")
      sync_rkey = field("sync_rkey")
      src = field("src")
      dst = field("dst")
      sync_remote = field("sync_remote")
      base_delta = field("base_delta")
      key = app_mode SUBSEP my_pe SUBSEP team_my_pe SUBSEP pe_size SUBSEP seq \
        SUBSEP dest_pe SUBSEP qpn SUBSEP bytes SUBSEP src_heap SUBSEP src_sync \
        SUBSEP dst_heap SUBSEP dst_sync SUBSEP sync_remote_sync SUBSEP src_heap_off \
        SUBSEP src_sync_off SUBSEP dst_heap_off SUBSEP dst_sync_off \
        SUBSEP sync_remote_off SUBSEP src_lkey SUBSEP dst_rkey SUBSEP heap_lkey \
        SUBSEP heap_rkey SUBSEP sync_lkey SUBSEP sync_rkey SUBSEP src SUBSEP dst \
        SUBSEP sync_remote SUBSEP base_delta
      count[key]++
    }
    END {
      for (key in count) {
        split(key, p, SUBSEP)
        printf "%s %s %s %s %s %s %s %s %d %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s\n",
          p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], count[key],
          p[9], p[10], p[11], p[12], p[13], p[14], p[15], p[16], p[17],
          p[18], p[19], p[20], p[21], p[22], p[23], p[24], p[25], p[26],
          p[27], p[28]
      }
    }
  ' "${logs[@]}" | sort -V
}

print_usb4_a2a_timing_aggregates() {
  local -a logs=("$@")

  ((${#logs[@]} > 0)) || return 0
  if ! grep -h 'USB4_GDA_A2A_TIMING' "${logs[@]}" >/dev/null 2>&1; then
    return 0
  fi

  printf '\nusb4_a2a_timing aggregates:\n'
  printf 'app_mode my_pe team_my_pe pe_size dest_pe bytes src_heap_off dst_heap_off count post_avg post_p50 post_p90 post_max quiet_avg quiet_p50 quiet_p90 quiet_max total_avg total_p50 total_p90 total_max\n'
  awk '
    function file_mode(path, parts, n, i) {
      n = split(path, parts, "/")
      for (i = 1; i <= n; i++) {
        if (parts[i] == "pytorch" && i + 1 <= n)
          return parts[i + 1]
      }
      return "unknown"
    }
    function field(name, m) {
      if (match($0, name "=([^[:space:]]+)", m))
        return m[1]
      return "NA"
    }
    function percentile(src, len, pct, tmp, i, pos) {
      delete tmp
      for (i = 1; i <= len; i++)
        tmp[i] = src[i]
      asort(tmp)
      pos = int((len * pct) + 0.999999)
      if (pos < 1)
        pos = 1
      if (pos > len)
        pos = len
      return tmp[pos]
    }
    FNR == 1 {
      app_mode = file_mode(FILENAME)
    }
    /USB4_GDA_A2A_TIMING/ {
      my_pe = field("my_pe")
      team_my_pe = field("team_my_pe")
      pe_size = field("pe_size")
      dest_pe = field("dest_pe")
      bytes = field("bytes")
      src_heap_off = field("src_heap_off")
      dst_heap_off = field("dst_heap_off")
      post_ticks = field("post_ticks") + 0
      quiet_ticks = field("quiet_ticks") + 0
      total_ticks = field("total_ticks") + 0
      key = app_mode SUBSEP my_pe SUBSEP team_my_pe SUBSEP pe_size \
        SUBSEP dest_pe SUBSEP bytes SUBSEP src_heap_off SUBSEP dst_heap_off
      idx = ++count[key]
      post_v[key, idx] = post_ticks
      quiet_v[key, idx] = quiet_ticks
      total_v[key, idx] = total_ticks
      post_sum[key] += post_ticks
      quiet_sum[key] += quiet_ticks
      total_sum[key] += total_ticks
      if (post_ticks > post_max[key])
        post_max[key] = post_ticks
      if (quiet_ticks > quiet_max[key])
        quiet_max[key] = quiet_ticks
      if (total_ticks > total_max[key])
        total_max[key] = total_ticks
    }
    END {
      for (key in count) {
        split(key, p, SUBSEP)
        len = count[key]
        for (i = 1; i <= len; i++) {
          post[i] = post_v[key, i]
          quiet[i] = quiet_v[key, i]
          total[i] = total_v[key, i]
        }
        printf "%s %s %s %s %s %s %s %s %d %.1f %.1f %.1f %d %.1f %.1f %.1f %d %.1f %.1f %.1f %d\n",
          p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], len,
          post_sum[key] / len,
          percentile(post, len, 0.50),
          percentile(post, len, 0.90),
          post_max[key],
          quiet_sum[key] / len,
          percentile(quiet, len, 0.50),
          percentile(quiet, len, 0.90),
          quiet_max[key],
          total_sum[key] / len,
          percentile(total, len, 0.50),
          percentile(total, len, 0.90),
          total_max[key]
        delete post
        delete quiet
        delete total
      }
    }
  ' "${logs[@]}" | sort -V
}

print_rccl_timing_aggregates() {
  local -a logs=("$@")

  ((${#logs[@]} > 0)) || return 0
  printf '\nrccl_timing aggregates:\n'
  printf 'collective mode bytes count time_us_min time_us_avg time_us_max algbw_max busbw_max wrong\n'
  awk '
    function file_field(path, want, parts, n, i) {
      n = split(path, parts, "/")
      for (i = 1; i <= n; i++) {
        if (parts[i] == "rccl" && i + 2 <= n) {
          if (want == "collective")
            return parts[i + 1]
          if (want == "mode")
            return parts[i + 2]
        }
      }
      return "unknown"
    }
    FNR == 1 {
      collective = file_field(FILENAME, "collective")
      mode = file_field(FILENAME, "mode")
    }
    NF >= 13 && $1 ~ /^[0-9]+$/ {
      key = collective SUBSEP mode SUBSEP $1
      count[key]++
      time = $6 + 0
      algbw = $7 + 0
      busbw = $8 + 0
      wrong[key] += $9
      sum_time[key] += time
      if (!(key in min_time) || time < min_time[key])
        min_time[key] = time
      if (time > max_time[key])
        max_time[key] = time
      if (algbw > max_algbw[key])
        max_algbw[key] = algbw
      if (busbw > max_busbw[key])
        max_busbw[key] = busbw
    }
    END {
      for (key in count) {
        split(key, p, SUBSEP)
        printf "%s %s %s %d %.2f %.2f %.2f %.2f %.2f %d\n",
          p[1], p[2], p[3], count[key],
          min_time[key], sum_time[key] / count[key], max_time[key],
          max_algbw[key], max_busbw[key], wrong[key]
      }
    }
  ' "${logs[@]}" | sort -V
}

print_counter_aggregates() {
  local -a logs=("$@")

  ((${#logs[@]} > 0)) || return 0
  printf '\ncounter aggregates:\n'
  printf 'suite collective mode cases wr_retx rnr_retx ack_retry ack64 late_ack dup_ack write_gap_rnr tx_rnr rx_rnr reord_to active_to rnr_exh dv_hard wr_to wr_exh tx_err tx_post tx_comp\n'
  awk '
    function file_group(path, parts, n, i) {
      n = split(path, parts, "/")
      for (i = 1; i <= n; i++) {
        if (parts[i] == "rccl" && i + 2 <= n)
          return "rccl" SUBSEP parts[i + 1] SUBSEP parts[i + 2]
        if (parts[i] == "pytorch" && i + 1 <= n)
          return "pytorch" SUBSEP "-" SUBSEP parts[i + 1]
      }
      return "unknown" SUBSEP "-" SUBSEP "-"
    }
    function alias(key) {
      if (key == "data_wr_retransmit") return "wr_retx"
      if (key == "data_wr_rnr_retransmit") return "rnr_retx"
      if (key == "data_rx_ack_match_retried") return "ack_retry"
      if (key == "data_rx_ack_match_over_64ms") return "ack64"
      if (key == "data_rx_late_ack") return "late_ack"
      if (key == "data_rx_duplicate_ack") return "dup_ack"
      if (key == "data_rx_write_gap_rnr") return "write_gap_rnr"
      if (key == "data_tx_ack_rnr") return "tx_rnr"
      if (key == "data_rx_ack_rnr") return "rx_rnr"
      if (key == "data_rx_reorder_timeout") return "reord_to"
      if (key == "data_rx_active_timeout") return "active_to"
      if (key == "data_wr_rnr_retry_exhausted") return "rnr_exh"
      if (key == "dv_hard_error") return "dv_hard"
      if (key == "data_wr_timeout") return "wr_to"
      if (key == "data_wr_retry_exhausted") return "wr_exh"
      if (key == "data_tx_errors") return "tx_err"
      if (key == "data_tx_posted") return "tx_post"
      if (key == "data_tx_completed") return "tx_comp"
      return ""
    }
    FNR == 1 {
      group = file_group(FILENAME)
      cases[group]++
    }
    $2 == "sum" {
      short = alias($1)
      if (short != "")
        values[group, short] += $3
    }
    END {
      for (group in cases) {
        split(group, p, SUBSEP)
        printf "%s %s %s %d", p[1], p[2], p[3], cases[group]
        printf " %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
          values[group, "wr_retx"], values[group, "rnr_retx"],
          values[group, "ack_retry"], values[group, "ack64"],
          values[group, "late_ack"], values[group, "dup_ack"],
          values[group, "write_gap_rnr"], values[group, "tx_rnr"],
          values[group, "rx_rnr"], values[group, "reord_to"],
          values[group, "active_to"], values[group, "rnr_exh"],
          values[group, "dv_hard"], values[group, "wr_to"],
          values[group, "wr_exh"], values[group, "tx_err"],
          values[group, "tx_post"], values[group, "tx_comp"]
      }
    }
  ' "${logs[@]}" | sort -V
}

print_dv_write_tx_mr_bucket_aggregates() {
  local root=$1
  local -a before_files=()
  local before after rel suite collective mode

  mapfile -d '' -t before_files < <(
    find "$root" -path '*/counters/*.before.*.summary' -type f -print0 | sort -z
  )
  ((${#before_files[@]} > 0)) || return 0

  {
    for before in "${before_files[@]}"; do
      after=${before/.before./.after.}
      [[ -f "$after" ]] || continue

      rel=${before#"$root"/}
      suite=unknown
      collective=-
      mode=-
      IFS=/ read -r -a parts <<<"$rel"
      if [[ ${parts[0]:-} == "pytorch" && -n ${parts[1]:-} ]]; then
        suite=pytorch
        mode=${parts[1]}
      elif [[ ${parts[0]:-} == "rccl" && -n ${parts[1]:-} && -n ${parts[2]:-} ]]; then
        suite=rccl
        collective=${parts[1]}
        mode=${parts[2]}
      fi

      awk -v suite="$suite" -v collective="$collective" -v mode="$mode" '
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
          for (i = 0; i < 8; i++) {
            count_key = "dv_write_tx_mr_bucket_" i "_count"
            ns_key = "dv_write_tx_mr_bucket_" i "_ns"
            bytes_key = "dv_write_tx_mr_bucket_" i "_bytes"
            copy_ns_key = "dv_write_copy_mr_bucket_" i "_ns"
            postcopy_ns_key = "dv_write_postcopy_mr_bucket_" i "_ns"
            count = (after[count_key] + 0) - (before[count_key] + 0)
            ns = (after[ns_key] + 0) - (before[ns_key] + 0)
            bytes = (after[bytes_key] + 0) - (before[bytes_key] + 0)
            copy_ns = (after[copy_ns_key] + 0) - (before[copy_ns_key] + 0)
            postcopy_ns = (after[postcopy_ns_key] + 0) - (before[postcopy_ns_key] + 0)
            if (count || ns || bytes || copy_ns || postcopy_ns)
              print suite, collective, mode, i, count, ns, bytes, copy_ns, postcopy_ns
          }
        }
      ' "$before" "$after"
    done
  } | awk '
    {
      key = $1 SUBSEP $2 SUBSEP $3 SUBSEP $4
      count[key] += $5
      ns[key] += $6
      bytes[key] += $7
      copy_ns[key] += $8
      postcopy_ns[key] += $9
    }
    END {
      if (!length(count))
        exit
      for (key in count) {
        split(key, p, SUBSEP)
        avg_ms = count[key] ? ns[key] / count[key] / 1000000 : 0
        copy_avg_ms = count[key] ? copy_ns[key] / count[key] / 1000000 : 0
        postcopy_avg_ms = count[key] ? postcopy_ns[key] / count[key] / 1000000 : 0
        printf "%s %s %s %s %d %d %.3f %.3f %.3f\n",
          p[1], p[2], p[3], p[4], count[key], bytes[key], avg_ms,
          copy_avg_ms, postcopy_avg_ms
      }
    }
  ' | sort -V | awk '
    BEGIN {
      printed = 0
    }
    {
      if (!printed) {
        printf "\ndv_write_tx_mr_bucket aggregates:\n"
        printf "suite collective mode bucket count bytes avg_ms copy_avg_ms postcopy_avg_ms\n"
        printed = 1
      }
      print
    }
  '
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
    printf 'mode rep t1_us t2_us wr_retx rnr_retx ack_retry ack64 late_ack dup_ack ack_miss ack_probe ack_probe_fb tx_ack_req tx_ack_req_err rx_ack_req rx_ack_req_reack rx_ack_req_miss rx_ack_req_miss_past rx_ack_req_miss_cur rx_ack_req_miss_cur_active rx_ack_req_miss_cur_reorder rx_ack_req_miss_cur_idle rx_ack_req_miss_fut reord_to reord_retry reord_drop reord_dup_refresh active_to active_retry active_dup_refresh active_merge active_merge_bytes active_merge_complete write_gap_rnr tx_rnr rx_rnr rnr_exh rnr_exh_cap rnr_exh_close rnr_exh_qperr rnr_wait_notret rnr_wait_retrying rnr_wait_txpend rnr_wait_exh rnr_wait_close rnr_wait_qperr rnr_wait_unknown dv_hard wr_to wr_exh tx_err tx_post tx_comp\n'
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
    data_rx_write_gap_rnr \
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

mapfile -d '' -t pytorch_rank0_logs < <(find "$root" -path '*/pytorch/*/rank0.*.log' -type f -print0 | sort -z)
mapfile -d '' -t pytorch_rank_logs < <(find "$root" -path '*/pytorch/*/rank*.log' -type f -print0 | sort -z)
mapfile -d '' -t rccl_logs < <(find "$root" -path '*/rccl/*/*.log' -type f -print0 | sort -z)
mapfile -d '' -t counter_logs < <(
  {
    find "$root" -path '*/pytorch/*/counters.log' -type f -print0
    find "$root" -path '*/rccl/*/*.log' -type f -print0
  } | sort -z
)

print_pytorch_timing_aggregates "${pytorch_rank0_logs[@]}"
print_hoststream_phase_aggregates "${pytorch_rank_logs[@]}"
print_hoststream_addr_layouts "${pytorch_rank_logs[@]}"
print_usb4_a2a_post_layouts "${pytorch_rank_logs[@]}"
print_usb4_a2a_timing_aggregates "${pytorch_rank_logs[@]}"
print_loaded_collective_lib_counts "${pytorch_rank_logs[@]}"
print_rccl_timing_aggregates "${rccl_logs[@]}"
print_counter_aggregates "${counter_logs[@]}"
print_dv_write_tx_mr_bucket_aggregates "$root"

if [[ $printed -eq 0 && ${#pytorch_rank0_logs[@]} -eq 0 && ${#rccl_logs[@]} -eq 0 ]]; then
  echo "no tbv_app_gate logs found under: $root" >&2
  exit 1
fi
