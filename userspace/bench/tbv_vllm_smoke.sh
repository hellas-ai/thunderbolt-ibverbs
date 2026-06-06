#!/usr/bin/env bash
set -euo pipefail

hosts=${TBV_VLLM_HOSTS:-192.168.23.136,192.168.23.192}
iface=${TBV_VLLM_IFACE:-eno1}
log_parent=${TBV_VLLM_LOG_PARENT:-/mnt/Home/tmp/tbv-vllm-smoke}
log_root=${TBV_VLLM_LOG_ROOT:-$log_parent/$(date +%Y%m%d-%H%M%S)}
ssh_cmd=${TBV_SSH:-ssh}

wrapper=${VLLM_USB4_ENV:-${TBV_VLLM_WRAPPER:-}}
model=${TBV_VLLM_MODEL:-/mnt/Home/tmp/tbv-vllm-tiny-qwen3-gda}
hf_source=${TBV_VLLM_HF_SOURCE:-Qwen/Qwen3-4B}
prepare_tiny_model=${TBV_VLLM_PREPARE_TINY_MODEL:-1}

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
run_single_first=${TBV_VLLM_SINGLE_FIRST:-1}

usage() {
  cat <<EOF
Usage: tbv_vllm_smoke.sh [options]

Runs a tiny vLLM throughput smoke on the Strix pair using Ray TP=2.

Options:
  --hosts H1,H2              Default: $hosts
  --iface IFACE              Default: $iface
  --log-root DIR             Default: $log_root
  --ssh CMD                  Default: $ssh_cmd
  --wrapper DIR              vLLM/PyTorch wrapper prefix
  --model DIR                Tiny model directory. Default: $model
  --hf-source MODEL          Cached HF tokenizer/config source. Default: $hf_source
  --prepare-tiny-model 0|1   Default: $prepare_tiny_model
  --rccl-install DIR         Rebuilt RCCL install prefix
  --rocshmem-install DIR     Rebuilt rocSHMEM install prefix
  --chunk-bytes N            ROCSHMEM_GDA_USB4_A2A_CHUNK_BYTES. Default: $chunk_bytes
  --threshold N              RCCL_ROCSHMEM_THRESHOLD. Default: $threshold
  --num-prompts N            Default: $num_prompts
  --input-len N              Default: $input_len
  --output-len N             Default: $output_len
  --max-model-len N          Default: $max_model_len
  --kv-cache-bytes N         Default: $kv_cache_bytes
  --tp-size N                Default: $tp_size
  --single-first 0|1         Run a single-node smoke before TP=2. Default: $run_single_first
EOF
}

while (($#)); do
  case "$1" in
    --hosts) hosts=$2; shift 2 ;;
    --iface) iface=$2; shift 2 ;;
    --log-root) log_root=$2; shift 2 ;;
    --ssh) ssh_cmd=$2; shift 2 ;;
    --wrapper) wrapper=$2; shift 2 ;;
    --model) model=$2; shift 2 ;;
    --hf-source) hf_source=$2; shift 2 ;;
    --prepare-tiny-model) prepare_tiny_model=$2; shift 2 ;;
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
    --single-first) run_single_first=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

IFS=, read -r -a host_list <<< "$hosts"
if ((${#host_list[@]} != 2)); then
  echo "expected exactly two hosts, got: $hosts" >&2
  exit 2
fi
head_host=${host_list[0]}
worker_host=${host_list[1]}

require_dir() {
  local name=$1
  local path=$2
  if [[ -z "$path" || ! -d "$path" ]]; then
    echo "$name is required and must be a directory: ${path:-<unset>}" >&2
    exit 2
  fi
}

require_dir "wrapper" "$wrapper"
require_dir "rccl install" "$rccl_install"
require_dir "rocshmem install" "$rocshmem_install"

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
  echo "wrapper=$wrapper"
  echo "python=$python"
  echo "vllm=$vllm"
  echo "ray=$ray"
  echo "rccl_install=$rccl_install"
  echo "rocshmem_install=$rocshmem_install"
  echo "model=$model"
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
config.torch_dtype = "float16"
config.save_pretrained(out)
print(f"model={out}")
print(f"vocab_size={config.vocab_size} pad_token_id={config.pad_token_id}")
PY
fi

for host in "${host_list[@]}"; do
  "$ssh_cmd" -o BatchMode=yes "$host" "test -d $(printf '%q' "$model")"
done

remote_env() {
  printf 'export LD_LIBRARY_PATH=%q:%q:${LD_LIBRARY_PATH:-}; ' \
    "$rccl_install/lib" "$rocshmem_install/lib"
  printf 'export GLOO_SOCKET_IFNAME=%q NCCL_SOCKET_IFNAME=%q RCCL_SOCKET_IFNAME=%q; ' \
    "$iface" "$iface" "$iface"
  printf 'export RCCL_ROCSHMEM_ENABLE=1 RCCL_ROCSHMEM_FORCE_ENABLE=1 RCCL_ROCSHMEM_THRESHOLD=%q RCCL_ROCSHMEM_HOST_STREAM_TIMING=1; ' \
    "$threshold"
  printf 'export ROCSHMEM_GDA_PROVIDER=ib ROCSHMEM_GDA_ENABLE_DMABUF=1 ROCSHMEM_GDA_USB4_A2A_CHUNK_BYTES=%q ROCSHMEM_GDA_USB4_ALLTOALL_MODE=0 ROCSHMEM_GDA_USB4_ALLTOALL_ACK=0; ' \
    "$chunk_bytes"
  printf 'export ROCSHMEM_GDA_QP_TIMEOUT=14 ROCSHMEM_GDA_QP_RETRY_CNT=7 ROCSHMEM_GDA_QP_RNR_RETRY=7 ROCSHMEM_NUM_SYM_BUF=%q ROCSHMEM_HEAP_SIZE=%q; ' \
    "$num_sym_buf" "$heap_size"
  printf 'export VLLM_NO_USAGE_STATS=1 VLLM_DO_NOT_TRACK=1 RAY_USAGE_STATS_ENABLED=0 NCCL_DEBUG=ERROR; '
}

stop_ray() {
  local host=$1
  local log=$2
  "$ssh_cmd" -o BatchMode=yes "$host" \
    "$(remote_env) $(printf '%q' "$ray") stop --force 2>&1 | tail -200" > "$log" 2>&1 || true
}

cleanup() {
  stop_ray "$worker_host" "$log_root/ray-stop-worker.log"
  stop_ray "$head_host" "$log_root/ray-stop-head.log"
}
trap cleanup EXIT

stop_ray "$worker_host" "$log_root/ray-stop-worker-before.log"
stop_ray "$head_host" "$log_root/ray-stop-head-before.log"

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
  "$ssh_cmd" -o BatchMode=yes "$head_host" "$(run_vllm_cmd "$single_root" "")" \
    | tee "$single_root.results.json"
fi

"$ssh_cmd" -o BatchMode=yes "$head_host" \
  "$(remote_env) $(printf '%q' "$ray") start --head --node-ip-address=$(printf '%q' "$head_host") --port=6379 --num-gpus=1 --disable-usage-stats" \
  > "$log_root/ray-head-start.log" 2>&1
"$ssh_cmd" -o BatchMode=yes "$worker_host" \
  "$(remote_env) $(printf '%q' "$ray") start --address=$(printf '%q' "$head_host:6379") --node-ip-address=$(printf '%q' "$worker_host") --num-gpus=1 --disable-usage-stats" \
  > "$log_root/ray-worker-start.log" 2>&1
"$ssh_cmd" -o BatchMode=yes "$head_host" \
  "$(remote_env) $(printf '%q' "$ray") status" > "$log_root/ray-status-before.log" 2>&1

tp_root=$log_root/tp2
"$ssh_cmd" -o BatchMode=yes "$head_host" \
  "$(run_vllm_cmd "$tp_root" "--distributed-executor-backend ray --tensor-parallel-size $tp_size")" \
  | tee "$tp_root.results.json"

echo "vLLM smoke complete: log_root=$log_root"
