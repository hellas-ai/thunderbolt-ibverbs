# 2026-06-04 GDA Correctness Ledger

Goal: turn the current GDA workaround into a falsifiable correctness model.

## 2026-06-05 Stop Marker: Kernel Patch Stack Was Incomplete

New information supersedes the evidentiary weight of the hardware rows below:
the Strix machines may not have been booted with the full intended
Thunderbolt kernel patch stack. PR #24 (`codex/tb-upstream-patch-stack`) adds
the portable upstream Thunderbolt patch series back to this repo. Two other
pending correctness PRs are also relevant before retesting:

- PR #22 (`grw/reliability-engine-spine`): substantial module rewrite,
  reliability engine spine, configfs link model, and native RC ACK retry /
  correctness hardening.
- PR #23 (`grw/native-qpn-no-reuse`): robust native RC send teardown and QPN
  reuse/churn coverage.

Treat all rows below this marker as pre-corrected-stack baselines only. They
remain useful for hypotheses and instrumentation design, but they should not be
used as final evidence about GDA correctness until the branch is rebased onto
the PR #24 + PR #22 + PR #23 stack, the Strix hosts are rebuilt/rebooted with
that kernel/module set, and the minimal matrix is rerun from a clean boot.

## Current Known-Good Envelope

These statements are directly observed after both Strix hosts were rebooted
with `iommu=pt` and the patched module was reloaded.

1. `strix-1` and `strix-2` both boot `7.1.0-rc1`, expose `/dev/kfd` and
   `/dev/dri/renderD128`, and have two native rails in `data_ready=1`.
2. With `zcopy_min_bytes=0`, `write_zcopy=N`, `native_tx_max_inflight=8`,
   `send_ack_redundancy=3`, and `native_fragment_striping=N`, 64 KiB RDMA
   WRITE visibility from `strix-2` into `strix-1` passes for both
   `host-uncached` and `host-coherent` receiver memory.
3. In those passing 64 KiB runs, the sender completes every native TX
   descriptor it posts: `data_tx_posted=4608` and `data_tx_completed=4608`.
4. In those passing 64 KiB runs, sender WR completion is clean:
   `data_wr_send=512`, `data_wr_timeout=0`, `data_wr_copied=512`,
   `data_wr_zcopy=0`, and no verbs objects remain live after process exit.
5. Delayed duplicate ACKs can arrive after sender QP teardown. This is visible
   as `data_rx_no_qp` on the sender, but did not cause the two passing 64 KiB
   runs to fail.

## Historical Hard Facts

These are from the same revive branch before the current workaround.

1. A 64 KiB WRITE zcopy run failed with sender timeout and left raw-stream TX
   state stuck: `zcopy_inflight=3`, `raw_active=1`.
2. Disabling WRITE zcopy was not enough. A copied 64 KiB run still failed with
   all receiver WRITE frames accepted, but the sender had one missing local TX
   completion: `data_tx_posted=1962`, `data_tx_completed=1961`,
   `path_tx inflight=1`, `data_wr_timeout=1`.
3. Adding redundant immediate ACKs without TX throttling did not fix copied
   64 KiB. It increased observed ACK frames, but the sender still timed out
   and had missing TX completions.
4. Capping copied payload fragments at `TBV_NATIVE_DATA_MAX_PAYLOAD - 1` did
   not fix copied 64 KiB. It still failed with missing sender TX completions.
5. Adding `native_tx_max_inflight=8` without delayed ACK retries fixed the
   sender local TX completion leak in one run, but the WR still timed out with
   missing ACKs.

## Candidate Failure Classes

### A. WRITE zcopy/raw-stream lifetime bug

Claim: native WRITE zcopy can corrupt or stall raw-stream state independently
of ACK handling.

Predictions:

- Enabling `write_zcopy=1` with `zcopy_min_bytes=4096` should reintroduce
  `data_wr_zcopy > 0`.
- If this class is still present, a failure should show `zcopy_inflight > 0`
  or `raw_active=1` after timeout.
- If TX throttling alone fixes it, then zcopy should pass with
  `native_tx_max_inflight=8` and ACK redundancy enabled.

### B. Native TX completion pressure bug

Claim: native data TX descriptors can stop completing when too many are posted
on one path, independent of payload visibility.

Predictions:

- With `native_tx_max_inflight=0`, copied 64 KiB should be able to reproduce
  `data_tx_posted > data_tx_completed` and `path_tx inflight > 0`.
- With `native_tx_max_inflight=8`, the same copied 64 KiB run should leave
  `data_tx_posted == data_tx_completed` and `path_tx inflight=0`.
- ACK redundancy cannot repair this class; if TX completions are missing, WR
  timeout is expected even when ACK counters increase.

### C. SEND_ACK delivery loss/reorder bug

Claim: copied WRITE data reaches the receiver and local TX completes on the
sender, but the sender sometimes misses the SEND_ACK that completes the WR.

Predictions:

- With `native_tx_max_inflight=8` and `send_ack_redundancy=1`, copied 64 KiB
  can fail with `data_tx_posted == data_tx_completed` but insufficient
  `data_rx_ack` for the signaled writes.
- With `send_ack_redundancy=3`, the same run passes, but sender
  `data_rx_ack` exceeds the number required and late duplicates may appear as
  `data_rx_no_qp`.
- If `send_ack_redundancy=1` passes repeatedly, this claim is weakened and the
  earlier missing-ACK observation may have been coupled to another condition.

### D. GPU visibility/coherence bug

Claim: RDMA WRITE reaches host memory but the GPU does not observe the signal
or payload correctly.

Predictions:

- Receiver should time out while sender has clean WR completion and no driver
  errors.
- Counter evidence should not show missing TX completions or missing ACKs.
- This claim is contradicted when both host-coherent and host-uncached
  receiver memory pass with matching sequence checks and clean driver state.

Current status: contradicted for the known-good envelope. It may still apply
outside that envelope, but it is not the active 64 KiB failure signature.

## Test Matrix

All tests use `strix-1` receiver, `strix-2` sender, `usb4_rdma5`, GID index 1,
64 KiB WRITE, count 256, and a clean module reload before each row.

| ID | zcopy | tx limit | ack redundancy | Expected discriminator |
| --- | --- | --- | --- | --- |
| C1 | off | 8 | 1 | Is ACK redundancy actually required? |
| C2 | off | 8 | 3 | Known-good control after reload. |
| B1 | off | 0 | 3 | Does unthrottled copied TX still leak completions? |
| A1 | on | 8 | 3 | Does WRITE zcopy still break raw-stream lifetime? |

## Results

### C1: copied WRITE, TX limit 8, ACK redundancy 1

Command shape: `host-uncached`, 64 KiB, count 256.

Result: FAIL.

```text
sender:
send wc error wr_id=133 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=67 got=66 signal=66
```

Post-run counters:

```text
strix-2 sender:
data_wr_send=132 data_wr_copied=132 data_wr_zcopy=0
data_wr_timeout=2
data_tx_posted=1188 data_tx_completed=1188
data_rx_ack=130
path_tx inflight=0 zcopy_inflight=0 raw_active=0

strix-1 receiver:
data_rx_completed=1188 data_rx_op_write=1188
data_tx_posted=169 data_tx_completed=169
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

Irrefutable statement: C1 is not a sender native TX completion failure. Every
sender TX descriptor completed, but two WRs timed out and the sender observed
two fewer ACKs than WRs.

Contradiction: this rules out "TX throttling alone is sufficient" for the
copied 64 KiB path. It also contradicts a pure GPU visibility failure: the
driver stopped because WR ACK completion was missing, not because the sender
completed cleanly while the GPU failed to observe memory.

Post-watchdog redeploy repeat: FAIL with the same signature.

```text
sender:
send wc error wr_id=133 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=67 got=66 signal=66

strix-2 sender:
data_wr_send=132 data_wr_copied=132 data_wr_zcopy=0
data_wr_timeout=2
data_tx_posted=1188 data_tx_completed=1188
data_rx_ack=130
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

The repeat strengthens the ACK-delivery claim: the failure is stable across
reboot and watchdog hardening.

### C2: copied WRITE, TX limit 8, ACK redundancy 3

Command shape: `host-uncached`, 64 KiB, count 256.

Result: PASS.

```text
send_result ... size=65536 count=256 status=OK elapsed_sec=0.096421 avg_us=376.645
recv_result kind=host-uncached mode=normal size=65536 count=256 status=OK elapsed_sec=0.096454 avg_us=376.773
```

Post-run counters:

```text
strix-2 sender:
data_wr_send=512 data_wr_copied=512 data_wr_zcopy=0
data_wr_timeout=0
data_tx_posted=4608 data_tx_completed=4608
data_rx_ack=1508 data_rx_no_qp=28
path_tx inflight=0 zcopy_inflight=0 raw_active=0

strix-1 receiver:
data_rx_completed=4608 data_rx_op_write=4608
data_tx_posted=1680 data_tx_completed=1680
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

Irrefutable statement: changing only ACK redundancy from 1 to 3 converted the
same copied 64 KiB test from timeout to pass in this run sequence.

Contradiction: a model that only blames receiver GPU coherence cannot explain
C1 versus C2, because the receiver memory kind and data path were unchanged.

### B1: copied WRITE, no TX limit, ACK redundancy 3

Command shape: `host-uncached`, 64 KiB, count 256.

Result: PASS.

```text
send_result ... size=65536 count=256 status=OK elapsed_sec=0.094623 avg_us=369.622
recv_result kind=host-uncached mode=normal size=65536 count=256 status=OK elapsed_sec=0.094661 avg_us=369.768
```

Post-run counters:

```text
strix-2 sender:
data_wr_send=512 data_wr_copied=512 data_wr_zcopy=0
data_wr_timeout=0
data_tx_posted=4608 data_tx_completed=4608
data_rx_ack=1504 data_rx_no_qp=32
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

Irrefutable statement: after reboot, copied 64 KiB does not require
`native_tx_max_inflight=8` for every 256-count run. Unthrottled copied TX can
complete cleanly when delayed ACK redundancy is enabled.

Contradiction: the strong claim "unthrottled copied TX always leaks local
completions" is false.

### B2: copied WRITE, no TX limit, ACK redundancy 3, stress

Command shape: `host-uncached`, 64 KiB, count 1024.

Result: PASS.

```text
send_result ... size=65536 count=1024 status=OK elapsed_sec=0.349227 avg_us=341.042
recv_result kind=host-uncached mode=normal size=65536 count=1024 status=OK elapsed_sec=0.349261 avg_us=341.075
```

Post-run counters:

```text
strix-2 sender:
data_wr_send=2048 data_wr_copied=2048 data_wr_zcopy=0
data_wr_timeout=0
data_tx_posted=18432 data_tx_completed=18432
data_rx_ack=6118 data_rx_no_qp=26
path_tx inflight=0 zcopy_inflight=0 raw_active=0

strix-1 receiver:
data_rx_completed=18432 data_rx_op_write=18432
data_tx_posted=6720 data_tx_completed=6720
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

Irrefutable statement: unthrottled copied TX can also survive a larger
1024-count run with clean TX completion on this rebooted state.

Contradiction: the earlier copied-path TX completion leak is not reproduced by
removing the TX limit alone once delayed ACK retries are present. It is either
nondeterministic, coupled to another condition, or was influenced by an older
diagnostic patch/stale ring state.

### A1-small: WRITE zcopy enabled, TX limit 8, ACK redundancy 3

Command shape: `host-uncached`, 64 KiB, count 64,
`zcopy_min_bytes=4096`, `write_zcopy=1`.

Result: FAIL.

```text
sender:
send wc error wr_id=27 status=12 opcode=1 byte_len=0
ibv_destroy_qp: Connection timed out
ibv_destroy_cq: Device or resource busy
ibv_dealloc_pd: Device or resource busy

receiver:
timeout waiting gpu_seen=13 got=12 signal=12
```

Post-run counters:

```text
strix-2 sender:
verbs_pds=1 verbs_cqs=1 verbs_qps=1 verbs_mrs=1
data_wr_send=26 data_wr_copied=13 data_wr_zcopy=13
data_wr_live=1 data_wr_timeout=1
data_tx_posted=246 data_tx_completed=244
data_tx_credit_received=224 data_rx_ack=75
path_tx inflight=2 zcopy_inflight=2 raw_active=1

strix-1 receiver:
data_rx_completed=246 data_rx_op_write=233
data_tx_posted=82 data_tx_completed=82
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

Recent `strix-2` dmesg:

```text
thunderbolt_ibverbs: QP 2304 destroy timed out with 2 refs; leaving it closing for retry
WARNING: drivers/infiniband/core/rdma_core.c:957 at uverbs_destroy_ufile_hw+0xd3/0xf0 [ib_uverbs]
```

Recovery attempt:

```text
thunderbolt_ibverbs 249856 -1 - Unloading ... (O-)
rmmod thunderbolt_ibverbs in D state
```

Irrefutable statement: WRITE zcopy is an independent hard correctness bug. It
fails even with TX limit 8 and ACK redundancy 3, leaves zcopy/raw-stream state
active, leaks verbs objects, and can wedge module unload.

Contradiction: ACK redundancy is not enough to make zcopy safe. TX throttling
is not enough to make zcopy safe. The zcopy failure signature is different from
C1: C1 had complete sender TX and no live objects; A1-small has missing sender
TX completions, `zcopy_inflight`, `raw_active`, and leaked QP refs.

Code-level explanation:

- `tbv_qp_timeout_work()` times out the pending send and calls
  `tbv_cancel_send_ctx_packets()`.
- `tbv_cancel_send_ctx_packets()` maps the send context to the pinned rail and
  calls `tbv_path_cancel_data_owner_ctx()`.
- `tbv_path_cancel_data_match()` deliberately refuses to cancel queued or
  inflight packets whose `owner_ctx` is the currently active raw-stream owner:

```text
if (active_raw_owner && packet->owner_ctx == active_raw_owner)
        continue;
```

That rule protects framing for a raw stream that can still drain. In A1-small,
however, two zcopy TX completions never arrived. The rule therefore preserved
the active stream forever, left `tbv_send_tx_done()` callbacks uncalled for the
inflight zcopy packets, and kept QP refs live. This explains the userspace
destroy failures, the `QP 2304 destroy timed out with 2 refs` dmesg, and the
subsequent stuck module unload.

Recovery status: after `sudo systemctl reboot`, `strix-2` remained pingable but
SSH on port 22 returned `Connection refused`. This matches the earlier
post-wedge recovery symptom and requires console or out-of-band recovery before
more two-host tests.

## Updated Model

1. Copied 64 KiB has a reproducible ACK-delivery weakness. With TX limit 8 and
   ACK redundancy 1, the sender can miss ACKs despite complete local TX.
2. Delayed ACK redundancy fixes the copied 64 KiB failure in the tested rows,
   but introduces late duplicate ACK noise (`data_rx_no_qp`) after sender QP
   teardown.
3. WRITE zcopy/raw-stream has a separate lifetime bug. It cannot be treated as
   a performance option until `zcopy_inflight` and `raw_active` are guaranteed
   to drain or cancel under timeout/error.
4. The earlier unthrottled copied TX completion leak is not reproduced by
   removing `native_tx_max_inflight` alone after reboot and delayed ACK retries.
   Keep the TX limit as a conservative guard for now, but it is no longer the
   strongest active root-cause candidate for copied WRITE.

## SEND_ACK Observability Run

Added debugfs counters for SEND_ACK-only accounting:

```text
receiver side:
data_tx_send_ack
data_tx_send_ack_ok
data_tx_send_ack_error
data_tx_send_ack_retry
data_tx_send_ack_last_psn
data_tx_send_ack_error_last_psn

sender side:
data_rx_send_ack
data_rx_send_ack_matched
data_rx_send_ack_unmatched
data_rx_send_ack_error_status
data_rx_send_ack_last_psn
data_rx_send_ack_unmatched_last_psn

timeout side:
data_wr_send_timeout
data_wr_timeout_last_psn
```

These counters do not change protocol behavior. They separate:

- receiver wanted to send a SEND_ACK,
- local ACK control-frame software enqueue succeeded or failed,
- sender received a SEND_ACK frame,
- sender matched that ACK to an outstanding WR,
- sender timed out a WR and the last timeout PSN.

Important limitation: `data_tx_send_ack_ok` is not a wire-delivery counter. It
increments when `tbv_send_control_frame_on_path()` returns 0. For the native
path that means the frame was copied into the path's software
`tx_control_queue` and TX scheduling was requested. It does not prove the frame
was posted to the Thunderbolt ring, completed by the ring, delivered on the
wire, or decoded by the peer RX path.

### ACK redundancy 3 validates the counters

Command shape: 64 KiB, count 70, `native_tx_max_inflight=8`,
`send_ack_redundancy=3`.

Result: PASS.

```text
strix-1 receiver:
data_tx_send_ack=420 data_tx_send_ack_ok=420 data_tx_send_ack_error=0
data_tx_send_ack_retry=280

strix-2 sender:
data_wr_send=140 data_wr_timeout=0
data_rx_send_ack=420
data_rx_send_ack_matched=140
data_rx_send_ack_unmatched=280
data_rx_no_qp=30
```

Irrefutable statement: for 140 WRs, redundancy 3 queued 420 SEND_ACK frames in
the receiver's software control path. All 420 were observed by the sender.
Exactly 140 matched pending WRs; the rest were duplicate/late ACKs, including
some after QP teardown.

### ACK redundancy 1, TX pressure 8, original shape

Command shape: 64 KiB, count 256, `native_tx_max_inflight=8`,
`send_ack_redundancy=1`.

Result: FAIL.

```text
sender:
send wc error wr_id=23 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=12 got=11 signal=11

strix-1 receiver:
data_rx_completed=198 data_rx_op_write=198
data_tx_send_ack=22 data_tx_send_ack_ok=22 data_tx_send_ack_error=0
data_tx_send_ack_last_psn=14049791

strix-2 sender:
data_wr_send=22 data_wr_send_timeout=2 data_wr_timeout_last_psn=14049791
data_tx_posted=198 data_tx_completed=197
data_rx_send_ack=20 data_rx_send_ack_matched=20
data_rx_send_ack_unmatched=0 data_rx_no_qp=0
```

Irrefutable statement: the receiver accepted all WRITE frames it saw and
successfully queued 22 SEND_ACK frames in software. The sender observed only 20
SEND_ACK frames. The two missing ACKs correspond to the payload/signal WR pair
for the timed-out sequence. This run also had one missing sender TX completion.

### ACK redundancy 1, lower TX pressure

Command shape: 64 KiB, count 256, `send_ack_redundancy=1`.

Results:

```text
native_tx_max_inflight=1: PASS
sender data_wr_send=512 data_wr_timeout=0
sender data_tx_posted=4608 data_tx_completed=4608
sender data_rx_send_ack=512 data_rx_send_ack_matched=512
receiver data_tx_send_ack=512 data_tx_send_ack_ok=512

native_tx_max_inflight=2: PASS
sender data_wr_send=512 data_wr_timeout=0
sender data_tx_posted=4608 data_tx_completed=4608
sender data_rx_send_ack=512 data_rx_send_ack_matched=512
receiver data_tx_send_ack=512 data_tx_send_ack_ok=512

native_tx_max_inflight=4: PASS
sender data_wr_send=512 data_wr_timeout=0
sender data_tx_posted=4608 data_tx_completed=4608
sender data_rx_send_ack=512 data_rx_send_ack_matched=512
receiver data_tx_send_ack=512 data_tx_send_ack_ok=512
```

Irrefutable statement: reducing native TX pressure can convert the ACK=1
256-count run from fail to pass with exact sender-side ACK observation and
complete local TX.

This is not a root-cause explanation. It is a discriminator and a possible
temporary containment mechanism.

### ACK redundancy 1, TX pressure 6

Command shape: 64 KiB, count 256, `native_tx_max_inflight=6`,
`send_ack_redundancy=1`.

Result: FAIL.

```text
sender:
send wc error wr_id=243 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=122 got=121 signal=121

strix-1 receiver:
data_rx_completed=2178 data_rx_op_write=2178
data_tx_send_ack=242 data_tx_send_ack_ok=242 data_tx_send_ack_error=0
data_tx_send_ack_last_psn=8876261

strix-2 sender:
data_wr_send=242 data_wr_send_timeout=2 data_wr_timeout_last_psn=8876261
data_tx_posted=2178 data_tx_completed=2178
data_rx_send_ack=240 data_rx_send_ack_matched=240
data_rx_send_ack_unmatched=0 data_rx_no_qp=0
```

Irrefutable statement: SEND_ACK loss can occur even when sender native TX
completion is perfect. The receiver queued 242 ACK control frames in software;
the sender received only 240. Therefore a local sender TX completion leak is
not required for the copied-WR ACK failure.

Contradiction: the model "copied 64 KiB fails only because sender data TX
completions leak" is false.

### Existing-counter cheap check after critique

Command shape: no rebuild, 64 KiB, count 256,
`native_tx_max_inflight=6`, `send_ack_redundancy=1`.

Result: FAIL.

```text
sender:
send wc error wr_id=329 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=165 got=164 signal=164

strix-1 receiver:
data_tx_send_ack=328 data_tx_send_ack_ok=328 data_tx_send_ack_error=0
data_tx_send_ack_last_psn=3455057
data_tx_posted=420 data_tx_completed=420 data_tx_errors=0
data_rx_repost_failed=0 data_rx_bad_frame=0 data_rx_bad_header=0
data_rx_credit_sent=2944 data_rx_credit_send_error=0

strix-2 sender:
data_wr_send=328 data_wr_send_timeout=1 data_wr_timeout_last_psn=3455057
data_tx_posted=2952 data_tx_completed=2951 data_tx_errors=0
data_rx_send_ack=327 data_rx_send_ack_matched=327
data_rx_send_ack_unmatched=0 data_rx_no_qp=0
data_rx_repost_failed=0 data_rx_bad_frame=0 data_rx_bad_header=0
data_tx_credit_received=2944
```

Irrefutable statement: this run did not show sender RX repost failure,
mis-decoded RX frames, bad native headers, or receiver state-level TX
post-without-completion. The receiver software-queued 328 SEND_ACKs and its
state-level TX completions drained. The sender observed only 327 SEND_ACKs.

Limitation: this row is not as clean as the previous `tx=6` row because sender
native TX completions also ended short by one (`2952/2951`). It still rules out
several cheap existing-counter explanations, but it does not by itself isolate
ACK loss from sender TX completion pressure.

### Receiver TX completion versus sender RX callback

Code facts:

- `tbv_path_tx_complete()` increments state-level `data_tx_completed` for both
  control and data frames. The per-path `data_tx_completed` excludes control,
  but the state-level counter does not.
- `tbv_path_rx_complete()` increments state-level `data_rx_completed` before
  native header decode, QP lookup, or ACK matching.
- Native defaults start with `e2e=false`, but `tbv_peer_add_rail()` enables
  `RING_FLAG_E2E` and sets `path_cfg.e2e=true` for native Linux peers. Live
  debugfs confirmed native rails with `tx_flags=0x6`, `rx_flags=0x6`,
  `e2e=1`, and `wire_flags=0x3`.

Fresh command shape: no rebuild, 64 KiB, count 256,
`native_tx_max_inflight=6`, `send_ack_redundancy=1`, device `usb4_rdma0`
after reload reset device numbering.

Result: FAIL.

```text
sender:
send wc error wr_id=37 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=19 got=18 signal=18

strix-1 receiver:
data_rx_completed=324
data_rx_credit_sent=320
data_tx_send_ack=36 data_tx_send_ack_ok=36 data_tx_send_ack_error=0
data_tx_posted=46 data_tx_completed=46 data_tx_canceled=0 data_tx_errors=0
data_rx_repost_failed=0 data_rx_bad_frame=0 data_rx_bad_header=0
path_cfg e2e=1 tx_ring=1024 rx_ring=1024

strix-2 sender:
data_wr_send=36 data_wr_send_timeout=2 data_wr_timeout_last_psn=9442826
data_tx_posted=324 data_tx_completed=324 data_tx_canceled=0 data_tx_errors=0
data_rx_completed=44
data_tx_credit_received=320
data_rx_send_ack=34 data_rx_send_ack_matched=34
data_rx_send_ack_unmatched=0 data_rx_no_qp=0
data_rx_repost_failed=0 data_rx_bad_frame=0 data_rx_bad_header=0
path_tx inflight=0 data_q=0 ctrl_q=0 raw_active=0
path_cfg e2e=1 tx_ring=1024 rx_ring=1024
```

Irrefutable statement: receiver state-level TX posted and completed 46 reverse
frames. The sender RX callback processed only 44 reverse frames. The accounting
matches the semantic split: 320 receiver RX credits arrived at the sender, and
34 of 36 SEND_ACKs arrived at the sender. The two missing frames disappeared
after receiver TX completion and before sender `tbv_path_rx_complete()`.

This row also has clean sender data TX completion (`324/324`), zero sender TX
inflight, zero sender TX errors/canceled, and no RX repost/bad-frame/bad-header
evidence. It is therefore a clean receiver-TX-completed to sender-RX-callback
loss row.

Contradiction: the model "the missing ACK was dropped in receiver software
queueing or receiver `tb_ring_tx()` before TX completion" is false for this
row. The model "the missing ACK reached sender RX callback but was misdecoded
or mismatched above the ring" is also false for this row.

Important correction: the loss is not explained by native E2E being absent.
The live native paths are configured with E2E enabled. The next question is
whether E2E is effective for these native paths and hops, whether the E2E hop
configuration is actually valid for both directions, or whether another
Thunderbolt/NHI condition can still drop completed frames before peer RX
callback.

Separate state observation: after clean reload, debugfs `peers` listed two
native peers with two rails each, all active and data-ready, on the same route
and path ids. The traffic counters in this failing row advanced on only one
peer's rails. This may be benign source-aware xdomain enumeration, but it is a
routing/state invariant to verify before trusting higher-level path selection.

### RX-canceled counter and native E2E discriminator

Added two diagnostic changes:

- `data_rx_canceled` at the `tbv_path_rx_complete(canceled=true)` early return,
  exposed at both state and per-path debugfs levels.
- `native_e2e=<0|1>` load-time module parameter. Default is `1`, preserving
  the existing native Linux behavior. `0` skips `RING_FLAG_E2E` on native data
  TX/RX rings.

Local history correction: Strix Halo already had an empirically solid AMD NHI
E2E defect in the older apple_rdma work. The local notes characterize it as a
multiple-E2E-RX-ring credit-accounting collision: a second E2E RX ring on the
same NHI can wedge another ring's TX completions, and the refined model was
"at most one E2E-flagged RX ring active per NHI" before credits collide. That
history is directly relevant because the current native topology creates two
native peers with two rails each, and with `native_e2e=1` every rail has
`rx_flags=0x6 e2e=1`.

Architecture correction from the local Thunderbolt driver source:

- The NHI flag is named `RING_FLAG_E2E_FLOW_CONTROL`. RX ring options only
  program the E2E credit-return TX hop. There is no NHI retransmission or
  reliable-delivery layer.
- The upstream `QUIRK_E2E` reserved-hop workaround is applied only to Intel
  Falcon Ridge. AMD Strix Halo runs the un-quirked E2E path, so multiple E2E
  rings are not protected by that workaround.

Consequence: getting E2E "right" can prevent buffer overrun and may remove the
TX-completion wedge, but it cannot recover a genuinely dropped SEND_ACK. ACK
correctness needs software reliability: retransmit until the PSN-specific ACK
is matched, or an equivalent closed-loop control-frame ARQ.

#### `native_e2e=1` after adding `data_rx_canceled`

Fresh command shape: rebooted both hosts, reloaded patched module with default
`native_e2e=1`, then set `native_tx_max_inflight=6`,
`send_ack_redundancy=1`. 64 KiB, count 256, device `usb4_rdma0`.

Result: FAIL.

```text
sender:
send wc error wr_id=339 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=170 got=169 signal=169

strix-1 receiver:
data_tx_posted=433 data_tx_completed=433 data_tx_canceled=0 data_tx_errors=0
data_tx_send_ack=338 data_tx_send_ack_ok=338 data_tx_send_ack_error=0
data_rx_completed=3042 data_rx_canceled=0
data_rx_credit_sent=3040

strix-2 sender:
data_wr_send=338 data_wr_send_timeout=1 data_wr_timeout_last_psn=4577728
data_tx_posted=3042 data_tx_completed=3039 data_tx_canceled=0 data_tx_errors=0
path_tx inflight=3
data_rx_completed=432 data_rx_canceled=0
data_tx_credit_received=3040
data_rx_send_ack=337 data_rx_send_ack_matched=337
data_rx_send_ack_unmatched=0
```

Irrefutable statement: the receiver completed 433 reverse TX frames and the
sender RX callback processed 432 frames. The missing reverse frame did not hide
behind the `canceled=true` RX callback branch; `data_rx_canceled=0` on both
hosts. This row also reproduced the older TX-side wedge shape on the sender:
three posted TX frames remained inflight indefinitely (`3042/3039`), with
`verbs_qps=1` until reboot. That makes this row less clean for ACK-loss
isolation, but it strongly connects the current native topology to the older
Strix E2E credit-completion hazard.

#### `native_e2e=0`

Fresh command shape: rebooted both hosts, reloaded patched module with
`native_e2e=0`, `native_tx_max_inflight=6`, `send_ack_redundancy=1`. 64 KiB,
count 256, device `usb4_rdma0`.

Live verification before traffic:

```text
native_e2e=N
send_ack_redundancy=1
native_tx_max_inflight=6
path_cfg tx_flags=0x2 rx_flags=0x2 e2e=0 wire_flags=0x1
all state counters zero
```

Result: FAIL.

```text
sender:
send wc error wr_id=179 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=90 got=89 signal=89

strix-1 receiver:
data_tx_posted=228 data_tx_completed=228 data_tx_canceled=0 data_tx_errors=0
data_tx_send_ack=178 data_tx_send_ack_ok=178 data_tx_send_ack_error=0
data_rx_completed=1602 data_rx_canceled=0
data_rx_credit_sent=1600

strix-2 sender:
data_wr_send=178 data_wr_send_timeout=2 data_wr_timeout_last_psn=12676331
data_tx_posted=1602 data_tx_completed=1602 data_tx_canceled=0 data_tx_errors=0
path_tx inflight=0
data_rx_completed=226 data_rx_canceled=0
data_tx_credit_received=1600
data_rx_send_ack=176 data_rx_send_ack_matched=176
data_rx_send_ack_unmatched=0
```

Irrefutable statement: disabling native E2E did not eliminate reverse-frame
loss. Receiver completed 228 reverse TX frames, while sender RX callback
processed 226. The two missing frames correspond exactly to missing SEND_ACKs:
receiver sent 178 ACKs, sender received 176. `data_rx_canceled=0` again, and
this time sender TX completion was clean (`1602/1602`, inflight 0).

Conclusion from the pair: multiple native E2E rings remain a credible
explanation for the TX-completion wedge component, especially given the prior
Strix Halo history. ACK/control-frame loss also occurs with E2E disabled
globally, but the E2E-on and E2E-off loss mechanisms should not be assumed
identical. A plausible split is:

- E2E on with duplicate peers/multiple rings: AMD credit mis-accounting can
  over-advertise or stop returning credits, producing both reverse-frame loss
  and TX-completion wedge symptoms.
- E2E off: the channel has no hardware backpressure, so reverse control frames
  can still disappear under pressure.

The shared invariant across the clean rows is still receiver TX completion
exceeding sender RX callback count, with no RX canceled, bad-frame,
bad-header, or QP-mismatch evidence.

### Stopped State

After the E2E discriminator runs, both Strix hosts were reloaded back to the
branch defaults:

```text
write_zcopy=N
zcopy_min_bytes=0
native_e2e=Y
native_tx_max_inflight=8
send_ack_redundancy=3
native_fragment_striping=N
verbs_ucontexts/pds/cqs/qps/mrs/recv_wqes=0
data_wr_send=0 data_wr_timeout=0
data_tx_posted=0 data_tx_completed=0
data_rx_completed=0 data_rx_canceled=0
watchdog state=active timeout=15 nowayout=1
native path_cfg e2e=1 tx_ring=1024 rx_ring=1024
```

## Current Scientific Position

1. ACK redundancy is a workaround, not an explanation. It hides a real loss
   mode by making at least one duplicate ACK likely to survive.
2. Native TX pressure is a discriminator, not a root cause. `tx=4` passed in
   this matrix and `tx=6` failed, but this is a probability/pressure signal,
   not a proven safe threshold.
3. The copied-WR active root cause is now narrower: in a clean reproduced row,
   reverse frames disappeared between receiver state-level TX completion and
   sender `tbv_path_rx_complete()` callback.
4. The `canceled=true` RX callback path is not the explanation for the two
   freshest failures. `data_rx_canceled=0` in both the E2E-on and E2E-off rows.
5. E2E is flow-control only, not retransmission. Therefore no E2E setting can
   make one-shot SEND_ACK reliable after a real transport drop.
6. Native E2E is now split into two questions. Multiple E2E rings can plausibly
   explain the separate sender TX-completion wedge, while ACK/control-frame
   loss requires software reliability even if E2E flow control is made sane.
7. The next layer of proof must move below verbs/QP ACK handling into the path
   and Thunderbolt/NHI layer: prove whether TX completion is only local
   descriptor consumption, identify whether sender RX callback absence has an
   NHI interrupt/ring symptom, and separate control-frame reliability from
   data TX completion pressure.
8. WRITE zcopy remains a separate raw-stream lifetime bug and should stay
   disabled while the copied-control-frame loss is isolated.

## Watchdog Recovery Hardening

Recovered the removed `nix-strix-halo` watchdog module from commit
`82dbdcd275c50984896c3859772e99358aaaaa97` and reinstated the relevant policy
directly in `/mnt/Home/src/nixos-config/machines/x86/strix-halo/default.nix`.

Deployed settings:

```text
sp5100_tco heartbeat=30 nowayout=1 action=0
RuntimeWatchdogSec=15s
RebootWatchdogSec=30s
KExecWatchdogSec=30s
panic=5 panic_on_oops=1 softlockup_panic=1 hung_task_panic=1 nmi_watchdog=panic,1
```

Post-deploy verification on both `strix-1` and `strix-2`:

```text
systemd_watchdogs=15s,30s,30s
/sys/class/watchdog/watchdog0/identity=SP5100 TCO timer
/sys/class/watchdog/watchdog0/state=active
/sys/class/watchdog/watchdog0/timeout=15
/sys/class/watchdog/watchdog0/nowayout=1
kernel.panic = 5
kernel.watchdog = 1
kernel.panic_on_oops = 1
kernel.softlockup_panic = 1
kernel.hung_task_panic = 1
kernel.nmi_watchdog = 1
kernel.hardlockup_panic = 1
```

Safe post-watchdog RDMA baseline:

```text
send_result ... size=65536 count=256 status=OK elapsed_sec=0.102886 avg_us=401.900
recv_result kind=host-uncached mode=normal size=65536 count=256 status=OK elapsed_sec=0.102971 avg_us=402.230

strix-2 sender:
data_wr_send=512 data_wr_copied=512 data_wr_zcopy=0
data_wr_timeout=0
data_tx_posted=4608 data_tx_completed=4608
data_rx_ack=1504 data_rx_no_qp=32
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

Important operational note: after the NixOS reboot, `strix-2` booted the
installed `/run/booted-system/.../extra/thunderbolt_ibverbs.ko` from the
`thunderbolt-ibverbs-rewrite` input. That module lacks the GDA revive params
(`write_zcopy`, `native_tx_max_inflight`, `send_ack_redundancy`) and had
`native_fragment_striping=Y`. For the GDA tests, it was unloaded and replaced
with the rebuilt module from
`/mnt/Home/src/thunderbolt-ibverbs-gda-iommu-revive`.

## Next Falsifiers

1. Verify the native E2E setup rather than assuming it is absent. Live paths
   show `e2e=1`, and local history says Strix Halo can mis-account multiple
   E2E RX rings. Next useful discriminator is single-E2E-ring/single-native-peer
   topology, not merely E2E-on versus E2E-off.
2. Add path/NHI-level ACK tracepoints/counters. Required split: receiver
   `SEND_ACK` control-frame build, selected rail/path, path enqueue,
   Thunderbolt ring post, Thunderbolt TX completion, sender RX callback, native
   header decode, QP lookup, and QP match. Include opcode, PSN, src_qp,
   dest_qp, rail id, path id, route, ring hop, E2E hop, and peer id.
3. Verify the duplicate native peer state after reload. It may be harmless, but
   in a path/routing investigation the invariant should be explicit: exactly
   which peer/rail owns the QP and which duplicate peer, if any, can receive or
   transmit on the same route/path ids.
4. For WRITE zcopy, either keep it disabled or make timeout fatal to the raw
   stream owner: cancel remaining queued/inflight zcopy packets, complete their
   callbacks with `-ETIMEDOUT`/`-ECANCELED`, mark the path not data-ready, and
   force a ring/path reset before accepting later traffic on that rail.
5. Repeat the ACK=1 matrix only after path/NHI-level counters exist. The goal is
   not to find a lower "safe" TX limit; it is to find the exact layer where the
   ACK disappears.

## 2026-06-05 Correctness Stack Retest After Rebase

The missing HIP/GDA probes are now packaged in the flake so this test artifact
is reproducible instead of being a local binary:

```text
36d1fc3 nix: package HIP GDA probes
d83fc33 native: auto-disable E2E on AMD NHI
```

The new package is exposed as `.#tbv-hip-gda-probes`, included in the Linux
dev shell, and exported by the overlay as `tbv-hip-gda-probes`. The built output
used for this retest was:

```text
/nix/store/z9nf2cxbzh3d9c9bi4i8r8hjyampwndb-tbv-hip-gda-probes-0.1.0
```

Verification before running:

```text
nix build --no-link .#tbv-hip-gda-probes .#thunderbolt-ibverbs \
  .#checks.x86_64-linux.proto-smoke \
  .#checks.x86_64-linux.portable-kernel-patches
nix build --no-link .#bench-tools
nix flake check --no-build
git diff --check
```

Both Strix hosts were clean-reloaded with `thunderbolt-ibverbs-reload-system`,
then run with:

```text
native_e2e=-1
native_tx_max_inflight=6
```

On AMD/Strix, `native_e2e=-1` auto-disables hardware E2E for native rings. Live
paths showed `path_cfg ... e2e=0 wire_flags=0x1`.

Important caveat: this is not a literal replay of the old
`send_ack_redundancy=1` row. That parameter is gone in the rebased correctness
stack. OK SEND_ACKs are now sent on every live native path for the QP, so one
logical ACK produces two ACK frames in this two-rail run.

Five repeated runs of the old pressure row all passed:

```text
hip_rdma_write_visibility_probe \
  --kind host-uncached --mode normal --size 65536 --count 256 \
  --dev usb4_rdma0 --gid-index 1 --timeout-ms 15000
```

Each run returned `send_result ... status=OK` and
`recv_result ... status=OK`; observed elapsed times were about 0.052-0.061 s.

Cumulative post-run counters:

```text
strix-1 receiver:
data_tx_posted=5840 data_tx_completed=5840
data_tx_canceled=0 data_tx_errors=0
data_rx_completed=23040 data_rx_canceled=0 data_rx_repost_failed=0
data_rx_bad_frame=0 data_rx_bad_header=0
data_tx_ack_ok=2560 native_tx_send_ack=5120
data_tx_ack_send_error=0
data_rx_no_qp=0 data_rx_bad_peer=0 data_rx_unconnected_qp=0 data_rx_qp_error=0
path_tx inflight=0 on all paths

strix-2 sender:
data_wr_send=2560 data_wr_live=0 data_wr_timeout=0 data_wr_retransmit=0
data_tx_posted=23040 data_tx_completed=23040
data_tx_canceled=0 data_tx_errors=0
data_rx_completed=5840 data_rx_canceled=0 data_rx_repost_failed=0
data_rx_ack=5120 data_rx_ack_matched=2560
data_rx_late_ack=2560 data_rx_ack_miss=0
native_rx_send_ack=5120
data_rx_no_qp=0 data_rx_bad_peer=0 data_rx_unconnected_qp=0 data_rx_qp_error=0
path_tx inflight=0 on all paths
```

Interpretation:

1. The correctness stack no longer reproduces the old copied-WR ACK timeout in
   this pressure row across five runs.
2. The ACK path now has the expected duplicate shape: every logical ACK is
   matched once and observed once more as a late duplicate.
3. `data_rx_ack_miss=0` is not a lost-ACK metric; it only says no arriving ACKs
   were unexplained orphans. Lost ACKs are visible through retransmit/timeout
   behavior, not this counter.
4. The old late-duplicate teardown wart did not appear here: `data_rx_no_qp=0`
   after userspace exit on both hosts.
5. This does not prove the transport is reliable. It proves that the current
   software ACK replication plus AMD E2E auto-disable survives this formerly
   failing GDA visibility row.

## 2026-06-05 ACK-Loss Fault Injection

Added a disabled-by-default kernel fault injector:

```text
native_ack_drop_every=0
```

When set to `N`, the receiver silently drops every Nth logical native OK
SEND_ACK before the all-rail fan-out. This deliberately suppresses all rail
copies for that ACK and returns success to the caller, modeling a transport
loss rather than a local send error. Debugfs counters:

```text
data_tx_ack_drop_checked
data_tx_ack_drop_injected
```

The first forced-loss attempts exposed a test-harness bug rather than a clean
protocol result. With the original probe, the receiver exited as soon as the GPU
observed all signals, even if the sender still had WRs pending because their
ACKs had been dropped. That destroyed the receiver QP while the sender was
still retransmitting, producing `data_rx_no_qp` on the receiver and hard resets
on `strix-2` with no preserved previous-boot journal.

Bad/stressful attempts:

```text
count=64 native_ack_drop_every=16 qp_timeout_ms=200
receiver status=OK elapsed=0.721s
receiver data_tx_ack_drop_checked=225 data_tx_ack_drop_injected=14
receiver data_rx_duplicate_ack=97 data_rx_no_qp=119 data_rx_canceled=4096
strix-2 hard reset around 04:19

count=16 native_ack_drop_every=16 qp_timeout_ms=5000
receiver status=OK elapsed=0.075s
receiver data_tx_ack_drop_checked=33 data_tx_ack_drop_injected=2
receiver data_rx_duplicate_ack=1 data_rx_no_qp=119
strix-2 hard reset around 04:22
```

The HIP visibility probe was then fixed to include a final sender-completion
fence: after sending all per-sequence GPU-visible ACKs, the receiver keeps its
QP alive until the sender reports that all send completions have been observed
or until the probe timeout expires.

With that fenced probe, the single-drop validation passed:

```text
receiver:
native_ack_drop_every=32 qp_timeout_ms=5000
hip_rdma_write_visibility_probe --kind host-uncached --mode normal \
  --size 65536 --count 16 --timeout-ms 30000

send_result ... count=16 status=OK elapsed_sec=0.141131
recv_result ... count=16 status=OK elapsed_sec=0.141199
```

Post-run counters:

```text
strix-1 receiver:
data_tx_ack_ok=33
data_tx_ack_drop_checked=33
data_tx_ack_drop_injected=1
native_tx_send_ack=64
data_rx_duplicate_ack=1
data_rx_ack_history_miss=0
data_rx_no_qp=0
data_tx_posted=73 data_tx_completed=73
path_tx inflight=0 on all paths

strix-2 sender:
data_wr_send=32
data_wr_retransmit=1
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=289 data_tx_completed=289
data_rx_ack=64
data_rx_ack_matched=32
data_rx_ack_match_retried=1
data_rx_ack_match_max_ms=138
data_rx_ack_miss=0
data_rx_late_ack=32
data_rx_no_qp=0
path_tx inflight=0 on all paths
```

Interpretation:

1. The ACK-loss injector now gives a repeatable way to force the reliability
   backstop.
2. The receiver duplicate-PSN re-ack path worked for the injected lost ACK:
   the WR completed after one retransmit and did not double-deliver to
   userspace-visible state.
3. The probe must keep the receiver QP alive until the sender has observed all
   send completions; otherwise ACK-loss testing can turn into teardown-induced
   `data_rx_no_qp` and sender reset instead of a protocol validation.
4. The aggressive `qp_timeout_ms=200` run is not a correctness baseline. It is
   a separate stress/failure case showing that retransmit storms and teardown
   races can still be dangerous.

## 2026-06-05 NixOS Retest After Strix LAN/Deployment Fixes

Host deployment changes before retest:

```text
nixos-config:
- strix-1/strix-2 LAN now uses eno1 directly; br0.lan removed.
- thunderbolt-ibverbs roce_netdev=eno1.
- netconsole sender enabled on eno1 for both Strix hosts.
- crashDump enabled with crashkernel=512M.
- thunderbolt-ibverbs-kernel lock overridden to the local
  thunderbolt-ibverbs-gda-iommu-revive branch.
- tbv-hip-gda-probes included in the Strix system profile.
```

Verification:

```text
strix-1 eno1: 192.168.23.136/24, netconsole-sender active
strix-2 eno1: 192.168.23.192/24, netconsole-sender active
hip_rdma_write_visibility_probe present on both hosts
module params present: native_e2e, native_tx_max_inflight,
  native_ack_drop_every, qp_timeout_ms
```

Netconsole was tested with emergency `/dev/kmsg` markers from both hosts; both
arrived on the local UDP listener. During the GDA retests below, the only
netconsole lines were those two deliberate test markers.

All tests used:

```text
native_e2e=-1
native_tx_max_inflight=6
roce_netdev=eno1
receiver=strix-1
sender=strix-2
dev=usb4_rdma0 gid-index=1 kind=host-uncached mode=normal size=65536
```

Fresh clean pressure-row baseline:

```text
count=256 timeout_ms=15000 native_ack_drop_every=0 qp_timeout_ms=5000
send_result status=OK elapsed_sec=0.053407
recv_result status=OK elapsed_sec=0.053490
```

Counters:

```text
strix-1 receiver:
data_tx_ack_ok=512 native_tx_send_ack=1024
data_tx_posted=1168 data_tx_completed=1168
data_rx_completed=4608 data_rx_canceled=0 data_rx_repost_failed=0
data_rx_bad_frame=0 data_rx_bad_header=0
data_rx_no_qp=0 data_rx_bad_peer=0 data_rx_unconnected_qp=0
path_tx inflight=0 on all paths

strix-2 sender:
data_wr_send=512 data_wr_retransmit=0 data_wr_timeout=0
data_tx_posted=4608 data_tx_completed=4608
data_rx_ack=1024 data_rx_ack_matched=512 data_rx_late_ack=512
data_rx_ack_match_max_ms=3 data_rx_ack_miss=0
data_rx_canceled=0 data_rx_repost_failed=0 data_rx_no_qp=0
path_tx inflight=0 on all paths
```

Single forced ACK-loss validation on the redeployed system:

```text
count=16 timeout_ms=30000 native_ack_drop_every=32 qp_timeout_ms=5000
send_result status=OK elapsed_sec=0.075117
recv_result status=OK elapsed_sec=0.075238
```

Counters:

```text
strix-1 receiver:
data_tx_ack_ok=33
data_tx_ack_drop_checked=33 data_tx_ack_drop_injected=1
native_tx_send_ack=64
data_rx_duplicate_ack=1 data_rx_ack_history_miss=0
data_rx_no_qp=0 data_rx_canceled=0 data_rx_repost_failed=0
data_tx_posted=73 data_tx_completed=73
path_tx inflight=0 on all paths

strix-2 sender:
data_wr_send=32 data_wr_retransmit=1 data_wr_timeout=0
data_wr_retry_enqueue_error=0 data_wr_retry_exhausted=0
data_tx_posted=289 data_tx_completed=289
data_rx_ack=64 data_rx_ack_matched=32 data_rx_late_ack=32
data_rx_ack_match_retried=1 data_rx_ack_match_max_ms=71
data_rx_ack_miss=0 data_rx_no_qp=0 data_rx_canceled=0
path_tx inflight=0 on all paths
```

Moderate retransmit stress:

```text
count=64 timeout_ms=30000 native_ack_drop_every=8 qp_timeout_ms=5000
send_result status=OK elapsed_sec=1.299223
recv_result status=OK elapsed_sec=1.299278
```

Counters:

```text
strix-1 receiver:
data_tx_ack_ok=290
data_tx_ack_drop_checked=290 data_tx_ack_drop_injected=36
native_tx_send_ack=508
data_rx_duplicate_ack=162 data_rx_ack_history_miss=0
data_rx_no_qp=0 data_rx_canceled=0 data_rx_repost_failed=0
data_tx_posted=549 data_tx_completed=549
path_tx inflight=0 on all paths

strix-2 sender:
data_wr_send=128 data_wr_retransmit=18 data_wr_timeout=0
data_wr_retry_enqueue_error=0 data_wr_retry_exhausted=0
data_tx_posted=1314 data_tx_completed=1314
data_rx_ack=508 data_rx_ack_matched=137 data_rx_late_ack=371
data_rx_ack_match_retried=18 data_rx_ack_match_max_ms=74
data_rx_ack_miss=0 data_rx_no_qp=0 data_rx_canceled=0
path_tx inflight=0 on all paths
```

Old hard-reset parameter row replayed with the fenced probe:

```text
count=64 timeout_ms=30000 native_ack_drop_every=16 qp_timeout_ms=200
send_result status=OK elapsed_sec=0.719094
recv_result status=OK elapsed_sec=0.719143
```

Counters:

```text
strix-1 receiver:
data_tx_ack_ok=199
data_tx_ack_drop_checked=199 data_tx_ack_drop_injected=12
native_tx_send_ack=374
data_rx_duplicate_ack=71 data_rx_ack_history_miss=0
data_rx_no_qp=0 data_rx_canceled=0 data_rx_repost_failed=0
data_tx_posted=412 data_tx_completed=412
path_tx inflight=0 on all paths

strix-2 sender:
data_wr_send=128 data_wr_retransmit=10 data_wr_timeout=0
data_wr_retry_enqueue_error=0 data_wr_retry_exhausted=0
data_tx_posted=1223 data_tx_completed=1223
data_rx_ack=374 data_rx_ack_matched=133 data_rx_late_ack=241
data_rx_ack_match_retried=10 data_rx_ack_match_max_ms=72
data_rx_ack_miss=0 data_rx_no_qp=0 data_rx_canceled=0
path_tx inflight=0 on all paths
```

Interpretation:

1. The redeployed GDA correctness branch reproduces the previous green baseline:
   AMD native E2E auto-disables to `path_cfg e2e=0`, ACK fan-out is 2x, and the
   former pressure row passes with no retransmits or teardown artifacts.
2. The forced-loss backstop now survives more than the single-drop toy case.
   `drop_every=8` drove 36 injected logical ACK drops and 18 sender
   retransmits, all completing without timeout, retry exhaustion, RX cancel,
   or QP teardown fallout.
3. Replaying the old hard-reset parameters with the fenced probe passed, so
   `qp_timeout_ms=200` retransmit pressure alone was not sufficient to crash
   this build.
4. This does not resolve the unfenced crash. The fenced probe is a test control:
   it avoids the legal peer-teardown-during-retransmit condition. The kernel
   still needs an unfenced capture run before claiming that arbitrary peer/QP
   teardown is crash-proof.

## 2026-06-05 Reusable Crash Capture + Unfenced Retest

NixOS capture setup added/redeployed:

```text
trex:
- netconsole collector switched on and persistent:
  /var/log/netconsole/strix.log

strix-1/strix-2:
- netconsole sender remains on eno1.
- sconfig.ramoops enabled with reserved RAM:
  memmap=2M$0x205d000000
  ramoops.mem_address=0x205d000000
  ramoops.mem_size=0x200000
  ramoops.record_size=0x40000
- efi_pstore disabled so ramoops can own the singleton pstore backend:
  efi_pstore.pstore_disable=1
  pstore.backend=ramoops
```

Post-reboot verification:

```text
strix-1:
pstore.backend=ramoops
efi_pstore.pstore_disable=Y
pstore: Registered ramoops as persistent store backend
ramoops: using 0x200000@0x205d000000, ecc: 16

strix-2:
pstore.backend=ramoops
efi_pstore.pstore_disable=Y
pstore: Registered ramoops as persistent store backend
ramoops: using 0x200000@0x205d000000, ecc: 16
```

The HIP visibility probe now has an explicit unsafe mode:

```text
--unsafe-no-final-fence
```

This skips the sender-to-receiver final completion fence and makes the teardown
condition visible in logs as `final_fence=0`.

Unfenced replay of the old hard-reset row:

```text
count=64 timeout_ms=30000 native_ack_drop_every=16 qp_timeout_ms=200
final_fence=0
send_result status=OK elapsed_sec=0.579185
recv_result status=OK elapsed_sec=0.579171
```

Counters:

```text
strix-1 receiver:
data_tx_ack_ok=200
data_tx_ack_drop_checked=200 data_tx_ack_drop_injected=12
data_rx_duplicate_ack=72
data_tx_posted=414 data_tx_completed=414 data_tx_errors=0
data_rx_completed=1224 data_rx_canceled=0

strix-2 sender:
data_wr_send=128 data_wr_retransmit=8 data_wr_timeout=0
data_wr_retry_enqueue_error=0 data_wr_retry_exhausted=0
data_tx_posted=1224 data_tx_completed=1224 data_tx_errors=0
data_rx_completed=414 data_rx_canceled=0
data_rx_ack=376 data_rx_ack_matched=132
data_rx_ack_match_retried=8 data_rx_late_ack=244
data_rx_ack_miss=0
```

Higher-loss unfenced stress:

```text
count=256 timeout_ms=60000 native_ack_drop_every=4 qp_timeout_ms=200
final_fence=0
send_result status=OK elapsed_sec=10.867799
recv_result status=OK elapsed_sec=10.867822
```

Counters:

```text
strix-1 receiver:
data_tx_ack_ok=1875
data_tx_ack_drop_checked=1875 data_tx_ack_drop_injected=468
data_rx_duplicate_ack=1363
data_tx_posted=3000 data_tx_completed=3000 data_tx_errors=0
data_rx_completed=5971 data_rx_canceled=0

strix-2 sender:
data_wr_send=512 data_wr_retransmit=150 data_wr_timeout=0
data_wr_retry_enqueue_error=0 data_wr_retry_exhausted=0
data_tx_posted=5971 data_tx_completed=5971 data_tx_errors=0
data_rx_completed=3000 data_rx_canceled=4096
data_rx_ack=2814 data_rx_ack_matched=594
data_rx_ack_match_retried=150 data_rx_late_ack=2220
data_rx_ack_miss=0
```

Netconsole and pstore:

```text
trex netconsole log contained only deliberate start markers; no crash/oops
records were emitted.
/sys/fs/pstore and /var/lib/pstore contained no crash records after the runs.
```

Follow-up reload check:

```text
Immediately after a clean post-test module reload:
strix-1 data_rx_canceled=0
strix-2 data_rx_canceled=0
```

Interpretation:

1. The unfenced old-row replay did not reproduce the previous hard reset on the
   current rebased/patched build. It did exercise the intended path:
   8 sender retransmits, 72 receiver duplicate re-acks, no timeout, no retry
   exhaustion, no crash.
2. A much stronger unfenced row also completed: 468 injected ACK drops,
   150 sender retransmits, 1363 receiver duplicate re-acks, no timeout, no
   retry exhaustion, no crash.
3. This is a non-reproduction, not a proof of absence. The old host reset
   remains historically real but is now uncaptured on this build and may have
   depended on code before the current patch/rebase state or on a narrower
   lifecycle race window.
4. The high-loss run exposed a new anomaly: `strix-2 data_rx_canceled=4096`.
   Because a clean reload immediately afterward produced `data_rx_canceled=0`,
   this is not a deterministic reload counter artifact. Treat it as a live
   lifecycle/ring-cancel observation to investigate, not as a settled cause.

## 2026-06-05 PR24 Kernel Patch Recipe + Retransmit/Teardown Guard Retest

Before this retest, one important negative result was reclassified:

```text
git log ba44378..HEAD -- kernel/
```

was empty at the time of the prior unfenced non-repro. The kernel
teardown/retry/peer code was therefore byte-identical to the crash-era
injector commit. That non-repro was not a fix; it dodged the timing window.

Deployment changes:

```text
nixos-config:
- restored the referenced thunderbolt-xdomain-bridge-hardening.patch in the
  Thunderbolt kernel recipe, fixed the malformed hunk metadata, staged it so
  flakes cannot silently drop it, and rebuilt the Strix kernel.
- thunderbolt-ibverbs-kernel input was locked to the local
  thunderbolt-ibverbs-gda-iommu-revive worktree.
- deployed with colmena switch, then colmena apply boot --reboot.

strix-1 booted:
/nix/store/ww15x29ycl0cmb9rnq1cmydsab0jsvqq-nixos-system-strix-1-26.11pre-git

strix-2 booted:
/nix/store/fzv7pkfvlfz1gv7q9cq758zgavc2svph-nixos-system-strix-2-26.11pre-git
```

Instrumentation added to the GDA module:

```text
data_wr_retransmit_closing_qp
data_wr_retransmit_no_live_path
data_wr_retransmit_teardown_path
```

The guard logs retransmit attempts that see a local closing QP, no live native
path, or a selected native path whose rail/path/rings are visibly tearing down.
It uses `pr_warn_ratelimited()` rather than `WARN_ONCE()` so the watchdog/panic
configuration does not turn instrumentation into a new crash trigger.

### Instrumented Unfenced High-Loss Row

```text
count=256 timeout_ms=60000 native_ack_drop_every=4 qp_timeout_ms=200
final_fence=0
receiver: status=OK elapsed_sec=11.593501
sender: wc error status=12 opcode=1 wr_id=513
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=168
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=1
data_wr_timeout=1
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_tx_posted=6251 data_tx_completed=6251 data_tx_errors=0
data_rx_completed=3417 data_rx_canceled=0
data_rx_ack=3222 data_rx_ack_matched=616
data_rx_ack_match_retried=161 data_rx_late_ack=2606
data_rx_ack_miss=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=2148
data_tx_ack_drop_checked=2148 data_tx_ack_drop_injected=537
data_rx_duplicate_ack=1636
data_tx_posted=3417 data_tx_completed=3417 data_tx_errors=0
data_rx_completed=6251 data_rx_canceled=0
data_rx_no_qp=7
```

### Fenced Control With Same Loss Pressure

```text
count=256 timeout_ms=60000 native_ack_drop_every=4 qp_timeout_ms=200
final_fence=1
sender: status=OK elapsed_sec=12.022799
receiver: status=OK elapsed_sec=12.022945
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=163
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_tx_posted=6193 data_tx_completed=6193 data_tx_errors=0
data_rx_completed=3339 data_rx_canceled=0
data_rx_ack=3146 data_rx_ack_matched=608
data_rx_ack_match_retried=163 data_rx_late_ack=2538
data_rx_ack_miss=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=2097
data_tx_ack_drop_checked=2097 data_tx_ack_drop_injected=524
data_rx_duplicate_ack=1585
data_tx_posted=3339 data_tx_completed=3339 data_tx_errors=0
data_rx_completed=6193 data_rx_canceled=0
data_rx_no_qp=0
```

Netconsole and pstore remained quiet during both rows:

```text
trex netconsole tail contained only the earlier deliberate markers.
/sys/fs/pstore contained no crash records on either Strix host.
```

Interpretation:

1. The fenced control proves the live-peer retransmit backstop still works after
   the PR24 kernel patch reboot and guard instrumentation: 163 retransmits,
   524 injected ACK drops, no timeout, no retry exhaustion, no RX cancels, and
   no local retransmit/path teardown guard hits.
2. The unfenced failure is now a clean remote-QP-teardown failure, not a local
   sender path-teardown failure. The receiver finished and destroyed the QP,
   then received 7 retransmitted data frames with no QP (`data_rx_no_qp=7`).
   The sender never saw a local closing-QP/no-path/teardown-path condition, so
   it retried until one WR exhausted and surfaced `wc status=12`.
3. This is a useful contradiction to the earlier `data_rx_canceled=4096`
   near-miss: the deterministic artifact in this run is `data_rx_no_qp`, while
   `data_rx_canceled=0` on both hosts. The old hard reset is still not
   backtraced, but the most reproducible unfenced failure mode is now remote QP
   tombstone/lost-ACK teardown, not sender-side path/ring teardown.
4. A principled kernel hardening target is now visible: keep enough destroyed-QP
   tombstone/ACK history to re-ACK already-consumed duplicate PSNs briefly after
   QP teardown, or send an explicit native error ACK for no-QP retransmits so
   the sender completes with a bounded remote error instead of burning the full
   retry budget. Either way, the kernel must continue to treat arbitrary peer
   teardown as a clean completion/error path, never a host reset.

## 2026-06-05 Destroyed-QP Tombstone Retest

Change under test:

```text
When a native QP is destroyed, publish a short-lived tombstone keyed by
(local_qpn, remote_qpn) containing the QP ACK history. If later duplicate
SEND/WRITE frames arrive for the destroyed QP, re-ACK matched PSNs from the
tombstone. If no history entry exists, send an explicit SEND_ACK_ERROR instead
of silently dropping the frame.

New counters:
data_rx_no_qp_reack
data_rx_no_qp_error_ack
```

Deployment:

```text
nixos-config flake lock pointed thunderbolt-ibverbs-kernel at the local
gda-iommu-revive worktree, then both Strix hosts were deployed and rebooted
with colmena apply boot --reboot.

strix-1 booted:
/nix/store/lch1h99xwxlqdc1x22177vrv82gn4a9f-nixos-system-strix-1-26.11pre-git

strix-2 booted:
/nix/store/kbs0alh39160ds6fd9jddy2v74vdpsf0-nixos-system-strix-2-26.11pre-git
```

Netconsole remained live after reboot; emergency markers from both hosts were
received by the persistent collector on `trex`.

### Unfenced High-Loss Row, drop_every=4

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=4
final_fence=0
sender:   status=OK elapsed_sec=10.871897
receiver: status=OK elapsed_sec=10.799840
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=151
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_tx_posted=6005 data_tx_completed=6005 data_tx_errors=0
data_rx_completed=3050 data_rx_canceled=0
data_rx_ack=2863 data_rx_ack_matched=595
data_rx_ack_match_retried=151 data_rx_late_ack=2268
data_rx_ack_miss=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=1909
data_tx_ack_drop_checked=1908 data_tx_ack_drop_injected=477
data_rx_duplicate_ack=1396
data_rx_ack_history_miss=0
data_rx_no_qp=1
data_rx_no_qp_reack=1
data_rx_no_qp_error_ack=0
data_tx_posted=3050 data_tx_completed=3050 data_tx_errors=0
data_rx_completed=6005 data_rx_canceled=0
```

Receiver dmesg recorded the intended mechanism:

```text
native RX no-QP re-acked dest_qp=0x900 src_qp=0x900 psn=15781175 opcode=3 ret=0 tombstone=1
```

### Stronger Unfenced Row, drop_every=2

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
final_fence=0
sender:   status=OK elapsed_sec=18.427499
receiver: status=OK elapsed_sec=18.355733
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=256
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_tx_posted=6928 data_tx_completed=6928 data_tx_errors=0
data_rx_completed=3049 data_rx_canceled=0
data_rx_ack=2833 data_rx_ack_matched=657
data_rx_ack_match_retried=256 data_rx_late_ack=2176
data_rx_ack_miss=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=2832
data_tx_ack_drop_checked=2815 data_tx_ack_drop_injected=1407
data_rx_duplicate_ack=2303
data_rx_ack_history_miss=0
data_rx_no_qp=17
data_rx_no_qp_reack=17
data_rx_no_qp_error_ack=0
data_tx_posted=3049 data_tx_completed=3049 data_tx_errors=0
data_rx_completed=6928 data_rx_canceled=0
```

Netconsole and pstore remained quiet during both rows except for deliberate
test markers; `/sys/fs/pstore` was empty on both Strix hosts.

Interpretation:

1. The tombstone path is exercised, not inferred. The receiver accepted
   duplicate retransmits after userspace destroyed the QP, found matching
   destroyed-QP ACK history, and re-acked them.
2. This directly fixes the reproducible unfenced failure from the previous
   section: the old row had `data_rx_no_qp=7` and the sender exhausted one WR;
   the new rows have `data_rx_no_qp_reack == data_rx_no_qp` and no sender
   timeout/retry exhaustion.
3. The stronger row drove 256 retransmits and 17 post-destroy no-QP arrivals
   with no local sender teardown guard hits and no RX cancels. That separates
   this fixed no-QP lifecycle bug from the older uncaptured hard reset/ring
   cancel race, which remains a separate latent issue until a stack or a direct
   teardown guard trigger proves otherwise.

### Repeated Unfenced Stress After Commit

After committing the tombstone fix, ran three fresh module-reload trials with
the same aggressive teardown/loss shape:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
final_fence=0
ports=18531,18532,18533
```

All three trials passed:

```text
trial 1: sender OK elapsed_sec=18.433222 receiver OK elapsed_sec=18.361304
trial 2: sender OK elapsed_sec=18.430722 receiver OK elapsed_sec=18.359194
trial 3: sender OK elapsed_sec=18.432509 receiver OK elapsed_sec=18.360856
```

Sender (`strix-2`) invariants in each trial:

```text
data_wr_retransmit=256
data_wr_retry_exhausted=0
data_wr_timeout=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_rx_canceled=0
data_rx_ack_match_retried=256
data_rx_ack_miss=0
```

Receiver (`strix-1`) post-destroy behavior:

```text
trial 1: data_tx_ack_drop_injected=1583 data_rx_duplicate_ack=2655 data_rx_no_qp=16 data_rx_no_qp_reack=16
trial 2: data_tx_ack_drop_injected=1552 data_rx_duplicate_ack=2593 data_rx_no_qp=16 data_rx_no_qp_reack=16
trial 3: data_tx_ack_drop_injected=1544 data_rx_duplicate_ack=2577 data_rx_no_qp=17 data_rx_no_qp_reack=17

all trials:
data_rx_no_qp_error_ack=0
data_rx_ack_history_miss=0
data_rx_canceled=0
```

Pstore remained empty on both Strix hosts. Netconsole recorded only deliberate
test markers, with no panic/oops/watchdog output.

Interpretation:

1. The no-QP teardown fix is repeatable across reloads and not a one-off pass.
   The strict invariant held: every post-destroy no-QP arrival was re-acked from
   tombstone history, and the sender never exhausted or timed out.
2. The older `data_rx_canceled=4096` / hard-reset concern did not reproduce in
   this three-trial series. This is evidence that the reproducible unfenced
   artifact was the remote no-QP ACK-history gap, but it is not proof that the
   older ring-cancel race is impossible. It remains tracked as latent unless a
   direct ring teardown guard fires or a captured crash stack says otherwise.

### Longer Unfenced Stress Row

Ran one longer row with the same loss shape:

```text
count=1024 timeout_ms=120000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
final_fence=0
port=18540
sender:   status=OK elapsed_sec=73.729803
receiver: status=OK elapsed_sec=73.657998
```

Sender (`strix-2`) counters:

```text
data_wr_send=2048
data_wr_retransmit=1024
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_tx_posted=28716 data_tx_completed=28716 data_tx_errors=0
data_rx_completed=13230 data_rx_canceled=0
data_rx_ack=12333 data_rx_ack_matched=2741
data_rx_ack_match_retried=1024 data_rx_late_ack=9592
data_rx_ack_miss=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=12332
data_tx_ack_drop_checked=12315 data_tx_ack_drop_injected=6157
data_rx_duplicate_ack=10267
data_rx_ack_history_miss=0
data_rx_no_qp=17
data_rx_no_qp_reack=17
data_rx_no_qp_error_ack=0
data_tx_posted=13230 data_tx_completed=13230 data_tx_errors=0
data_rx_completed=28716 data_rx_canceled=0
```

Pstore was empty on both hosts. Netconsole recorded only deliberate markers.

Interpretation:

1. The tombstone fix scales through at least 1024 retransmits in this workload.
   The no-QP post-destroy invariant still holds exactly.
2. The RX-cancel/hard-reset concern still did not reappear under longer
   high-loss unfenced pressure. Current evidence points to the no-QP ACK-history
   gap as the reproducible correctness bug fixed here; the older hard reset is
   now lower-frequency or a separate trigger, not reproduced by this row.

### Post-Stress Normal Sanity

Reloaded both modules, restored no-injection defaults, and ran the ordinary
fenced row:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=30000
receiver native_ack_drop_every=0
final_fence=1
port=18541
sender:   status=OK elapsed_sec=0.055204
receiver: status=OK elapsed_sec=0.055296
```

Counters after the row:

```text
sender data_wr_send=512 data_wr_retransmit=0 data_wr_retry_exhausted=0 data_wr_timeout=0
sender data_rx_ack_match_retried=0 data_rx_ack_miss=0 data_rx_canceled=0

receiver data_tx_ack_drop_injected=0 data_rx_duplicate_ack=0
receiver data_rx_no_qp=0 data_rx_no_qp_reack=0 data_rx_no_qp_error_ack=0
receiver data_rx_canceled=0
```

Interpretation: after the hostile unfenced rows, the clean fast path still
behaves normally and the live hosts were left with ACK injection disabled.

### Follow-Up: Sender Guard Counters and Tombstone Bounds

Code hardening after reviewing the first tombstone series:

```text
- tombstone expiry is now bounded by the destroyed QP's actual retry window:
  max(30s, tx_timeout * (max_retries + 2)).
- the 128-entry tombstone cap now increments data_qp_tombstone_evicted and logs
  the evicted QPN pair when the cap forces early eviction.
```

Important timeout nuance:

```text
qp_timeout_ms=200 was set in the stress rows, but this RC probe sets verbs
attr.timeout=14. Therefore the operative send ACK timeout is the verbs value,
about 67ms, not the fallback module parameter. The dmesg retransmit ages
around 70-73ms confirm this.
```

Retry/tombstone bound:

```text
TBV_SEND_MAX_RETRIES=7
tbv_rel_retry_interval(timeout, retries) == timeout

For the RC probe:
  verbs timeout 14 => ~67ms
  tombstone lifetime=max(30s, 9 * 67ms) = 30s

For a QP using the default fallback:
  qp_timeout_ms=5000
  tombstone lifetime=max(30s, 9 * 5000ms) = 45s

For the explicit fallback setting used in stress if a QP did not set verbs
timeout:
  qp_timeout_ms=200
  tombstone lifetime=max(30s, 9 * 200ms) = 30s
```

Explicit post-bound trigger row:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
final_fence=0
port=18550
sender:   status=OK elapsed_sec=18.432606
receiver: status=OK elapsed_sec=18.358310
booted strix-1=/nix/store/3h21xhxiyj85kvxgzna16y9lv1fi9zgf-nixos-system-strix-1-26.11pre-git
booted strix-2=/nix/store/jq5vzd96alzf4wx8pcp2mw57s7y2r5qy-nixos-system-strix-2-26.11pre-git
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=256
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=7010 data_tx_completed=7010 data_tx_errors=0
data_rx_completed=3134 data_rx_canceled=0
data_rx_ack=2903 data_rx_ack_matched=669
data_rx_ack_match_retried=256 data_rx_late_ack=2234
data_rx_ack_miss=0
data_qp_tombstone_evicted=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=2914
data_tx_ack_drop_checked=2897 data_tx_ack_drop_injected=1448
data_rx_duplicate_ack=2385
data_rx_ack_history_miss=0
data_rx_no_qp=17
data_rx_no_qp_reack=17
data_rx_no_qp_error_ack=0
data_qp_tombstone_evicted=0
data_tx_posted=3134 data_tx_completed=3134 data_tx_errors=0
data_rx_completed=7010 data_rx_canceled=0
```

Pstore was empty on both hosts. Netconsole recorded only deliberate markers
plus unrelated USB messages; no panic/oops/watchdog output.

Interpretation:

1. The sender-side teardown guards did **not** fire in this row, nor in the
   earlier `count=1024` row. Therefore the sender guard is instrumented
   coverage for the plausible crash path, but it is not yet demonstrated as the
   crash-fixing path. The precise statement is: sender teardown guard
   counters remained zero while the workload passed.
2. The receiver tombstone path is the directly exercised fix: all 17 no-QP
   post-destroy arrivals were re-acked from tombstone history, with no history
   misses, no error ACKs, and no evictions.
3. The old `data_rx_canceled=4096` signature still has not reappeared after the
   tombstone fix and dynamic lifetime/cap instrumentation, but without a fired
   sender guard or crash stack it remains a latent lower-frequency concern
   rather than a proven fixed sender-side race.

### Tombstone-Off A/B

Added `native_qp_tombstone_reack` as an A/B-only debug knob. Default is enabled.
Disabling it suppresses both tombstone publication and no-QP tombstone re-ack,
recreating the old behavior where late retransmits to a destroyed QP are dropped.

Both Strix hosts booted the new closure:

```text
strix-1=/nix/store/ssm5q3x6nbkl4yjnvns9kvx63rxyys74-nixos-system-strix-1-26.11pre-git
strix-2=/nix/store/dx1ckv6hs3a5fmsj6hj6lr1g5bxkdl6j-nixos-system-strix-2-26.11pre-git
kernel=7.0.10
```

Capture rig state:

```text
trex listener: nc -u -k -l 6666 -> /var/log/netconsole/strix.log
netconsole markers from both Strix hosts reached the log
pstore empty before and after both A/B rows
```

Tombstone re-ack disabled:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
native_qp_tombstone_reack=0 on both hosts
final_fence=0
port=18560
sender:   failed, wc error wr_id=513 status=12
receiver: status=OK elapsed_sec=18.362758
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=262
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=1
data_wr_timeout=1
data_rx_ack_matched=660
data_rx_ack_match_retried=255
data_rx_ack_match_over_64ms=255
data_rx_late_ack=2230
data_rx_canceled=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=2890
data_tx_ack_drop_checked=2890 data_tx_ack_drop_injected=1445
data_rx_duplicate_ack=2378
data_rx_no_qp=7
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_rx_canceled=0
```

Tombstone re-ack enabled, same trigger:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
native_qp_tombstone_reack=1 on both hosts
final_fence=0
port=18561
sender:   status=OK elapsed_sec=18.433667
receiver: status=OK elapsed_sec=18.361957
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=256
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_rx_ack_matched=692
data_rx_ack_match_retried=256
data_rx_ack_match_over_64ms=256
data_rx_late_ack=2519
data_rx_canceled=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=3210
data_tx_ack_drop_checked=3193 data_tx_ack_drop_injected=1596
data_rx_duplicate_ack=2681
data_rx_no_qp=17
data_rx_no_qp_reack=17
data_rx_no_qp_error_ack=0
data_qp_tombstone_evicted=0
data_rx_canceled=0
```

Interpretation:

1. This is the first direct cause/effect A/B for the tombstone mechanism.
   With tombstone re-ack disabled, the old no-QP drop behavior returns and one
   sender WR exhausts retries. With tombstone re-ack enabled, the same
   unfenced trigger passes and every no-QP post-destroy arrival is re-acked
   from tombstone history. This proves the tombstone fixes the graceful WR
   timeout caused by retransmit-to-dead-QP.
2. The sender teardown guards remained zero in both halves of the A/B. For this
   workload, the demonstrated failure mechanism is receiver-side destroyed-QP
   no-QP handling, not sender-side retransmit into a closing path.
3. The tombstone-off half did not reproduce the older strix-2 hard reset. It
   failed gracefully with retry exhaustion and the box stayed up. Therefore
   this A/B does not prove the sender teardown guard is the crash-fixing path;
   the guard counters were zero precisely when tombstones were disabled.
4. The older hard reset remains uncaptured and unexplained by this row. A
   crash-era reproduction would need the sender guards disabled as well, with
   netconsole/pstore armed, if we decide the remaining risk justifies the
   destructive test.
5. Crash attribution also has uncontrolled kernel-side variables: these runs
   include the Nix-carried Thunderbolt XDomain hardening and resync patches.
   Those patches are part of the tested baseline, but they prevent attributing
   the absence of the old hard reset solely to GDA module changes.
6. After the A/B, both modules were reloaded and restored to safe defaults:
   `native_qp_tombstone_reack=1`, `native_ack_drop_every=0`,
   `qp_timeout_ms=30000`, `native_tx_max_inflight=6`.

Post-A/B clean sanity:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=30000
native_ack_drop_every=0
native_qp_tombstone_reack=1
final_fence=1
port=18562
sender:   status=OK elapsed_sec=0.056416
receiver: status=OK elapsed_sec=0.056488

sender:
data_wr_send=512
data_wr_retransmit=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_rx_ack_match_retried=0
data_rx_ack_miss=0
data_rx_canceled=0
data_rx_no_qp=0

receiver:
data_tx_ack_drop_injected=0
data_rx_duplicate_ack=0
data_rx_no_qp=0
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_rx_canceled=0
```

### Unsafe Guard-Off Crash Reproduction

Added `native_unsafe_retransmit_teardown_guard_disable` as an explicitly
unsafe debug knob. Default is `0`, which keeps the sender retransmit teardown
guard enabled. Setting it to `1` keeps the observation/counter code but allows a
retry retransmit to proceed after selecting a tearing-down native path.

Both Strix hosts were redeployed/rebooted through Colmena with the new module:

```text
strix-1=/nix/store/4x5wcywcaxscxrds93hpbjq89cxw5h1g-nixos-system-strix-1-26.11pre-git
strix-2=/nix/store/f45362p11si1cx0fp6qbcmx958igwg1d-nixos-system-strix-2-26.11pre-git
kernel=7.0.10
```

Initial safe sanity, fenced:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=30000
native_ack_drop_every=0
native_qp_tombstone_reack=1
native_retransmit_teardown_guard=1
final_fence=1
port=18563
sender:   status=OK elapsed_sec=0.054901
receiver: status=OK elapsed_sec=0.054981

sender:
data_wr_send=512
data_wr_retransmit=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_rx_ack=1024
data_rx_ack_matched=512
data_rx_ack_match_retried=0
data_rx_canceled=0
data_rx_no_qp=0

receiver:
data_tx_ack_ok=512
data_tx_ack_drop_checked=0 data_tx_ack_drop_injected=0
data_rx_duplicate_ack=0
data_rx_no_qp=0
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_rx_canceled=0
```

Unsafe crash-era row:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
native_qp_tombstone_reack=0 on both hosts
native_retransmit_teardown_guard=0 on both hosts
final_fence=0
port=18564
sender:   SSH died with exit code 255, no probe result
receiver: status=OK elapsed_sec=18.358444
```

`strix-2` reset during the sender run:

```text
pre-run boot:  155855bcc5c246dd81d6f6f9f74f3320
pre-run journal last persisted line: 2026-06-05 23:24:37.524
current boot:  73b5247a2da14cd992a024f0e2761e86
current boot start: 2026-06-05 23:26:29
post-reset uptime when checked: <1 minute
pstore: empty
netconsole: no panic/oops captured for this reset
```

Receiver (`strix-1`) counters after the reset:

```text
data_tx_posted=3136 data_tx_completed=3136
data_tx_canceled=0 data_tx_errors=0
data_rx_completed=7128
data_rx_canceled=4096
data_rx_repost_failed=0
data_rx_bad_frame=0 data_rx_bad_header=0
data_rx_send=7009
data_tx_ack_ok=2913
native_tx_send_ack=2914
data_tx_ack_drop_checked=2913 data_tx_ack_drop_injected=1456
data_rx_duplicate_ack=2401
data_rx_no_qp=119
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
```

Receiver journal showed the peer disappearing during the run:

```text
2026-06-05 23:25:42 thunderbolt retimers disconnected
2026-06-05 23:25:42 thunderbolt_ibverbs unregistered per-rail ib_devices
2026-06-05 23:25:42 thunderbolt 0-2/1-2 host disconnected
2026-06-05 23:26:37 new XDomain/peer bring-up after strix-2 reboot
```

After restoring safe defaults and reloading both modules, a final fenced sanity
row passed:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=30000
native_ack_drop_every=0
native_qp_tombstone_reack=1
native_retransmit_teardown_guard=1
final_fence=1
port=18565
sender:   status=OK elapsed_sec=0.054349
receiver: status=OK elapsed_sec=0.054420

sender:
data_wr_send=512
data_wr_retransmit=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_rx_ack=1024
data_rx_ack_matched=512
data_rx_ack_match_retried=0
data_rx_canceled=0
data_rx_no_qp=0

receiver:
data_tx_ack_ok=512
data_tx_ack_drop_checked=0 data_tx_ack_drop_injected=0
data_rx_duplicate_ack=0
data_rx_no_qp=0
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_rx_canceled=0
```

Direct same-build guard-on contrast:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
native_qp_tombstone_reack=0 on both hosts
native_retransmit_teardown_guard=1 on both hosts
final_fence=0
port=18566
sender:   failed, wc error wr_id=513 status=12
receiver: status=OK elapsed_sec=18.357129
pstore: empty on both hosts
netconsole: explicit <0> markers from both hosts reached collector
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=262
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=1
data_wr_timeout=1
data_rx_ack=2802
data_rx_ack_matched=650
data_rx_ack_match_retried=255
data_rx_ack_match_over_64ms=255
data_rx_late_ack=2152
data_rx_canceled=0
data_rx_no_qp=0
```

Receiver (`strix-1`) counters:

```text
data_tx_posted=3017 data_tx_completed=3017
data_rx_completed=6905
data_rx_canceled=0
data_tx_ack_ok=2802
native_tx_send_ack=2801
data_tx_ack_drop_checked=2802 data_tx_ack_drop_injected=1401
data_rx_duplicate_ack=2290
data_rx_no_qp=7
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_qp_tombstone_evicted=0
```

Interpretation:

1. Disabling both tombstone re-ack and the sender retransmit teardown guard
   reproduced the destructive `strix-2` hard reset. On the same deployed build,
   the tombstone-off/guard-on contrast failed gracefully with retry exhaustion
   and the host stayed up. That is strong evidence the guard is not cosmetic:
   removing it reopens the crash-era behavior.
2. This still did not produce a stack. The sender rebooted without pstore
   contents and without a netconsole panic/oops. Persistent journald has no
   final kernel line after the unsafe parameters were written; normal logging
   stopped before reset.
3. The old receiver-side near-miss signature returned at the same time:
   `data_rx_canceled=4096` and `data_rx_no_qp=119`, followed by peer/ring
   teardown on `strix-1`. The tombstone/guard-safe rows keep this at zero.
4. The same-build guard-on/tombstone-off row still had sender guard counters at
   zero, so there is no positive non-destructive counter trace from the exact
   sender line that reset. The honest statement is: guard-off makes the crash
   reproducible again; guard-on has so far converted this test family to either
   graceful timeout (tombstone off) or success (tombstone on).
5. Netconsole requires explicit high-priority markers (`<0>...`) for reliable
   operator breadcrumbs. A post-run `<0>` live check from both hosts reached the
   collector; the plain `/dev/kmsg` marker used before this unsafe row did not.
6. Operational default remains safe: `native_qp_tombstone_reack=1`,
   `native_unsafe_retransmit_teardown_guard_disable=0`,
   `native_ack_drop_every=0`, and `qp_timeout_ms=30000`.

Post-breadcrumb redeploy sanity:

After adding the emergency-level breadcrumb for the unsafe teardown retry path,
the branch was redeployed to both Strix hosts. The paired Colmena deploy first
failed for `strix-2` because `strix-1` was being used as a remote builder while
also being rebooted; retrying `strix-2` alone completed cleanly.

```text
strix-1=/nix/store/g4ncxvshjv7m031z6i9pdq0xbrrl9zq2-nixos-system-strix-1-26.11pre-git
strix-2=/nix/store/i63c0v4snl1q7hyibj06wnmaasfrpvx0-nixos-system-strix-2-26.11pre-git
kernel=7.0.10
```

Final fenced sanity on the deployed baseline:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=30000
native_ack_drop_every=0
native_qp_tombstone_reack=1
native_retransmit_teardown_guard=1
final_fence=1
port=18567
sender:   status=OK elapsed_sec=0.054835
receiver: status=OK elapsed_sec=0.055007

sender:
data_wr_send=512
data_wr_retransmit=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_rx_ack=1024
data_rx_ack_matched=512
data_rx_ack_match_retried=0
data_rx_canceled=0
data_rx_no_qp=0

receiver:
data_tx_ack_ok=512
data_tx_ack_drop_checked=0 data_tx_ack_drop_injected=0
data_rx_duplicate_ack=0
data_rx_no_qp=0
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_rx_canceled=0
```
