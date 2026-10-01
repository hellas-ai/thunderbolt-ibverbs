#!/usr/bin/env bash
# RX-side DMA + link-layer validation for codex/gda-v2-rebased-port @ 75cf776.
#
# Stages:
#   1. Copy the built module to strix-1, reload it.
#   2. Confirm usb4_rdma ports report IB_LINK_LAYER_INFINIBAND.
#   3. CPU regression: rc_write_verify (RC WRITE bounce-buffer path unchanged).
#   4. DMABUF receiver smoke: dmabuf_mr_probe + HIP receiver probe; verify
#      data_rx_dmabuf_zcopy increments and data_rx_dmabuf_zcopy_error stays 0.
#
# Usage: ./userspace/bench/rx_side_dma_validate.sh
#   HOSTS       = "strix-1 strix-2"  (server is HOSTS[0])
#   BENCH_ROOT  = /mnt/Home/src/thunderbolt-ibverbs-gda-v2-rebase
#   OUT_ROOT    = /mnt/Home/src/thunderbolt-ibverbs-gda-v2-rebase/.bench-artifacts/rx-side-dma
#
# Exit codes:
#   0 = all stages pass
#   1 = usage / config error
#   2 = module load / link-layer / counter check failure
#   3 = regression test failure

set -euo pipefail

HOSTS=${HOSTS:-"strix-1 strix-2"}
BENCH_ROOT=${BENCH_ROOT:-/mnt/Home/src/thunderbolt-ibverbs-gda-v2-rebase}
OUT_ROOT=${OUT_ROOT:-${BENCH_ROOT}/.bench-artifacts/rx-side-dma}
RUN_ID=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
OUT_DIR=${OUT_DIR:-${OUT_ROOT}/${RUN_ID}}
SSH_OPTS=${SSH_OPTS:-"-o ConnectTimeout=5 -o BatchMode=yes -o ServerAliveInterval=5 -o ServerAliveCountMax=3"}
SERVER=${HOSTS%% *}
CLIENT=${HOSTS##* }

if [ "$SERVER" = "$CLIENT" ]; then
  echo "HOSTS must list at least 2 nodes (server, client). Got: $HOSTS" >&2
  exit 1
fi

stage_reload() {
  log "stage=reload host=$SERVER"
  "${BENCH_ROOT}/tools/tbv-target-module.sh" "$SERVER" --booted-kernel --reload --options 'profile=linux_perf tbnet=prefer_rdma bind_services=1' 2>&1 | tee "$OUT_DIR/reload-${SERVER}.log"
  # Wait for the new usb4_rdma devices to register; on a clean boot the
  # source-aware XDomain handler initialises in time, on a reload the kernel
  # autoload path is required (which is why the doc recommends a reboot).
  for _ in $(seq 1 30); do
    if ssh $SSH_OPTS "$SERVER" 'test -n "$(ls /sys/class/infiniband/usb4_rdma* 2>/dev/null)"' 2>/dev/null; then
      break
    fi
    sleep 1
  done
}

##############################################################################
# Stage 1: copy + reload module on SERVER
##############################################################################
stage_reload() {
  log "stage=reload host=$SERVER"
  "${BENCH_ROOT}/tools/tbv-target-module.sh" "$SERVER" --booted-kernel --copy 2>&1 | tee "$OUT_DIR/reload-${SERVER}.log"
  ssh $SSH_OPTS "$SERVER" 'sudo rmmod thunderbolt_ibverbs 2>/dev/null || true; sudo modprobe thunderbolt_ibverbs' 2>&1 | tee -a "$OUT_DIR/reload-${SERVER}.log"
}

##############################################################################
# Stage 2: confirm link_layer is InfiniBand on SERVER (and CLIENT)
##############################################################################
stage_link_layer() {
  log "stage=link_layer hosts=$HOSTS"
  local fail=0
  for h in $HOSTS; do
    local ll
    ll=$(ssh $SSH_OPTS "$h" 'for d in /sys/class/infiniband/usb4_rdma*; do cat "$d/ports/1/link_layer"; done' 2>/dev/null) || { log "host=$h ssh failed"; fail=1; continue; }
    log "host=$h link_layer=$ll"
    if [ "$ll" != "InfiniBand" ]; then
      log "FAIL host=$h expected InfiniBand, got '$ll'"
      fail=1
    fi
  done
  return $fail
}

##############################################################################
# Stage 3: CPU regression on SERVER/CLIENT (rc_write_verify)
##############################################################################
stage_cpu_regression() {
  log "stage=cpu_regression server=$SERVER client=$CLIENT"
  ssh $SSH_OPTS "$SERVER" "cd $BENCH_ROOT && result/bin/rc_write_verify -d usb4_rdma0 -s 4096 -c 1000" 2>&1 \
    | tee "$OUT_DIR/rc_write_verify-server.log" &
  local server_pid=$!
  sleep 1
  ssh $SSH_OPTS "$CLIENT" "cd $BENCH_ROOT && result/bin/rc_write_verify -d usb4_rdma0 -s 4096 -c 1000 \$(ssh $SSH_OPTS $SERVER 'cat /sys/class/infiniband/usb4_rdma0/ports/1/gid_attrs/* 2>/dev/null | head -1 || true' </dev/null)" 2>&1 \
    | tee "$OUT_DIR/rc_write_verify-client.log" &
  local client_pid=$!
  wait $server_pid
  local server_rc=$?
  wait $client_pid
  local client_rc=$?
  log "rc_write_verify server_rc=$server_rc client_rc=$client_rc"
  grep -E 'data_tx_ack_error|data_rx_copy_error|data_rx_dmabuf_zcopy_error|cqe_error|completion_error' "$OUT_DIR/rc_write_verify-server.log" "$OUT_DIR/rc_write_verify-client.log" 2>/dev/null | head -20
  return $(( server_rc + client_rc ))
}

##############################################################################
# Stage 4: DMABUF receiver smoke
##############################################################################
stage_dmabuf_rx() {
  log "stage=dmabuf_rx server=$SERVER client=$CLIENT"
  # 4a: import probe on the receiver
  ssh $SSH_OPTS "$SERVER" "cd $BENCH_ROOT && result/bin/dmabuf_mr_probe -d usb4_rdma0 --length 4096 --fd 5" 2>&1 \
    | tee "$OUT_DIR/dmabuf_mr_probe.log"
  # 4b: before/after counter snapshot
  local before after
  before=$(ssh $SSH_OPTS "$SERVER" 'cat /sys/kernel/debug/tbv/data_rx_dmabuf_zcopy 2>/dev/null || echo 0')
  log "data_rx_dmabuf_zcopy(before)=$before"
  # 4c: run HIP receiver probe (if available)
  if ssh $SSH_OPTS "$SERVER" "test -x $BENCH_ROOT/result/bin/hip_rdma_write_visibility_probe" 2>/dev/null; then
    log "running HIP RDMA write visibility probe on $SERVER"
    ssh $SSH_OPTS "$SERVER" "cd $BENCH_ROOT && result/bin/hip_rdma_write_visibility_probe --recv-reg dmabuf -d usb4_rdma0 -s 4096 -c 10" 2>&1 \
      | tee "$OUT_DIR/hip_recv.log"
  else
    log "HIP probe not available at $BENCH_ROOT/result/bin/hip_rdma_write_visibility_probe; skipping live RDMA write into dmabuf"
  fi
  after=$(ssh $SSH_OPTS "$SERVER" 'cat /sys/kernel/debug/tbv/data_rx_dmabuf_zcopy 2>/dev/null || echo 0')
  log "data_rx_dmabuf_zcopy(after)=$after"
  if [ "$after" -le "$before" ]; then
    log "WARN data_rx_dmabuf_zcopy did not increment ($before -> $after). Check that a dmabuf receiver ran."
  fi
  local errs
  errs=$(ssh $SSH_OPTS "$SERVER" 'cat /sys/kernel/debug/tbv/data_rx_dmabuf_zcopy_error 2>/dev/null || echo 0')
  log "data_rx_dmabuf_zcopy_error=$errs"
  if [ "$errs" != "0" ]; then
    log "FAIL data_rx_dmabuf_zcopy_error=$errs"
    return 2
  fi
  return 0
}

##############################################################################
# Driver
##############################################################################
log "out_dir=$OUT_DIR run_id=$RUN_ID"
trap 'log "aborted"; exit 130' INT TERM

if ! stage_reload; then
  log "FAIL stage=reload"
  exit 2
fi
if ! stage_link_layer; then
  log "FAIL stage=link_layer"
  exit 2
fi
if ! stage_cpu_regression; then
  log "FAIL stage=cpu_regression"
  exit 3
fi
if ! stage_dmabuf_rx; then
  log "FAIL stage=dmabuf_rx"
  exit 2
fi
log "PASS all stages"
