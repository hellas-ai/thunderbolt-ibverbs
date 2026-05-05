thunderbolt-ibverbs
===================

kernel module + userspace `rdma_core` driver for ibverbs via thunderbolt/usb4 soft-DMA

context / background
--------------------

running AI models locally requires lots of VRAM - for enthusiasts its easy to chuck
a bunch of gpus in a milk-crate, but where does that leave non-technical people?

historically, consumer demand for 'high performance computing at home' has been
satisfied by game consoles- xbox, playstation etc have been nothing more than a
high-performance desktop gaming pc in plastic case to hide the scary electronics

the main architectural innovation has been 'unified memory' - to lower costs, instead
of including separate system memory and video memory like a desktop pc, console-makers 
shipped a single unified fast memory that cpu and gpu (together- APU) share access to.

this shape has been adopted by Apple for all of their computers since the shift 
to their own M-series silicon, and the enormous memory bandwidth (~800gb/s on M4,
close to the 1.2tb/s of a nvidia 4090). AMD's version of this is the 'strix halo'
platform, used in laptops but also ai-focused mini PCs. nvidia's is the GB10, used
in DGX spark mini-workstation, builds on their experience making the soc for nintendo's
switch portable console.

so we have three broad classes of device for 'inference at home':

 - apple mac mini/studio - $4099 for 128GB/2TB
 - nvidia gb10 (dgx spark, some other OEM devices) - $4699 for 128gb/4tb
 - amd ryzen ai max+ 395 - $2799 for 128GB/2TB ($1500 at launch!)

of these, only the amd solution is interesting to me:

 - dgx spark is too expensive, nobody outside of wealthy nerds will buy it
 - apple devices have closed-source OS and poor linux support - it's Somebody Else's Computer- 
not to mention the extortionate 'apple tax' on increasing storage, memory.
   (remember when they put U2 on everyones ipod- what's the AI equivalent?)

scaling
-------

so we have our 'open' platform for 'at-home' inference- but what if i want to run smth >128gb? how can i team these?
this comes down to preserving the 'lots of fast vram' property, and requires unpacking
'fast' into two things:

 - low latency - how long do i have to wait to fetch a single item? unit: time

in computing, low latency usually required specialised communication libraries with simpler,
stateless semantics that bypass the traditional stack. your userspace library can point 
to some data and tell the library- transfer that to a remote machine- it doesnt need to spend
cpu cycles copying the data word-by-word- we call this RDMA, and the closest thing there 
is for a 'standard' how to do this is the simple set of verbs (SEND, RECEIVE, etc) used by 
the Infiniband protocol- hence ibverbs.

 - bandwidth - assuming many fetches happen in parallel, what the total items per second: units: time^-1

dgx spark contains a high-end ($1k+ alone) 400gb connectx7 nic - you can connect these
point-to-point/ring or via a switch ($2k++) if you want more. extremely low latency and
very high bandwidth, supports RDMA via ibverbs api- NCCL, etc work.

apple devices support networking via thunderbolt 5 at 80gbps per port - most devices have
2 - 4 thunderbolt connections, same p2p/ring config as spark. their own comms lib, JACCL
works very similarly to NCCL.

AMD strix halo devices support... nothing?

 - most only have 2.5g ethernet- i believe RoCE2 (RDMA-over-converged-ethernet) is supported,
but this runs on top of the existing ethernet and tcp/ip stacks, so worse in terms of latency 

 - some pcie 4.0 lanes are exposed- you can, for example, purchase a faster nic, use pcie
extender to attach it to these lanes and you'll get low latency and ~64gbps per slot, but cumbersome
afaict the pcie controller in this SOC does not support 'endpoint mode', only 'host mode', so you 
can't connect these symmetrically- both want to be the 'host', neither the 'device'.

 - 2x usb4 '80gbps' ports- perhaps the fault of my crappy aliexpress cables but i've never 
seen these train above 40gbps (2x 20g lanes), and usually only 20 (2x 10g lanes). linux does
have support for IP networking over thunderbolt via the thunderbolt-net driver, but
nobody really seems to use it (or PC hardware is not capable of emulating the Apple protocol):

 - locks up under load, often 'wedging' the controller, requiring reboot to fix
 - has no hardware offloading for things like acks, checksum calc, coalescing- high cpu load, poor latency
 - interrupts are course ("some bytes have arrived on the tb wire" vs "the reply to request x has arrived")- ditto
 - resource utilisation is poor- connect two machines via two cables you may naively expect an 80gbps 
link (i think apple DOES? todo: check)- but check via iperf and you'll see a max of about 8gbps- before it locks up

this isn't a criticism of the existing driver (or its maintainer) it but simply an observation
of something common in OSS- if a use-case isn't popular, it wont get many eyeballs and many fixes,
reinforcing its unpopularity.

usb4_rdma
=========

usb4_rdma is an experimental Linux RDMA-over-USB4/Thunderbolt transport. It exposes Thunderbolt XDomain/NHI DMA rings as normal libibverbs devices (usb4_rdma*), so RDMA-aware software like RCCL, MPI, JACCL,
and vLLM can use direct host-to-host Thunderbolt links without putting bulk traffic through thunderbolt_net TCP/IP.

At a high level, the driver claims a Thunderbolt peer service, opens raw DMA rings, implements the core verbs queue-pair/completion/memory-registration path, and can expose each USB4 rail as a separate RDMA
HCA. TCP is still useful for bootstrap and metadata, but collectives move over Thunderbolt DMA.

Current Linux↔Linux results on AMD Strix Halo are promising. UC SEND reaches about 9.1-9.6 Gb/s on a 10 Gb/s x2 link, RC write/read hit about 20.4/18.3 Gb/s, and four exposed rails aggregate to about 38.5 Gb/s
in concurrent perftest. Latency is also much lower than the stock IP path: recent usb4_rdma UC SEND latency at 4 KiB measured about 8.4 us min, ~12.2 us median, ~12.6 us average, ~20 us p99, and ~26.7 us
p99.9. By comparison, rxe-over-Thunderbolt-net had an 18-23 us best-case floor but a typical RTT around 65 us.

RCCL/vLLM successfully uses the devices via NET/IB; in one Llama-3.1-8B TP=2 run, usb4_rdma reached 876.8 total tok/s at concurrency 256 versus 635.5 tok/s over Thunderbolt TCP and 562.0 tok/s over rxe-on-
Thunderbolt-net. A four-HCA rail-mode run reached 1207.4 tok/s at concurrency 512.

apple_rdma
==========

after i got the above working- it occurred to me- why not speak the macos protocol- maybe we can even have x-compat? turns out- the thunderbolt-net procotocl IS the apple protocol- probably RDMA is similar.

• Mac compatibility is partially working, but Apple’s protocol is not the same as our Linux↔Linux usb4_rdma wire format. Apple exposes RDMA through an AD/FA57 Thunderbolt service, uses UC verbs only, 4 KiB frame
  accounting, strict receive-credit flow control, and an Apple-specific descriptor/PDF marker layout. We have matched enough of that to advertise a service, bring macOS rdma_en* ports to PORT_ACTIVE, expose
  IPv4-mapped GIDs, create QPs, receive Mac-originated frames, send small accepted Linux→Mac FRAME+E2E messages, and get JACCL through its barrier.

  Known Apple-format details: the useful TX unit is not our old 4096-byte raw frame. In FRAME mode, Linux must account for NHI per-descriptor CRC overhead, so a full Apple-compatible chunk carries 4032 user
  bytes over a 4096-byte wire frame, with marker bits distinguishing start, interior, split/tail, and terminal descriptors. Apple also appears to require Thunderbolt E2E credit behavior for correctness, while
  our original raw/no-E2E Linux transport could run fast but overran macOS during timing-sensitive workloads.

  The current blocker is sustained bidirectional flow control. Mac↔Linux JACCL can initialize and pass the barrier, but all-sum still stalls after the first message. On AMD Strix Halo, Apple-compatible RDMA E2E
  and thunderbolt_net E2E also appear to collide in the NHI credit accounting, so the remaining work is less “decode the payload” and more “make Apple’s E2E credit model coexist reliably with Linux’s Thunderbolt
  stack.”

