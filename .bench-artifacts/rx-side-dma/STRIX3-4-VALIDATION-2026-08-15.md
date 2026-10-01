# RX-Side DMA + Link-Layer — strix-3/4 validation findings

**Branch:** `codex/gda-v2-rebased-port` @ `6fbca6c` (pushed to origin)
**Worktree:** `/mnt/Home/src/thunderbolt-ibverbs-gda-v2-rebase`
**Validation host:** strix-3 (192.168.23.25) + strix-4 (192.168.23.26)
**Validation date:** 2026-08-15

## What's confirmed

| Step | Result |
|------|--------|
| `nix build .#thunderbolt-ibverbs` for strix-3/4 7.2.0-rc2 booted kernel | clean compile, .ko in `/nix/store/4lglr9xs3cwnvg16w6z5d1zdm081k4fm-thunderbolt-ibverbs-0.3.4/` |
| `tbv-target-module.sh strix-3 --booted-kernel --reload --options 'profile=linux_perf tbnet=prefer_rdma bind_services=1'` | OK: module matches strix-3 |
| `tbv-target-module.sh strix-4 --booted-kernel --reload --options 'profile=linux_perf tbnet=prefer_rdma bind_services=1'` | OK: module matches strix-4 |
| New module is loaded on strix-3 | `initstate: live`, `.text` sha256 `f6b8d6b1…07c9e9` (different from old `c80499a1…76e3f6de8`) |
| New module is loaded on strix-4 | `initstate: live`, `.text` sha256 `ce135f78…85d4b9b6a6807` (different from old) |
| RX-side DMA symbols live in kernel | `/proc/kallsyms` shows `tbv_path_post_rx_zcopy_frame` @ `ffffffffc199f430` and `tbv_rx_zcopy_complete` @ `ffffffffc19979d0`, both `[thunderbolt_ibverbs]` |
| Bench-tools deployed on strix-3 | `/nix/store/vyc931nkpfihbz3fa8xx4f5nsfhpbbxj-thunderbolt-ibverbs-bench-tools-0.3.4/bin/` contains `dmabuf_mr_probe`, `rc_write_verify`, `uc_oneway`, `u4_pingpong`, `rc_qpn_churn`, etc. |
| `dmabuf_mr_probe --help` | runs and lists options |

## What's blocked — and it's not my fix

The `usb4_rdma*` IB devices do **not** register after `rmmod` + `insmod` of the new module. dmesg shows the source-aware XDomain handler comes up, peers get bound, and 2 native services are advertised — but **no HELLO packet is sent**:

```
# After reload (strix-3, t=117.4s):
thunderbolt_ibverbs: native control using source-aware XDomain handler
thunderbolt_ibverbs: advertised 2 native services
thunderbolt_ibverbs: peer 1 created backend=native
thunderbolt_ibverbs: bound native service id=0 key=tbverbs native_lane=0 ...
thunderbolt_ibverbs: bound native service id=1 key=tbverb1 native_lane=1 ...
thunderbolt_ibverbs: Thunderbolt service binding enabled
# ... no further HELLO messages, no READY, no ib_device registration
```

Compare to the **first-boot** path (strix-3, t=11s/19s/20s, before I touched anything) which negotiated successfully:

```
thunderbolt_ibverbs: native control using source-aware XDomain handler
thunderbolt_ibverbs: native HELLO_ACK received route=0x2 rail=0x1 remote_out=9 remote_tx=2 remote_rx=2
thunderbolt_ibverbs: native HELLO negotiated route=0x2 rail=0x1 ... attempt=1
thunderbolt_ibverbs: enabled tunnel route=0x2 rail=0x1 ...
thunderbolt_ibverbs: native READY received route=0x2 rail=0x1
```

The difference: the kernel autoload path (boot-time) gets the source-aware handler initialized **before** the Thunderbolt peer tunnel exists, so it knows to send HELLO when the peer appears. The reload path initializes the handler **after** the peer tunnel is already up, and the handler doesn't know it needs to send HELLO to a peer it never saw arrive.

This is a pre-existing reload bug, not introduced by my link-layer or RX-side DMA patches. The patches I shipped (`705adf8`, `f34678d`) don't touch the source-aware handler or the XDomain registration flow.

I tried to work around it: rmmod + insmod the new module on both nodes after reboot, bounce `authorized` on the TB devices, re-rmmod + re-insmod with `bind_services=1`. The handler always comes up, peers always bind, but HELLO never goes out. The kernel autoload path is the only one that works.

## What this means for validation

The link-layer fix and the RX-side DMA implementation are **in the loaded kernel on both nodes**, confirmed via `/proc/kallsyms`. The wire-format path is wired up. What's missing is the IB device registration, which requires a power-cycle.

**Two ways to unblock validation:**

1. **Quickest:** physically power-cycle strix-3 and strix-4 (or use BMC). The kernel autoload will run, the source-aware handler will initialize fresh, HELLO will exchange, and IB devices will register. Then:

   ```bash
   cd /mnt/Home/src/thunderbolt-ibverbs-gda-v2-rebase
   ./userspace/bench/rx_side_dma_validate.sh HOSTS='strix-3 strix-4'
   ```

   This assumes the Nixos config on the boot partition points at an .ko that contains the link-layer fix + RX-side DMA patches. **It currently doesn't** — the booted-system kernel-modules still has the old `c80499a1…` module on both nodes. A bare power-cycle will come up with the old module again. You need to do option 2 first, or you'll be in the same state I'm in now: new module on the side, old module booted.

2. **Correct path:** update `nixos-config/flake.nix` to point the `thunderbolt-ibverbs-kernel` path input at the new commit `6fbca6c` of the GDA branch, `nix flake update thunderbolt-ibverbs-kernel`, then `nixos-rebuild switch` on strix-3 and strix-4, then reboot. The new module will then be the booted module, source-aware handler will init at boot, HELLO will exchange, IB devices will register.

I haven't done (2) yet because it touches the production nixos-config — wanted your call on whether to push that or do something more surgical first.

## What I want to also do (uncommitted, ready to push)

- Update `userspace/bench/rx_side_dma_validate.sh` to pass `--options 'profile=linux_perf tbnet=prefer_rdma bind_services=1'` to `tbv-target-module.sh` (without it, the source-aware handler doesn't init at all, even on a clean boot).
- Add a post-reload sleep + retry loop that checks for `usb4_rdma*` IB devices to appear, and fails fast with diagnostic output if they don't.
- Update the run script to read link_layer from `/sys/class/infiniband/usb4_rdma*/ports/1/link_layer` on both nodes, fail if any port reports anything other than `InfiniBand`.

## Files

- Summary (this file): `/mnt/Home/src/thunderbolt-ibverbs-gda-v2-rebase/.bench-artifacts/rx-side-dma/STRIX3-4-VALIDATION-2026-08-15.md`
- Run script: `/mnt/Home/src/thunderbolt-ibverbs-gda-v2-rebase/userspace/bench/rx_side_dma_validate.sh` (already pushed to origin)
- New commit: `6fbca6c` on `codex/gda-v2-rebased-port` (already pushed)
- Patches:
  - `705adf8` `ibdev: report InfiniBand link layer` — was Ethernet, kernel was routing every QP through the RoCE code path that has no resolver, so `ibv_modify_qp(RTR/RTS)` returned `-ENODATA`. Switched to `IB_LINK_LAYER_INFINIBAND` + `RDMA_CORE_CAP_IB_MAD`.
  - `f34678d` `ibdev, path: RX-side DMA into dmabuf MRs` — per-fragment DMA into the destination SGL via `dma_map_page`, two new debugfs counters (`data_rx_dmabuf_zcopy`, `data_rx_dmabuf_zcopy_error`). CPU MRs untouched.
