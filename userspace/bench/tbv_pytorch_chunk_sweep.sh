#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

chunks=${TBV_PYTORCH_CHUNK_SWEEP:-262144,524288,1048576,2097152}
hosts=${TBV_APP_HOSTS:-192.168.23.136,192.168.23.192}
counter_hosts=${TBV_COUNTER_HOSTS:-root@192.168.23.136,root@192.168.23.192}
iface=${TBV_APP_IFACE:-eno1}
log_parent=${TBV_APP_LOG_PARENT:-/mnt/Home/tmp/tbv-app-gate-logs}
timeout_s=${TBV_APP_TIMEOUT:-420}
threshold=${RCCL_ROCSHMEM_THRESHOLD:-67108864}
bench_mode=${RCCL_ROCSHMEM_GDA_BENCH_MODE:-2}
pytorch_sizes=${TBV_TORCH_SIZES:-4194304}
pytorch_iters=${TBV_TORCH_ITERS:-4}
reps=${TBV_REPS:-3}
pytorch_wrapper=${VLLM_USB4_ENV:-${TBV_PYTORCH_WRAPPER:-/nix/store/3mr3fgrn6znah88jrc42r2wh692x24km-vllm-env-therock-usb4-hostheap-gfx1151}}
rccl_install=${RCCL_INSTALL_DIR:-/mnt/Home/tmp/rccl-hoststream-waitbudget-install}
rocshmem_install=${ROCSHMEM_INSTALL_DIR:-/mnt/Home/tmp/rocshmem-waitbudget-install}
rocm_path=${ROCM_PATH:-/nix/store/263sdskvmyld0qqcz8f7qf0zsx11i6l8-therock-rocm-sdk-gfx1151-7.13.0a20260515}
mpi_home=${MPI_HOME:-/nix/store/ciq3sjjgih6p38rlyfjsd2jjkzl8nfz1-openmpi-5.0.10}
rdma_lib=${TBV_RDMA_LIB:-${USB4_RDMA_LIB:-/nix/store/wc6j2l3k5qdjzwkvd27nb4v490qn0i9w-rdma-core-usb4-62.0/lib}}
numactl_lib=${NUMACTL_LIB:-/nix/store/8xlwd35bpmj7n6bzjwfnr6vidpwicjdd-numactl-2.0.18/lib}
expected_rccl_lib=${RCCL_EXPECTED_LIB:-/mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1}
torch_rccl_lib=${TBV_TORCH_RCCL_LIB:-auto}
pytorch_dv_check=${TBV_PYTORCH_DV_CHECK:-require}
torch_collectives=${TBV_TORCH_COLLECTIVES:-all_to_all}
torch_validate=${TBV_TORCH_VALIDATE:-0}
dry_run=0
continue_on_error=0
summarize=1
allow_late_send_ack_no_qp=0

usage() {
  cat <<EOF
Usage: tbv_pytorch_chunk_sweep.sh [options]

Runs the PyTorch hoststream chunk-size discriminator through tbv_app_gate.sh.
Use --dry-run to print the commands without touching the Strix hosts.

Options:
  --chunks CSV              Default: $chunks
  --hosts H1,H2             Default: $hosts
  --counter-hosts H1,H2     Default: $counter_hosts
  --iface IFACE             Default: $iface
  --log-parent DIR          Default: $log_parent
  --timeout SECONDS         Default: $timeout_s
  --threshold BYTES         RCCL_ROCSHMEM_THRESHOLD. Default: $threshold
  --bench-mode N            RCCL_ROCSHMEM_GDA_BENCH_MODE. Default: $bench_mode
  --pytorch-wrapper DIR     Default: $pytorch_wrapper
  --rccl-install DIR        Default: $rccl_install
  --rocshmem-install DIR    Default: $rocshmem_install
  --rocm-path DIR           Default: $rocm_path
  --mpi-home DIR            Default: $mpi_home
  --rdma-lib DIR            Default: $rdma_lib
  --numactl-lib DIR         Default: $numactl_lib
  --expected-rccl-lib PATH  Default: $expected_rccl_lib
  --torch-rccl-lib PATH     Default: $torch_rccl_lib
  --pytorch-sizes CSV       Default: $pytorch_sizes
  --pytorch-iters N         Default: $pytorch_iters
  --torch-collectives CSV   Default: $torch_collectives
  --reps N                  Default: $reps
  --torch-validate 0|1      Default: $torch_validate
  --pytorch-dv-check MODE   Default: $pytorch_dv_check
  --allow-late-send-ack-no-qp
  --continue-on-error       Keep sweeping after a failed chunk
  --no-summary              Skip tbv_app_gate_summarize.sh after each run
  --dry-run                 Print commands only
EOF
}

while (($#)); do
  case "$1" in
    --chunks) chunks=$2; shift 2 ;;
    --hosts) hosts=$2; shift 2 ;;
    --counter-hosts) counter_hosts=$2; shift 2 ;;
    --iface) iface=$2; shift 2 ;;
    --log-parent) log_parent=$2; shift 2 ;;
    --timeout) timeout_s=$2; shift 2 ;;
    --threshold) threshold=$2; shift 2 ;;
    --bench-mode) bench_mode=$2; shift 2 ;;
    --pytorch-wrapper) pytorch_wrapper=$2; shift 2 ;;
    --rccl-install) rccl_install=$2; shift 2 ;;
    --rocshmem-install) rocshmem_install=$2; shift 2 ;;
    --rocm-path) rocm_path=$2; shift 2 ;;
    --mpi-home) mpi_home=$2; shift 2 ;;
    --rdma-lib) rdma_lib=$2; shift 2 ;;
    --numactl-lib) numactl_lib=$2; shift 2 ;;
    --expected-rccl-lib) expected_rccl_lib=$2; shift 2 ;;
    --torch-rccl-lib) torch_rccl_lib=$2; shift 2 ;;
    --pytorch-sizes) pytorch_sizes=$2; shift 2 ;;
    --pytorch-iters) pytorch_iters=$2; shift 2 ;;
    --torch-collectives) torch_collectives=$2; shift 2 ;;
    --reps) reps=$2; shift 2 ;;
    --torch-validate) torch_validate=$2; shift 2 ;;
    --pytorch-dv-check) pytorch_dv_check=$2; shift 2 ;;
    --allow-late-send-ack-no-qp) allow_late_send_ack_no_qp=1; shift ;;
    --continue-on-error) continue_on_error=1; shift ;;
    --no-summary) summarize=0; shift ;;
    --dry-run) dry_run=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

split_csv() {
  printf '%s\n' "${1//,/ }"
}

print_command() {
  printf '%q' "$1"
  shift
  while (($#)); do
    printf ' %q' "$1"
    shift
  done
  printf '\n'
}

append_dir_arg() {
  local -n argv=$1
  local opt=$2
  local value=$3

  [[ -n "$value" ]] || return 0
  argv+=("$opt" "$value")
}

run_chunk() {
  local chunk=$1
  local root
  local status=0
  local -a env_vars
  local -a cmd

  root="${log_parent%/}/postcopy-chunk${chunk}-$(date +%Y%m%d-%H%M%S)"
  env_vars=(
    "RCCL_ROCSHMEM_THRESHOLD=$threshold"
    "RCCL_ROCSHMEM_GDA_BENCH_MODE=$bench_mode"
    "ROCSHMEM_GDA_USB4_A2A_CHUNK_BYTES=$chunk"
    "TBV_APP_TIMEOUT=$timeout_s"
  )
  cmd=(
    bash "$script_dir/tbv_app_gate.sh"
    --hosts "$hosts"
    --counter-hosts "$counter_hosts"
    --iface "$iface"
    --log-root "$root"
    --skip-rccl
    --pytorch
    --modes hoststream
    --reps "$reps"
    --pytorch-sizes "$pytorch_sizes"
    --pytorch-iters "$pytorch_iters"
    --torch-collectives "$torch_collectives"
    --torch-validate "$torch_validate"
    --pytorch-dv-check "$pytorch_dv_check"
    --torch-rccl-lib "$torch_rccl_lib"
  )
  append_dir_arg cmd --pytorch-wrapper "$pytorch_wrapper"
  append_dir_arg cmd --rccl-install "$rccl_install"
  append_dir_arg cmd --rocshmem-install "$rocshmem_install"
  append_dir_arg cmd --rocm-path "$rocm_path"
  append_dir_arg cmd --mpi-home "$mpi_home"
  append_dir_arg cmd --rdma-lib "$rdma_lib"
  append_dir_arg cmd --numactl-lib "$numactl_lib"
  append_dir_arg cmd --expected-rccl-lib "$expected_rccl_lib"
  if [[ "$allow_late_send_ack_no_qp" == 1 ]]; then
    cmd+=(--allow-late-send-ack-no-qp)
  fi

  if [[ "$dry_run" == 1 ]]; then
    printf 'chunk=%s log_root=%s\n' "$chunk" "$root"
    print_command env "${env_vars[@]}" "${cmd[@]}"
    return 0
  fi

  if env "${env_vars[@]}" "${cmd[@]}"; then
    status=0
  else
    status=$?
  fi
  if [[ "$summarize" == 1 && -d "$root" ]]; then
    bash "$script_dir/tbv_app_gate_summarize.sh" "$root" | tee "$root/summary.txt"
  fi
  return "$status"
}

overall=0
for chunk in $(split_csv "$chunks"); do
  [[ -n "$chunk" ]] || continue
  if ! run_chunk "$chunk"; then
    overall=1
    if [[ "$continue_on_error" != 1 ]]; then
      exit "$overall"
    fi
  fi
done

exit "$overall"
