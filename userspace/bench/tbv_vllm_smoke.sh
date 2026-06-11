#!/usr/bin/env bash
set -euo pipefail

hosts=${TBV_VLLM_HOSTS:-192.168.23.136,192.168.23.192}
counter_hosts=${TBV_VLLM_COUNTER_HOSTS:-$hosts}
iface=${TBV_VLLM_IFACE:-eno1}
transport=${TBV_VLLM_TRANSPORT:-native}
log_parent=${TBV_VLLM_LOG_PARENT:-/mnt/Home/tmp/tbv-vllm-smoke}
log_root=${TBV_VLLM_LOG_ROOT:-$log_parent/$(date +%Y%m%d-%H%M%S)}
ssh_cmd=${TBV_SSH:-ssh}
ssh_opts=${TBV_SSH_OPTS:-"-o BatchMode=yes -o ControlMaster=no -S none -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=3"}
capture_counters_enabled=${TBV_VLLM_COUNTERS:-1}
counter_summary=${TBV_DEBUGFS_SUMMARY:-/sys/kernel/debug/thunderbolt_ibverbs/summary}
counter_keys=${TBV_VLLM_COUNTER_KEYS:-"data_wr_retransmit data_wr_retry_exhausted data_wr_timeout data_tx_posted data_tx_completed data_tx_errors data_tx_canceled data_rx_completed data_rx_canceled data_rx_repost_failed data_rx_bad_frame data_rx_bad_header data_rx_bad_header_parse data_rx_bad_header_len data_rx_bad_header_path_credit data_rx_bad_header_opcode data_rx_bad_header_recv_credit data_rx_bad_header_ack data_rx_bad_header_write data_rx_bad_header_read_req data_rx_bad_header_mad data_rx_ack data_rx_ack_matched data_rx_ack_match_retried data_rx_ack_miss data_rx_late_ack data_rx_no_qp"}
hard_error_keys=${TBV_VLLM_HARD_ERROR_KEYS:-"data_wr_retry_exhausted data_wr_timeout data_tx_errors data_tx_canceled data_rx_canceled data_rx_repost_failed data_rx_bad_frame data_rx_bad_header data_rx_bad_header_parse data_rx_bad_header_len data_rx_bad_header_path_credit data_rx_bad_header_opcode data_rx_bad_header_recv_credit data_rx_bad_header_ack data_rx_bad_header_write data_rx_bad_header_read_req data_rx_bad_header_mad data_rx_no_qp"}
require_rdma=${TBV_VLLM_REQUIRE_RDMA:-auto}

wrapper=${VLLM_USB4_ENV:-${TBV_VLLM_WRAPPER:-}}
model=${TBV_VLLM_MODEL:-/mnt/Home/tmp/tbv-vllm-tiny-qwen3}
hf_source=${TBV_VLLM_HF_SOURCE:-Qwen/Qwen3-4B}
prepare_tiny_model=${TBV_VLLM_PREPARE_TINY_MODEL:-1}

hca=${TBV_VLLM_HCA:-usb4_rdma5}
gid_index=${TBV_VLLM_GID_INDEX:-}
qps_per_connection=${TBV_VLLM_QPS_PER_CONNECTION:-1}
split_data_on_qps=${TBV_VLLM_SPLIT_DATA_ON_QPS:-0}
channels_per_peer=${TBV_VLLM_CHANNELS_PER_PEER:-1}
min_channels=${TBV_VLLM_MIN_CHANNELS:-1}
max_channels=${TBV_VLLM_MAX_CHANNELS:-1}
nccl_debug=${TBV_VLLM_NCCL_DEBUG:-WARN}
rdma_lib_dirs=${TBV_VLLM_RDMA_LIB_DIRS:-/run/current-system/sw/lib}

rccl_install=${RCCL_INSTALL_DIR:-}
rocshmem_install=${ROCSHMEM_INSTALL_DIR:-}
chunk_bytes=${TBV_ROCSHMEM_USB4_A2A_CHUNK_BYTES:-${ROCSHMEM_GDA_USB4_A2A_CHUNK_BYTES:-524288}}
threshold=${RCCL_ROCSHMEM_THRESHOLD:-67108864}
heap_size=${ROCSHMEM_HEAP_SIZE:-1073741824}
num_sym_buf=${ROCSHMEM_NUM_SYM_BUF:-4}

input_len=${TBV_VLLM_INPUT_LEN:-8}
output_len=${TBV_VLLM_OUTPUT_LEN:-4}
num_prompts=${TBV_VLLM_NUM_PROMPTS:-2}
max_model_len=${TBV_VLLM_MAX_MODEL_LEN:-512}
kv_cache_bytes=${TBV_VLLM_KV_CACHE_BYTES:-16777216}
tp_size=${TBV_VLLM_TP_SIZE:-2}
distributed_backend=${TBV_VLLM_DISTRIBUTED_BACKEND:-ray}
run_single_first=${TBV_VLLM_SINGLE_FIRST:-1}

usage() {
  cat <<EOF
Usage: tbv_vllm_smoke.sh [options]

Runs a small vLLM throughput smoke on two hosts and checks thunderbolt-ibverbs
debugfs counter deltas. The default transport is native Linux ibverbs. Use
--transport gda only with a rebuilt RCCL/rocSHMEM stack.

Options:
  --hosts H1,H2              Default: $hosts
  --counter-hosts H1,H2      Default: hosts
  --iface IFACE              Default: $iface
  --transport native|gda     Default: $transport
  --log-root DIR             Default: $log_root
  --ssh CMD                  Default: $ssh_cmd
  --ssh-opts OPTS            Default: $ssh_opts
  --no-counters              Skip thunderbolt-ibverbs debugfs counter deltas
  --require-rdma 0|1|auto    Fail if TP run moves no RDMA counters. Default: $require_rdma
  --wrapper DIR              vLLM/PyTorch wrapper prefix
  --model DIR                Tiny model directory. Default: $model
  --hf-source MODEL          Cached HF tokenizer/config source. Default: $hf_source
  --prepare-tiny-model 0|1   Default: $prepare_tiny_model
  --hca LIST                 NCCL/RCCL IB HCA list. Default: $hca
  --gid-index N              Optional NCCL/RCCL IB GID index
  --qps-per-connection N     Default: $qps_per_connection
  --split-data-on-qps 0|1    Default: $split_data_on_qps
  --channels-per-peer N      Default: $channels_per_peer
  --min-channels N           Default: $min_channels
  --max-channels N           Default: $max_channels
  --nccl-debug LEVEL         Default: $nccl_debug
  --rdma-lib-dirs DIRS       Colon-separated libibverbs/librdmacm dirs. Default: $rdma_lib_dirs
  --rccl-install DIR         Required only for --transport gda
  --rocshmem-install DIR     Required only for --transport gda
  --chunk-bytes N            GDA chunk bytes. Default: $chunk_bytes
  --threshold N              GDA RCCL/rocSHMEM threshold. Default: $threshold
  --num-prompts N            Default: $num_prompts
  --input-len N              Default: $input_len
  --output-len N             Default: $output_len
  --max-model-len N          Default: $max_model_len
  --kv-cache-bytes N         Default: $kv_cache_bytes
  --tp-size N                Default: $tp_size
  --distributed-backend NAME Default: $distributed_backend
  --single-first 0|1         Run a single-node smoke before TP. Default: $run_single_first
EOF
}

while (($#)); do
  case "$1" in
    --hosts) hosts=$2; shift 2 ;;
    --counter-hosts) counter_hosts=$2; shift 2 ;;
    --iface) iface=$2; shift 2 ;;
    --transport) transport=$2; shift 2 ;;
    --log-root) log_root=$2; shift 2 ;;
    --ssh) ssh_cmd=$2; shift 2 ;;
    --ssh-opts) ssh_opts=$2; shift 2 ;;
    --no-counters) capture_counters_enabled=0; shift ;;
    --require-rdma) require_rdma=$2; shift 2 ;;
    --wrapper) wrapper=$2; shift 2 ;;
    --model) model=$2; shift 2 ;;
    --hf-source) hf_source=$2; shift 2 ;;
    --prepare-tiny-model) prepare_tiny_model=$2; shift 2 ;;
    --hca) hca=$2; shift 2 ;;
    --gid-index) gid_index=$2; shift 2 ;;
    --qps-per-connection) qps_per_connection=$2; shift 2 ;;
    --split-data-on-qps) split_data_on_qps=$2; shift 2 ;;
    --channels-per-peer) channels_per_peer=$2; shift 2 ;;
    --min-channels) min_channels=$2; shift 2 ;;
    --max-channels) max_channels=$2; shift 2 ;;
    --nccl-debug) nccl_debug=$2; shift 2 ;;
    --rdma-lib-dirs) rdma_lib_dirs=$2; shift 2 ;;
    --rccl-install) rccl_install=$2; shift 2 ;;
    --rocshmem-install) rocshmem_install=$2; shift 2 ;;
    --chunk-bytes) chunk_bytes=$2; shift 2 ;;
    --threshold) threshold=$2; shift 2 ;;
    --num-prompts) num_prompts=$2; shift 2 ;;
    --input-len) input_len=$2; shift 2 ;;
    --output-len) output_len=$2; shift 2 ;;
    --max-model-len) max_model_len=$2; shift 2 ;;
    --kv-cache-bytes) kv_cache_bytes=$2; shift 2 ;;
    --tp-size) tp_size=$2; shift 2 ;;
    --distributed-backend) distributed_backend=$2; shift 2 ;;
    --single-first) run_single_first=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

case "$transport" in
  native|gda) ;;
  *) echo "unsupported transport: $transport" >&2; exit 2 ;;
esac
case "$require_rdma" in
  0|1|auto) ;;
  *) echo "unsupported --require-rdma value: $require_rdma" >&2; exit 2 ;;
esac

IFS=, read -r -a host_list <<< "$hosts"
if ((${#host_list[@]} != 2)); then
  echo "expected exactly two hosts, got: $hosts" >&2
  exit 2
fi
head_host=${host_list[0]}
worker_host=${host_list[1]}
read -r -a ssh_opts_array <<< "$ssh_opts"

run_remote() {
  local host=$1
  shift

  "$ssh_cmd" "${ssh_opts_array[@]}" "$host" "$@"
}

host_list_from_csv() {
  local raw=${1//,/ }
  local host

  for host in $raw; do
    [[ -n "$host" ]] && printf '%s\n' "$host"
  done
}

safe_name() {
  printf '%s' "$1" | tr -c 'A-Za-z0-9_.-' '_'
}

counter_file() {
  printf '%s/counters/%s.%s.summary' "$log_root" "$(safe_name "$1")" "$(safe_name "$2")"
}

capture_counters() {
  local label=$1
  local quoted_summary
  local host
  local out
  local err
  local cmd

  [[ "$capture_counters_enabled" == 1 ]] || return 0
  mkdir -p "$log_root/counters"
  printf -v quoted_summary '%q' "$counter_summary"
  cmd="if command -v sudo >/dev/null 2>&1; then sudo -n cat $quoted_summary 2>/dev/null || cat $quoted_summary; else cat $quoted_summary; fi"

  for host in $(host_list_from_csv "$counter_hosts"); do
    out=$(counter_file "$label" "$host")
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
  local host=$3
  local key=$4
  local before_value
  local after_value

  before_value=$(counter_value "$(counter_file "$before_label" "$host")" "$key")
  after_value=$(counter_value "$(counter_file "$after_label" "$host")" "$key")
  printf '%d\n' "$((after_value - before_value))"
}

counter_delta_sum() {
  local before_label=$1
  local after_label=$2
  local key=$3
  local host
  local total=0

  for host in $(host_list_from_csv "$counter_hosts"); do
    total=$((total + $(counter_delta_one "$before_label" "$after_label" "$host" "$key")))
  done
  printf '%d\n' "$total"
}

print_counter_deltas() {
  local before_label=$1
  local after_label=$2
  local key
  local host
  local delta
  local total

  [[ "$capture_counters_enabled" == 1 ]] || return 0
  {
    printf 'TBV counter deltas (%s -> %s):\n' "$before_label" "$after_label"
    for key in $counter_keys; do
      total=0
      for host in $(host_list_from_csv "$counter_hosts"); do
        delta=$(counter_delta_one "$before_label" "$after_label" "$host" "$key")
        total=$((total + delta))
        printf '  %-34s %-16s %+d\n' "$key" "$host" "$delta"
      done
      printf '  %-34s %-16s %+d\n' "$key" "sum" "$total"
    done
  } | tee "$log_root/counters/deltas.log"
}

assert_clean_counters() {
  local before_label=$1
  local after_label=$2
  local key
  local delta
  local failed=0

  [[ "$capture_counters_enabled" == 1 ]] || return 0
  for key in $hard_error_keys; do
    delta=$(counter_delta_sum "$before_label" "$after_label" "$key")
    if ((delta != 0)); then
      echo "ERROR: expected $key delta == 0, got $delta" | tee -a "$log_root/counters/deltas.log" >&2
      failed=1
    fi
  done
  return "$failed"
}

assert_rdma_used() {
  local before_label=$1
  local after_label=$2
  local tx_delta
  local rx_delta

  [[ "$capture_counters_enabled" == 1 ]] || return 0
  [[ "$require_rdma" == 1 ]] || {
    [[ "$require_rdma" == auto && "$tp_size" -gt 1 ]] || return 0
  }

  tx_delta=$(counter_delta_sum "$before_label" "$after_label" data_tx_posted)
  rx_delta=$(counter_delta_sum "$before_label" "$after_label" data_rx_completed)
  if ((tx_delta == 0 && rx_delta == 0)); then
    echo "ERROR: TP run completed but thunderbolt-ibverbs data counters did not move" | tee -a "$log_root/counters/deltas.log" >&2
    echo "ERROR: data_tx_posted delta=$tx_delta data_rx_completed delta=$rx_delta" | tee -a "$log_root/counters/deltas.log" >&2
    return 1
  fi
}

require_dir() {
  local name=$1
  local path=$2
  if [[ -z "$path" || ! -d "$path" ]]; then
    echo "$name is required and must be a directory: ${path:-<unset>}" >&2
    exit 2
  fi
}

resolve_wrapped_bin() {
  local path=$1
  local target

  target=$(sed -n 's/^exec "\([^"]*\)".*/\1/p' "$path" | head -n 1 || true)
  if [[ -n "$target" && -x "$target" ]]; then
    printf '%s\n' "$target"
  else
    printf '%s\n' "$path"
  fi
}

require_dir "wrapper" "$wrapper"
if [[ "$transport" == gda ]]; then
  require_dir "rccl install" "$rccl_install"
  require_dir "rocshmem install" "$rocshmem_install"
fi

python_wrapper=$wrapper/bin/python3
vllm_wrapper=$wrapper/bin/vllm
ray_wrapper=$wrapper/bin/ray
if [[ ! -x "$python_wrapper" || ! -x "$vllm_wrapper" || ! -x "$ray_wrapper" ]]; then
  echo "wrapper must provide bin/python3, bin/vllm, and bin/ray: $wrapper" >&2
  exit 2
fi

python=$(resolve_wrapped_bin "$python_wrapper")
vllm=$(resolve_wrapped_bin "$vllm_wrapper")
ray=$(resolve_wrapped_bin "$ray_wrapper")
if [[ ! -x "$python" || ! -x "$vllm" || ! -x "$ray" ]]; then
  echo "resolved wrapper commands must be executable: python=$python vllm=$vllm ray=$ray" >&2
  exit 2
fi

mkdir -p "$log_root"
{
  echo "transport=$transport"
  echo "wrapper=$wrapper"
  echo "python=$python"
  echo "vllm=$vllm"
  echo "ray=$ray"
  echo "rccl_install=$rccl_install"
  echo "rocshmem_install=$rocshmem_install"
  echo "model=$model"
  echo "hosts=$hosts"
  echo "counter_hosts=$counter_hosts"
  echo "iface=$iface"
  echo "hca=$hca"
  echo "gid_index=$gid_index"
  echo "qps_per_connection=$qps_per_connection"
  echo "split_data_on_qps=$split_data_on_qps"
  echo "channels_per_peer=$channels_per_peer"
  echo "rdma_lib_dirs=$rdma_lib_dirs"
  echo "capture_counters=$capture_counters_enabled"
} > "$log_root/config.txt"

if [[ "$prepare_tiny_model" == 1 ]]; then
  "$python" - "$hf_source" "$model" > "$log_root/prepare-model.log" <<'PY'
from pathlib import Path
import sys

from transformers import AutoConfig, AutoTokenizer, Qwen3Config

src = sys.argv[1]
out = Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)

tok = AutoTokenizer.from_pretrained(src, local_files_only=True)
tok.save_pretrained(out)
base = AutoConfig.from_pretrained(src, local_files_only=True)
config = Qwen3Config(
    vocab_size=len(tok),
    hidden_size=128,
    intermediate_size=384,
    num_hidden_layers=2,
    num_attention_heads=2,
    num_key_value_heads=2,
    head_dim=64,
    hidden_act=getattr(base, "hidden_act", "silu"),
    max_position_embeddings=512,
    initializer_range=getattr(base, "initializer_range", 0.02),
    rms_norm_eps=getattr(base, "rms_norm_eps", 1e-6),
    use_cache=True,
    tie_word_embeddings=False,
    rope_theta=getattr(base, "rope_theta", 1000000.0),
    bos_token_id=tok.bos_token_id,
    eos_token_id=tok.eos_token_id,
    pad_token_id=tok.pad_token_id if tok.pad_token_id is not None else tok.eos_token_id,
)
config.architectures = ["Qwen3ForCausalLM"]
config.dtype = "float16"
config.save_pretrained(out)
print(f"model={out}")
print(f"vocab_size={config.vocab_size} pad_token_id={config.pad_token_id}")
PY
fi

for host in "${host_list[@]}"; do
  run_remote "$host" \
    "test -x $(printf '%q' "$vllm_wrapper") && test -x $(printf '%q' "$ray_wrapper") && test -d $(printf '%q' "$model")"
done

base_remote_env() {
  printf 'export GLOO_SOCKET_IFNAME=%q NCCL_SOCKET_IFNAME=%q RCCL_SOCKET_IFNAME=%q; ' \
    "$iface" "$iface" "$iface"
  printf 'export NCCL_IB_DISABLE=0 RCCL_IB_DISABLE=0; '
  if [[ -n "$hca" ]]; then
    printf 'export NCCL_IB_HCA=%q RCCL_IB_HCA=%q; ' "$hca" "$hca"
  fi
  if [[ -n "$gid_index" ]]; then
    printf 'export NCCL_IB_GID_INDEX=%q RCCL_IB_GID_INDEX=%q; ' "$gid_index" "$gid_index"
  fi
  printf 'export NCCL_IB_QPS_PER_CONNECTION=%q RCCL_IB_QPS_PER_CONNECTION=%q; ' \
    "$qps_per_connection" "$qps_per_connection"
  printf 'export NCCL_IB_SPLIT_DATA_ON_QPS=%q RCCL_IB_SPLIT_DATA_ON_QPS=%q; ' \
    "$split_data_on_qps" "$split_data_on_qps"
  printf 'export NCCL_NCHANNELS_PER_NET_PEER=%q RCCL_NCHANNELS_PER_NET_PEER=%q; ' \
    "$channels_per_peer" "$channels_per_peer"
  printf 'export NCCL_MIN_NCHANNELS=%q RCCL_MIN_NCHANNELS=%q NCCL_MAX_NCHANNELS=%q RCCL_MAX_NCHANNELS=%q; ' \
    "$min_channels" "$min_channels" "$max_channels" "$max_channels"
  printf 'export VLLM_NO_USAGE_STATS=1 VLLM_DO_NOT_TRACK=1 RAY_USAGE_STATS_ENABLED=0 RAY_DEDUP_LOGS=0 NCCL_DEBUG=%q RCCL_DEBUG=%q; ' \
    "$nccl_debug" "$nccl_debug"
  if [[ -n "$rdma_lib_dirs" ]]; then
    # RCCL dlopens libibverbs/librdmacm by soname; Nix wrappers do not expose
    # system RDMA libraries to Ray workers unless we pass the library path here.
    # shellcheck disable=SC2016
    printf 'export LD_LIBRARY_PATH=%q:${LD_LIBRARY_PATH:-}; ' "$rdma_lib_dirs"
  fi
}

gda_remote_env() {
  # Expand LD_LIBRARY_PATH on the remote host, not while building this command.
  # shellcheck disable=SC2016
  printf 'export LD_LIBRARY_PATH=%q:%q:${LD_LIBRARY_PATH:-}; ' \
    "$rccl_install/lib" "$rocshmem_install/lib"
  printf 'export RCCL_ROCSHMEM_ENABLE=1 RCCL_ROCSHMEM_FORCE_ENABLE=1 RCCL_ROCSHMEM_THRESHOLD=%q RCCL_ROCSHMEM_HOST_STREAM_TIMING=1; ' \
    "$threshold"
  printf 'export ROCSHMEM_GDA_PROVIDER=ib ROCSHMEM_GDA_ENABLE_DMABUF=1 ROCSHMEM_GDA_USB4_A2A_CHUNK_BYTES=%q ROCSHMEM_GDA_USB4_ALLTOALL_MODE=0 ROCSHMEM_GDA_USB4_ALLTOALL_ACK=0; ' \
    "$chunk_bytes"
  printf 'export ROCSHMEM_GDA_QP_TIMEOUT=14 ROCSHMEM_GDA_QP_RETRY_CNT=7 ROCSHMEM_GDA_QP_RNR_RETRY=7 ROCSHMEM_NUM_SYM_BUF=%q ROCSHMEM_HEAP_SIZE=%q; ' \
    "$num_sym_buf" "$heap_size"
}

remote_env() {
  base_remote_env
  if [[ "$transport" == gda ]]; then
    gda_remote_env
  fi
}

stop_ray() {
  local host=$1
  local log=$2

  run_remote "$host" \
    "$(remote_env) $(printf '%q' "$ray") stop --force 2>&1 | tail -200" > "$log" 2>&1 || true
}

cleanup() {
  stop_ray "$worker_host" "$log_root/ray-stop-worker.log"
  stop_ray "$head_host" "$log_root/ray-stop-head.log"
}

finish() {
  local status=$?

  set +e
  capture_counters after
  print_counter_deltas before after
  if ((status == 0)); then
    assert_clean_counters before after || status=1
    assert_rdma_used before after || status=1
  fi
  cleanup
  exit "$status"
}
trap finish EXIT

stop_ray "$worker_host" "$log_root/ray-stop-worker-before.log"
stop_ray "$head_host" "$log_root/ray-stop-head-before.log"

capture_counters before

run_vllm_cmd() {
  local out=$1
  local distributed_args=$2

  printf 'mkdir -p %q; ' "$out"
  printf '%s ' "$(remote_env)"
  printf '%q bench throughput ' "$vllm"
  printf -- '--backend vllm --dataset-name random '
  printf -- '--random-input-len %q --random-output-len %q --num-prompts %q ' \
    "$input_len" "$output_len" "$num_prompts"
  printf -- '--model %q --runner generate --load-format dummy --dtype float16 ' "$model"
  printf -- '--max-model-len %q --kv-cache-memory-bytes %q ' "$max_model_len" "$kv_cache_bytes"
  printf -- '--enforce-eager --disable-custom-all-reduce --disable-detokenize '
  printf '%s ' "$distributed_args"
  printf -- '--output-json %q > %q 2>&1; cat %q' \
    "$out/results.json" "$out/run.log" "$out/results.json"
}

if [[ "$run_single_first" == 1 ]]; then
  single_root=$log_root/single
  run_remote "$head_host" "$(run_vllm_cmd "$single_root" "")" \
    | tee "$single_root.results.json"
fi

run_remote "$head_host" \
  "$(remote_env) $(printf '%q' "$ray") start --head --node-ip-address=$(printf '%q' "$head_host") --port=6379 --num-gpus=1 --disable-usage-stats" \
  > "$log_root/ray-head-start.log" 2>&1
run_remote "$worker_host" \
  "$(remote_env) $(printf '%q' "$ray") start --address=$(printf '%q' "$head_host:6379") --node-ip-address=$(printf '%q' "$worker_host") --num-gpus=1 --disable-usage-stats" \
  > "$log_root/ray-worker-start.log" 2>&1
run_remote "$head_host" \
  "$(remote_env) $(printf '%q' "$ray") status" > "$log_root/ray-status-before.log" 2>&1

tp_root=$log_root/tp$tp_size
run_remote "$head_host" \
  "$(run_vllm_cmd "$tp_root" "--distributed-executor-backend $distributed_backend --tensor-parallel-size $tp_size")" \
  | tee "$tp_root.results.json"

echo "vLLM smoke complete: log_root=$log_root"
