#!/usr/bin/env bash
set -euo pipefail

hosts=${TBV_ROCSHMEM_HOSTS:-192.168.23.136,192.168.23.192}
counter_hosts=${TBV_COUNTER_HOSTS:-$hosts}
iface=${TBV_ROCSHMEM_IFACE:-eno1}
log_parent=${TBV_ROCSHMEM_LOG_PARENT:-/mnt/Home/tmp/tbv-rocshmem-example}
log_root=${TBV_ROCSHMEM_LOG_ROOT:-$log_parent/$(date +%Y%m%d-%H%M%S)}
ssh_cmd=${TBV_SSH:-ssh}
timeout_s=${TBV_ROCSHMEM_TIMEOUT:-120}
np=${TBV_ROCSHMEM_NP:-2}

exe=${TBV_ROCSHMEM_EXE:-}
rocm_path=${ROCM_PATH:-}
mpi_home=${MPI_HOME:-}
rocshmem_install=${ROCSHMEM_INSTALL_DIR:-}
rdma_core_lib=${TBV_RDMA_LIB:-${USB4_RDMA_LIB:-}}
numactl_lib=${NUMACTL_LIB:-}

heap_size=${ROCSHMEM_HEAP_SIZE:-1073741824}
max_contexts=${ROCSHMEM_MAX_NUM_CONTEXTS:-2}
max_teams=${ROCSHMEM_MAX_NUM_TEAMS:-1}
debug=${TBV_ROCSHMEM_DEBUG:-0}
chunk_bytes=${TBV_ROCSHMEM_USB4_A2A_CHUNK_BYTES:-${ROCSHMEM_GDA_USB4_A2A_CHUNK_BYTES:-524288}}
alltoall_mode=${TBV_ROCSHMEM_USB4_ALLTOALL_MODE:-${ROCSHMEM_GDA_USB4_ALLTOALL_MODE:-0}}
alltoall_ack=${TBV_ROCSHMEM_USB4_ALLTOALL_ACK:-${ROCSHMEM_GDA_USB4_ALLTOALL_ACK:-0}}
qp_timeout=${ROCSHMEM_GDA_QP_TIMEOUT:-14}
qp_retry_cnt=${ROCSHMEM_GDA_QP_RETRY_CNT:-7}
qp_rnr_retry=${ROCSHMEM_GDA_QP_RNR_RETRY:-7}
backend=${ROCSHMEM_BACKEND:-gda}
provider=${ROCSHMEM_GDA_PROVIDER:-usb4}
hca_list=${ROCSHMEM_HCA_LIST:-usb4_rdma0}
enable_dmabuf=${ROCSHMEM_GDA_ENABLE_DMABUF:-1}
counter_summary=${TBV_DEBUGFS_SUMMARY:-/sys/kernel/debug/thunderbolt_ibverbs/summary}

counter_keys=${TBV_COUNTER_KEYS:-"dv_poll_wqes dv_hard_error data_wr_retransmit data_wr_rnr_retransmit data_wr_retry_exhausted data_wr_timeout data_wr_rnr_retry_exhausted data_wr_retransmit_closing_qp data_wr_retransmit_no_live_path data_wr_retransmit_teardown_path data_tx_errors data_rx_canceled data_rx_no_qp data_rx_no_qp_reack data_rx_no_qp_error_ack data_qp_tombstone_evicted data_rx_ack data_rx_ack_matched data_rx_late_ack data_rx_duplicate_ack data_rx_write_gap_rnr native_rx_atomic_req native_rx_atomic_resp data_rx_atomic_req data_rx_atomic_replay data_rx_atomic_history_miss data_rx_atomic_reorder_buffered data_rx_atomic_reorder_delivered data_tx_atomic_resp_ok data_tx_atomic_resp_error data_rx_reorder_buffered data_rx_reorder_delivered data_rx_reorder_duplicate_refresh data_rx_reorder_dropped data_rx_reorder_timeout data_rx_reorder_retry data_rx_reorder_window data_tx_posted data_tx_completed"}

usage() {
  cat <<EOF
Usage: tbv_rocshmem_example.sh [options] -- [example args...]

Runs a rocSHMEM example under the USB4 GDA environment and captures
thunderbolt-ibverbs counter deltas.

Options:
  --hosts H1,H2             Default: $hosts
  --counter-hosts H1,H2     Default: hosts
  --iface IFACE             Default: $iface
  --log-root DIR            Default: $log_root
  --ssh CMD                 Default: $ssh_cmd
  --timeout SECONDS         Default: $timeout_s
  --np N                    MPI rank count. Default: $np
  --exe PATH                Example executable
  --rocm-path DIR           ROCm/TheRock SDK prefix
  --mpi-home DIR            OpenMPI prefix
  --rocshmem-install DIR    rocSHMEM install prefix for LD_LIBRARY_PATH
  --rdma-lib DIR            rdma-core lib directory
  --numactl-lib DIR         numactl lib directory
  --heap-size BYTES         ROCSHMEM_HEAP_SIZE. Default: $heap_size
  --max-contexts N          ROCSHMEM_MAX_NUM_CONTEXTS. Default: $max_contexts
  --max-teams N             ROCSHMEM_MAX_NUM_TEAMS. Default: $max_teams
  --debug 0|1               Enable INFO logs plus USB4 route/A2A logs. Default: $debug
  --chunk-bytes N           ROCSHMEM_GDA_USB4_A2A_CHUNK_BYTES. Default: $chunk_bytes
  --alltoall-mode N         ROCSHMEM_GDA_USB4_ALLTOALL_MODE. Default: $alltoall_mode
  --alltoall-ack 0|1        ROCSHMEM_GDA_USB4_ALLTOALL_ACK. Default: $alltoall_ack
EOF
}

while (($#)); do
  case "$1" in
    --hosts) hosts=$2; shift 2 ;;
    --counter-hosts) counter_hosts=$2; shift 2 ;;
    --iface) iface=$2; shift 2 ;;
    --log-root) log_root=$2; shift 2 ;;
    --ssh) ssh_cmd=$2; shift 2 ;;
    --timeout) timeout_s=$2; shift 2 ;;
    --np) np=$2; shift 2 ;;
    --exe) exe=$2; shift 2 ;;
    --rocm-path) rocm_path=$2; shift 2 ;;
    --mpi-home) mpi_home=$2; shift 2 ;;
    --rocshmem-install) rocshmem_install=$2; shift 2 ;;
    --rdma-lib) rdma_core_lib=$2; shift 2 ;;
    --numactl-lib) numactl_lib=$2; shift 2 ;;
    --heap-size) heap_size=$2; shift 2 ;;
    --max-contexts) max_contexts=$2; shift 2 ;;
    --max-teams) max_teams=$2; shift 2 ;;
    --debug) debug=$2; shift 2 ;;
    --chunk-bytes) chunk_bytes=$2; shift 2 ;;
    --alltoall-mode) alltoall_mode=$2; shift 2 ;;
    --alltoall-ack) alltoall_ack=$2; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    --) shift; break ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done
example_args=("$@")

require_dir() {
  local label=$1
  local path=$2
  if [[ -z "$path" || ! -d "$path" ]]; then
    echo "ERROR: $label is not set or not a directory: ${path:-<unset>}" >&2
    exit 2
  fi
}

require_exe() {
  local label=$1
  local path=$2
  if [[ -z "$path" || ! -x "$path" ]]; then
    echo "ERROR: $label is not set or not executable: ${path:-<unset>}" >&2
    exit 2
  fi
}

host_list() {
  local raw=${1//,/ }
  local host
  for host in $raw; do
    [[ -n "$host" ]] && printf '%s\n' "$host"
  done
}

prepend_path() {
  local var=$1
  local path=$2
  local current=${!var:-}
  [[ -n "$path" && -d "$path" ]] || return 0
  case ":$current:" in
    *":$path:"*) ;;
    *) printf -v "$var" '%s%s%s' "$path" "${current:+:}" "$current"; export "${var?}" ;;
  esac
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
  "$ssh_cmd" -o BatchMode=yes "$host" "$@"
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
      printf '  %-38s %-16s %+d\n' "$key" "$host" "$delta"
    done
    printf '  %-38s %-16s %+d\n' "$key" "sum" "$total"
  done
}

require_exe "example executable" "$exe"
require_dir "ROCM_PATH" "$rocm_path"
require_dir "MPI_HOME" "$mpi_home"
require_dir "ROCSHMEM_INSTALL_DIR" "$rocshmem_install"
if [[ -n "$rdma_core_lib" && ! -d "$rdma_core_lib" ]]; then
  echo "WARN: rdma lib not found: $rdma_core_lib" >&2
fi

export ROCM_PATH="$rocm_path"
export ROCM_HOME="$rocm_path"
export HIP_PATH="$rocm_path"
export HIP_PLATFORM=amd
prepend_path PATH "$mpi_home/bin"
prepend_path PATH "$rocm_path/bin"
prepend_path LD_LIBRARY_PATH "$rocshmem_install/lib"
prepend_path LD_LIBRARY_PATH "$rdma_core_lib"
prepend_path LD_LIBRARY_PATH "$numactl_lib"
prepend_path LD_LIBRARY_PATH "$mpi_home/lib"
prepend_path LD_LIBRARY_PATH "$rocm_path/lib"
prepend_path LD_LIBRARY_PATH "$rocm_path/lib/rocm_sysdeps/lib"

export HIP_VISIBLE_DEVICES=${HIP_VISIBLE_DEVICES:-0}
export ROCR_VISIBLE_DEVICES=${ROCR_VISIBLE_DEVICES:-0}
export HSA_NO_SCRATCH_RECLAIM=${HSA_NO_SCRATCH_RECLAIM:-1}
export HSA_ENABLE_INTERRUPT=${HSA_ENABLE_INTERRUPT:-0}
export HSA_OVERRIDE_GFX_VERSION=${HSA_OVERRIDE_GFX_VERSION:-11.5.1}
export ROCSHMEM_BACKEND="$backend"
export ROCSHMEM_GDA_PROVIDER="$provider"
export ROCSHMEM_GDA_ENABLE_DMABUF="$enable_dmabuf"
export ROCSHMEM_HCA_LIST="$hca_list"
export ROCSHMEM_HEAP_SIZE="$heap_size"
export ROCSHMEM_MAX_NUM_CONTEXTS="$max_contexts"
export ROCSHMEM_MAX_NUM_TEAMS="$max_teams"
export ROCSHMEM_GDA_USB4_A2A_CHUNK_BYTES="$chunk_bytes"
export ROCSHMEM_GDA_USB4_ALLTOALL_MODE="$alltoall_mode"
export ROCSHMEM_GDA_USB4_ALLTOALL_ACK="$alltoall_ack"
export ROCSHMEM_GDA_QP_TIMEOUT="$qp_timeout"
export ROCSHMEM_GDA_QP_RETRY_CNT="$qp_retry_cnt"
export ROCSHMEM_GDA_QP_RNR_RETRY="$qp_rnr_retry"
export IB_GID_INDEX=${IB_GID_INDEX:-1}

if [[ "$debug" == 1 ]]; then
  export ROCSHMEM_DEBUG_LEVEL=${ROCSHMEM_DEBUG_LEVEL:-INFO:nocolor}
  export ROCSHMEM_GDA_USB4_ROUTE_TRACE=${ROCSHMEM_GDA_USB4_ROUTE_TRACE:-1}
  export ROCSHMEM_GDA_USB4_A2A_POST_LOG=${ROCSHMEM_GDA_USB4_A2A_POST_LOG:-1}
  export ROCSHMEM_GDA_USB4_A2A_TIMING_LOG=${ROCSHMEM_GDA_USB4_A2A_TIMING_LOG:-1}
else
  export ROCSHMEM_DEBUG_LEVEL=${ROCSHMEM_DEBUG_LEVEL:-ERROR}
  export ROCSHMEM_GDA_USB4_ROUTE_TRACE=${ROCSHMEM_GDA_USB4_ROUTE_TRACE:-0}
  export ROCSHMEM_GDA_USB4_A2A_POST_LOG=${ROCSHMEM_GDA_USB4_A2A_POST_LOG:-0}
  export ROCSHMEM_GDA_USB4_A2A_TIMING_LOG=${ROCSHMEM_GDA_USB4_A2A_TIMING_LOG:-0}
fi

mkdir -p "$log_root/counters"
label="rocshmem_$(basename "$exe")"
before_label="$label.before"
after_label="$label.after"
log="$log_root/$label.log"

{
  printf 'hosts=%s\ncounter_hosts=%s\niface=%s\nnp=%s\nexe=%s\n' "$hosts" "$counter_hosts" "$iface" "$np" "$exe"
  printf 'command: timeout %q mpirun -np %q --host %q --map-by ppr:1:node ... %q' "$timeout_s" "$np" "$hosts" "$exe"
  printf ' %q' "${example_args[@]}"
  printf '\n'
  printf 'ROCSHMEM_BACKEND=%s ROCSHMEM_GDA_PROVIDER=%s ROCSHMEM_HCA_LIST=%s ROCSHMEM_MAX_NUM_CONTEXTS=%s debug=%s\n' \
    "$ROCSHMEM_BACKEND" "$ROCSHMEM_GDA_PROVIDER" "$ROCSHMEM_HCA_LIST" "$ROCSHMEM_MAX_NUM_CONTEXTS" "$debug"
} >"$log_root/run.meta"

capture_counters "$before_label" "$log_root/counters" "$counter_hosts"

mpirun_cmd=(
  timeout "$timeout_s"
  mpirun -np "$np" --host "$hosts" --map-by ppr:1:node
  --mca pml ob1 --mca btl "self,tcp" --mca btl_tcp_if_include "$iface"
  -x LD_LIBRARY_PATH -x PATH -x ROCM_PATH -x ROCM_HOME -x HIP_PATH -x HIP_PLATFORM
  -x HIP_VISIBLE_DEVICES -x ROCR_VISIBLE_DEVICES -x HSA_NO_SCRATCH_RECLAIM -x HSA_ENABLE_INTERRUPT -x HSA_OVERRIDE_GFX_VERSION
  -x ROCSHMEM_BACKEND -x ROCSHMEM_GDA_PROVIDER -x ROCSHMEM_GDA_ENABLE_DMABUF -x ROCSHMEM_HCA_LIST
  -x ROCSHMEM_HEAP_SIZE -x ROCSHMEM_MAX_NUM_CONTEXTS -x ROCSHMEM_MAX_NUM_TEAMS -x ROCSHMEM_DEBUG_LEVEL
  -x ROCSHMEM_GDA_USB4_ROUTE_TRACE -x ROCSHMEM_GDA_USB4_A2A_POST_LOG -x ROCSHMEM_GDA_USB4_A2A_TIMING_LOG
  -x ROCSHMEM_GDA_USB4_A2A_CHUNK_BYTES -x ROCSHMEM_GDA_USB4_ALLTOALL_MODE -x ROCSHMEM_GDA_USB4_ALLTOALL_ACK
  -x ROCSHMEM_GDA_QP_TIMEOUT -x ROCSHMEM_GDA_QP_RETRY_CNT -x ROCSHMEM_GDA_QP_RNR_RETRY -x IB_GID_INDEX
  "$exe"
  "${example_args[@]}"
)

printf 'rocSHMEM example log=%s\n' "$log"
if "${mpirun_cmd[@]}" >"$log" 2>&1; then
  rc=0
else
  rc=$?
fi

capture_counters "$after_label" "$log_root/counters" "$counter_hosts"
print_counter_deltas "$before_label" "$after_label" "$log_root/counters" "$counter_hosts" >"$log_root/counters.log"

cat "$log_root/run.meta"
sed -n '1,220p' "$log"
cat "$log_root/counters.log"
exit "$rc"
