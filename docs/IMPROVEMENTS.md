# Improvements on `feat/write-striping`

Changes on top of upstream `76ba39b`, made to run two-host tensor parallelism
(TP2) of an inference server that talks to **one RDMA device with one QP**
over USB4 between two Strix Halo hosts. Upstream reaches its bandwidth by
having the application spread traffic over several devices and QPs (for
example NCCL with `NCCL_IB_HCA`); a single QP stayed on one rail at about
6 Gbit/s.

Measured with two passive 0.3 m cables, both links at 2 × 20 Gb/s, four
rails (two DMA rings per USB4 controller), `amd_iommu=off`.

## Results

| perftest, 1 QP, one device | upstream | this branch |
|---|---:|---:|
| RDMA WRITE 1 MiB, one way | ~6 Gbit/s | 44 Gbit/s |
| RDMA WRITE 4 MiB, both ways | – | 85 Gbit/s |
| RDMA WRITE 64 KiB, one way | – | 32 Gbit/s |

Four rails deliver about 11 Gbit/s each, so one QP now reaches what the
hardware gives all rails together. The inference server's TP2 over USB4
produces output identical to InfiniBand; its prefill runs at 96 % of an FDR
InfiniBand link (ConnectX-3, PCIe 3.0 x4), decode at 95 %.

## Striping one QP across rails

- **Block-striped RDMA WRITEs** (`native_write_striping`,
  `native_write_stripe_min_bytes`). A WRITE of at least 64 KiB is cut into
  one contiguous block per rail. Every fragment carries its offset and the
  total length (flag `F_BLOCK`), so the receiver copies each fragment
  straight into the target memory region whatever rail it arrived on, and
  completes the message (with its immediate) in PSN order once all
  fragments are there. Retransmissions only fill gaps.
- **Several cables to one host.** Rails of all links to the same remote host
  form one pool for a QP. For that the native control protocol names the
  sending link and host (wire version 2 of HELLO, still parsing version 1),
  so two links whose rails use the same route no longer get mixed up.
- **`native_domain_mask`** chooses which USB4 controllers carry native rails.

## Throughput of the data path

- **Asynchronous frame building.** `post_send` used to copy a whole message
  into 4 KiB frames itself, which kept the application's communication
  thread busy for most of the transfer. Each rail now builds its block on
  its own worker, in chunks of 64 frames that are queued as soon as they are
  built, so sending starts while the rest is still being copied.
- **Parallel receive placement.** Fragments of one QP arriving on several
  rails are copied into place without holding the QP's receive lock; a
  generation number detects entries that went away meanwhile.
- **O(1) lookup of receive entries.** Messages in reassembly are found by PSN
  in a hash table instead of a list walk under the receive lock. With deep
  send queues the rails drift apart, the list grew long, and the receive
  workers blocked each other (26 instead of 44 Gbit/s).
- **Memory region page index.** Copies into and out of a memory region
  looked up their offset by walking the scatterlist from the start, which
  grew with the offset; a page array makes it constant.
- **No cancel walk on success.** Every completed send walked the TX queue
  under its lock to cancel leftover frames; it now does so only after
  retransmissions or errors. This alone lifted two rails from 6.6 to 20.9
  Gbit/s.

## Correctness

- **Lost frames no longer strand credits.** Native rings run without USB4
  E2E flow control (on AMD it stays off because of TX completion wedges), so
  a frame can be lost, and every lost data frame used to take its data
  credit with it for good until the rail stopped sending. Data frames now
  carry a per-path sequence number in two spare header bytes (ignored by
  older peers); the receiver refunds the credits of any gap, and
  retransmission recovers the messages. `data_rx_lost` counts them.
- **Ordered send completions** when ACKs for consecutive sends race on
  different rails.
- **RNR handling**: a send waiting for a receive buffer finishes its current
  attempt before it is retried instead of failing the QP.
- **Messages up to 64 MiB** (was 16 MiB).
- **Reorder timeouts are logged** with the state of the stuck message.

## Tools

- `userspace/bench/rc_write_imm_verify`: checked stream of RC
  WRITE_WITH_IMM messages with varying sizes, depths and forced RNR; every
  byte and completion length is verified.
- `bench/sweep_perftest.py` and `bench/plot_perftest.py`: bandwidth and
  latency sweep with the CPU load on both hosts, and the README charts (see
  [bench/README.md](../bench/README.md#readme-charts)).

## Known limits

- RDMA READs above 512 KiB fail, also in upstream `76ba39b`, and long
  series of small READs (over 100 000 in a row) occasionally end with
  retries exceeded.
- SENDs above 512 KiB fail with write striping enabled, which turns on the
  existing fragment striping for SENDs.
- Frames still get lost under bidirectional or SEND-heavy load (thousands
  per minute), probably when control frames, which take no credit, overflow
  an RX ring; each loss costs a retransmission timeout.
- A single READ stream stays on one rail (about 10 Gbit/s).
- Sends are still copied once into frames; zero-copy from the memory
  region's pages is the next step.
