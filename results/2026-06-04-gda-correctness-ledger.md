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
