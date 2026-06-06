#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

hosts=${TBV_APP_HOSTS:-strix-1,strix-2}
counter_hosts=${TBV_COUNTER_HOSTS:-$hosts}
iface=${TBV_APP_IFACE:-eno1}
log_root=${TBV_APP_LOG_ROOT:-/tmp/tbv-app-gate/$(date +%Y%m%d-%H%M%S)}
ssh_cmd=${TBV_SSH:-ssh}
timeout_s=${TBV_APP_TIMEOUT:-300}
dv_check=${TBV_DV_CHECK:-auto}

rccl_tests_dir=${RCCL_TESTS_DIR:-}
rccl_install=${RCCL_INSTALL_DIR:-}
rocshmem_install=${ROCSHMEM_INSTALL_DIR:-}
rocm_path=${ROCM_PATH:-}
mpi_home=${MPI_HOME:-}
rdma_core_lib=${TBV_RDMA_LIB:-${USB4_RDMA_LIB:-}}
numactl_lib=${NUMACTL_LIB:-}

sizes=${TBV_RCCL_SIZES:-262144,524288,1048576}
iters=${TBV_RCCL_ITERS:-5}
warmup=${TBV_RCCL_WARMUP:-2}
reps=${TBV_REPS:-3}
collectives=${TBV_RCCL_COLLECTIVES:-alltoall,alltoallv}
modes=${TBV_MODES:-fallback,hoststream,device}

run_rccl=${TBV_RUN_RCCL:-1}
run_pytorch=${TBV_RUN_PYTORCH:-0}
pytorch_wrapper=${VLLM_USB4_ENV:-${TBV_PYTORCH_WRAPPER:-}}
pytorch_sizes=${TBV_TORCH_SIZES:-65536,262144}
pytorch_iters=${TBV_TORCH_ITERS:-2}
pytorch_collectives=${TBV_TORCH_COLLECTIVES:-all_to_all}
pytorch_validate=${TBV_TORCH_VALIDATE:-1}
pytorch_master_addr=${TBV_TORCH_MASTER_ADDR:-192.168.23.136}
pytorch_master_port=${TBV_TORCH_MASTER_PORT:-29617}
pytorch_timeout=${TBV_TORCH_TIMEOUT:-240}
pytorch_ld_preload=${TBV_TORCH_LD_PRELOAD:-off}
pytorch_rccl_lib=${TBV_TORCH_RCCL_LIB:-auto}
pytorch_remote_script=${TBV_TORCH_REMOTE_SCRIPT:-/tmp/tbv_pytorch_smoke_${USER:-tbv}_$$.py}
expected_rccl_lib=${RCCL_EXPECTED_LIB:-}

counter_summary=${TBV_DEBUGFS_SUMMARY:-/sys/kernel/debug/thunderbolt_ibverbs/summary}
counter_keys=${TBV_COUNTER_KEYS:-"dv_poll_wqes dv_admission_attempts dv_backpressure_retry dv_fence_retry dv_hard_error data_wr_copy_error data_wr_retransmit data_wr_timeout data_wr_retry_exhausted data_wr_retransmit_closing_qp data_wr_retransmit_no_live_path data_wr_retransmit_teardown_path data_tx_errors data_rx_canceled data_rx_no_qp data_rx_no_qp_apple data_rx_no_qp_mad data_rx_no_qp_native_ackable data_rx_no_qp_native_non_ack data_rx_no_qp_send_ack data_rx_no_qp_send_ack_ok data_rx_no_qp_send_ack_rnr data_rx_no_qp_send_ack_error data_rx_no_qp_send_ack_bad_status data_rx_no_qp_opcode_1 data_rx_no_qp_opcode_2 data_rx_no_qp_opcode_3 data_rx_no_qp_opcode_4 data_rx_no_qp_opcode_5 data_rx_no_qp_opcode_6 data_rx_no_qp_opcode_7 data_rx_no_qp_opcode_8 data_rx_no_qp_opcode_9 data_rx_no_qp_opcode_10 data_rx_no_qp_opcode_11 data_rx_no_qp_opcode_12 data_rx_no_qp_opcode_13 data_rx_no_qp_reack data_rx_no_qp_error_ack data_rx_duplicate_ack data_qp_tombstone_evicted data_tx_posted data_tx_completed"}
hard_error_keys=${TBV_HARD_ERROR_KEYS:-"dv_hard_error data_wr_copy_error data_wr_timeout data_wr_retry_exhausted data_wr_retransmit_closing_qp data_wr_retransmit_no_live_path data_wr_retransmit_teardown_path data_tx_errors data_rx_canceled data_rx_no_qp data_rx_no_qp_reack data_rx_no_qp_error_ack data_qp_tombstone_evicted"}
allow_late_send_ack_no_qp=${TBV_ALLOW_LATE_SEND_ACK_NO_QP:-0}

usage() {
  cat <<EOF
Usage: tbv_app_gate.sh [options]

Runs RCCL/PyTorch USB4 GDA app gates with thunderbolt-ibverbs counter deltas.

Options:
  --hosts H1,H2             Default: $hosts
  --counter-hosts H1,H2     Default: hosts
  --iface IFACE             Default: $iface
  --log-root DIR            Default: $log_root
  --timeout SECONDS         RCCL test timeout. Default: $timeout_s
  --dv-check MODE           auto, require, forbid, off. Default: $dv_check
  --rccl-tests-dir DIR      Directory containing alltoall_perf/alltoallv_perf
  --rccl-install DIR        RCCL install prefix
  --rocshmem-install DIR    rocSHMEM install prefix
  --rocm-path DIR           ROCm/TheRock SDK prefix
  --mpi-home DIR            OpenMPI prefix
  --rdma-lib DIR            rdma-core lib directory
  --numactl-lib DIR         numactl lib directory
  --sizes CSV               RCCL sizes. Default: $sizes
  --iters N                 RCCL iterations. Default: $iters
  --warmup N                RCCL warmup iterations. Default: $warmup
  --reps N                  Repetitions per mode/collective. Default: $reps
  --collectives CSV         alltoall,alltoallv. Default: $collectives
  --modes CSV               fallback,hoststream,device. Default: $modes
  --skip-rccl               Do not run rccl-tests gates
  --pytorch                 Run PyTorch distributed smoke
  --pytorch-wrapper DIR     vLLM/PyTorch wrapper prefix
  --pytorch-sizes CSV       Default: $pytorch_sizes
  --pytorch-iters N         Default: $pytorch_iters
  --torch-validate 0|1      Default: $pytorch_validate
  --pytorch-timeout SECONDS Default: $pytorch_timeout
  --torch-ld-preload PATH   PyTorch LD_PRELOAD value, auto, or off. Default: $pytorch_ld_preload
  --torch-rccl-lib PATH      PyTorch rocm_sdk RCCL preload override, auto, or off. Default: $pytorch_rccl_lib
  --pytorch-remote-script P Remote script path. Default: $pytorch_remote_script
  --torch-collectives CSV   Default: $pytorch_collectives
  --master-addr ADDR        Default: $pytorch_master_addr
  --master-port PORT        Default: $pytorch_master_port
  --expected-rccl-lib PATH  Require this RCCL path to appear in PyTorch logs
  --allow-late-send-ack-no-qp
                            Do not fail when all data_rx_no_qp frames are OK SEND_ACKs
EOF
}

while (($#)); do
  case "$1" in
    --hosts) hosts=$2; shift 2 ;;
    --counter-hosts) counter_hosts=$2; shift 2 ;;
    --iface) iface=$2; shift 2 ;;
    --log-root) log_root=$2; shift 2 ;;
    --timeout) timeout_s=$2; shift 2 ;;
    --dv-check) dv_check=$2; shift 2 ;;
    --rccl-tests-dir) rccl_tests_dir=$2; shift 2 ;;
    --rccl-install) rccl_install=$2; shift 2 ;;
    --rocshmem-install) rocshmem_install=$2; shift 2 ;;
    --rocm-path) rocm_path=$2; shift 2 ;;
    --mpi-home) mpi_home=$2; shift 2 ;;
    --rdma-lib) rdma_core_lib=$2; shift 2 ;;
    --numactl-lib) numactl_lib=$2; shift 2 ;;
    --sizes) sizes=$2; shift 2 ;;
    --iters) iters=$2; shift 2 ;;
    --warmup) warmup=$2; shift 2 ;;
    --reps) reps=$2; shift 2 ;;
    --collectives) collectives=$2; shift 2 ;;
    --modes) modes=$2; shift 2 ;;
    --skip-rccl) run_rccl=0; shift ;;
    --pytorch) run_pytorch=1; shift ;;
    --pytorch-wrapper) pytorch_wrapper=$2; shift 2 ;;
    --pytorch-sizes) pytorch_sizes=$2; shift 2 ;;
    --pytorch-iters) pytorch_iters=$2; shift 2 ;;
    --torch-validate) pytorch_validate=$2; shift 2 ;;
    --pytorch-timeout) pytorch_timeout=$2; shift 2 ;;
    --torch-ld-preload) pytorch_ld_preload=$2; shift 2 ;;
    --torch-rccl-lib) pytorch_rccl_lib=$2; shift 2 ;;
    --pytorch-remote-script) pytorch_remote_script=$2; shift 2 ;;
    --torch-collectives) pytorch_collectives=$2; shift 2 ;;
    --master-addr) pytorch_master_addr=$2; shift 2 ;;
    --master-port) pytorch_master_port=$2; shift 2 ;;
    --expected-rccl-lib) expected_rccl_lib=$2; shift 2 ;;
    --allow-late-send-ack-no-qp) allow_late_send_ack_no_qp=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

host_list() {
  local raw=${1//,/ }
  local host
  for host in $raw; do
    [[ -n "$host" ]] && printf '%s\n' "$host"
  done
}

torch_collectives_include_all_to_all() {
  local raw=$1
  local item

  raw=${raw//,/ }
  for item in $raw; do
    case "$item" in
      all_to_all|alltoall|all_to_all_single) return 0 ;;
    esac
  done
  return 1
}

safe_name() {
  printf '%s' "$1" | tr -c 'A-Za-z0-9_.-' '_'
}

counter_file() {
  printf '%s/%s.%s.summary' "$3" "$(safe_name "$1")" "$(safe_name "$2")"
}

run_remote() {
  local host=$1
  shift
  "$ssh_cmd" "$host" "$@"
}

capture_counters() {
  local label=$1
  local dir=$2
  local target_hosts=$3
  local quoted_summary
  local host
  local out
  local err
  local cmd

  mkdir -p "$dir"
  printf -v quoted_summary '%q' "$counter_summary"
  cmd="if command -v sudo >/dev/null 2>&1; then sudo -n cat $quoted_summary 2>/dev/null || cat $quoted_summary; else cat $quoted_summary; fi"

  for host in $(host_list "$target_hosts"); do
    out=$(counter_file "$label" "$host" "$dir")
    err="$out.err"
    if run_remote "$host" "$cmd" >"$out.tmp" 2>"$err"; then
      mv "$out.tmp" "$out"
      rm -f "$err"
    else
      mv "$out.tmp" "$out" 2>/dev/null || :
      echo "WARN: could not capture counters from $host; see $err" >&2
    fi
  done
}

counter_value() {
  local file=$1
  local key=$2

  awk -F':[[:space:]]*' -v key="$key" '
    $1 == key {
      value = $2
      sub(/[[:space:]].*/, "", value)
      print value ~ /^-?[0-9]+$/ ? value : 0
      found = 1
      exit
    }
    END { if (!found) print 0 }
  ' "$file" 2>/dev/null || printf '0\n'
}

counter_delta_one() {
  local before_label=$1
  local after_label=$2
  local dir=$3
  local host=$4
  local key=$5
  local before
  local after
  local before_value
  local after_value

  before=$(counter_file "$before_label" "$host" "$dir")
  after=$(counter_file "$after_label" "$host" "$dir")
  before_value=$(counter_value "$before" "$key")
  after_value=$(counter_value "$after" "$key")
  printf '%d\n' "$((after_value - before_value))"
}

counter_delta_sum() {
  local before_label=$1
  local after_label=$2
  local dir=$3
  local target_hosts=$4
  local key=$5
  local host
  local total=0

  for host in $(host_list "$target_hosts"); do
    total=$((total + $(counter_delta_one "$before_label" "$after_label" "$dir" "$host" "$key")))
  done
  printf '%d\n' "$total"
}

print_counter_deltas() {
  local before_label=$1
  local after_label=$2
  local dir=$3
  local target_hosts=$4
  local key
  local host
  local delta
  local total

  printf 'TBV counter deltas (%s -> %s):\n' "$before_label" "$after_label"
  for key in $counter_keys; do
    total=0
    for host in $(host_list "$target_hosts"); do
      delta=$(counter_delta_one "$before_label" "$after_label" "$dir" "$host" "$key")
      total=$((total + delta))
      printf '  %-34s %-16s %+d\n' "$key" "$host" "$delta"
    done
    printf '  %-34s %-16s %+d\n' "$key" "sum" "$total"
  done
}

assert_clean_counters() {
  local before_label=$1
  local after_label=$2
  local dir=$3
  local target_hosts=$4
  local key
  local delta
  local no_qp
  local send_ack
  local send_ack_ok
  local failed=0

  for key in $hard_error_keys; do
    delta=$(counter_delta_sum "$before_label" "$after_label" "$dir" "$target_hosts" "$key")
    if [[ "$key" == data_rx_no_qp && "$allow_late_send_ack_no_qp" == 1 && "$delta" -gt 0 ]]; then
      no_qp=$delta
      send_ack=$(counter_delta_sum "$before_label" "$after_label" "$dir" "$target_hosts" data_rx_no_qp_send_ack)
      send_ack_ok=$(counter_delta_sum "$before_label" "$after_label" "$dir" "$target_hosts" data_rx_no_qp_send_ack_ok)
      if ((no_qp == send_ack && send_ack == send_ack_ok)); then
        echo "INFO: allowing $no_qp late OK SEND_ACK no-QP frames" >&2
        continue
      fi
    fi
    if ((delta != 0)); then
      echo "ERROR: expected $key delta == 0, got $delta" >&2
      failed=1
    fi
  done
  return "$failed"
}

assert_dv_delta() {
  local before_label=$1
  local after_label=$2
  local dir=$3
  local target_hosts=$4
  local expected=$5
  local delta

  if [[ "$expected" == skip ]]; then
    return 0
  fi
  delta=$(counter_delta_sum "$before_label" "$after_label" "$dir" "$target_hosts" dv_poll_wqes)
  if [[ "$expected" == 1 && "$delta" -lt 1 ]]; then
    echo "ERROR: expected dv_poll_wqes delta >= 1, got $delta" >&2
    return 1
  fi
  if [[ "$expected" == 0 && "$delta" -ne 0 ]]; then
    echo "ERROR: expected dv_poll_wqes delta == 0, got $delta" >&2
    return 1
  fi
}

resolve_dv_expectation() {
  local mode_default=$1

  case "$dv_check" in
    auto) printf '%s\n' "$mode_default" ;;
    require) printf '1\n' ;;
    forbid) printf '0\n' ;;
    off|skip|none) printf 'skip\n' ;;
    *) echo "ERROR: unknown --dv-check mode: $dv_check" >&2; exit 2 ;;
  esac
}

split_csv() {
  printf '%s\n' "${1//,/ }"
}

require_dir() {
  local label=$1
  local path=$2
  if [[ -z "$path" || ! -d "$path" ]]; then
    echo "ERROR: $label is not set or not a directory: ${path:-<unset>}" >&2
    exit 2
  fi
}

prepend_path() {
  local var=$1
  local path=$2
  local current=${!var:-}
  [[ -n "$path" && -d "$path" ]] || return 0
  case ":$current:" in
    *":$path:"*) ;;
    *) printf -v "$var" '%s%s%s' "$path" "${current:+:}" "$current"; export "$var" ;;
  esac
}

configure_mode() {
  local mode=$1
  case "$mode" in
    fallback)
      export RCCL_ROCSHMEM_ENABLE=0
      export RCCL_ROCSHMEM_FORCE_ENABLE=0
      export RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=0
      export RCCL_ROCSHMEM_SOURCE_HEAP=0
      export RCCL_ROCSHMEM_DEST_HEAP=0
      TBV_EXPECT_DV=0
      ;;
    hoststream)
      export RCCL_ROCSHMEM_ENABLE=1
      export RCCL_ROCSHMEM_FORCE_ENABLE=1
      export RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=1
      export RCCL_ROCSHMEM_SOURCE_HEAP=1
      export RCCL_ROCSHMEM_DEST_HEAP=1
      TBV_EXPECT_DV=1
      ;;
    device)
      export RCCL_ROCSHMEM_ENABLE=1
      export RCCL_ROCSHMEM_FORCE_ENABLE=1
      export RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=0
      export RCCL_ROCSHMEM_SOURCE_HEAP=1
      export RCCL_ROCSHMEM_DEST_HEAP=1
      TBV_EXPECT_DV=1
      ;;
    *) echo "ERROR: unknown mode: $mode" >&2; exit 2 ;;
  esac
  export RCCL_ROCSHMEM_THRESHOLD=${RCCL_ROCSHMEM_THRESHOLD:-1048576}
  export RCCL_ROCSHMEM_GDA_BENCH_MODE=${RCCL_ROCSHMEM_GDA_BENCH_MODE:-0}
  export RCCL_ROCSHMEM_HOST_STREAM_TIMING=${RCCL_ROCSHMEM_HOST_STREAM_TIMING:-0}
  export ROCSHMEM_GDA_QP_TIMEOUT=${ROCSHMEM_GDA_QP_TIMEOUT:-14}
  export ROCSHMEM_GDA_QP_RETRY_CNT=${ROCSHMEM_GDA_QP_RETRY_CNT:-7}
  export ROCSHMEM_GDA_QP_RNR_RETRY=${ROCSHMEM_GDA_QP_RNR_RETRY:-7}
}

assert_pytorch_validation_mode() {
  local mode=$1

  [[ "$pytorch_validate" == 1 ]] || return 0
  [[ "$mode" == hoststream ]] || return 0
  torch_collectives_include_all_to_all "$pytorch_collectives" || return 0
  [[ "${RCCL_ROCSHMEM_GDA_BENCH_MODE:-0}" == 0 ]] && return 0

  cat >&2 <<EOF
ERROR: PyTorch all_to_all validation requires RCCL_ROCSHMEM_GDA_BENCH_MODE=0.
Modes 1-5 are phase probes; they intentionally skip parts of the all_to_all
pipeline and can return zero or partial payloads. Re-run with
--torch-validate 0 for phase timing, or use bench mode 0 for correctness.
EOF
  exit 2
}

setup_app_env() {
  if [[ "$run_rccl" == 1 ]]; then
    require_dir "RCCL_INSTALL_DIR" "$rccl_install"
    require_dir "ROCSHMEM_INSTALL_DIR" "$rocshmem_install"
    require_dir "ROCM_PATH" "$rocm_path"
    require_dir "MPI_HOME" "$mpi_home"
  fi
  if [[ "$run_pytorch" == 1 ]]; then
    require_dir "TBV_PYTORCH_WRAPPER/VLLM_USB4_ENV" "$pytorch_wrapper"
    require_dir "ROCM_PATH" "$rocm_path"
  fi
  if [[ -n "$rdma_core_lib" && ! -d "$rdma_core_lib" ]]; then
    echo "WARN: rdma lib not found: $rdma_core_lib" >&2
  fi

  export ROCM_PATH="$rocm_path"
  export ROCM_HOME="$rocm_path"
  export HIP_PATH="$rocm_path"
  export HIP_PLATFORM=amd
  export RCCL_ROCR_PATH=${RCCL_ROCR_PATH:-$rocm_path/lib/}
  prepend_path PATH "$mpi_home/bin"
  prepend_path PATH "$rocm_path/bin"
  prepend_path LD_LIBRARY_PATH "$rccl_install/lib"
  prepend_path LD_LIBRARY_PATH "$rocshmem_install/lib"
  prepend_path LD_LIBRARY_PATH "$rdma_core_lib"
  prepend_path LD_LIBRARY_PATH "$numactl_lib"
  prepend_path LD_LIBRARY_PATH "$mpi_home/lib"
  prepend_path LD_LIBRARY_PATH "$rocm_path/lib"
  prepend_path LD_LIBRARY_PATH "$rocm_path/lib/rocm_sysdeps/lib"

  export HIP_VISIBLE_DEVICES=${HIP_VISIBLE_DEVICES:-0}
  export ROCR_VISIBLE_DEVICES=${ROCR_VISIBLE_DEVICES:-0}
  export HSA_NO_SCRATCH_RECLAIM=${HSA_NO_SCRATCH_RECLAIM:-1}
  export HSA_OVERRIDE_GFX_VERSION=${HSA_OVERRIDE_GFX_VERSION:-11.5.1}
  export ROCSHMEM_GDA_PROVIDER=${ROCSHMEM_GDA_PROVIDER:-usb4}
  export ROCSHMEM_GDA_ENABLE_DMABUF=${ROCSHMEM_GDA_ENABLE_DMABUF:-1}
  export ROCSHMEM_HCA_LIST=${ROCSHMEM_HCA_LIST:-usb4_rdma0}
  export ROCSHMEM_HEAP_SIZE=${ROCSHMEM_HEAP_SIZE:-1073741824}
  export ROCSHMEM_MAX_NUM_TEAMS=${ROCSHMEM_MAX_NUM_TEAMS:-1}
  export ROCSHMEM_DEBUG_LEVEL=${ROCSHMEM_DEBUG_LEVEL:-ERROR}
  export ROCSHMEM_GDA_USB4_ROUTE_TRACE=${ROCSHMEM_GDA_USB4_ROUTE_TRACE:-0}
  export IB_GID_INDEX=${IB_GID_INDEX:-1}
  export RCCL_FORCE_ENABLE_DMABUF=${RCCL_FORCE_ENABLE_DMABUF:-1}
  export RCCL_INIT_CHANNELS=${RCCL_INIT_CHANNELS:-1}
  export NCCL_DEBUG=${NCCL_DEBUG:-WARN}
}

run_with_counters() {
  local label=$1
  local log=$2
  local expect_dv=$3
  shift 3
  local dir
  local before_label
  local after_label
  local status=0

  dir=$(dirname "$log")
  before_label="$label.before"
  after_label="$label.after"
  mkdir -p "$dir"
  {
    printf 'label: %s\ncommand:' "$label"
    printf ' %q' "$@"
    printf '\n'
  } >"$log"

  capture_counters "$before_label" "$dir/counters" "$counter_hosts"
  if "$@" >>"$log" 2>&1; then
    status=0
  else
    status=$?
  fi
  capture_counters "$after_label" "$dir/counters" "$counter_hosts"
  print_counter_deltas "$before_label" "$after_label" "$dir/counters" "$counter_hosts" >>"$log"

  if ((status == 0)); then
    if ! assert_clean_counters "$before_label" "$after_label" "$dir/counters" "$counter_hosts" >>"$log" 2>&1; then
      status=1
    fi
    if ! assert_dv_delta "$before_label" "$after_label" "$dir/counters" "$counter_hosts" "$(resolve_dv_expectation "$expect_dv")" >>"$log" 2>&1; then
      status=1
    fi
  fi
  return "$status"
}

run_rccl_case() {
  local collective=$1
  local mode=$2
  local rep=$3
  local bin
  local label
  local log
  local factor=2
  local min_size
  local max_size

  configure_mode "$mode"
  case "$collective" in
    alltoall) bin=alltoall_perf ;;
    alltoallv) bin=alltoallv_perf ;;
    *) echo "ERROR: unknown collective: $collective" >&2; exit 2 ;;
  esac
  if [[ ! -x "$rccl_tests_dir/$bin" ]]; then
    echo "ERROR: missing RCCL test binary: $rccl_tests_dir/$bin" >&2
    exit 2
  fi
  min_size=${sizes%%,*}
  max_size=${sizes##*,}
  label="rccl_${collective}_${mode}_rep${rep}"
  log="$log_root/rccl/$collective/$mode/rep-$rep/${label}.log"

  echo "RCCL $collective mode=$mode rep=$rep log=$log"
  run_with_counters "$label" "$log" "$TBV_EXPECT_DV" \
    timeout "$timeout_s" mpirun -np 2 --host "$hosts" --map-by ppr:1:node \
      --mca pml ob1 --mca btl self,tcp --mca btl_tcp_if_include "$iface" \
      -x LD_LIBRARY_PATH -x PATH -x ROCM_PATH -x ROCM_HOME -x HIP_PATH -x HIP_PLATFORM \
      -x RCCL_ROCR_PATH \
      -x HIP_VISIBLE_DEVICES -x ROCR_VISIBLE_DEVICES -x HSA_NO_SCRATCH_RECLAIM -x HSA_OVERRIDE_GFX_VERSION \
      -x ROCSHMEM_GDA_PROVIDER -x ROCSHMEM_GDA_ENABLE_DMABUF -x ROCSHMEM_HCA_LIST -x ROCSHMEM_HEAP_SIZE \
      -x ROCSHMEM_MAX_NUM_TEAMS -x ROCSHMEM_DEBUG_LEVEL -x ROCSHMEM_GDA_USB4_ROUTE_TRACE -x IB_GID_INDEX \
      -x ROCSHMEM_GDA_QP_TIMEOUT -x ROCSHMEM_GDA_QP_RETRY_CNT -x ROCSHMEM_GDA_QP_RNR_RETRY \
      -x RCCL_ROCSHMEM_ENABLE -x RCCL_ROCSHMEM_FORCE_ENABLE -x RCCL_ROCSHMEM_THRESHOLD \
      -x RCCL_ROCSHMEM_SOURCE_HEAP -x RCCL_ROCSHMEM_DEST_HEAP -x RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL \
      -x RCCL_ROCSHMEM_GDA_BENCH_MODE -x RCCL_ROCSHMEM_HOST_STREAM_TIMING \
      -x RCCL_FORCE_ENABLE_DMABUF -x RCCL_INIT_CHANNELS -x NCCL_DEBUG \
      "$rccl_tests_dir/$bin" -b "$min_size" -e "$max_size" -f "$factor" \
      -n "$iters" -w "$warmup" -g 1 -c 1 -a 1
}

stage_pytorch_smoke() {
  local host
  for host in $(host_list "$hosts"); do
    scp -q "$script_dir/tbv_pytorch_smoke.py" "$host:$pytorch_remote_script"
  done
}

pytorch_ld_library_path() {
  local out=
  local path

  for path in \
    "$rccl_install/lib" \
    "$rocshmem_install/lib" \
    "$rdma_core_lib" \
    "$numactl_lib" \
    "$mpi_home/lib" \
    "$pytorch_wrapper"/lib/python*/site-packages/_rocm_sdk_core/lib; do
    [[ -d "$path" ]] || continue
    case ":$out:" in
      *":$path:"*) ;;
      *) out="${out:+$out:}$path" ;;
    esac
  done

  printf '%s' "$out"
}

pytorch_ld_preload_value() {
  case "$pytorch_ld_preload" in
    ""|0|off|none|false)
      return 0
      ;;
    auto)
      if [[ -n "$rccl_install" && -e "$rccl_install/lib/librccl.so.1" ]]; then
        printf '%s' "$rccl_install/lib/librccl.so.1"
      fi
      ;;
    *)
      printf '%s' "$pytorch_ld_preload"
      ;;
  esac
}

pytorch_rccl_lib_value() {
  case "$pytorch_rccl_lib" in
    ""|0|off|none|false)
      return 0
      ;;
    auto)
      if [[ -n "$rccl_install" && -e "$rccl_install/lib/librccl.so.1" ]]; then
        printf '%s' "$rccl_install/lib/librccl.so.1"
      fi
      ;;
    *)
      printf '%s' "$pytorch_rccl_lib"
      ;;
  esac
}

build_torch_remote_command() {
  local rank=$1
  local python_bin="$pytorch_wrapper/bin/python"
  local torch_ld_path
  local torch_ld_preload
  local torch_rccl_lib
  local cmd=(timeout "$pytorch_timeout" env)

  torch_ld_path=$(pytorch_ld_library_path)
  torch_ld_preload=$(pytorch_ld_preload_value)
  torch_rccl_lib=$(pytorch_rccl_lib_value)
  cmd+=(
    "PATH=$(dirname "$python_bin"):$mpi_home/bin:/run/current-system/sw/bin:/usr/bin:/bin"
    "LD_LIBRARY_PATH=$torch_ld_path"
    "RCCL_ROCR_PATH=${RCCL_ROCR_PATH:-$rocm_path/lib/}"
    "HSA_NO_SCRATCH_RECLAIM=${HSA_NO_SCRATCH_RECLAIM:-1}"
    "HSA_ENABLE_INTERRUPT=${HSA_ENABLE_INTERRUPT:-0}"
    "HSA_OVERRIDE_GFX_VERSION=${HSA_OVERRIDE_GFX_VERSION:-11.5.1}"
    "HIP_PLATFORM=amd"
    "ROCM_HOME=$rocm_path"
    "ROCM_PATH=$rocm_path"
    "HIP_PATH=$rocm_path"
    "HIP_VISIBLE_DEVICES=${HIP_VISIBLE_DEVICES:-0}"
    "ROCR_VISIBLE_DEVICES=${ROCR_VISIBLE_DEVICES:-0}"
    "GLOO_SOCKET_IFNAME=$iface"
    "NCCL_SOCKET_IFNAME=$iface"
    "ROCSHMEM_GDA_PROVIDER=${ROCSHMEM_GDA_PROVIDER:-usb4}"
    "ROCSHMEM_GDA_ENABLE_DMABUF=${ROCSHMEM_GDA_ENABLE_DMABUF:-1}"
    "ROCSHMEM_HCA_LIST=${ROCSHMEM_HCA_LIST:-usb4_rdma0}"
    "ROCSHMEM_HEAP_SIZE=${ROCSHMEM_HEAP_SIZE:-1073741824}"
    "ROCSHMEM_MAX_NUM_TEAMS=${ROCSHMEM_MAX_NUM_TEAMS:-1}"
    "ROCSHMEM_DEBUG_LEVEL=${ROCSHMEM_DEBUG_LEVEL:-ERROR}"
    "ROCSHMEM_GDA_USB4_ROUTE_TRACE=${ROCSHMEM_GDA_USB4_ROUTE_TRACE:-0}"
    "ROCSHMEM_GDA_QP_TIMEOUT=${ROCSHMEM_GDA_QP_TIMEOUT:-14}"
    "ROCSHMEM_GDA_QP_RETRY_CNT=${ROCSHMEM_GDA_QP_RETRY_CNT:-7}"
    "ROCSHMEM_GDA_QP_RNR_RETRY=${ROCSHMEM_GDA_QP_RNR_RETRY:-7}"
    "IB_GID_INDEX=${IB_GID_INDEX:-1}"
    "RCCL_ROCSHMEM_ENABLE=$RCCL_ROCSHMEM_ENABLE"
    "RCCL_ROCSHMEM_FORCE_ENABLE=$RCCL_ROCSHMEM_FORCE_ENABLE"
    "RCCL_ROCSHMEM_THRESHOLD=$RCCL_ROCSHMEM_THRESHOLD"
    "RCCL_ROCSHMEM_SOURCE_HEAP=$RCCL_ROCSHMEM_SOURCE_HEAP"
    "RCCL_ROCSHMEM_DEST_HEAP=$RCCL_ROCSHMEM_DEST_HEAP"
    "RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=$RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL"
    "RCCL_ROCSHMEM_GDA_BENCH_MODE=$RCCL_ROCSHMEM_GDA_BENCH_MODE"
    "RCCL_ROCSHMEM_HOST_STREAM_TIMING=$RCCL_ROCSHMEM_HOST_STREAM_TIMING"
    "RCCL_INIT_CHANNELS=${RCCL_INIT_CHANNELS:-1}"
    "NCCL_DEBUG=${NCCL_DEBUG:-WARN}"
    "TBV_TORCH_SIZES=$pytorch_sizes"
    "TBV_TORCH_ITERS=$pytorch_iters"
    "TBV_TORCH_COLLECTIVES=$pytorch_collectives"
    "TBV_TORCH_VALIDATE=$pytorch_validate"
    "PYTHONUNBUFFERED=1"
  )
  if [[ -n "$torch_rccl_lib" ]]; then
    cmd+=("TBV_TORCH_RCCL_LIB=$torch_rccl_lib")
  fi
  if [[ -n "${RCCL_ROCSHMEM_HOST_STREAM_FIXED_SYMID:-}" ]]; then
    cmd+=("RCCL_ROCSHMEM_HOST_STREAM_FIXED_SYMID=$RCCL_ROCSHMEM_HOST_STREAM_FIXED_SYMID")
  fi
  if [[ -n "$torch_ld_preload" ]]; then
    cmd+=("LD_PRELOAD=$torch_ld_preload")
  fi
  cmd+=(
    "$python_bin"
    -m torch.distributed.run
    --nnodes=2
    --nproc_per_node=1
    "--node_rank=$rank"
    "--master_addr=$pytorch_master_addr"
    "--master_port=$pytorch_master_port"
    "$pytorch_remote_script"
  )
  printf '%q ' "${cmd[@]}"
}

run_pytorch_case() {
  local mode=$1
  local rep=$2
  local logdir="$log_root/pytorch/$mode/rep-$rep"
  local before_label="pytorch_${mode}_rep${rep}.before"
  local after_label="pytorch_${mode}_rep${rep}.after"
  local status=0
  local pids=()
  local rank=0
  local host
  local cmd
  local expected_rccl_realpath

  require_dir "TBV_PYTORCH_WRAPPER/VLLM_USB4_ENV" "$pytorch_wrapper"
  configure_mode "$mode"
  assert_pytorch_validation_mode "$mode"
  mkdir -p "$logdir"

  capture_counters "$before_label" "$logdir/counters" "$counter_hosts"
  for host in $(host_list "$hosts"); do
    cmd=$(build_torch_remote_command "$rank")
    {
      printf 'command: %s\n' "$cmd"
    } >"$logdir/rank${rank}.${host}.log"
    run_remote "$host" "$cmd" >>"$logdir/rank${rank}.${host}.log" 2>&1 &
    pids+=("$!")
    rank=$((rank + 1))
  done

  for pid in "${pids[@]}"; do
    if ! wait "$pid"; then
      status=1
    fi
  done

  capture_counters "$after_label" "$logdir/counters" "$counter_hosts"
  {
    for host in $(host_list "$hosts"); do
      :
    done
    print_counter_deltas "$before_label" "$after_label" "$logdir/counters" "$counter_hosts"
  } >"$logdir/counters.log"

  if ((status == 0)); then
    if ! assert_clean_counters "$before_label" "$after_label" "$logdir/counters" "$counter_hosts" >>"$logdir/counters.log" 2>&1; then
      status=1
    fi
    if ! assert_dv_delta "$before_label" "$after_label" "$logdir/counters" "$counter_hosts" "$(resolve_dv_expectation "$TBV_EXPECT_DV")" >>"$logdir/counters.log" 2>&1; then
      status=1
    fi
    if [[ -n "$expected_rccl_lib" ]]; then
      expected_rccl_realpath=$(readlink -f "$expected_rccl_lib" 2>/dev/null || printf '%s' "$expected_rccl_lib")
      if ! grep -Fq "loaded_collective_lib=$expected_rccl_lib" "$logdir"/rank*.log &&
         ! grep -Fq "loaded_collective_lib=$expected_rccl_realpath" "$logdir"/rank*.log; then
        echo "ERROR: expected RCCL library not reported: $expected_rccl_lib" | tee -a "$logdir/counters.log" >&2
        status=1
      fi
    fi
  fi

  echo "PyTorch mode=$mode rep=$rep status=$status log=$logdir"
  return "$status"
}

mkdir -p "$log_root"
setup_app_env

echo "TBV app gate"
echo "  hosts=$hosts"
echo "  counter_hosts=$counter_hosts"
echo "  iface=$iface"
echo "  log_root=$log_root"
echo "  modes=$modes"
echo "  collectives=$collectives"
echo "  dv_check=$dv_check"

gate_status=0

if [[ "$run_rccl" == 1 ]]; then
  require_dir "RCCL_TESTS_DIR" "$rccl_tests_dir"
  for rep in $(seq 1 "$reps"); do
    for collective in $(split_csv "$collectives"); do
      for mode in $(split_csv "$modes"); do
        if ! run_rccl_case "$collective" "$mode" "$rep"; then
          gate_status=1
        fi
      done
    done
  done
fi

if [[ "$run_pytorch" == 1 ]]; then
  stage_pytorch_smoke
  for rep in $(seq 1 "$reps"); do
    for mode in $(split_csv "$modes"); do
      if ! run_pytorch_case "$mode" "$rep"; then
        gate_status=1
      fi
    done
  done
fi

echo "TBV app gate complete: status=$gate_status log_root=$log_root"
exit "$gate_status"
