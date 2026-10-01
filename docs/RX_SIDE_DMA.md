# RX-side DMA for dmabuf MRs

**Status:** implemented on `codex/apple-xdomain-property-match` (commit `dc8f43f`).
Pending: live dmabuf-destination smoke test on `strix-1`.
**Goal:** make RDMA WRITE destinations land in dmabuf MRs without CPU-side
copies. CPU-pinned MRs continue to use the existing bounce-buffer path; this
change does not regress them.

## Why a spec first

The current RX path always lands ring DMA into a kernel bounce buffer, then
copies from the bounce buffer into the destination MR. That works for
`ib_umem` (CPU-pinned) but breaks for `ib_umem_dmabuf`:

```
tbv_umem_copy_to_iova():
  mr->umem->is_dmabuf == true
  -> dma_buf_begin_cpu_access()
  -> kmap_local_page() + memcpy()   // bails on ZONE_DEVICE pages
  -> dma_buf_end_cpu_access()
```

So GPU dmabuf MRs cannot be RDMA-WRITE destinations today. The dmabuf MR
import path exists (see `tbv_reg_user_mr_dmabuf`), it just has no place to
land data.

## The change

DMA each ring frame **directly into the destination MR page** when the MR is
a dmabuf MR, or when the MR's pages cannot be CPU-kmap'd. Otherwise fall
through to the bounce-buffer path unchanged.

Per-fragment DMA is required because NHI ring descriptors point at one
contiguous physical address; an MR's SGL is scattered. Each PDF frame
becomes its own DMA into one MR page.

### What stays the same

- The NHI ring. The descriptor interface already accepts per-frame DMA
  addresses (`ring->descriptors[ring->head].address = frame->dma`,
  `ring_write_descriptors()`). We are not patching the kernel.
- The reorder / fragment reassembly / WC push logic in `ibdev.c`.
- The CPU-MR bounce-buffer path.
- All TX paths.

### What changes

Three pieces in `kernel/path.c` and `kernel/ibdev.c`:

1. **MR-side mapping.** When the reorder layer accepts the first fragment of
   a WRITE that targets a dmabuf MR, look up the rkey, validate the range,
   `dma_map_sg_attrs(... DMA_FROM_DEVICE)` on the destination page range,
   and stash the mapped SGL on `tbv_rx_reorder_msg`.
2. **Per-fragment DMA.** Replace the call into
   `tbv_rx_reorder_store_fragment_locked` for dmabuf destinations with
   `tbv_rx_zcopy_store_fragment_locked` that:
   - allocates a `tbv_data_frame` from a small `rx_zcopy_pool`
   - sets `frame->buf` to the MR page + offset, `frame->dma` to the mapped
     address
   - `dma_sync_single_for_device(..., DMA_FROM_DEVICE)` then `tb_ring_rx()`
   - on completion, `dma_unmap_page(..., DMA_FROM_DEVICE)`, push WC if the
     fragment was last, free the frame
3. **CPU fallback unchanged.** If `mr->umem->is_dmabuf == false`, the
   existing kernel-bounce-buffer RX path stays in use.

### Where it lives

- `kernel/path.c`: extend `tbv_data_frame` with `enum tbv_rx_mode { CPU,
  ZCOPY_DMABUF }` and a `tbv_rx_zcopy` struct holding the unmap state.
  Add `tbv_path_post_rx_zcopy_frame()` that submits a per-fragment DMA
  into the MR page.
- `kernel/ibdev.c`: branch in
  `tbv_rx_buffer_write_fragment_locked()`: dmabuf MR -> the new path; CPU
  MR -> existing path.
- `kernel/tbv.h`: add the new mode enum and per-frame zcopy bookkeeping on
  `tbv_rx_reorder_msg`.

### Failure handling

- If the MR's SGL map fails (`dma_map_sg_attrs` returns 0), ack the
  fragment with `TBV_NATIVE_SEND_ACK_ERROR`, push `IB_WC_LOC_PROT_ERR`,
  drop the reorder message. This is identical to the existing error path
  for bad rkey.
- If `dma_map_page` returns an error during per-fragment DMA setup, same
  treatment.
- Completion ordering: NHI completes descriptors in arrival order; PDF
  frames already carry the fragment index, and the existing
  `frag_seen[]` bitmap stays correct because we record `set_bit(frag_idx,
  msg->frag_seen)` only after the DMA is posted. The WC is pushed when
  the **last** fragment completes, not when the first does. This matches
  what dmabuf MR consumers expect.

### What we explicitly do not do

- We do not reorder or coalesce DMA. Per-fragment DMA, period.
- We do not modify the NHI ring or the kernel module.
- We do not change TX-side zcopy or the SEND path. SEND lands in a kernel
  WQE buffer, not in an MR.
- We do not introduce per-MR DMA contexts; the existing ring DMA device
  is reused.

## Test plan (on strix-1 + strix-2, kernel 7.2.0-rc2)

1. **CPU regression.** Re-run the existing `userspace/bench/ibv_*.c`
   send/recv suite (sender CPU MR, receiver CPU MR). Confirm
   `data_rx_completed` matches `data_rx_reorder_delivered` and no
   `data_rx_copy_error` increments.
2. **Dmabuf destination regression.** Run
   `userspace/bench/dmabuf_mr_probe` with sender CPU MR, receiver GPU
   dmabuf MR. Confirm `data_rx_completed > 0`, `data_rx_copy_error`
   stays at 0, and `data_rx_dmabuf_zcopy` (new counter) increments.
3. **GPU P2P smoke.** Run
   `userspace/bench/hip_rdma_write_visibility_probe.cpp`. The PR-thread
   symptom was `data_rx_copy_error=2`, `gpu_seen=0`. With this change we
   expect `gpu_seen > 0` and no copy errors.

## Open questions

- **RX ring pool size.** NHI rings have a fixed descriptor count. While a
  zcopy frame is in flight, the ring has one fewer descriptor for the
  bounce path. For bursty small messages this could starve CPU MR RX.
  Mitigation: cap concurrent zcopy fragments per QP at
  `min(ring->size / 2, TBV_RX_ZCOPY_MAX_PER_QP)`, default 32.
- **GPU page alignment.** ZONE_DEVICE pages are not necessarily aligned
  to `TBV_DATA_FRAME_SIZE`. The per-frame DMA must respect
  `frame->size` and the offset within the page; this is already how the
  ring works (`frame->size` is what the descriptor programs as length),
  so no kernel change is needed. We do, however, need to refuse to map
  fragments whose first-byte offset into the page is non-zero, because
  NHI DMA into a partial page will tear the page. Constraint: a
  fragment's destination `(iova & (PAGE_SIZE - 1))` must be 0 modulo the
  DMA page granularity. We can satisfy this by mapping page-aligned,
  `bytes_to_copy = min(remaining_in_page, frag_len)`, and pushing
  fragment splits if needed.

## Estimated scope

- ~150-200 lines added in `kernel/path.c` and `kernel/ibdev.c`
- one new counter in `kernel/debugfs.c`
- one probe binary already exists; minimal new tests
- no kernel patch changes

## Decision

Approve to proceed? If yes, I will start with the dmabuf-only branch
(smallest diff, leaves CPU MRs untouched) and report back with a
buildable module + a smoke test on strix-1.

## Run book for the next experiment on strix-1 + strix-2

1. **Reload the module.** `tools/tbv-target-module.sh strix-1 --booted-kernel --reload --options 'profile=linux_perf apple_data=N native_data=Y bind_services=Y allocate_rings=Y start_rings=Y negotiate_native=Y enable_tunnels=Y register_verbs=Y zcopy_min_bytes=4294967295'`. Repeat for `strix-2` if the kernel is rebuilt there. Verify `/sys/class/infiniband/usb4_rdma*/ports/1/link_layer` reads `InfiniBand` (not `Ethernet`).
2. **CPU regression first.** Run `userspace/bench/rc_write_verify` between the two nodes. Confirm `data_rx_completed` matches `data_rx_reorder_delivered` and no `data_rx_copy_error` increments in `/sys/kernel/debug/tbv/usb4_rdma*/summary`. This must pass before touching dmabuf.
3. **dmabuf MR probe.** Open a HIP-allocated region on `strix-1`, export it via `hsa_amd_portable_export_dmabuf`, register it via `ibv_reg_dmabuf_mr` on `usb4_rdma0` (or `usb4_rdma1`). Have the peer do an RDMA WRITE into it. Confirm `data_rx_dmabuf_zcopy` (new counter) increments and `data_rx_copy_error` stays at 0.
4. **HIP visibility probe.** Run `userspace/bench/hip_rdma_write_visibility_probe` with `--role recv --kind device --recv-reg dmabuf` on the GPU node and `--role send --kind malloc --source-fill cpu` on the peer. Compare against the `--recv-reg reg_mr` baseline. The PR-thread failure mode was `gpu_seen=0`, `data_rx_copy_error=2`. With this change we expect `gpu_seen > 0` and zero copy errors.
5. **Failure triage.** If `data_rx_dmabuf_zcopy_error` increments, the most common cause is `dma_map_sg` returning 0 (device not IOMMU-mapped or BO not contiguous) or the SGL walk failing (`dest_iova` outside any sg entry). Both are recoverable by aborting the WRITE and pushing `IB_WC_LOC_PROT_ERR`, so the wire stays consistent.

## What's still missing

- The same RX-side DMA change needs to be **ported onto `codex/gda-v2-rebased-port`** (the path input the cluster actually builds from). Currently it lives only on `codex/apple-xdomain-property-match`.
- The `nixos-config` flake.lock pin in this commit points at `codex/apple-xdomain-property-match`, but the cluster still consumes the GDA v2 rebase path input. Once the GDA branch picks up these commits, a `colmena build` will pull them in.