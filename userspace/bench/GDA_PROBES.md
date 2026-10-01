# USB4 GDA Smoke Probes

These probes separate two questions that are easy to conflate.

`tbv-dv-caps-probe -q` validates the DV queue-memory contract on one host:
the kernel can pin CPU-visible SQ/CQ/doorbell memory, consume signaled NOP
WQEs through `KICK`, write CQEs, and publish doorbell head/tail updates. This
does not prove Thunderbolt/NHI payload DMA into GPU VRAM.

`hip_dv_kernel_cqe_probe.cpp` validates GPU visibility of kernel-written DV
completion state. Build it on a ROCm host and run it against a loaded module
after `tbv-dv-caps-probe -q` passes.

`hip_reg_mr_probe.cpp`, `hsa_fine_grain_pool_probe.cpp`, and
`hip_rdma_write_visibility_probe.cpp` are payload-memory probes. The two-host
visibility probe is the closest "true GPU DMA" smoke: the sender writes a
payload and signal through RDMA WRITE into a HIP allocation, then a GPU kernel
on the receiver observes the signal and checks the payload. A pass demonstrates
the tested allocation mode and synchronization path on that topology; it does
not imply generic PCIe P2PDMA support for arbitrary GPU memory.

Expected conservative result for this branch: queue-memory probes can pass
with host-visible coherent memory, while arbitrary device-memory payloads may
fail registration or fall back to staged CPU-visible paths until the kernel has
an explicit peer-direct/P2PDMA contract.
