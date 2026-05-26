#!/usr/bin/env bash
# Benchmark the fixed vLLM model set across usb4_rdma, TCP, thunderbolt-net,
# rxe-over-thunderbolt-net, and single-device mode.
#
# Default axes:
#   models: Qwen3-0.6B, SmolLM3-3B, Qwen3-4B, Olmo-7B, Llama-3.1-8B
#   transports: solo lan_tcp usb4_rdma tb_tcp tb_rxe
#   concurrencies: 1 4 16 64 256
#
# By default the script runs a deterministic random subset. Use
# SAMPLE_CASES=0 for the full Cartesian product. The selected plan is saved
# beside the output CSV.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=${REPO_DIR:-$(cd -- "$SCRIPT_DIR/../.." && pwd)}

find_prefix_for() {
  local tool=$1 path
  path=$(command -v "$tool" 2>/dev/null || true)
  [[ -n "$path" ]] || return 1
  cd -- "$(dirname -- "$path")/.." && pwd
}

find_bin_dir_for() {
  local tool=$1 path
  path=$(command -v "$tool" 2>/dev/null || true)
  [[ -n "$path" ]] || return 1
  cd -- "$(dirname -- "$path")" && pwd
}

VLLM_ENV=${VLLM_ENV:-$(find_prefix_for vllm || true)}
GCCBIN=${GCCBIN:-$(find_bin_dir_for gcc || true)}
RDMA=${RDMA:-$(find_prefix_for ibv_devices || true)}
ROCR=${ROCR:-${RCCL_ROCR_PATH:-}}
ROCR=${ROCR%/}
if [[ -z "$VLLM_ENV" || ! -x "$VLLM_ENV/bin/vllm" || ! -x "$VLLM_ENV/bin/ray" ]]; then
  echo "VLLM_ENV is not set to a vLLM environment; use the Nix-generated vllm-transport-matrix wrapper or export VLLM_ENV" >&2
  exit 2
fi
if [[ -z "$GCCBIN" || ! -x "$GCCBIN/gcc" ]]; then
  echo "GCCBIN is not set to a directory containing gcc; use the Nix-generated vllm-transport-matrix wrapper or export GCCBIN" >&2
  exit 2
fi
if [[ -z "$RDMA" || ! -d "$RDMA/lib" ]]; then
  echo "RDMA is not set to an rdma-core prefix; use the Nix-generated vllm-transport-matrix wrapper or export RDMA" >&2
  exit 2
fi
if [[ -n "$ROCR" && ! -d "$ROCR" ]]; then
  echo "ROCR is set but is not a directory: $ROCR" >&2
  exit 2
fi
RDMA_BIN=${RDMA_BIN:-/run/current-system/sw/bin/rdma}
if [[ ! -x "$RDMA_BIN" ]]; then
  RDMA_BIN=$(command -v rdma || echo rdma)
fi

SSH_HOST=${SSH_HOST:-strix-2}
LAN_IFNAME=${LAN_IFNAME:-br0.lan}
REMOTE_LAN_IFNAME=${REMOTE_LAN_IFNAME:-$LAN_IFNAME}
TB_IFNAME=${TB_IFNAME:-thunderbolt0}
REMOTE_TB_IFNAME=${REMOTE_TB_IFNAME:-$TB_IFNAME}
USB4_HCA=${USB4_HCA:-usb4_rdma2,usb4_rdma3}
RXE_HCA=${RXE_HCA:-rxe_tb}
PORT=${PORT:-8000}

MODELS=${MODELS:-"Qwen/Qwen3-0.6B HuggingFaceTB/SmolLM3-3B Qwen/Qwen3-4B-Instruct-2507 allenai/Olmo-3-7B-Instruct unsloth/Meta-Llama-3.1-8B-Instruct"}
TRANSPORTS=${TRANSPORTS:-"solo lan_tcp usb4_rdma tb_tcp tb_rxe"}
CONCURRENCIES=${CONCURRENCIES:-"1 4 16 64 256"}
MAX_TOKENS=${MAX_TOKENS:-64}
MAX_MODEL_LEN=${MAX_MODEL_LEN:-2048}
MAX_NUM_SEQS=${MAX_NUM_SEQS:-512}
MAX_NUM_BATCHED_TOKENS=${MAX_NUM_BATCHED_TOKENS:-}
PROMPT_SECTIONS=${PROMPT_SECTIONS:-24}
IGNORE_EOS=${IGNORE_EOS:-true}
VLLM_EXTRA_ARGS=${VLLM_EXTRA_ARGS:-}
VLLM_ENFORCE_EAGER=${VLLM_ENFORCE_EAGER:-1}
VLLM_DISABLE_CUSTOM_ALL_REDUCE=${VLLM_DISABLE_CUSTOM_ALL_REDUCE:-1}
NCCL_DEBUG_LEVEL=${NCCL_DEBUG_LEVEL:-${NCCL_DEBUG:-WARN}}
CLIENT_TIMEOUT_S=${CLIENT_TIMEOUT_S:-900}
STARTUP_TIMEOUT_S=${STARTUP_TIMEOUT_S:-360}
SAMPLE_CASES=${SAMPLE_CASES:-30}
RANDOM_SEED=${RANDOM_SEED:-20260502}
ALLOW_MODPROBE_TB=${ALLOW_MODPROBE_TB:-0}
SETUP_RXE=${SETUP_RXE:-1}
DRY_RUN=${DRY_RUN:-0}
RESUME=${RESUME:-0}

RUN_ID=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
LOG_DIR=${LOG_DIR:-/tmp/usb4-rdma-vllm-matrix/$RUN_ID}
OUTPUT_CSV=${OUTPUT_CSV:-$REPO_DIR/results/vllm-transport-matrix-$RUN_ID.csv}
PLAN_FILE=${PLAN_FILE:-$LOG_DIR/plan.tsv}
PROMPT_FILE=${PROMPT_FILE:-$LOG_DIR/long-prompt.txt}

VLLM_PID=
CURRENT_LOG=
CURRENT_TRANSPORT=
CURRENT_MODEL=

read -r -a VLLM_EXTRA_ARGS_ARY <<< "$VLLM_EXTRA_ARGS"
VLLM_LIMIT_ARGS=(--max-model-len "$MAX_MODEL_LEN" --max-num-seqs "$MAX_NUM_SEQS")
if [[ -n "$MAX_NUM_BATCHED_TOKENS" ]]; then
  VLLM_LIMIT_ARGS+=(--max-num-batched-tokens "$MAX_NUM_BATCHED_TOKENS")
fi
VLLM_MODE_ARGS=()
if [[ "$VLLM_ENFORCE_EAGER" == "1" ]]; then
  VLLM_MODE_ARGS+=(--enforce-eager)
fi
if [[ "$VLLM_DISABLE_CUSTOM_ALL_REDUCE" == "1" ]]; then
  VLLM_MODE_ARGS+=(--disable-custom-all-reduce)
fi

mkdir -p "$LOG_DIR" "$(dirname -- "$OUTPUT_CSV")"

if [[ ! -s "$PROMPT_FILE" ]]; then
  "$VLLM_ENV/bin/python" - "$PROMPT_FILE" "$PROMPT_SECTIONS" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
sections = int(sys.argv[2])
if sections < 1:
    raise SystemExit("PROMPT_SECTIONS must be >= 1")
text = []
for i in range(1, sections + 1):
    text.append(
        f"Section {i:03d}: The system connects two compact workstations over "
        "USB4 and evaluates tensor parallel inference. Each request contains "
        "a technical design note with latency, bandwidth, memory bandwidth, "
        "completion queue, and transport observations. "
    )
text.append("Summarize the design tradeoffs, identify bottlenecks, and give practical next steps.")
path.write_text("".join(text))
PY
fi

iface_ipv4() {
  local iface=$1
  ip -o -4 addr show dev "$iface" 2>/dev/null | awk '{split($4,a,"/"); print a[1]; exit}'
}

remote_iface_ipv4() {
  local iface=$1
  ssh -n -o ConnectTimeout=5 "$SSH_HOST" \
    "ip -o -4 addr show dev '$iface' 2>/dev/null | awk '{split(\$4,a,\"/\"); print a[1]; exit}'"
}

have_iface_both() {
  local iface=$1
  [[ -n "$(iface_ipv4 "$iface")" ]] && [[ -n "$(remote_iface_ipv4 "$iface")" ]]
}

have_iface_pair() {
  local local_iface=$1 remote_iface=$2
  [[ -d "/sys/class/net/$local_iface" ]] &&
    ssh -n -o ConnectTimeout=5 "$SSH_HOST" "test -d /sys/class/net/'$remote_iface'"
}

stop_ray() {
  "$VLLM_ENV/bin/ray" stop --force >/dev/null 2>&1 || true
  ssh -n -o ConnectTimeout=5 "$SSH_HOST" "$VLLM_ENV/bin/ray stop --force" >/dev/null 2>&1 || true
}

stop_server() {
  local status=${1:-0}
  if [[ -n "${VLLM_PID:-}" ]]; then
    kill "$VLLM_PID" 2>/dev/null || true
    wait "$VLLM_PID" 2>/dev/null || true
    VLLM_PID=
  fi
  stop_ray
  if [[ "$status" -ne 0 && -n "${CURRENT_LOG:-}" && -s "$CURRENT_LOG" ]]; then
    echo "recent vLLM log ($CURRENT_LOG):" >&2
    tail -120 "$CURRENT_LOG" >&2 || true
  fi
}

cleanup() {
  local status=$?
  trap - EXIT INT TERM
  stop_server "$status"
  exit "$status"
}
trap cleanup EXIT INT TERM

tune_env=()
TUNE_VARS=(
  NCCL_P2P_NET_CHUNKSIZE
  NCCL_CHUNK_SIZE
  NCCL_BUFFSIZE
  NCCL_P2P_LL_THRESHOLD
  RCCL_P2P_NET_THRESHOLD
  NCCL_NCHANNELS_PER_NET_PEER
  NCCL_NCHANNELS_PER_PEER
  NCCL_MIN_P2P_NCHANNELS
  NCCL_MAX_P2P_NCHANNELS
  NCCL_MIN_NCHANNELS
  NCCL_MAX_NCHANNELS
  NCCL_IB_QPS_PER_CONNECTION
  NCCL_IB_SPLIT_DATA_ON_QPS
  NCCL_IB_GID_INDEX
  NCCL_NET_MERGE_LEVEL
  NCCL_DEBUG_SUBSYS
  NCCL_PROTO
  NCCL_NET_SHARED_BUFFERS
  NCCL_NET_SHARED_COMMS
  RCCL_P2P_BATCH_ENABLE
  RCCL_P2P_BATCH_THRESHOLD
  HSA_NO_SCRATCH_RECLAIM
  HSA_ENABLE_INTERRUPT
  CUDA_VISIBLE_DEVICES
  HIP_VISIBLE_DEVICES
  ROCR_VISIBLE_DEVICES
  RAY_ACCEL_ENV_VAR_OVERRIDE_ON_ZERO
  RAY_EXPERIMENTAL_NOSET_CUDA_VISIBLE_DEVICES
  RAY_EXPERIMENTAL_NOSET_HIP_VISIBLE_DEVICES
  RAY_EXPERIMENTAL_NOSET_ROCR_VISIBLE_DEVICES
  RAY_CGRAPH_get_timeout
  RAY_DEDUP_LOGS
  VLLM_ROCM_USE_AITER
  VLLM_ROCM_SHUFFLE_KV_CACHE_LAYOUT
  VLLM_SLEEP_WHEN_IDLE
  VLLM_USE_DEEP_GEMM
  VLLM_USE_FLASHINFER_SAMPLER
  VLLM_USE_FLASHINFER_MOE_FP16
  OMP_NUM_THREADS
  TOKENIZERS_PARALLELISM
  TORCHDYNAMO_DISABLE
  SAFETENSORS_FAST_GPU
  VLLM_USE_RAY_COMPILED_DAG
  VLLM_USE_RAY_COMPILED_DAG_CHANNEL_TYPE
  VLLM_USE_RAY_COMPILED_DAG_OVERLAP_COMM
  VLLM_USE_RAY_WRAPPED_PP_COMM
  VLLM_USE_RAY_V2_EXECUTOR_BACKEND
  VLLM_USE_RAY_SPMD_WORKER
  NCCL_GDRCOPY_ENABLE
  NCCL_GDRCOPY_FIFO_ENABLE
  NCCL_GDRCOPY_SYNC_ENABLE
  NCCL_GDRCOPY_FLUSH_ENABLE
  NCCL_NET_GDR_LEVEL
  NCCL_NET_GDR_READ
)
for name in "${TUNE_VARS[@]}"; do
  if [[ -v "$name" ]]; then
    tune_env+=("$name=${!name}")
  fi
done

remote_tune_env() {
  local item
  for item in "${tune_env[@]}"; do
    printf ' %q' "$item"
  done
}

remote_rocr_env() {
  if [[ -n "${RCCL_ROCR_PATH:-}" ]]; then
    printf ' RCCL_ROCR_PATH=%q' "$RCCL_ROCR_PATH"
  fi
}

rail_map_hcas() {
  local rdma_bin=${RDMA}/bin/ibv_devices

  if [[ -x "$rdma_bin" ]]; then
    "$rdma_bin" | awk '$1 ~ /^usb4_rdma/ {print $1}'
    return
  fi
  sudo -n cat /sys/kernel/debug/usb4_rdma/data/rail_map 2>/dev/null |
    awk 'NR > 1 && $1 ~ /^usb4_rdma/ {print $1}'
}

remote_rail_map_hcas() {
  ssh -n -o ConnectTimeout=5 "$SSH_HOST" \
    "if [ -x '$RDMA/bin/ibv_devices' ]; then '$RDMA/bin/ibv_devices' | awk '\$1 ~ /^usb4_rdma/ {print \$1}'; else sudo -n cat /sys/kernel/debug/usb4_rdma/data/rail_map 2>/dev/null | awk 'NR > 1 && \$1 ~ /^usb4_rdma/ {print \$1}'; fi"
}

resolve_usb4_hca() {
  local local_hcas remote_hcas resolved hca

  if [[ "$USB4_HCA" != "auto" ]]; then
    printf '%s\n' "$USB4_HCA"
    return
  fi

  local_hcas=$(rail_map_hcas || true)
  remote_hcas=$(remote_rail_map_hcas || true)
  resolved=
  while IFS= read -r hca; do
    [[ -n "$hca" ]] || continue
    if grep -qxF "$hca" <<<"$remote_hcas"; then
      resolved+="${resolved:+,}$hca"
    fi
  done <<<"$local_hcas"
  if [[ -z "$resolved" ]]; then
    echo "USB4_HCA=auto failed: no common HCAs in local/remote rail_map" >&2
    echo "local:" >&2
    printf '%s\n' "$local_hcas" >&2
    echo "remote:" >&2
    printf '%s\n' "$remote_hcas" >&2
    return 1
  fi
  printf '%s\n' "$resolved"
}

make_plan() {
  "$VLLM_ENV/bin/python" - "$PLAN_FILE" "$SAMPLE_CASES" "$RANDOM_SEED" \
    "$MODELS" "$TRANSPORTS" "$CONCURRENCIES" "$MAX_TOKENS" "$RESUME" "$OUTPUT_CSV" <<'PY'
import csv
import itertools
import random
import sys
from pathlib import Path

path = Path(sys.argv[1])
sample_cases = int(sys.argv[2])
seed = int(sys.argv[3])
models = sys.argv[4].split()
transports = sys.argv[5].split()
concs = [int(x) for x in sys.argv[6].split()]
max_tokens = int(sys.argv[7])
resume = sys.argv[8] == "1"
csv_path = Path(sys.argv[9])

done = set()
if resume and csv_path.exists():
    raw = csv_path.read_bytes().replace(b"\0", b"")
    text = raw.decode("utf-8", "replace")
    rows = csv.DictReader(line for line in text.splitlines() if line.strip())
    for row in rows:
        if row.get("status") not in {"ok", "skipped"}:
            continue
        try:
            done.add((
                row["transport"],
                row["model"],
                int(row["concurrency"]),
                int(row["max_tokens"]),
            ))
        except (KeyError, TypeError, ValueError):
            pass

cases = [
    (transport, model, conc, max_tokens)
    for transport, model, conc in itertools.product(transports, models, concs)
]
if done:
    cases = [case for case in cases if case not in done]
if sample_cases > 0 and sample_cases < len(cases):
    rnd = random.Random(seed)
    cases = rnd.sample(cases, sample_cases)

transport_order = {name: i for i, name in enumerate(transports)}
model_order = {name: i for i, name in enumerate(models)}
cases.sort(key=lambda c: (transport_order[c[0]], model_order[c[1]], c[2]))

with path.open("w") as fh:
    for case in cases:
        fh.write("\t".join(map(str, case)) + "\n")
PY
}

append_skip() {
  local transport=$1 model=$2 conc=$3 max_tokens=$4 parallelism=$5 reason=$6 socket_ifname=${7:-} rdma_hca=${8:-}
  "$VLLM_ENV/bin/python" "$SCRIPT_DIR/vllm-stream-client.py" \
    --csv "$OUTPUT_CSV" \
    --endpoint "http://127.0.0.1:$PORT/v1/completions" \
    --model "$model" \
    --transport "$transport" \
    --parallelism "$parallelism" \
    --concurrency "$conc" \
    --max-tokens "$max_tokens" \
    --prompt-file "$PROMPT_FILE" \
    --run-id "$RUN_ID" \
    --socket-ifname "$socket_ifname" \
    --rdma-hca "$rdma_hca" \
    --vllm-extra-args="$VLLM_EXTRA_ARGS" \
    --server-log "$CURRENT_LOG" \
    --timeout "$CLIENT_TIMEOUT_S" \
    --skip-reason "$reason"
}

wait_ready() {
  local log=$1
  local waited=0
  while (( waited < STARTUP_TIMEOUT_S )); do
    sleep 5
    waited=$((waited + 5))
    if grep -q "Application startup complete" "$log"; then
      echo "  ready in ${waited}s"
      return 0
    fi
    if [[ -n "${VLLM_PID:-}" ]] && ! kill -0 "$VLLM_PID" 2>/dev/null; then
      echo "vLLM exited before startup completed" >&2
      return 1
    fi
  done
  echo "vLLM did not become ready within ${STARTUP_TIMEOUT_S}s" >&2
  return 1
}

warmup() {
  "$VLLM_ENV/bin/python" - "$CURRENT_MODEL" "$IGNORE_EOS" <<'PY' | \
    curl --fail-with-body -sS --max-time "$CLIENT_TIMEOUT_S" "http://127.0.0.1:8000/v1/completions" \
      -H 'Content-Type: application/json' -d @- >/dev/null
import json
import sys

model, ignore_eos = sys.argv[1:]
print(json.dumps({
    "model": model,
    "prompt": "Hi",
    "max_tokens": 4,
    "temperature": 0,
    "ignore_eos": ignore_eos == "true",
}))
PY
}

transport_parallelism() {
  case "$1" in
    solo) echo "solo" ;;
    *) echo "tp2" ;;
  esac
}

ensure_thunderbolt_net() {
  if have_iface_pair "$TB_IFNAME" "$REMOTE_TB_IFNAME"; then
    return 0
  fi
  if [[ "$ALLOW_MODPROBE_TB" != "1" ]]; then
    return 1
  fi
  sudo modprobe thunderbolt_net || true
  ssh -n -o ConnectTimeout=5 "$SSH_HOST" "sudo modprobe thunderbolt_net || true" || true
  sleep 8
  have_iface_pair "$TB_IFNAME" "$REMOTE_TB_IFNAME"
}

rdma_link_exists() {
  local hca=$1
  "$RDMA_BIN" link show 2>/dev/null | grep -q "link $hca/"
}

remote_rdma_link_exists() {
  local hca=$1
  ssh -n -o ConnectTimeout=5 "$SSH_HOST" \
    "'$RDMA_BIN' link show 2>/dev/null | grep -q 'link $hca/'"
}

ensure_rxe() {
  if [[ "$SETUP_RXE" != "1" ]]; then
    rdma_link_exists "$RXE_HCA" && remote_rdma_link_exists "$RXE_HCA"
    return
  fi

  sudo modprobe rdma_rxe || true
  ssh -n -o ConnectTimeout=5 "$SSH_HOST" "sudo modprobe rdma_rxe || true" || true

  if ! rdma_link_exists "$RXE_HCA"; then
    sudo "$RDMA_BIN" link add "$RXE_HCA" type rxe netdev "$TB_IFNAME"
  fi
  if ! remote_rdma_link_exists "$RXE_HCA"; then
    ssh -n -o ConnectTimeout=5 "$SSH_HOST" \
      "sudo '$RDMA_BIN' link add '$RXE_HCA' type rxe netdev '$REMOTE_TB_IFNAME'"
  fi
}

start_server() {
  local transport=$1 model=$2
  local safe_model=${model//\//_}
  local socket_ifname= remote_socket_ifname= rdma_hca= ib_disable= head= worker= ld_path= parallelism

  parallelism=$(transport_parallelism "$transport")
  CURRENT_TRANSPORT=$transport
  CURRENT_MODEL=$model
  CURRENT_LOG="$LOG_DIR/${transport}-${safe_model}.log"

  stop_server 0
  mkdir -p /tmp/aiter-jit
  ssh -n -o ConnectTimeout=5 "$SSH_HOST" 'mkdir -p /tmp/aiter-jit' >/dev/null 2>&1 || true

  case "$transport" in
    solo)
      socket_ifname=""
      rdma_hca=""
      ib_disable=1
      ;;
    lan_tcp)
      socket_ifname=$LAN_IFNAME
      remote_socket_ifname=$REMOTE_LAN_IFNAME
      head=${HEAD:-$(iface_ipv4 "$LAN_IFNAME")}
      worker=${WORKER:-$(remote_iface_ipv4 "$REMOTE_LAN_IFNAME")}
      rdma_hca=""
      ib_disable=1
      ;;
    usb4_rdma)
      socket_ifname=$LAN_IFNAME
      remote_socket_ifname=$REMOTE_LAN_IFNAME
      head=${HEAD:-$(iface_ipv4 "$LAN_IFNAME")}
      worker=${WORKER:-$(remote_iface_ipv4 "$REMOTE_LAN_IFNAME")}
      rdma_hca=$USB4_HCA
      ib_disable=0
      ld_path=$RDMA/lib
      ;;
    tb_tcp)
      ensure_thunderbolt_net || return 10
      socket_ifname=$TB_IFNAME
      remote_socket_ifname=$REMOTE_TB_IFNAME
      head=${TB_HEAD:-$(iface_ipv4 "$TB_IFNAME")}
      worker=${TB_WORKER:-$(remote_iface_ipv4 "$REMOTE_TB_IFNAME")}
      rdma_hca=""
      ib_disable=1
      ;;
    tb_rxe)
      ensure_thunderbolt_net || return 10
      ensure_rxe
      socket_ifname=$TB_IFNAME
      remote_socket_ifname=$REMOTE_TB_IFNAME
      head=${TB_HEAD:-$(iface_ipv4 "$TB_IFNAME")}
      worker=${TB_WORKER:-$(remote_iface_ipv4 "$REMOTE_TB_IFNAME")}
      rdma_hca=$RXE_HCA
      ib_disable=0
      ld_path=$RDMA/lib
      ;;
    *)
      echo "unknown transport: $transport" >&2
      return 2
      ;;
  esac

  if [[ "$transport" != "solo" ]]; then
    if [[ -z "$head" || -z "$worker" ]]; then
      echo "missing IP for transport=$transport iface=$socket_ifname" >&2
      return 10
    fi
  fi

  if [[ -n "$ld_path" && -n "$ROCR" ]]; then
    ld_path=$ld_path:$ROCR
    export RCCL_ROCR_PATH=${RCCL_ROCR_PATH:-$ROCR/}
  fi

  echo
  echo "=== transport=$transport parallelism=$parallelism model=$model ==="
  printf '  prompt-chars: %d\n' "$(wc -c <"$PROMPT_FILE")"
  if [[ -n "$socket_ifname" ]]; then
    echo "  socket-ifname: $socket_ifname remote=$remote_socket_ifname head=$head worker=$worker"
  fi
  if [[ -n "$rdma_hca" ]]; then
    echo "  rdma-hca: $rdma_hca"
    echo "  rocr: ${ROCR:-}"
  fi
  if [[ "${#VLLM_EXTRA_ARGS_ARY[@]}" -gt 0 ]]; then
    printf '  vllm-extra:'
    printf ' %s' "${VLLM_EXTRA_ARGS_ARY[@]}"
    printf '\n'
  fi
  if [[ "${#VLLM_MODE_ARGS[@]}" -gt 0 ]]; then
    printf '  vllm-mode:'
    printf ' %s' "${VLLM_MODE_ARGS[@]}"
    printf '\n'
  fi

  >"$CURRENT_LOG"

  if [[ "$transport" == "solo" ]]; then
    env "${tune_env[@]}" \
      PATH=$GCCBIN:$PATH \
      AITER_JIT_DIR=/tmp/aiter-jit CC=$GCCBIN/gcc NCCL_IB_DISABLE=1 \
      "$VLLM_ENV/bin/vllm" serve "$model" \
        --host 0.0.0.0 --port "$PORT" \
        "${VLLM_LIMIT_ARGS[@]}" \
        "${VLLM_MODE_ARGS[@]}" \
        "${VLLM_EXTRA_ARGS_ARY[@]}" \
        >>"$CURRENT_LOG" 2>&1 &
    VLLM_PID=$!
  else
    env "${tune_env[@]}" \
      PATH=$GCCBIN:$PATH LD_LIBRARY_PATH=${ld_path:-} \
      VLLM_HOST_IP=$head NCCL_SOCKET_IFNAME=$socket_ifname GLOO_SOCKET_IFNAME=$socket_ifname \
      NCCL_IB_HCA=$rdma_hca NCCL_IB_DISABLE=$ib_disable NCCL_DEBUG=$NCCL_DEBUG_LEVEL \
      RCCL_ROCR_PATH=${RCCL_ROCR_PATH:-} \
      AITER_JIT_DIR=/tmp/aiter-jit \
      "$VLLM_ENV/bin/ray" start --head --port=6379 --node-ip-address="$head" \
        --num-gpus=1 --include-dashboard=false --disable-usage-stats >/dev/null 2>&1
    ssh -n -o ConnectTimeout=5 "$SSH_HOST" \
      "env$(remote_tune_env)$(remote_rocr_env) PATH=$GCCBIN:\$PATH LD_LIBRARY_PATH=$(printf '%q' "${ld_path:-}") VLLM_HOST_IP=$worker NCCL_SOCKET_IFNAME=$remote_socket_ifname GLOO_SOCKET_IFNAME=$remote_socket_ifname NCCL_IB_HCA=$rdma_hca NCCL_IB_DISABLE=$ib_disable NCCL_DEBUG=$NCCL_DEBUG_LEVEL AITER_JIT_DIR=/tmp/aiter-jit $VLLM_ENV/bin/ray start --address=$head:6379 --node-ip-address=$worker --num-gpus=1 --disable-usage-stats" >/dev/null 2>&1
    sleep 2

    env "${tune_env[@]}" \
      PATH=$GCCBIN:$PATH LD_LIBRARY_PATH=${ld_path:-} \
      VLLM_HOST_IP=$head NCCL_SOCKET_IFNAME=$socket_ifname GLOO_SOCKET_IFNAME=$socket_ifname \
      NCCL_IB_HCA=$rdma_hca NCCL_IB_DISABLE=$ib_disable NCCL_DEBUG=$NCCL_DEBUG_LEVEL \
      RCCL_ROCR_PATH=${RCCL_ROCR_PATH:-} \
      AITER_JIT_DIR=/tmp/aiter-jit CC=$GCCBIN/gcc \
      "$VLLM_ENV/bin/vllm" serve "$model" \
        --tensor-parallel-size 2 --distributed-executor-backend ray \
        --host 0.0.0.0 --port "$PORT" \
        "${VLLM_LIMIT_ARGS[@]}" \
        "${VLLM_MODE_ARGS[@]}" \
        "${VLLM_EXTRA_ARGS_ARY[@]}" \
        >>"$CURRENT_LOG" 2>&1 &
    VLLM_PID=$!
  fi

  wait_ready "$CURRENT_LOG"
  warmup
  echo "  --- transport log sample ---"
  grep -iE "NET/Socket|NET/IB|$USB4_HCA|$RXE_HCA|thunderbolt|P2P Chunksize|Channel [0-9]|fall.*back" \
    "$CURRENT_LOG" | head -25 || true
}

run_case() {
  local transport=$1 model=$2 conc=$3 max_tokens=$4 parallelism socket_ifname= rdma_hca=
  parallelism=$(transport_parallelism "$transport")

  case "$transport" in
    lan_tcp) socket_ifname="$LAN_IFNAME/$REMOTE_LAN_IFNAME" ;;
    usb4_rdma) socket_ifname="$LAN_IFNAME/$REMOTE_LAN_IFNAME"; rdma_hca=$(resolve_usb4_hca) ;;
    tb_tcp) socket_ifname="$TB_IFNAME/$REMOTE_TB_IFNAME" ;;
    tb_rxe) socket_ifname="$TB_IFNAME/$REMOTE_TB_IFNAME"; rdma_hca=$RXE_HCA ;;
  esac

  "$VLLM_ENV/bin/python" "$SCRIPT_DIR/vllm-stream-client.py" \
    --csv "$OUTPUT_CSV" \
    --endpoint "http://127.0.0.1:$PORT/v1/completions" \
    --model "$model" \
    --transport "$transport" \
    --parallelism "$parallelism" \
    --concurrency "$conc" \
    --max-tokens "$max_tokens" \
    --prompt-file "$PROMPT_FILE" \
    --ignore-eos="$IGNORE_EOS" \
    --run-id "$RUN_ID" \
    --socket-ifname "$socket_ifname" \
    --rdma-hca "$rdma_hca" \
    --vllm-extra-args="$VLLM_EXTRA_ARGS" \
    --server-log "$CURRENT_LOG" \
    --timeout "$CLIENT_TIMEOUT_S"
}

make_plan

echo "run-id: $RUN_ID"
echo "plan: $PLAN_FILE"
echo "csv: $OUTPUT_CSV"
echo "logs: $LOG_DIR"
echo "sample-cases: $SAMPLE_CASES seed=$RANDOM_SEED"
echo "resume: $RESUME"
if [[ "$DRY_RUN" == "1" ]]; then
  echo
  echo "dry-run plan:"
  column -t -s $'\t' "$PLAN_FILE" || cat "$PLAN_FILE"
  exit 0
fi

last_key=
start_failed_reason=
while IFS=$'\t' read -r transport model conc max_tokens; do
  key="$transport	$model"
  parallelism=$(transport_parallelism "$transport")

  if [[ "$key" != "$last_key" ]]; then
    start_failed_reason=
    if ! start_server "$transport" "$model"; then
      start_failed_reason="server-start-failed-or-transport-unavailable"
      echo "  skipping transport=$transport model=$model: $start_failed_reason" >&2
      stop_server 1
    fi
    last_key=$key
  fi

  if [[ -n "$start_failed_reason" ]]; then
    case "$transport" in
      lan_tcp) append_skip "$transport" "$model" "$conc" "$max_tokens" "$parallelism" "$start_failed_reason" "$LAN_IFNAME" "" ;;
      usb4_rdma) append_skip "$transport" "$model" "$conc" "$max_tokens" "$parallelism" "$start_failed_reason" "$LAN_IFNAME" "$USB4_HCA" ;;
      tb_tcp) append_skip "$transport" "$model" "$conc" "$max_tokens" "$parallelism" "$start_failed_reason" "$TB_IFNAME" "" ;;
      tb_rxe) append_skip "$transport" "$model" "$conc" "$max_tokens" "$parallelism" "$start_failed_reason" "$TB_IFNAME" "$RXE_HCA" ;;
      *) append_skip "$transport" "$model" "$conc" "$max_tokens" "$parallelism" "$start_failed_reason" "" "" ;;
    esac
    continue
  fi

  run_case "$transport" "$model" "$conc" "$max_tokens"
done < "$PLAN_FILE"

stop_server 0
echo
echo "wrote $OUTPUT_CSV"
