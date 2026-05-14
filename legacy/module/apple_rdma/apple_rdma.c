// SPDX-License-Identifier: GPL-2.0
/*
 * apple_rdma.c - experimental Apple RDMA-over-Thunderbolt verbs peer.
 *
 * This is intentionally separate from the Linux/Linux usb4_rdma module. It
 * claims Apple's AD/FA57 Thunderbolt service, advertises the reciprocal
 * service that macOS requires for rdma_enN PORT_ACTIVE, programs the E2E path
 * shape that made Mac -> Linux SEND complete in capture, and exposes a small
 * UC-only verbs device.
 *
 * First target: let a Mac UC SEND on QPN 0x900 land in a Linux posted RECV WR
 * and produce a normal RECV CQE. Linux -> Mac SEND is also shaped like the
 * captured Apple descriptor stream, but previous raw replay showed that this
 * direction still needs more Apple-specific state.
 */

#define pr_fmt(fmt) "apple_rdma: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/refcount.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/bottom_half.h>
#include <linux/idr.h>
#include <linux/uuid.h>
#include <linux/thunderbolt.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/dma-mapping.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/ktime.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/inet.h>
#include <linux/string.h>
#include <linux/hex.h>
#include <net/addrconf.h>
#include <net/net_namespace.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/uverbs_ioctl.h>
#include <rdma/ib_mad.h>

#define ARDMA_DRV_NAME		"apple_rdma"

#define APPLE_RDMA_PRTCID	0xFA57
#define APPLE_RDMA_PRTCVERS	1
#define APPLE_RDMA_PRTCREVS	0

#define ARDMA_FRAME_SIZE	SZ_4K
#define ARDMA_SLOT_WIRE_SIZE	256
#define ARDMA_RAW_SLOT_USER_SIZE	256
#define ARDMA_FRAME_SLOT_USER_SIZE	252
#define ARDMA_RAW_SPLIT_USER_SIZE	16
#define ARDMA_FRAME_SPLIT_USER_SIZE	12
#define ARDMA_TAIL_USER_SIZE	240
#define ARDMA_DEFAULT_RING_DEPTH	16384
#define ARDMA_MIN_RING_DEPTH		256
#define ARDMA_MAX_RING_DEPTH		32768
#define ARDMA_RING_RESERVE		64
#define ARDMA_MAX_SGE		4
#define ARDMA_MAX_CQE		4096
#define ARDMA_MAX_MSG_SIZE	SZ_16M
#define ARDMA_PENDING_RX_BYTES	SZ_512K
#define ARDMA_PENDING_RX_SLOTS	16
#define ARDMA_DEFAULT_TX_POOL_FRAMES	4096
#define ARDMA_MAX_TX_POOL_FRAMES	16384
#define ARDMA_MAX_PATH_LANES		4
#define ARDMA_APPLE_RAW_CHUNK_BYTES	SZ_4K
#define ARDMA_APPLE_RAW_FRAMES_PER_CHUNK	17
#define ARDMA_APPLE_RAW_MAX_RECV_BYTES	SZ_512K
#define ARDMA_APPLE_RAW_RX_WINDOW_BYTES	SZ_2M
#define ARDMA_RAW_RX_CREDIT_HEADROOM	64
#define ARDMA_RX_MARKER_LOG_ENTRIES	8192
#define ARDMA_RX_MARKER_PREFIX	128
#define ARDMA_EVENT_LOG_ENTRIES	65536
#define ARDMA_UVERBS_ABI	1
#define ARDMA_QPN_MIN		0x900
#define ARDMA_QPN_MAX		0x00ffffff
#define ARDMA_QPN_STRIDE	0x100
#define ARDMA_QPN_BASE_SHIFT	8

#define ARDMA_RX_RAW_SOF_MASK	0xffff
#define ARDMA_RX_RAW_EOF_MASK	0xffff
#define ARDMA_RX_FRAME_SOF_MASK	BIT(1)
#define ARDMA_RX_FRAME_EOF_MASK	(BIT(2) | BIT(3))
#define ARDMA_MAX_CTRL_MSGS	32
#define ARDMA_MAX_CTRL_BYTES	256
#define ARDMA_NHI_REG_TX_RING_BASE	0x00000
#define ARDMA_NHI_REG_RX_RING_BASE	0x08000
#define ARDMA_NHI_REG_TX_OPTIONS_BASE	0x19800
#define ARDMA_NHI_REG_RX_OPTIONS_BASE	0x29800
#define ARDMA_NHI_RING_SLOT_SIZE	16
#define ARDMA_NHI_OPTIONS_SLOT_SIZE	32
#define ARDMA_NHI_OPTIONS_DWORDS	(ARDMA_NHI_OPTIONS_SLOT_SIZE / sizeof(u32))
#define ARDMA_NHI_OPT_E2E_HOP_SHIFT	12
#define ARDMA_NHI_OPT_E2E_HOP_MASK	GENMASK(22, 12)
#define ARDMA_NHI_OPT_E2E_FLOW		BIT(28)
#define ARDMA_NHI_OPT_NO_SNOOP		BIT(29)
#define ARDMA_NHI_OPT_RAW		BIT(30)
#define ARDMA_NHI_OPT_ENABLE		BIT(31)
#define ARDMA_DESC_FLAGS_SHIFT	20

struct ardma_ring_desc {
	u64 phys;
	u32 meta;
	u32 time;
} __packed;

static_assert(sizeof(struct ardma_ring_desc) == 16);

static const char apple_rdma_key[9] = {
	(char)0xff, (char)0xff, (char)0xff, (char)0xff,
	(char)0xff, (char)0xff, 'A', 'D', '\0',
};

static const char apple_rdma_ca_key[9] = {
	(char)0xff, (char)0xff, (char)0xff, (char)0xff,
	(char)0xff, (char)0xff, 'C', 'A', '\0',
};

static const uuid_t apple_rdma_default_service_uuid =
	UUID_INIT(0x49bf223e, 0xd4aa, 0x44d7,
		  0x87, 0x91, 0x50, 0x44, 0x5a, 0xc5, 0x2d, 0x5e);
static uuid_t apple_rdma_service_uuid =
	UUID_INIT(0x49bf223e, 0xd4aa, 0x44d7,
		  0x87, 0x91, 0x50, 0x44, 0x5a, 0xc5, 0x2d, 0x5e);

static bool advertise_service = true;
module_param(advertise_service, bool, 0444);
MODULE_PARM_DESC(advertise_service,
		 "Advertise reciprocal Apple AD/FA57 service (default: true)");

static unsigned int advertise_prtcstns;
module_param(advertise_prtcstns, uint, 0444);
MODULE_PARM_DESC(advertise_prtcstns,
		 "Protocol settings value advertised in the Apple RDMA service prtcstns property (default: 0)");

static char *service_uuid;
module_param(service_uuid, charp, 0444);
MODULE_PARM_DESC(service_uuid,
		 "Override advertised Apple AD/FA57 service UUID");

static unsigned int login_timeout_ms = 500;
module_param(login_timeout_ms, uint, 0644);
MODULE_PARM_DESC(login_timeout_ms,
		 "tb_xdomain_request timeout for debugfs login_send probe (default: 500)");

static int receive_path = 9;
module_param(receive_path, int, 0444);
MODULE_PARM_DESC(receive_path,
		 "Incoming Apple transmit HopID to bind as our RX path (default: 9)");

static int transmit_path = 9;
module_param(transmit_path, int, 0444);
MODULE_PARM_DESC(transmit_path,
		 "Outgoing Apple transmit HopID to request for our TX path (-1 = auto; default: 9)");

static unsigned int reserve_low_out_hops;
module_param(reserve_low_out_hops, uint, 0444);
MODULE_PARM_DESC(reserve_low_out_hops,
		 "Diagnostic: reserve this many low auto out HopIDs before allocating the real TX path");

static int local_tx_hop = 2;
module_param(local_tx_hop, int, 0444);
MODULE_PARM_DESC(local_tx_hop,
		 "Local NHI TX ring hop ID to allocate (default: 2 for the Mac-compatible profile; -1 = auto).");

static int local_rx_hop = 2;
module_param(local_rx_hop, int, 0444);
MODULE_PARM_DESC(local_rx_hop,
		 "Local NHI RX ring hop ID to allocate (default: 2 for the Mac-compatible profile; -1 = auto)");

static bool apple_vendor_only;
module_param(apple_vendor_only, bool, 0444);
MODULE_PARM_DESC(apple_vendor_only,
		 "Bind only peers whose xdomain vendor_name is Apple Inc. (default: false; Linux peers are allowed)");

static char *peer_device_name;
module_param(peer_device_name, charp, 0444);

/*
 * Linux<->Linux setups can have two TB cables between the same two
 * machines (one per NHI / TB controller). Each cable advertises a
 * separate FA57 peer at a different XDomain route.
 *
 * Set peer_route to the hex route value (e.g. "2") to force probing
 * only the peer at that route. Default empty = first to win the mutex.
 */
static char *peer_route;
module_param(peer_route, charp, 0444);
MODULE_PARM_DESC(peer_route,
		 "Bind only the AD/FA57 peer whose XDomain route matches this hex value (default: first to probe)");
static char *peer_gid_routes;
module_param(peer_gid_routes, charp, 0644);
MODULE_PARM_DESC(peer_gid_routes,
		 "Comma-separated IPv4-to-peer route map for multi-peer QP binding, e.g. 10.0.3.3=0-2,192.168.23.192=1-2");
MODULE_PARM_DESC(peer_device_name,
		 "Bind only a peer whose xdomain device_name exactly matches this value");

static char *cm_netdev = "auto";
module_param(cm_netdev, charp, 0444);
MODULE_PARM_DESC(cm_netdev,
		 "netdev used by RDMA core for RoCE GID table: 'auto' = thunderbolt<domain>, otherwise use this netdev for every peer");
static char *peer_netdevs;
module_param(peer_netdevs, charp, 0444);
MODULE_PARM_DESC(peer_netdevs,
		 "Comma-separated route-to-netdev map for per-peer RDMA devices, e.g. 0-2=thunderbolt0,1-2=thunderbolt1");

static bool tx_enabled = true;
module_param(tx_enabled, bool, 0444);
MODULE_PARM_DESC(tx_enabled,
		 "Enable TX data path (default: true)");

static bool tx_raw_mode;
module_param(tx_raw_mode, bool, 0444);
MODULE_PARM_DESC(tx_raw_mode,
		 "Transmit RAW descriptors instead of FRAME descriptors (default: false)");

static bool tx_e2e = true;
module_param(tx_e2e, bool, 0444);
MODULE_PARM_DESC(tx_e2e,
		 "Enable E2E flow control on the TX ring (default: true)");

static bool rx_e2e = true;
module_param(rx_e2e, bool, 0444);
MODULE_PARM_DESC(rx_e2e,
		 "Enable E2E flow control on the RX ring (default: true)");

static bool tx_honor_path_mtu;
module_param(tx_honor_path_mtu, bool, 0444);
MODULE_PARM_DESC(tx_honor_path_mtu,
		 "Honor QP path MTU in FRAME mode instead of active_mtu (default: false)");

static bool tx_frame_single_desc = true;
module_param(tx_frame_single_desc, bool, 0644);
MODULE_PARM_DESC(tx_frame_single_desc,
		 "Use one FRAME descriptor per Apple TX chunk when possible (default: true)");

static unsigned int active_mtu = 4096;
module_param(active_mtu, uint, 0444);
MODULE_PARM_DESC(active_mtu,
		 "Advertised and effective active MTU bytes (default: 4096)");

static bool rx_poll_mode;
module_param(rx_poll_mode, bool, 0444);
MODULE_PARM_DESC(rx_poll_mode,
		 "Use RX polling thread instead of ring callback (default: false)");

static unsigned int rx_post_frames;
module_param(rx_post_frames, uint, 0444);
MODULE_PARM_DESC(rx_post_frames,
		 "Number of RX descriptors to post initially and recycle (0 = auto: Apple RAW uses ring_depth - 64; Linux FRAME uses 64)");

static unsigned int tx_pool_frames = ARDMA_DEFAULT_TX_POOL_FRAMES;
module_param(tx_pool_frames, uint, 0444);
MODULE_PARM_DESC(tx_pool_frames,
		 "Pre-mapped per-QP TX bounce frames, 0..16384; exhausted pools fall back to dynamic DMA mapping (default: 4096)");

static bool tx_zcopy = true;
module_param(tx_zcopy, bool, 0444);
MODULE_PARM_DESC(tx_zcopy,
		 "Use pre-mapped registered MR pages directly for page-contained TX slices when possible (default: true)");

static unsigned int ring_depth = ARDMA_DEFAULT_RING_DEPTH;
module_param(ring_depth, uint, 0444);
MODULE_PARM_DESC(ring_depth,
		 "NHI TX/RX ring depth, power of two, 256..32768; load-time only (default: 16384)");

static unsigned int path_lanes = 1;
module_param(path_lanes, uint, 0444);
MODULE_PARM_DESC(path_lanes,
		 "Diagnostic Apple HopID lanes to wire starting at receive_path; lane i maps incoming HopID receive_path+i to QPN (receive_path+i)<<8 (default: 1, max: 4)");

enum {
	ARDMA_TX_MARKER_APPLE_EOF = 0,
	ARDMA_TX_MARKER_GEMINI_SOF = 1,
	ARDMA_TX_MARKER_BOTH_START = 2,
	ARDMA_TX_MARKER_EVERY_DESC = 3,
	ARDMA_TX_MARKER_EVERY_SOF_APPLE_EOF = 4,
};

static unsigned int tx_marker_mode = ARDMA_TX_MARKER_GEMINI_SOF;
module_param(tx_marker_mode, uint, 0644);
MODULE_PARM_DESC(tx_marker_mode,
		 "TX marker mode diagnostic knob (default: 1)");

static unsigned int tx_terminal_eof = 3;
module_param(tx_terminal_eof, uint, 0644);
MODULE_PARM_DESC(tx_terminal_eof,
		 "Terminal EOF marker value for Apple TX descriptors (default: 3)");
static unsigned int tx_pace_us;
module_param(tx_pace_us, uint, 0644);
MODULE_PARM_DESC(tx_pace_us,
		 "Microseconds to sleep between TX chunks (default: 0)");
static unsigned int tx_inter_wr_gap_us;
module_param(tx_inter_wr_gap_us, uint, 0644);
MODULE_PARM_DESC(tx_inter_wr_gap_us,
		 "Minimum microseconds between posted send WRs (default: 0)");

/* Diagnostic-only pacing knobs. Default 0 (no pacing). When nonzero,
 * ardma_send_apple sleeps tx_pace_us microseconds between negotiated MTU chunks
 * - used to test whether the JACCL bracket-transition failure is a
 * software-timing overrun (would be fixed by enabling raw+E2E hardware
 * flow control). Settable at runtime via sysfs. */
static bool event_log = true;
module_param(event_log, bool, 0644);
MODULE_PARM_DESC(event_log,
		 "Record per-peer TX/RX debugfs event rings with timestamps (default: true)");

static bool tx_force_drain_on_destroy = true;
module_param(tx_force_drain_on_destroy, bool, 0644);
MODULE_PARM_DESC(tx_force_drain_on_destroy,
		 "Force-cancel the shared TX ring when QP destroy cannot drain pending TX refs (default: true)");

static unsigned int mr_dereg_timeout_ms = 5000;
module_param(mr_dereg_timeout_ms, uint, 0644);
MODULE_PARM_DESC(mr_dereg_timeout_ms,
		 "Milliseconds to wait for MR refs before forcing TX drain; 0 waits forever (default: 5000)");

static bool enable_paths_on_setup = true;
module_param(enable_paths_on_setup, bool, 0444);
MODULE_PARM_DESC(enable_paths_on_setup,
		 "Enable Thunderbolt DMA paths during probe; disable for TB-IP coexistence diagnostics (default: true)");

static bool disable_paths_on_idle = true;
module_param(disable_paths_on_idle, bool, 0644);
MODULE_PARM_DESC(disable_paths_on_idle,
		 "Disable Thunderbolt DMA paths when the last UC QP is destroyed (default: true)");

static bool raw_rx_credit_guard = true;
module_param(raw_rx_credit_guard, bool, 0644);
MODULE_PARM_DESC(raw_rx_credit_guard,
		 "Reject Apple RAW recv windows that exceed the RX descriptor credit budget (default: true)");

static unsigned int raw_rx_max_recv_bytes = ARDMA_APPLE_RAW_MAX_RECV_BYTES;
module_param(raw_rx_max_recv_bytes, uint, 0644);
MODULE_PARM_DESC(raw_rx_max_recv_bytes,
		 "Maximum single posted receive size accepted for Apple RAW RX; 0 disables (default: 524288)");

static unsigned int raw_rx_window_bytes = ARDMA_APPLE_RAW_RX_WINDOW_BYTES;
module_param(raw_rx_window_bytes, uint, 0644);
MODULE_PARM_DESC(raw_rx_window_bytes,
		 "Maximum total bytes of posted Apple RAW receive WRs per peer; 0 disables (default: 2097152)");

struct ardma_peer;
struct ardma_qp;

struct ardma_ucontext {
	struct ib_ucontext base;
};

struct ardma_mr {
	struct ib_mr base;
	struct list_head pd_link;
	refcount_t refs;
	wait_queue_head_t ref_wait;
	bool dying;
	u64 user_va;
	u64 length;
	int npages;
	struct page **pages;
	dma_addr_t *dma_addrs;
	struct device *dma_dev;
};

struct ardma_pd {
	struct ib_pd base;
	struct list_head mrs;
	spinlock_t mr_lock;
};

struct ardma_ah {
	struct ib_ah base;
	struct rdma_ah_attr attr;
};

struct ardma_wc_entry {
	struct list_head list;
	struct ib_wc wc;
};

struct ardma_cq {
	struct ib_cq base;
	spinlock_t lock;
	struct list_head wc_list;
	struct list_head free_list;
	struct ardma_wc_entry *pool;
	int cqe_capacity;
	int wc_count;
	int free_count;
	enum ib_cq_notify_flags notify;
};

struct ardma_recv_wr {
	struct list_head list;
	u64 wr_id;
	struct ib_cqe *wr_cqe;
	u32 raw_rx_credits;
	u32 raw_rx_bytes;
	int num_sge;
	struct ib_sge sge[ARDMA_MAX_SGE];
	struct ardma_mr *mr[ARDMA_MAX_SGE];
};

struct ardma_pending_rx {
	u8 *buf;
	u32 len;
	bool active;
	bool ready;
	bool truncated;
};

struct ardma_qp {
	struct ib_qp base;
	struct ardma_peer *peer;
	enum ib_qp_type qp_type;
	enum ib_qp_state state;
	struct ib_qp_attr attr;
	struct ib_qp_init_attr init_attr;
	int attr_mask;
	refcount_t refs;
	wait_queue_head_t ref_wait;
	bool qpn_allocated;
	bool registered;
	bool peer_qp_active;
	bool sq_sig_all;

	spinlock_t recv_lock;
	struct list_head recv_q;
	u32 recv_q_depth;
	struct ardma_recv_wr *rx_wr;
	u32 rx_piece;
	u32 rx_byte_len;
	bool rx_truncated;
	struct ardma_pending_rx rx_pending[ARDMA_PENDING_RX_SLOTS];
	u8 rx_pending_head;
	u8 rx_pending_tail;
	u8 rx_pending_ready_count;
	int rx_pending_active;

	spinlock_t send_lock;
	u64 last_post_send_ns;
	spinlock_t tx_pool_lock;
	struct list_head tx_pool_free;
	struct ardma_tx_frame *tx_pool;
	struct device *tx_dma_dev;
	u32 tx_pool_count;
	u32 tx_pool_free_count;

	spinlock_t list_lock;
	struct list_head qps_link;
};

struct ardma_ibdev {
	struct ib_device base;
	struct ardma_peer *peer;
	struct net_device *netdev;
	struct notifier_block netdev_nb;
	struct mutex netdev_lock;
	char netdev_name[IFNAMSIZ];
	bool netdev_nb_registered;
	bool shutting_down;
};

struct ardma_rx_frame {
	struct ring_frame frame;
	struct ardma_peer *peer;
	u32 qpn;
	void *data;
	dma_addr_t dma;
};

struct ardma_path_lane {
	int local_in_hop;
	int local_out_hop;
	bool paths_enabled;
	bool tx_ring_running;
	struct tb_ring *tx_ring;
	struct tb_ring *rx_ring;
	struct ardma_rx_frame *rx_frames;
	u32 rx_posted_frames;
	u32 rx_frame_capacity;
	u32 qpn;
};

struct ardma_send_ctx {
	struct ardma_qp *qp;
	u64 wr_id;
	struct ib_cqe *wr_cqe;
	u32 qpn;
	u32 dest_qpn;
	u32 total_len;
	u32 blocks;
	atomic_t pending;
	atomic_t failed;
	bool signaled;
};

struct ardma_tx_frame {
	struct ring_frame frame;
	struct list_head pool_link;
	struct ardma_peer *peer;
	struct ardma_send_ctx *ctx;
	void *data;
	dma_addr_t dma;
	dma_addr_t dma_base;
	bool pooled;
	bool zcopy;
	struct ardma_mr *mr;
	u32 dma_off;
	u32 dma_len;
	u64 frame_id;
	u32 block;
	u32 piece;
	u32 app_off;
	u32 payload_len;
	u32 wire_len;
	u8 sof;
	u8 eof;
};

struct ardma_rx_marker_log_entry {
	u64 seq;
	u32 len;
	u32 meta;
	u16 flags;
	u8 sof;
	u8 eof;
	u8 prefix_len;
	u8 prefix[ARDMA_RX_MARKER_PREFIX];
};

enum ardma_tx_event_type {
	ARDMA_TX_EVT_WR_BEGIN,
	ARDMA_TX_EVT_DESC_SUBMIT,
	ARDMA_TX_EVT_DESC_COMPLETE,
	ARDMA_TX_EVT_DESC_CANCEL,
	ARDMA_TX_EVT_DESC_FAIL,
	ARDMA_TX_EVT_WR_POSTED,
	ARDMA_TX_EVT_WR_FAIL,
	ARDMA_TX_EVT_SEND_CQE,
};

struct ardma_tx_event_entry {
	u64 seq;
	u64 ns;
	u64 wr_id;
	u64 frame_id;
	u32 qpn;
	u32 dest_qpn;
	u32 total_len;
	u32 block;
	u32 piece;
	u32 app_off;
	u32 payload_len;
	u32 wire_len;
	u32 pending;
	s32 ret;
	u8 type;
	u8 sof;
	u8 eof;
	u8 marker_mode;
	u8 raw_mode;
	u8 e2e;
	u8 signaled;
	u8 canceled;
};

enum ardma_rx_event_type {
	ARDMA_RX_EVT_QP_CREATE,
	ARDMA_RX_EVT_QP_MODIFY,
	ARDMA_RX_EVT_QP_DESTROY,
	ARDMA_RX_EVT_POST_RECV,
	ARDMA_RX_EVT_FRAME,
	ARDMA_RX_EVT_NO_QP,
	ARDMA_RX_EVT_WR_START,
	ARDMA_RX_EVT_PENDING_FRAME,
	ARDMA_RX_EVT_PENDING_DONE,
	ARDMA_RX_EVT_FLUSH_PENDING,
	ARDMA_RX_EVT_MSG_DONE,
	ARDMA_RX_EVT_RECV_CQE,
	ARDMA_RX_EVT_RAW_COPY,
};

struct ardma_rx_event_entry {
	u64 seq;
	u64 ns;
	u64 wr_id;
	u32 qpn;
	u32 dest_qpn;
	u32 len;
	u32 byte_len;
	u32 expected_len;
	u32 recv_q_depth;
	u32 pending_ready;
	u32 rx_piece;
	s32 ret;
	u8 type;
	u8 sof;
	u8 eof;
	u8 status;
	u8 old_state;
	u8 new_state;
	u8 registered;
	u8 pending_active;
};

struct ardma_peer {
	struct list_head peers_link;
	struct tb_service *svc;
	struct tb_xdomain *xd;
	struct tb_protocol_handler remote_ad_protocol_handler;
	uuid_t remote_ad_uuid;
	bool remote_ad_protocol_registered;
	refcount_t refs;
	wait_queue_head_t ref_wait;
	bool closing;

	int local_in_hop;
	int local_out_hop;
	int reserved_out_hop;
	bool paths_enabled;
	bool remote_is_apple;
	bool rx_raw_wire;
	struct ardma_ibdev *ibdev;
	struct tb_ring *tx_ring;
	struct tb_ring *rx_ring;
	struct ardma_rx_frame *rx_frames;
	u32 rx_posted_frames;
	u32 rx_frame_capacity;
	struct mutex tx_lock;
	bool tx_ring_running;
	unsigned int active_uc_qps;

	/* RX poll-mode (rx_poll_mode=1): per-peer kthread drains the RX ring
	 * via tb_ring_poll() to bypass NHI IRQ moderation. Unused (NULL) when
	 * the IRQ-driven path is in effect. */
	struct task_struct *rx_poll_task;
	struct completion rx_poll_kick;
	atomic_t rx_poll_stop;

	struct dentry *debugfs_dir;
	spinlock_t rx_marker_lock;
	u64 rx_marker_seq;
	u32 rx_marker_pos;
	struct ardma_rx_marker_log_entry
		rx_marker_log[ARDMA_RX_MARKER_LOG_ENTRIES];
	spinlock_t tx_event_lock;
	u64 tx_event_seq;
	u32 tx_event_pos;
	atomic64_t tx_frame_ids;
	struct ardma_tx_event_entry *tx_event_log;
	spinlock_t rx_event_lock;
	u64 rx_event_seq;
	u32 rx_event_pos;
	struct ardma_rx_event_entry *rx_event_log;
	atomic64_t rx_frame_count;
	atomic64_t rx_eof[4];
	atomic64_t rx_eof_other;
	atomic64_t rx_messages;
	atomic64_t rx_drops;
	atomic64_t rx_bad_shape;
	atomic64_t rx_no_qp;
	atomic64_t tx_frames;
	atomic64_t tx_completions;
	atomic64_t tx_errors;
	atomic64_t tx_zcopy_frames;
	atomic64_t tx_pool_frames;
	atomic64_t tx_dynamic_frames;
	atomic_t raw_rx_credits_reserved;
	atomic64_t raw_rx_bytes_reserved;
	unsigned int extra_lane_count;
	struct ardma_path_lane extra_lanes[ARDMA_MAX_PATH_LANES - 1];
};

struct ardma_ctrl_log_entry {
	struct list_head list;
	char source[32];
	u32 size;
	u32 dump_len;
	u8 dump[ARDMA_MAX_CTRL_BYTES];
};

static struct tb_property_dir *ardma_property_dir;
static struct tb_protocol_handler ardma_protocol_handler;
static struct tb_service_driver ardma_service_driver;
static struct dentry *ardma_debugfs_root;

static DEFINE_MUTEX(ardma_peer_lock);
static LIST_HEAD(ardma_peer_list);
static DEFINE_IDA(ardma_qpn_slots);
static LIST_HEAD(ardma_qp_list);
static DEFINE_SPINLOCK(ardma_qp_lock);
static atomic_t ardma_ctrl_logged;
static atomic_t ardma_ctrl_received;
static DEFINE_SPINLOCK(ardma_ctrl_lock);
static LIST_HEAD(ardma_ctrl_list);

static int ardma_ctrl_callback(const void *buf, size_t size, void *data);
static void ardma_peer_put(struct ardma_peer *peer);
static void ardma_peer_qp_activate(struct ardma_qp *qp,
				   struct ardma_peer *peer);

static bool ardma_xdomain_is_apple(const struct tb_xdomain *xd)
{
	return xd && xd->vendor_name && !strcmp(xd->vendor_name, "Apple Inc.");
}

static bool ardma_peer_rx_raw(const struct ardma_peer *peer)
{
	return peer && peer->rx_raw_wire;
}

static int ardma_parse_route_spec(const char *spec, int *domain, u64 *route)
{
	const char *dash;
	char dom_buf[8] = {0};
	size_t dom_len;

	if (!spec || !*spec || !domain || !route)
		return -EINVAL;

	dash = strchr(spec, '-');
	if (!dash)
		return -EINVAL;
	dom_len = dash - spec;
	if (!dom_len || dom_len >= sizeof(dom_buf))
		return -EINVAL;
	memcpy(dom_buf, spec, dom_len);
	if (kstrtoint(dom_buf, 10, domain))
		return -EINVAL;
	if (kstrtou64(dash + 1, 16, route))
		return -EINVAL;
	return 0;
}

static bool ardma_gid_ipv4_mapped(const union ib_gid *gid, __be32 *addr)
{
	static const u8 prefix[12] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff
	};

	if (!gid || memcmp(gid->raw, prefix, sizeof(prefix)))
		return false;
	if (addr)
		memcpy(addr, gid->raw + 12, sizeof(*addr));
	return true;
}

static const char *ardma_tx_event_name(u8 type)
{
	switch (type) {
	case ARDMA_TX_EVT_WR_BEGIN:
		return "wr_begin";
	case ARDMA_TX_EVT_DESC_SUBMIT:
		return "desc_submit";
	case ARDMA_TX_EVT_DESC_COMPLETE:
		return "desc_complete";
	case ARDMA_TX_EVT_DESC_CANCEL:
		return "desc_cancel";
	case ARDMA_TX_EVT_DESC_FAIL:
		return "desc_fail";
	case ARDMA_TX_EVT_WR_POSTED:
		return "wr_posted";
	case ARDMA_TX_EVT_WR_FAIL:
		return "wr_fail";
	case ARDMA_TX_EVT_SEND_CQE:
		return "send_cqe";
	default:
		return "unknown";
	}
}

static const char *ardma_rx_event_name(u8 type)
{
	switch (type) {
	case ARDMA_RX_EVT_QP_CREATE:
		return "qp_create";
	case ARDMA_RX_EVT_QP_MODIFY:
		return "qp_modify";
	case ARDMA_RX_EVT_QP_DESTROY:
		return "qp_destroy";
	case ARDMA_RX_EVT_POST_RECV:
		return "post_recv";
	case ARDMA_RX_EVT_FRAME:
		return "frame";
	case ARDMA_RX_EVT_NO_QP:
		return "no_qp";
	case ARDMA_RX_EVT_WR_START:
		return "wr_start";
	case ARDMA_RX_EVT_PENDING_FRAME:
		return "pending_frame";
	case ARDMA_RX_EVT_PENDING_DONE:
		return "pending_done";
	case ARDMA_RX_EVT_FLUSH_PENDING:
		return "flush_pending";
	case ARDMA_RX_EVT_MSG_DONE:
		return "msg_done";
	case ARDMA_RX_EVT_RECV_CQE:
		return "recv_cqe";
	case ARDMA_RX_EVT_RAW_COPY:
		return "raw_copy";
	default:
		return "unknown";
	}
}

static void ardma_log_tx_event(struct ardma_peer *peer, u8 type, u32 qpn,
			       u32 dest_qpn, u64 wr_id, u32 total_len,
			       u64 frame_id, u32 block, u32 piece,
			       u32 app_off, u32 payload_len, u32 wire_len,
			       u8 sof, u8 eof, u32 pending, s32 ret,
			       bool signaled, bool canceled)
{
	struct ardma_tx_event_entry *e;
	unsigned long flags;
	u32 pos;

	if (!peer || !peer->tx_event_log || !READ_ONCE(event_log))
		return;

	spin_lock_irqsave(&peer->tx_event_lock, flags);
	pos = peer->tx_event_pos++ % ARDMA_EVENT_LOG_ENTRIES;
	e = &peer->tx_event_log[pos];
	memset(e, 0, sizeof(*e));
	e->seq = ++peer->tx_event_seq;
	e->ns = ktime_get_ns();
	e->type = type;
	e->wr_id = wr_id;
	e->frame_id = frame_id;
	e->qpn = qpn;
	e->dest_qpn = dest_qpn;
	e->total_len = total_len;
	e->block = block;
	e->piece = piece;
	e->app_off = app_off;
	e->payload_len = payload_len;
	e->wire_len = wire_len;
	e->sof = sof;
	e->eof = eof;
	e->pending = pending;
	e->ret = ret;
	e->marker_mode = READ_ONCE(tx_marker_mode);
	e->raw_mode = READ_ONCE(tx_raw_mode);
	e->e2e = READ_ONCE(tx_e2e);
	e->signaled = signaled;
	e->canceled = canceled;
	spin_unlock_irqrestore(&peer->tx_event_lock, flags);
}

static void ardma_log_rx_event(struct ardma_peer *peer, u8 type, u32 qpn,
			       u32 dest_qpn, u64 wr_id, u32 len,
			       u32 byte_len, u32 expected_len, u8 sof,
			       u8 eof, u8 status, u8 old_state,
			       u8 new_state, bool registered,
			       u32 recv_q_depth, u32 pending_ready,
			       int pending_active, u32 rx_piece, s32 ret)
{
	struct ardma_rx_event_entry *e;
	unsigned long flags;
	u32 pos;

	if (!peer || !peer->rx_event_log || !READ_ONCE(event_log))
		return;

	spin_lock_irqsave(&peer->rx_event_lock, flags);
	pos = peer->rx_event_pos++ % ARDMA_EVENT_LOG_ENTRIES;
	e = &peer->rx_event_log[pos];
	memset(e, 0, sizeof(*e));
	e->seq = ++peer->rx_event_seq;
	e->ns = ktime_get_ns();
	e->type = type;
	e->wr_id = wr_id;
	e->qpn = qpn;
	e->dest_qpn = dest_qpn;
	e->len = len;
	e->byte_len = byte_len;
	e->expected_len = expected_len;
	e->sof = sof;
	e->eof = eof;
	e->status = status;
	e->old_state = old_state;
	e->new_state = new_state;
	e->registered = registered;
	e->recv_q_depth = recv_q_depth;
	e->pending_ready = pending_ready;
	e->pending_active = pending_active >= 0;
	e->rx_piece = rx_piece;
	e->ret = ret;
	spin_unlock_irqrestore(&peer->rx_event_lock, flags);
}

static unsigned int ardma_active_mtu_bytes(void)
{
	switch (READ_ONCE(active_mtu)) {
	case 256:
	case 512:
	case 1024:
	case 2048:
	case 4096:
		return READ_ONCE(active_mtu);
	default:
		return 4096;
	}
}

static enum ib_mtu ardma_active_mtu_enum(void)
{
	return ib_mtu_int_to_enum(ardma_active_mtu_bytes());
}

static int ardma_qp_path_mtu_bytes(const struct ardma_qp *qp)
{
	if (qp->attr.path_mtu)
		return ib_mtu_enum_to_int(qp->attr.path_mtu);
	return ardma_active_mtu_bytes();
}

/* ----- MR helpers ------------------------------------------------ */

static struct ardma_mr *ardma_pd_get_mr(struct ardma_pd *pd, u32 lkey)
{
	struct ardma_mr *mr;
	unsigned long flags;

	spin_lock_irqsave(&pd->mr_lock, flags);
	list_for_each_entry(mr, &pd->mrs, pd_link) {
		if (mr->base.lkey == lkey && !mr->dying) {
			refcount_inc(&mr->refs);
			spin_unlock_irqrestore(&pd->mr_lock, flags);
			return mr;
		}
	}
	spin_unlock_irqrestore(&pd->mr_lock, flags);
	return NULL;
}

static void ardma_mr_put(struct ardma_mr *mr)
{
	if (refcount_dec_and_test(&mr->refs))
		WARN_ON_ONCE(1);
	if (refcount_read(&mr->refs) == 1)
		wake_up(&mr->ref_wait);
}

static void ardma_recv_wr_free(struct ardma_recv_wr *r)
{
	int i;

	if (!r)
		return;
	for (i = 0; i < r->num_sge; i++)
		if (r->mr[i])
			ardma_mr_put(r->mr[i]);
	kfree(r);
}

static u32 ardma_apple_raw_rx_credits(u32 len)
{
	u32 chunks;

	if (!len)
		return 1;
	chunks = DIV_ROUND_UP(len, ARDMA_APPLE_RAW_CHUNK_BYTES);
	if (chunks > U32_MAX / ARDMA_APPLE_RAW_FRAMES_PER_CHUNK)
		return U32_MAX;
	return chunks * ARDMA_APPLE_RAW_FRAMES_PER_CHUNK;
}

static u32 ardma_peer_raw_rx_credit_budget(const struct ardma_peer *peer)
{
	u32 budget;

	if (!peer || !peer->rx_frame_capacity)
		return 0;
	budget = peer->rx_frame_capacity;
	if (budget > ARDMA_RAW_RX_CREDIT_HEADROOM)
		budget -= ARDMA_RAW_RX_CREDIT_HEADROOM;
	return budget;
}

static void ardma_recv_wr_release(struct ardma_qp *qp, struct ardma_recv_wr *r)
{
	if (r && r->raw_rx_credits && qp && qp->peer)
		atomic_sub(r->raw_rx_credits,
			   &qp->peer->raw_rx_credits_reserved);
	if (r && r->raw_rx_bytes && qp && qp->peer)
		atomic64_sub(r->raw_rx_bytes,
			     &qp->peer->raw_rx_bytes_reserved);
	ardma_recv_wr_free(r);
}

static int ardma_mr_check_range(struct ardma_mr *mr, u64 vaddr, size_t len)
{
	if (vaddr < mr->user_va || len > mr->length ||
	    vaddr - mr->user_va > mr->length - len)
		return -ERANGE;
	return 0;
}

static int ardma_mr_xfer(struct ardma_mr *mr, u64 vaddr, void *kbuf,
			 size_t len, bool from_mr)
{
	u64 offset, page_idx, page_off;
	size_t copied = 0;
	int ret;

	ret = ardma_mr_check_range(mr, vaddr, len);
	if (ret)
		return ret;

	offset = (mr->user_va & ~PAGE_MASK) + (vaddr - mr->user_va);
	while (copied < len) {
		void *page_kva;
		size_t chunk;

		page_idx = (offset + copied) >> PAGE_SHIFT;
		page_off = (offset + copied) & ~PAGE_MASK;
		if (page_idx >= mr->npages)
			return -ERANGE;

		chunk = min_t(size_t, PAGE_SIZE - page_off, len - copied);
		page_kva = page_address(mr->pages[page_idx]);
		if (!page_kva)
			return -EFAULT;

		if (from_mr)
			memcpy((u8 *)kbuf + copied,
			       (u8 *)page_kva + page_off, chunk);
		else
			memcpy((u8 *)page_kva + page_off,
			       (u8 *)kbuf + copied, chunk);
		copied += chunk;
	}
	return 0;
}

static int ardma_mr_dma_page_slice(struct ardma_mr *mr, u64 vaddr, u32 len,
				   dma_addr_t *dma_base, u32 *dma_off)
{
	u64 offset, page_idx, page_off;
	int ret;

	if (!READ_ONCE(tx_zcopy) || !mr->dma_addrs || !len || len > PAGE_SIZE)
		return -EOPNOTSUPP;

	ret = ardma_mr_check_range(mr, vaddr, len);
	if (ret)
		return ret;

	offset = (mr->user_va & ~PAGE_MASK) + (vaddr - mr->user_va);
	page_idx = offset >> PAGE_SHIFT;
	page_off = offset & ~PAGE_MASK;
	if (page_idx >= mr->npages || page_off + len > PAGE_SIZE)
		return -EOPNOTSUPP;

	*dma_base = mr->dma_addrs[page_idx];
	*dma_off = page_off;
	return 0;
}

static int ardma_recv_scatter(struct ardma_pd *pd, struct ardma_recv_wr *r,
			      u32 dst_off, const void *payload, u32 len)
{
	u32 cur = 0, copied = 0;
	int i;

	for (i = 0; i < r->num_sge && copied < len; i++) {
		const struct ib_sge *sge = &r->sge[i];
		struct ardma_mr *mr;
		u32 in_sge_off, chunk;

		if (cur + sge->length <= dst_off) {
			cur += sge->length;
			continue;
		}

		in_sge_off = (dst_off + copied) - cur;
		chunk = min_t(u32, sge->length - in_sge_off, len - copied);
		mr = r->mr[i];
		if (!mr)
			return -EINVAL;
		if (ardma_mr_xfer(mr, sge->addr + in_sge_off,
				  (void *)payload + copied, chunk, false))
			return -EFAULT;
		copied += chunk;
	}

	return copied < len ? -ERANGE : 0;
}

/* FRAME-mode assembled-frame extractor.
 *
 * AMD silicon FRAME-mode RX assembles per-TLP arrivals into one callback
 * (gated on the configured eof_mask). Most 256-byte slots are 252 user
 * bytes followed by 4 silicon-owned trailer bytes.
 *
 * The Apple-shaped FRAME TX path packs the end of a full chunk as a small
 * split descriptor plus the 240-byte tail descriptor:
 *
 *   [split user][4-byte silicon trailer][240-byte tail user]
 *
 * A naive "copy first 252 bytes of every full slot" therefore copies the
 * split trailer at user offset 3792 in a 4096-byte SEND. Decode the terminal
 * slot shape explicitly and return only user bytes.
 */
static int ardma_recv_scatter_frame(struct ardma_pd *pd,
				    struct ardma_recv_wr *r, u32 dst_off,
				    const void *payload, u32 len,
				    u32 *out_user_len)
{
	u32 num_slots = len / ARDMA_SLOT_WIRE_SIZE;
	u32 tail = len - num_slots * ARDMA_SLOT_WIRE_SIZE;
	u32 written = 0;
	u32 normal_slots = num_slots;
	const u8 *p = payload;
	u32 i;
	int ret;

	if (!tail && num_slots)
		normal_slots--;

	for (i = 0; i < normal_slots; i++) {
		ret = ardma_recv_scatter(pd, r, dst_off + written,
					 p + i * ARDMA_SLOT_WIRE_SIZE,
					 ARDMA_FRAME_SLOT_USER_SIZE);
		if (ret)
			return ret;
		written += ARDMA_FRAME_SLOT_USER_SIZE;
	}

	if (!tail && num_slots) {
		const u8 *slot = p + normal_slots * ARDMA_SLOT_WIRE_SIZE;

		ret = ardma_recv_scatter(pd, r, dst_off + written, slot,
					 ARDMA_FRAME_SPLIT_USER_SIZE);
		if (ret)
			return ret;
		written += ARDMA_FRAME_SPLIT_USER_SIZE;

		ret = ardma_recv_scatter(pd, r, dst_off + written,
					 slot + ARDMA_FRAME_SPLIT_USER_SIZE + 4,
					 ARDMA_TAIL_USER_SIZE);
		if (ret)
			return ret;
		written += ARDMA_TAIL_USER_SIZE;
	} else if (tail > ARDMA_TAIL_USER_SIZE) {
		const u8 *frag = p + normal_slots * ARDMA_SLOT_WIRE_SIZE;
		u32 split = tail - ARDMA_TAIL_USER_SIZE - 4;

		if (!split || split > ARDMA_FRAME_SPLIT_USER_SIZE)
			return -EINVAL;

		ret = ardma_recv_scatter(pd, r, dst_off + written, frag, split);
		if (ret)
			return ret;
		written += split;

		ret = ardma_recv_scatter(pd, r, dst_off + written,
					 frag + split + 4,
					 ARDMA_TAIL_USER_SIZE);
		if (ret)
			return ret;
		written += ARDMA_TAIL_USER_SIZE;
	} else if (tail) {
		ret = ardma_recv_scatter(pd, r, dst_off + written,
					 p + normal_slots * ARDMA_SLOT_WIRE_SIZE,
					 tail);
		if (ret)
			return ret;
		written += tail;
	}
	if (out_user_len)
		*out_user_len = written;
	return 0;
}

static int ardma_copy_frame_to_buf(void *dst, u32 dst_size, u32 dst_off,
				   const void *payload, u32 len,
				   u32 *out_user_len)
{
	u32 num_slots = len / ARDMA_SLOT_WIRE_SIZE;
	u32 tail = len - num_slots * ARDMA_SLOT_WIRE_SIZE;
	u32 written = 0;
	u32 normal_slots = num_slots;
	const u8 *p = payload;
	u32 i;

#define ARDMA_COPY_FRAME_CHUNK(_src, _len) do {				\
		u32 __len = (_len);					\
		if (dst_off > dst_size ||				\
		    written > dst_size - dst_off ||			\
		    __len > dst_size - dst_off - written)		\
			return -ERANGE;					\
		memcpy((u8 *)dst + dst_off + written, (_src), __len);	\
		written += __len;					\
	} while (0)

	if (!tail && num_slots)
		normal_slots--;

	for (i = 0; i < normal_slots; i++)
		ARDMA_COPY_FRAME_CHUNK(p + i * ARDMA_SLOT_WIRE_SIZE,
				       ARDMA_FRAME_SLOT_USER_SIZE);

	if (!tail && num_slots) {
		const u8 *slot = p + normal_slots * ARDMA_SLOT_WIRE_SIZE;

		ARDMA_COPY_FRAME_CHUNK(slot, ARDMA_FRAME_SPLIT_USER_SIZE);
		ARDMA_COPY_FRAME_CHUNK(slot + ARDMA_FRAME_SPLIT_USER_SIZE + 4,
				       ARDMA_TAIL_USER_SIZE);
	} else if (tail > ARDMA_TAIL_USER_SIZE) {
		const u8 *frag = p + normal_slots * ARDMA_SLOT_WIRE_SIZE;
		u32 split = tail - ARDMA_TAIL_USER_SIZE - 4;

		if (!split || split > ARDMA_FRAME_SPLIT_USER_SIZE)
			return -EINVAL;

		ARDMA_COPY_FRAME_CHUNK(frag, split);
		ARDMA_COPY_FRAME_CHUNK(frag + split + 4,
				       ARDMA_TAIL_USER_SIZE);
	} else if (tail) {
		ARDMA_COPY_FRAME_CHUNK(p + normal_slots * ARDMA_SLOT_WIRE_SIZE,
				       tail);
	}

#undef ARDMA_COPY_FRAME_CHUNK

	if (out_user_len)
		*out_user_len = written;
	return 0;
}

static int ardma_copy_sges_to_buf(struct ardma_pd *pd,
				  const struct ib_sge *sg_list, int num_sge,
				  u32 src_off, void *dst, u32 len)
{
	u32 cur = 0, copied = 0;
	int i;

	for (i = 0; i < num_sge && copied < len; i++) {
		const struct ib_sge *sge = &sg_list[i];
		struct ardma_mr *mr;
		u32 in_sge_off, chunk;

		if (cur + sge->length <= src_off) {
			cur += sge->length;
			continue;
		}

		in_sge_off = (src_off + copied) - cur;
		chunk = min_t(u32, sge->length - in_sge_off, len - copied);
		mr = ardma_pd_get_mr(pd, sge->lkey);
		if (!mr)
			return -EINVAL;
		if (ardma_mr_xfer(mr, sge->addr + in_sge_off,
				  (u8 *)dst + copied, chunk, true)) {
			ardma_mr_put(mr);
			return -EFAULT;
		}
		ardma_mr_put(mr);
		copied += chunk;
	}

	return copied < len ? -ERANGE : 0;
}

static int ardma_dma_slice_from_sges(struct ardma_pd *pd,
				     const struct ib_sge *sg_list,
				     int num_sge, u32 src_off, u32 len,
				     struct ardma_mr **mr_out,
				     dma_addr_t *dma_base, u32 *dma_off)
{
	u32 cur = 0;
	int i;

	for (i = 0; i < num_sge; i++) {
		const struct ib_sge *sge = &sg_list[i];
		struct ardma_mr *mr;
		u32 in_sge_off;
		int ret;

		if (cur + sge->length <= src_off) {
			cur += sge->length;
			continue;
		}

		in_sge_off = src_off - cur;
		if (len > sge->length - in_sge_off)
			return -EOPNOTSUPP;

		mr = ardma_pd_get_mr(pd, sge->lkey);
		if (!mr)
			return -EINVAL;
		ret = ardma_mr_dma_page_slice(mr, sge->addr + in_sge_off,
					      len, dma_base, dma_off);
		if (ret) {
			ardma_mr_put(mr);
			return ret;
		}
		*mr_out = mr;
		return 0;
	}

	return -ERANGE;
}

/* ----- peer/QP refs ---------------------------------------------- */

static int ardma_peer_count_locked(void)
{
	struct ardma_peer *peer;
	int count = 0;

	list_for_each_entry(peer, &ardma_peer_list, peers_link) {
		if (!READ_ONCE(peer->closing))
			count++;
	}
	return count;
}

static int ardma_peer_count(void)
{
	int count;

	mutex_lock(&ardma_peer_lock);
	count = ardma_peer_count_locked();
	mutex_unlock(&ardma_peer_lock);
	return count;
}

static struct ardma_peer *ardma_peer_get_single(void)
{
	struct ardma_peer *peer, *found = NULL;

	mutex_lock(&ardma_peer_lock);
	list_for_each_entry(peer, &ardma_peer_list, peers_link) {
		if (READ_ONCE(peer->closing))
			continue;
		if (found) {
			found = NULL;
			goto out;
		}
		found = peer;
	}
	if (found)
		refcount_inc(&found->refs);
out:
	mutex_unlock(&ardma_peer_lock);
	return found;
}

static struct ardma_peer *ardma_peer_get_by_route(int domain, u64 route)
{
	struct ardma_peer *peer, *found = NULL;

	mutex_lock(&ardma_peer_lock);
	list_for_each_entry(peer, &ardma_peer_list, peers_link) {
		if (READ_ONCE(peer->closing))
			continue;
		if (peer->xd->tb->index == domain && peer->xd->route == route) {
			found = peer;
			refcount_inc(&found->refs);
			break;
		}
	}
	mutex_unlock(&ardma_peer_lock);
	return found;
}

static struct ardma_peer *ardma_peer_get_by_gid_map(const union ib_gid *gid)
{
	char *map, *cur, *entry;
	struct ardma_peer *peer = NULL;
	__be32 gid_addr;

	if (!peer_gid_routes || !*peer_gid_routes ||
	    !ardma_gid_ipv4_mapped(gid, &gid_addr))
		return NULL;

	map = kstrdup(peer_gid_routes, GFP_KERNEL);
	if (!map)
		return NULL;

	cur = map;
	while ((entry = strsep(&cur, ",")) != NULL) {
		char *eq, *ip, *route_spec;
		__be32 want_addr;
		int domain;
		u64 route;

		entry = strim(entry);
		if (!*entry)
			continue;
		eq = strchr(entry, '=');
		if (!eq)
			continue;
		*eq = '\0';
		ip = strim(entry);
		route_spec = strim(eq + 1);
		if (!in4_pton(ip, -1, (u8 *)&want_addr, -1, NULL))
			continue;
		if (want_addr != gid_addr)
			continue;
		if (ardma_parse_route_spec(route_spec, &domain, &route))
			continue;
		peer = ardma_peer_get_by_route(domain, route);
		break;
	}

	kfree(map);
	return peer;
}

static bool ardma_peer_netdev_override(const struct ardma_peer *peer, char *buf,
				       size_t buflen)
{
	char *map, *cur, *entry;
	bool found = false;

	if (!peer || !peer_netdevs || !*peer_netdevs)
		return false;

	map = kstrdup(peer_netdevs, GFP_KERNEL);
	if (!map)
		return false;

	cur = map;
	while ((entry = strsep(&cur, ",")) != NULL) {
		char *eq, *route_spec, *netdev;
		int domain;
		u64 route;

		entry = strim(entry);
		if (!*entry)
			continue;
		eq = strchr(entry, '=');
		if (!eq)
			continue;
		*eq = '\0';
		route_spec = strim(entry);
		netdev = strim(eq + 1);
		if (ardma_parse_route_spec(route_spec, &domain, &route))
			continue;
		if (domain != peer->xd->tb->index || route != peer->xd->route)
			continue;
		strscpy(buf, netdev, buflen);
		found = true;
		break;
	}

	kfree(map);
	return found;
}

static void ardma_peer_netdev_name(const struct ardma_peer *peer, char *buf,
				   size_t buflen)
{
	if (ardma_peer_netdev_override(peer, buf, buflen))
		return;
	if (cm_netdev && *cm_netdev && strcmp(cm_netdev, "auto")) {
		strscpy(buf, cm_netdev, buflen);
		return;
	}
	snprintf(buf, buflen, "thunderbolt%d", peer->xd->tb->index);
}

static int ardma_bind_qp_peer(struct ardma_qp *qp, struct ardma_peer *peer)
{
	if (!peer)
		return -ENOTCONN;
	if (qp->peer) {
		ardma_peer_put(peer);
		return qp->peer == peer ? 0 : -EISCONN;
	}

	qp->peer = peer;
	if (qp->qp_type == IB_QPT_UC)
		ardma_peer_qp_activate(qp, peer);
	return 0;
}

static void ardma_peer_put(struct ardma_peer *peer)
{
	if (!peer)
		return;
	if (refcount_dec_and_test(&peer->refs))
		WARN_ON_ONCE(1);
	if (refcount_read(&peer->refs) == 1)
		wake_up(&peer->ref_wait);
}

static void ardma_qp_get(struct ardma_qp *qp)
{
	refcount_inc(&qp->refs);
}

static void ardma_qp_put(struct ardma_qp *qp)
{
	if (refcount_dec_and_test(&qp->refs))
		WARN_ON_ONCE(1);
	if (refcount_read(&qp->refs) == 1)
		wake_up(&qp->ref_wait);
}

static void ardma_stop_tx_ring(struct ardma_peer *peer)
{
	if (!peer || !peer->tx_ring)
		return;

	mutex_lock(&peer->tx_lock);
	if (peer->tx_ring_running) {
		tb_ring_stop(peer->tx_ring);
		peer->tx_ring_running = false;
	}
	mutex_unlock(&peer->tx_lock);
}

static void ardma_restart_tx_ring(struct ardma_peer *peer)
{
	if (!peer || !peer->tx_ring || READ_ONCE(peer->closing))
		return;

	mutex_lock(&peer->tx_lock);
	if (peer->tx_ring && !peer->tx_ring_running &&
	    !READ_ONCE(peer->closing)) {
		tb_ring_start(peer->tx_ring);
		peer->tx_ring_running = true;
	}
	mutex_unlock(&peer->tx_lock);
}

static int ardma_enable_peer_paths(struct ardma_peer *peer)
{
	int ret = 0;

	if (!peer || !peer->tx_ring || !peer->rx_ring)
		return -ENOTCONN;
	if (READ_ONCE(peer->paths_enabled))
		return 0;
	if (READ_ONCE(peer->closing))
		return -ESHUTDOWN;

	mutex_lock(&peer->tx_lock);
	if (!peer->paths_enabled && !READ_ONCE(peer->closing)) {
		ret = tb_xdomain_enable_paths(peer->xd, peer->local_out_hop,
					      peer->tx_ring->hop,
					      peer->local_in_hop,
					      peer->rx_ring->hop);
		if (!ret) {
			peer->paths_enabled = true;
			pr_info("enabled DMA paths lazily: in_path=%d rx_hop=%d out_path=%d tx_hop=%d\n",
				peer->local_in_hop, peer->rx_ring->hop,
				peer->local_out_hop, peer->tx_ring->hop);
		} else {
			pr_warn("lazy enable_paths failed: %d\n", ret);
		}
	} else if (READ_ONCE(peer->closing)) {
		ret = -ESHUTDOWN;
	}
	mutex_unlock(&peer->tx_lock);
	return ret;
}

static void ardma_disable_peer_paths_locked(struct ardma_peer *peer)
{
	if (!peer || !peer->paths_enabled)
		return;

	tb_xdomain_disable_paths(peer->xd, peer->local_out_hop,
				 peer->tx_ring ? peer->tx_ring->hop : -1,
				 peer->local_in_hop,
				 peer->rx_ring ? peer->rx_ring->hop : -1);
	peer->paths_enabled = false;
	pr_info("disabled DMA paths at idle: in_path=%d rx_hop=%d out_path=%d tx_hop=%d\n",
		peer->local_in_hop, peer->rx_ring ? peer->rx_ring->hop : -1,
		peer->local_out_hop, peer->tx_ring ? peer->tx_ring->hop : -1);
}

static void ardma_peer_qp_activate(struct ardma_qp *qp, struct ardma_peer *peer)
{
	if (!qp || !peer || qp->peer_qp_active)
		return;

	mutex_lock(&peer->tx_lock);
	peer->active_uc_qps++;
	qp->peer_qp_active = true;
	mutex_unlock(&peer->tx_lock);
}

static void ardma_peer_qp_deactivate(struct ardma_qp *qp)
{
	struct ardma_peer *peer;

	if (!qp || !qp->peer_qp_active)
		return;
	peer = qp->peer;
	if (!peer)
		return;

	mutex_lock(&peer->tx_lock);
	if (peer->active_uc_qps)
		peer->active_uc_qps--;
	qp->peer_qp_active = false;
	if (!peer->active_uc_qps && READ_ONCE(disable_paths_on_idle) &&
	    !READ_ONCE(peer->closing))
		ardma_disable_peer_paths_locked(peer);
	mutex_unlock(&peer->tx_lock);
}

static struct ardma_qp *ardma_lookup_qp(struct ardma_peer *peer, u32 qpn)
{
	struct ardma_qp *qp;
	unsigned long flags;

	spin_lock_irqsave(&ardma_qp_lock, flags);
	list_for_each_entry(qp, &ardma_qp_list, qps_link) {
		if (qp->base.qp_num == qpn && qp->peer == peer &&
		    qp->registered) {
			ardma_qp_get(qp);
			spin_unlock_irqrestore(&ardma_qp_lock, flags);
			return qp;
		}
	}
	spin_unlock_irqrestore(&ardma_qp_lock, flags);
	return NULL;
}

static int ardma_alloc_qpn(void)
{
	int base = receive_path << ARDMA_QPN_BASE_SHIFT;
	int max_slots = (ARDMA_QPN_MAX - base) / ARDMA_QPN_STRIDE;
	int slot;

	if (base < ARDMA_QPN_MIN || base > ARDMA_QPN_MAX)
		return -ENOSPC;

	slot = ida_alloc_range(&ardma_qpn_slots, 0, max_slots, GFP_KERNEL);
	if (slot < 0)
		return slot;
	return base + slot * ARDMA_QPN_STRIDE;
}

static void ardma_free_qpn(u32 qpn)
{
	int base = receive_path << ARDMA_QPN_BASE_SHIFT;
	int slot;

	if (qpn < (u32)base || (qpn - base) % ARDMA_QPN_STRIDE)
		return;
	slot = (qpn - base) / ARDMA_QPN_STRIDE;
	ida_free(&ardma_qpn_slots, slot);
}

/* ----- CQ helpers ------------------------------------------------ */

static int ardma_cq_push_wc(struct ardma_cq *cq, const struct ib_wc *wc)
{
	struct ardma_wc_entry *e;
	unsigned long flags;

	spin_lock_irqsave(&cq->lock, flags);
	if (list_empty(&cq->free_list)) {
		spin_unlock_irqrestore(&cq->lock, flags);
		return -ENOMEM;
	}
	e = list_first_entry(&cq->free_list, struct ardma_wc_entry, list);
	list_del(&e->list);
	cq->free_count--;
	e->wc = *wc;
	list_add_tail(&e->list, &cq->wc_list);
	cq->wc_count++;
	if (cq->notify) {
		cq->notify = 0;
		spin_unlock_irqrestore(&cq->lock, flags);
		if (cq->base.comp_handler)
			cq->base.comp_handler(&cq->base, cq->base.cq_context);
	} else {
		spin_unlock_irqrestore(&cq->lock, flags);
	}
	return 0;
}

/* ----- Apple RX reassembly --------------------------------------- */

static struct ardma_recv_wr *ardma_pop_recv_locked(struct ardma_qp *qp)
{
	struct ardma_recv_wr *r;

	r = list_first_entry_or_null(&qp->recv_q, struct ardma_recv_wr, list);
	if (!r)
		return NULL;
	list_del(&r->list);
	if (qp->recv_q_depth)
		qp->recv_q_depth--;
	return r;
}

static int ardma_recv_wr_total_len(const struct ardma_recv_wr *r, u32 *total)
{
	u32 n = 0;
	int i;

	for (i = 0; i < r->num_sge; i++) {
		if (r->sge[i].length > U32_MAX - n)
			return -EMSGSIZE;
		n += r->sge[i].length;
	}
	*total = n;
	return 0;
}

static struct ardma_pending_rx *ardma_pending_active_locked(struct ardma_qp *qp)
{
	struct ardma_pending_rx *p;

	if (qp->rx_pending_active >= 0)
		return &qp->rx_pending[qp->rx_pending_active];
	if (qp->rx_pending_ready_count >= ARDMA_PENDING_RX_SLOTS)
		return NULL;

	p = &qp->rx_pending[qp->rx_pending_tail];
	p->len = 0;
	p->truncated = false;
	p->active = true;
	p->ready = false;
	qp->rx_pending_active = qp->rx_pending_tail;
	return p;
}

static void ardma_pending_finish_locked(struct ardma_qp *qp)
{
	struct ardma_pending_rx *p;

	if (qp->rx_pending_active < 0)
		return;

	p = &qp->rx_pending[qp->rx_pending_active];
	p->active = false;
	p->ready = true;
	qp->rx_pending_active = -1;
	qp->rx_pending_ready_count++;
	qp->rx_pending_tail = (qp->rx_pending_tail + 1) %
			      ARDMA_PENDING_RX_SLOTS;
}

static void ardma_push_rx_wc(struct ardma_qp *qp, struct ardma_recv_wr *r,
			     u32 byte_len, enum ib_wc_status status)
{
	struct ardma_cq *recv_cq =
		container_of(qp->base.recv_cq, struct ardma_cq, base);
	struct ib_wc wc = {};
	u32 expected_len = 0;

	wc.wr_id = r->wr_id;
	wc.wr_cqe = r->wr_cqe;
	wc.byte_len = byte_len;
	wc.status = status;
	wc.opcode = IB_WC_RECV;
	wc.qp = &qp->base;
	wc.src_qp = qp->attr.dest_qp_num;
	wc.port_num = 1;
	ardma_recv_wr_total_len(r, &expected_len);
	ardma_log_rx_event(qp->peer, ARDMA_RX_EVT_RECV_CQE,
			   qp->base.qp_num, qp->attr.dest_qp_num, r->wr_id,
			   0, byte_len, expected_len, 0, 0, status, 0,
			   qp->state, qp->registered, qp->recv_q_depth,
			   qp->rx_pending_ready_count, qp->rx_pending_active,
			   qp->rx_piece, 0);
	ardma_cq_push_wc(recv_cq, &wc);
	ardma_recv_wr_release(qp, r);
}

static struct ardma_recv_wr *
ardma_flush_pending_locked(struct ardma_qp *qp, u32 *byte_len,
			   enum ib_wc_status *status)
{
	struct ardma_pd *pd = container_of(qp->base.pd, struct ardma_pd, base);
	struct ardma_pending_rx *p;
	struct ardma_recv_wr *r;
	u32 expected_len = 0;
	int ret;

	if (!qp->rx_pending_ready_count)
		return NULL;

	r = ardma_pop_recv_locked(qp);
	if (!r)
		return NULL;

	p = &qp->rx_pending[qp->rx_pending_head];
	*byte_len = p->len;
	*status = p->truncated ? IB_WC_LOC_LEN_ERR : IB_WC_SUCCESS;
	if (!ardma_recv_wr_total_len(r, &expected_len) &&
	    p->len > expected_len)
		*status = IB_WC_LOC_LEN_ERR;
	ret = ardma_recv_scatter(pd, r, 0, p->buf, p->len);
	if (ret == -ERANGE)
		*status = IB_WC_LOC_LEN_ERR;
	else if (ret)
		*status = IB_WC_LOC_PROT_ERR;

	ardma_log_rx_event(qp->peer, ARDMA_RX_EVT_FLUSH_PENDING,
			   qp->base.qp_num, qp->attr.dest_qp_num, r->wr_id,
			   p->len, *byte_len, expected_len, 0, 3, *status, 0,
			   qp->state, qp->registered, qp->recv_q_depth,
			   qp->rx_pending_ready_count, qp->rx_pending_active,
			   qp->rx_piece, ret);

	p->ready = false;
	p->len = 0;
	p->truncated = false;
	qp->rx_pending_head = (qp->rx_pending_head + 1) %
			      ARDMA_PENDING_RX_SLOTS;
	qp->rx_pending_ready_count--;
	return r;
}

static void ardma_complete_rx_wr(struct ardma_qp *qp, enum ib_wc_status status)
{
	struct ardma_recv_wr *r;
	u32 expected_len = 0;
	u32 byte_len;
	unsigned long flags;

	spin_lock_irqsave(&qp->recv_lock, flags);
	r = qp->rx_wr;
	qp->rx_wr = NULL;
	byte_len = qp->rx_byte_len;
	qp->rx_piece = 0;
	qp->rx_byte_len = 0;
	qp->rx_truncated = false;
	spin_unlock_irqrestore(&qp->recv_lock, flags);

	if (!r)
		return;

	if (!ardma_recv_wr_total_len(r, &expected_len))
		ardma_log_rx_event(qp->peer, ARDMA_RX_EVT_MSG_DONE,
				   qp->base.qp_num, qp->attr.dest_qp_num,
				   r->wr_id, 0, byte_len, expected_len, 0, 3,
				   status, 0, qp->state, qp->registered,
				   qp->recv_q_depth, qp->rx_pending_ready_count,
				   qp->rx_pending_active, qp->rx_piece, 0);

	ardma_push_rx_wc(qp, r, byte_len, status);
}

static void ardma_rx_apple_frame(struct ardma_peer *peer, u32 qpn,
				 const void *payload, u32 len, u8 eof)
{
	struct ardma_qp *qp;
	struct ardma_pd *pd;
	struct ardma_recv_wr *r;
	struct ardma_recv_wr *pending_r = NULL;
	unsigned long flags;
	u32 dst_off;
	u32 copy_len = len;
	u32 split_len = 0;
	u32 split_off = 0;
	u8 split_eof = 0;
	u32 pending_byte_len = 0;
	enum ib_wc_status complete_status = IB_WC_SUCCESS;
	enum ib_wc_status pending_status = IB_WC_SUCCESS;
	int ret;

	qp = ardma_lookup_qp(peer, qpn);
	if (!qp) {
		atomic64_inc(&peer->rx_no_qp);
		ardma_log_rx_event(peer, ARDMA_RX_EVT_NO_QP, qpn, 0, 0, len,
				   0, 0, 0, eof, 0, 0, 0, false, 0, 0,
				   -1, 0, -ENOENT);
		return;
	}

	pd = container_of(qp->base.pd, struct ardma_pd, base);

	spin_lock_irqsave(&qp->recv_lock, flags);
	if (!qp->rx_wr) {
		bool buffer_pending = false;

		if (qp->rx_pending_active >= 0) {
			buffer_pending = true;
		} else {
			r = ardma_pop_recv_locked(qp);
			if (!r) {
				buffer_pending = true;
			}
		}

		if (buffer_pending) {
			struct ardma_pending_rx *p;
			int pending_ret = 0;

			p = ardma_pending_active_locked(qp);
			if (!p) {
				spin_unlock_irqrestore(&qp->recv_lock, flags);
				atomic64_inc(&peer->rx_drops);
				ardma_qp_put(qp);
				return;
			}
			copy_len = len;
			if (ardma_peer_rx_raw(peer) && (eof == 2 || eof == 3)) {
				if (len < 4) {
					copy_len = 0;
					p->truncated = true;
					atomic64_inc(&peer->rx_bad_shape);
				} else {
					copy_len = len - 4;
				}
			}
			if (ardma_peer_rx_raw(peer)) {
				if (p->len + copy_len > ARDMA_PENDING_RX_BYTES) {
					copy_len = ARDMA_PENDING_RX_BYTES -
						   p->len;
					p->truncated = true;
				}
				memcpy(p->buf + p->len, payload, copy_len);
				p->len += copy_len;
			} else {
				u32 user_len = 0;

				pending_ret = ardma_copy_frame_to_buf(p->buf,
					ARDMA_PENDING_RX_BYTES, p->len,
					payload, len, &user_len);
				if (pending_ret) {
					p->truncated = true;
					atomic64_inc(&peer->rx_bad_shape);
				} else {
					p->len += user_len;
				}
			}
			ardma_log_rx_event(peer, ARDMA_RX_EVT_PENDING_FRAME,
					   qpn, qp->attr.dest_qp_num, 0, len,
					   p->len, 0, 0, eof,
					   p->truncated ? IB_WC_LOC_LEN_ERR :
					   IB_WC_SUCCESS, 0, qp->state,
					   qp->registered, qp->recv_q_depth,
					   qp->rx_pending_ready_count,
					   qp->rx_pending_active,
					   qp->rx_piece, pending_ret);
			if (eof == 3) {
				ardma_log_rx_event(peer,
						   ARDMA_RX_EVT_PENDING_DONE,
						   qpn, qp->attr.dest_qp_num,
						   0, len, p->len, 0, 0, eof,
						   p->truncated ?
						   IB_WC_LOC_LEN_ERR :
						   IB_WC_SUCCESS, 0,
						   qp->state, qp->registered,
						   qp->recv_q_depth,
						   qp->rx_pending_ready_count,
						   qp->rx_pending_active,
						   qp->rx_piece, 0);
				ardma_pending_finish_locked(qp);
				atomic64_inc(&peer->rx_messages);
				pending_r = ardma_flush_pending_locked(qp,
					&pending_byte_len, &pending_status);
			}
			spin_unlock_irqrestore(&qp->recv_lock, flags);
			if (pending_r)
				ardma_push_rx_wc(qp, pending_r,
						 pending_byte_len,
						 pending_status);
			ardma_qp_put(qp);
			return;
		}
		qp->rx_wr = r;
		qp->rx_piece = 0;
		qp->rx_byte_len = 0;
		qp->rx_truncated = false;
		if (!ardma_recv_wr_total_len(r, &dst_off))
			ardma_log_rx_event(peer, ARDMA_RX_EVT_WR_START, qpn,
					   qp->attr.dest_qp_num, r->wr_id,
					   len, 0, dst_off, 0, eof, 0, 0,
					   qp->state, qp->registered,
					   qp->recv_q_depth,
					   qp->rx_pending_ready_count,
					   qp->rx_pending_active,
					   qp->rx_piece, 0);
	}

	if (eof == 1) {
		qp->rx_piece = 0;
	}

	if (ardma_peer_rx_raw(peer)) {
		dst_off = qp->rx_byte_len;
		if (eof == 2 || eof == 3) {
			if (len < 4) {
				copy_len = 0;
				atomic64_inc(&peer->rx_bad_shape);
				qp->rx_truncated = true;
			} else {
				copy_len = len - 4;
			}
		}
	} else {
		dst_off = qp->rx_byte_len;
		/* copy_len = len; for FRAME mode the helper recomputes
		 * the user portion (252 user + 4 CRC per 256-byte slot). */
	}
	r = qp->rx_wr;
	spin_unlock_irqrestore(&qp->recv_lock, flags);

	if (ardma_peer_rx_raw(peer)) {
		u32 expected_len = 0;

		if (!ardma_recv_wr_total_len(r, &expected_len) &&
		    dst_off < expected_len && dst_off + copy_len > expected_len &&
		    eof != 2 && eof != 3) {
			u32 current_len = expected_len - dst_off;

			split_off = current_len;
			split_len = copy_len - current_len;
			split_eof = eof;
			copy_len = current_len;
		}
		ret = ardma_recv_scatter(pd, r, dst_off, payload, copy_len);
	} else {
		u32 user_len = 0;

		ret = ardma_recv_scatter_frame(pd, r, dst_off, payload, len,
					       &user_len);
		copy_len = user_len;
	}

	spin_lock_irqsave(&qp->recv_lock, flags);
	if (ret == -ERANGE)
		qp->rx_truncated = true;
	else if (ret)
		qp->rx_truncated = true;
	qp->rx_byte_len = max(qp->rx_byte_len, dst_off + copy_len);
	/* The posted receive length is capacity. A shorter SEND completes
	 * successfully; only overflow is a local length error. */
	{
		u32 expected_len = 0;

		if (!ardma_recv_wr_total_len(r, &expected_len)) {
			if (ardma_peer_rx_raw(peer)) {
				s32 prefix = -1;

				if (len >= 4) {
					const u8 *p = payload;

					prefix = p[0] | (p[1] << 8) |
						 (p[2] << 16) | (p[3] << 24);
				}
				ardma_log_rx_event(peer, ARDMA_RX_EVT_RAW_COPY,
						   qpn, qp->attr.dest_qp_num,
						   r->wr_id, len, qp->rx_byte_len,
						   expected_len, 0, eof,
						   ret ? IB_WC_LOC_PROT_ERR :
							 IB_WC_SUCCESS,
						   0, qp->state, qp->registered,
						   qp->recv_q_depth,
						   qp->rx_pending_ready_count,
						   qp->rx_pending_active, dst_off,
						   prefix);
			}
			if (qp->rx_byte_len > expected_len) {
				qp->rx_truncated = true;
				atomic64_inc(&peer->rx_bad_shape);
			} else if (qp->rx_byte_len < expected_len && eof != 3) {
				spin_unlock_irqrestore(&qp->recv_lock, flags);
				ardma_qp_put(qp);
				return;
			}
		} else {
			qp->rx_truncated = true;
			atomic64_inc(&peer->rx_bad_shape);
		}
	}
	if (qp->rx_truncated)
		complete_status = ret && ret != -ERANGE ?
			IB_WC_LOC_PROT_ERR : IB_WC_LOC_LEN_ERR;
	spin_unlock_irqrestore(&qp->recv_lock, flags);

	atomic64_inc(&peer->rx_messages);
	ardma_complete_rx_wr(qp, complete_status);
	ardma_qp_put(qp);
	if (split_len)
		ardma_rx_apple_frame(peer, qpn,
				     (const u8 *)payload + split_off,
				     split_len, split_eof);
}

static u32 ardma_rx_frame_meta(const struct ring_frame *frame, u32 len)
{
	return (len & 0xfff) |
	       ((frame->eof & 0xf) << 12) |
	       ((frame->sof & 0xf) << 16) |
	       ((frame->flags & 0xfff) << ARDMA_DESC_FLAGS_SHIFT);
}

static void ardma_rx_marker_log(struct ardma_peer *peer,
				const struct ring_frame *frame,
				const void *payload, u32 len)
{
	struct ardma_rx_marker_log_entry *e;
	unsigned long flags;
	u32 pos;

	spin_lock_irqsave(&peer->rx_marker_lock, flags);
	pos = peer->rx_marker_pos;
	e = &peer->rx_marker_log[pos];
	memset(e, 0, sizeof(*e));
	e->seq = ++peer->rx_marker_seq;
	e->len = len;
	e->meta = ardma_rx_frame_meta(frame, len);
	e->flags = frame->flags;
	e->sof = frame->sof;
	e->eof = frame->eof;
	e->prefix_len = min_t(u32, len, ARDMA_RX_MARKER_PREFIX);
	memcpy(e->prefix, payload, e->prefix_len);
	peer->rx_marker_pos = (pos + 1) % ARDMA_RX_MARKER_LOG_ENTRIES;
	spin_unlock_irqrestore(&peer->rx_marker_lock, flags);
}

static void ardma_rx_callback(struct tb_ring *ring, struct ring_frame *frame,
			      bool canceled)
{
	struct ardma_rx_frame *rf = container_of(frame, typeof(*rf), frame);
	struct ardma_peer *peer = rf->peer;
	struct device *dma_dev = tb_ring_dma_device(ring);
	u32 qpn = rf->qpn ? rf->qpn :
		((u32)receive_path << ARDMA_QPN_BASE_SHIFT);
	u32 len;
	int ret;

	if (canceled)
		return;

	dma_sync_single_for_cpu(dma_dev, rf->dma, ARDMA_FRAME_SIZE,
				DMA_FROM_DEVICE);
	len = frame->size ?: (ardma_peer_rx_raw(peer) ? TB_FRAME_SIZE :
			      ARDMA_FRAME_SIZE);
	atomic64_inc(&peer->rx_frame_count);
	if (frame->eof < 4)
		atomic64_inc(&peer->rx_eof[frame->eof]);
	else
		atomic64_inc(&peer->rx_eof_other);
	ardma_rx_marker_log(peer, frame, rf->data, len);

	/* The incoming path is the destination HopID. Apple's visible QPN is
	 * HopID << 8 for the first QP shape we have observed. */
	ardma_log_rx_event(peer, ARDMA_RX_EVT_FRAME,
			   qpn, 0, 0, len, 0, 0,
			   frame->sof, frame->eof, 0, 0, 0, false, 0, 0,
			   -1, 0, 0);
	ardma_rx_apple_frame(peer, qpn, rf->data, len, frame->eof);

	dma_sync_single_for_device(dma_dev, rf->dma, ARDMA_FRAME_SIZE,
				   DMA_FROM_DEVICE);
	ret = tb_ring_rx(ring, frame);
	if (ret)
		pr_warn_ratelimited("RX repost failed: %d\n", ret);
}

/* ----- Apple-shaped TX ------------------------------------------- */

static void ardma_tx_ctx_put(struct ardma_send_ctx *ctx, bool failed)
{
	struct ardma_cq *send_cq =
		container_of(ctx->qp->base.send_cq, struct ardma_cq, base);

	if (failed)
		atomic_set(&ctx->failed, 1);

	if (atomic_dec_return(&ctx->pending) == 0) {
		if (ctx->signaled) {
			struct ib_wc wc = {};

			wc.wr_id = ctx->wr_id;
			wc.wr_cqe = ctx->wr_cqe;
			wc.status = atomic_read(&ctx->failed) ?
				IB_WC_GENERAL_ERR : IB_WC_SUCCESS;
			wc.opcode = IB_WC_SEND;
			wc.qp = &ctx->qp->base;
			ardma_log_tx_event(ctx->qp->peer,
					   ARDMA_TX_EVT_SEND_CQE, ctx->qpn,
					   ctx->dest_qpn, ctx->wr_id,
					   ctx->total_len, 0, ctx->blocks, 0,
					   0, 0, 0, 0, 0, 0, wc.status,
					   ctx->signaled,
					   atomic_read(&ctx->failed));
			ardma_cq_push_wc(send_cq, &wc);
		}
		ardma_qp_put(ctx->qp);
		kfree(ctx);
	}
}

static unsigned int ardma_tx_pool_count(void)
{
	unsigned int count = READ_ONCE(tx_pool_frames);

	return min_t(unsigned int, count, ARDMA_MAX_TX_POOL_FRAMES);
}

static int ardma_tx_pool_init(struct ardma_qp *qp, struct ardma_peer *peer)
{
	struct device *dma_dev;
	unsigned int count;
	unsigned int i;

	count = ardma_tx_pool_count();
	if (!count || !peer || !peer->tx_ring)
		return 0;

	dma_dev = tb_ring_dma_device(peer->tx_ring);
	qp->tx_dma_dev = dma_dev;
	qp->tx_pool = kvcalloc(count, sizeof(*qp->tx_pool), GFP_KERNEL);
	if (!qp->tx_pool)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct ardma_tx_frame *tf = &qp->tx_pool[i];

		INIT_LIST_HEAD(&tf->pool_link);
		INIT_LIST_HEAD(&tf->frame.list);
		tf->pooled = true;
		tf->data = kmalloc(ARDMA_FRAME_SIZE, GFP_KERNEL);
		if (!tf->data)
			goto err;
		tf->dma = dma_map_single(dma_dev, tf->data, ARDMA_FRAME_SIZE,
					 DMA_TO_DEVICE);
		if (dma_mapping_error(dma_dev, tf->dma)) {
			kfree(tf->data);
			tf->data = NULL;
			goto err;
		}
		dma_sync_single_for_cpu(dma_dev, tf->dma, ARDMA_FRAME_SIZE,
					DMA_TO_DEVICE);
		list_add_tail(&tf->pool_link, &qp->tx_pool_free);
		qp->tx_pool_free_count++;
	}
	qp->tx_pool_count = count;
	return 0;

err:
	while (i > 0) {
		struct ardma_tx_frame *tf = &qp->tx_pool[--i];

		if (tf->data) {
			dma_unmap_single(dma_dev, tf->dma, ARDMA_FRAME_SIZE,
					 DMA_TO_DEVICE);
			kfree(tf->data);
		}
	}
	kvfree(qp->tx_pool);
	qp->tx_pool = NULL;
	qp->tx_pool_free_count = 0;
	return -ENOMEM;
}

static void ardma_tx_pool_destroy(struct ardma_qp *qp)
{
	struct device *dma_dev;
	u32 i;

	if (!qp->tx_pool)
		return;

	dma_dev = qp->tx_dma_dev;

	if (qp->tx_pool_free_count != qp->tx_pool_count)
		pr_warn("destroy_qp[0x%x]: freeing TX pool with %u/%u frames free\n",
			qp->base.qp_num, qp->tx_pool_free_count,
			qp->tx_pool_count);

	for (i = 0; i < qp->tx_pool_count; i++) {
		struct ardma_tx_frame *tf = &qp->tx_pool[i];

		if (!tf->data)
			continue;
		if (dma_dev)
			dma_unmap_single(dma_dev, tf->dma, ARDMA_FRAME_SIZE,
					 DMA_TO_DEVICE);
		kfree(tf->data);
	}
	kvfree(qp->tx_pool);
	qp->tx_pool = NULL;
	qp->tx_dma_dev = NULL;
	qp->tx_pool_count = 0;
	qp->tx_pool_free_count = 0;
	INIT_LIST_HEAD(&qp->tx_pool_free);
}

static struct ardma_tx_frame *ardma_tx_pool_get(struct ardma_qp *qp)
{
	struct ardma_tx_frame *tf;
	unsigned long flags;

	if (!qp->tx_pool)
		return NULL;

	spin_lock_irqsave(&qp->tx_pool_lock, flags);
	tf = list_first_entry_or_null(&qp->tx_pool_free,
				      struct ardma_tx_frame, pool_link);
	if (tf) {
		list_del_init(&tf->pool_link);
		qp->tx_pool_free_count--;
	}
	spin_unlock_irqrestore(&qp->tx_pool_lock, flags);
	return tf;
}

static void ardma_tx_pool_put(struct ardma_qp *qp, struct ardma_tx_frame *tf)
{
	unsigned long flags;

	tf->peer = NULL;
	tf->ctx = NULL;
	INIT_LIST_HEAD(&tf->frame.list);

	spin_lock_irqsave(&qp->tx_pool_lock, flags);
	list_add_tail(&tf->pool_link, &qp->tx_pool_free);
	qp->tx_pool_free_count++;
	spin_unlock_irqrestore(&qp->tx_pool_lock, flags);
}

static struct ardma_tx_frame *ardma_tx_frame_alloc(struct ardma_qp *qp,
						   struct ardma_peer *peer)
{
	struct device *dma_dev = tb_ring_dma_device(peer->tx_ring);
	struct ardma_tx_frame *tf;

	tf = ardma_tx_pool_get(qp);
	if (tf) {
		dma_sync_single_for_cpu(dma_dev, tf->dma, ARDMA_FRAME_SIZE,
					DMA_TO_DEVICE);
		atomic64_inc(&peer->tx_pool_frames);
		return tf;
	}

	tf = kzalloc(sizeof(*tf), GFP_KERNEL);
	if (!tf)
		return NULL;
	atomic64_inc(&peer->tx_dynamic_frames);
	INIT_LIST_HEAD(&tf->pool_link);
	INIT_LIST_HEAD(&tf->frame.list);
	tf->data = kmalloc(ARDMA_FRAME_SIZE, GFP_KERNEL);
	if (!tf->data) {
		kfree(tf);
		return NULL;
	}
	return tf;
}

static void ardma_tx_frame_free_unsubmitted(struct ardma_qp *qp,
					    struct ardma_tx_frame *tf)
{
	if (tf->zcopy) {
		ardma_mr_put(tf->mr);
		kfree(tf);
	} else if (tf->pooled)
		ardma_tx_pool_put(qp, tf);
	else {
		kfree(tf->data);
		kfree(tf);
	}
}

static void ardma_tx_callback(struct tb_ring *ring, struct ring_frame *frame,
			      bool canceled);

static int ardma_submit_tx_zcopy(struct ardma_peer *peer,
				 struct ardma_send_ctx *ctx,
				 struct ardma_pd *pd,
				 const struct ib_sge *sg_list, int num_sge,
				 u32 block, u32 piece,
				 u32 app_off, u32 payload_len,
				 u32 wire_len, u8 sof, u8 eof)
{
	struct device *dma_dev = tb_ring_dma_device(peer->tx_ring);
	struct ardma_mr *mr = NULL;
	struct ardma_tx_frame *tf;
	dma_addr_t dma_base;
	u32 dma_off;
	int ret;

	if (!READ_ONCE(tx_zcopy) || payload_len != wire_len)
		return -EOPNOTSUPP;

	ret = ardma_dma_slice_from_sges(pd, sg_list, num_sge, app_off,
					payload_len, &mr, &dma_base, &dma_off);
	if (ret)
		return ret;

	tf = kzalloc(sizeof(*tf), GFP_KERNEL);
	if (!tf) {
		ardma_mr_put(mr);
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&tf->pool_link);
	INIT_LIST_HEAD(&tf->frame.list);
	tf->zcopy = true;
	tf->mr = mr;
	tf->peer = peer;
	tf->ctx = ctx;
	tf->dma_base = dma_base;
	tf->dma = dma_base + dma_off;
	tf->dma_off = dma_off;
	tf->dma_len = payload_len;
	tf->frame_id = atomic64_inc_return(&peer->tx_frame_ids);
	tf->block = block;
	tf->piece = piece;
	tf->app_off = app_off;
	tf->payload_len = payload_len;
	tf->wire_len = wire_len;
	tf->sof = sof;
	tf->eof = eof;

	refcount_inc(&peer->refs);
	dma_sync_single_range_for_device(dma_dev, dma_base, dma_off,
					 payload_len, DMA_TO_DEVICE);

	tf->frame.buffer_phy = tf->dma;
	tf->frame.callback = ardma_tx_callback;
	tf->frame.size = wire_len == ARDMA_FRAME_SIZE ? 0 : wire_len;
	tf->frame.sof = sof;
	tf->frame.eof = eof;

	atomic_inc(&ctx->pending);
	ret = tb_ring_tx(peer->tx_ring, &tf->frame);
	if (ret) {
		ardma_log_tx_event(peer, ARDMA_TX_EVT_DESC_FAIL, ctx->qpn,
				   ctx->dest_qpn, ctx->wr_id, ctx->total_len,
				   tf->frame_id, block, piece, app_off,
				   payload_len, wire_len, sof, eof,
				   atomic_read(&ctx->pending), ret,
				   ctx->signaled, false);
		atomic_dec(&ctx->pending);
		dma_sync_single_range_for_cpu(dma_dev, dma_base, dma_off,
					      payload_len, DMA_TO_DEVICE);
		ardma_mr_put(mr);
		kfree(tf);
		ardma_peer_put(peer);
		return ret;
	}

	atomic64_inc(&peer->tx_frames);
	atomic64_inc(&peer->tx_zcopy_frames);
	ardma_log_tx_event(peer, ARDMA_TX_EVT_DESC_SUBMIT, ctx->qpn,
			   ctx->dest_qpn, ctx->wr_id, ctx->total_len,
			   tf->frame_id, block, piece, app_off, payload_len,
			   wire_len, sof, eof, atomic_read(&ctx->pending), 0,
			   ctx->signaled, false);
	return 0;
}

static void ardma_tx_callback(struct tb_ring *ring, struct ring_frame *frame,
			      bool canceled)
{
	struct ardma_tx_frame *tf = container_of(frame, typeof(*tf), frame);
	struct device *dma_dev = tb_ring_dma_device(ring);
	struct ardma_peer *peer = tf->peer;
	struct ardma_send_ctx *ctx = tf->ctx;
	struct ardma_qp *qp = ctx->qp;

	if (tf->zcopy)
		dma_sync_single_range_for_cpu(dma_dev, tf->dma_base,
					      tf->dma_off, tf->dma_len,
					      DMA_TO_DEVICE);
	else if (tf->pooled)
		dma_sync_single_for_cpu(dma_dev, tf->dma, ARDMA_FRAME_SIZE,
					DMA_TO_DEVICE);
	else
		dma_unmap_single(dma_dev, tf->dma, ARDMA_FRAME_SIZE,
				 DMA_TO_DEVICE);
	if (canceled)
		atomic64_inc(&peer->tx_errors);
	else
		atomic64_inc(&peer->tx_completions);
	ardma_log_tx_event(peer,
			   canceled ? ARDMA_TX_EVT_DESC_CANCEL :
				      ARDMA_TX_EVT_DESC_COMPLETE,
			   ctx->qpn, ctx->dest_qpn, ctx->wr_id,
			   ctx->total_len, tf->frame_id, tf->block,
			   tf->piece, tf->app_off, tf->payload_len,
			   tf->wire_len, tf->sof, tf->eof,
			   atomic_read(&ctx->pending), canceled ? -ECANCELED : 0,
			   ctx->signaled, canceled);
	if (tf->zcopy) {
		ardma_mr_put(tf->mr);
		kfree(tf);
	} else if (tf->pooled)
		ardma_tx_pool_put(qp, tf);
	else {
		kfree(tf->data);
		kfree(tf);
	}
	ardma_tx_ctx_put(ctx, canceled);
	/* Drop the per-tx-frame peer ref taken in ardma_submit_tx_piece.
	 * Use the local peer pointer because pooled frames may already be
	 * back on the QP free list. */
	ardma_peer_put(peer);
}

enum ardma_tx_piece_kind {
	ARDMA_TX_PIECE_FIRST,
	ARDMA_TX_PIECE_MID,
	ARDMA_TX_PIECE_SPLIT,
	ARDMA_TX_PIECE_TAIL,
	ARDMA_TX_PIECE_ONLY,
};

static void ardma_tx_markers(unsigned int mode, enum ardma_tx_piece_kind kind,
			     u8 tail_eof, u8 *sof, u8 *eof)
{
	bool first = kind == ARDMA_TX_PIECE_FIRST ||
		     kind == ARDMA_TX_PIECE_ONLY;
	bool tail = kind == ARDMA_TX_PIECE_TAIL ||
		    kind == ARDMA_TX_PIECE_ONLY;

	if (mode > ARDMA_TX_MARKER_EVERY_SOF_APPLE_EOF)
		mode = ARDMA_TX_MARKER_APPLE_EOF;

	*sof = 0;
	*eof = 0;

	switch (mode) {
	case ARDMA_TX_MARKER_GEMINI_SOF:
		if (first)
			*sof = 1;
		break;
	case ARDMA_TX_MARKER_BOTH_START:
		if (first) {
			*sof = 1;
			*eof = 1;
		}
		break;
	case ARDMA_TX_MARKER_EVERY_DESC:
		*sof = 1;
		*eof = tail ? tail_eof : 1;
		break;
	case ARDMA_TX_MARKER_EVERY_SOF_APPLE_EOF:
		*sof = 1;
		if (first)
			*eof = 1;
		break;
	case ARDMA_TX_MARKER_APPLE_EOF:
	default:
		if (first)
			*eof = 1;
		break;
	}

	if (tail && mode != ARDMA_TX_MARKER_EVERY_DESC)
		*eof = tail_eof;
}

static int ardma_submit_tx_piece(struct ardma_peer *peer,
				 struct ardma_send_ctx *ctx,
				 struct ardma_pd *pd,
				 const struct ib_sge *sg_list, int num_sge,
				 u32 block, u32 piece,
				 u32 app_off, u32 payload_len,
				 u32 wire_len, u8 sof, u8 eof,
				 bool append_crc, u32 crc)
{
	struct device *dma_dev = tb_ring_dma_device(peer->tx_ring);
	struct ardma_qp *qp = ctx->qp;
	struct ardma_tx_frame *tf;
	__le32 crc_le;
	u32 valid_len;
	int ret;

	if (payload_len > ARDMA_FRAME_SIZE ||
	    wire_len > ARDMA_FRAME_SIZE ||
	    (append_crc && payload_len + sizeof(crc_le) > ARDMA_FRAME_SIZE))
		return -EINVAL;

	if (!append_crc && payload_len == wire_len) {
		ret = ardma_submit_tx_zcopy(peer, ctx, pd, sg_list, num_sge,
					    block, piece, app_off, payload_len,
					    wire_len, sof, eof);
		if (!ret)
			return 0;
		if (ret != -EOPNOTSUPP)
			return ret;
	}

	tf = ardma_tx_frame_alloc(qp, peer);
	if (!tf)
		return -ENOMEM;

	ret = ardma_copy_sges_to_buf(pd, sg_list, num_sge, app_off,
				     tf->data, payload_len);
	if (ret) {
		ardma_tx_frame_free_unsubmitted(qp, tf);
		return ret;
	}
	if (append_crc) {
		crc_le = cpu_to_le32(crc);
		memcpy((u8 *)tf->data + payload_len, &crc_le,
		       sizeof(crc_le));
	}
	valid_len = payload_len + (append_crc ? sizeof(crc_le) : 0);
	if (wire_len > valid_len)
		memset((u8 *)tf->data + valid_len, 0, wire_len - valid_len);

	tf->peer = peer;
	tf->ctx = ctx;
	tf->frame_id = atomic64_inc_return(&peer->tx_frame_ids);
	tf->block = block;
	tf->piece = piece;
	tf->app_off = app_off;
	tf->payload_len = payload_len;
	tf->wire_len = wire_len;
	tf->sof = sof;
	tf->eof = eof;
	/* Per-tx-frame peer ref so ardma_tx_callback (IRQ context) can
	 * safely deref tf->peer for stat increments without racing
	 * against ardma_remove freeing the peer. Dropped in the
	 * callback after the last use. */
	refcount_inc(&peer->refs);
	if (tf->pooled) {
		dma_sync_single_for_device(dma_dev, tf->dma, ARDMA_FRAME_SIZE,
					   DMA_TO_DEVICE);
	} else {
		tf->dma = dma_map_single(dma_dev, tf->data, ARDMA_FRAME_SIZE,
					 DMA_TO_DEVICE);
		if (dma_mapping_error(dma_dev, tf->dma)) {
			ardma_peer_put(peer);
			ardma_tx_frame_free_unsubmitted(qp, tf);
			return -EIO;
		}
	}

	tf->frame.buffer_phy = tf->dma;
	tf->frame.callback = ardma_tx_callback;
	/* FRAME-mode rings encode a full 4 KiB frame as descriptor length 0,
	 * matching thunderbolt-net. Keep wire_len unchanged in logs.
	 */
	tf->frame.size = wire_len == ARDMA_FRAME_SIZE ? 0 : wire_len;
	tf->frame.sof = sof;
	tf->frame.eof = eof;
	INIT_LIST_HEAD(&tf->frame.list);

	atomic_inc(&ctx->pending);
	ret = tb_ring_tx(peer->tx_ring, &tf->frame);
	if (ret) {
		ardma_log_tx_event(peer, ARDMA_TX_EVT_DESC_FAIL, ctx->qpn,
				   ctx->dest_qpn, ctx->wr_id, ctx->total_len,
				   tf->frame_id, block, piece, app_off,
				   payload_len, wire_len, sof, eof,
				   atomic_read(&ctx->pending), ret,
				   ctx->signaled, false);
		atomic_dec(&ctx->pending);
		if (tf->pooled)
			dma_sync_single_for_cpu(dma_dev, tf->dma,
						ARDMA_FRAME_SIZE,
						DMA_TO_DEVICE);
		else
			dma_unmap_single(dma_dev, tf->dma, ARDMA_FRAME_SIZE,
					 DMA_TO_DEVICE);
		ardma_tx_frame_free_unsubmitted(qp, tf);
		ardma_peer_put(peer);
		return ret;
	}
	atomic64_inc(&peer->tx_frames);
	ardma_log_tx_event(peer, ARDMA_TX_EVT_DESC_SUBMIT, ctx->qpn,
			   ctx->dest_qpn, ctx->wr_id, ctx->total_len,
			   tf->frame_id, block, piece, app_off, payload_len,
			   wire_len, sof, eof, atomic_read(&ctx->pending), 0,
			   ctx->signaled, false);
	return 0;
}

static int ardma_crc32c_sges(struct ardma_pd *pd,
			     const struct ib_sge *sg_list, int num_sge,
			     u32 src_off, u32 len, u32 *crc_out)
{
	void *buf;
	int ret;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = ardma_copy_sges_to_buf(pd, sg_list, num_sge, src_off, buf, len);
	if (!ret)
		*crc_out = crc32c(~0u, buf, len) ^ ~0u;
	kfree(buf);
	return ret;
}

static int ardma_wr_total_len(const struct ib_send_wr *wr, u32 *total)
{
	u32 n = 0;
	int i;

	for (i = 0; i < wr->num_sge; i++) {
		if (wr->sg_list[i].length > U32_MAX - n)
			return -EMSGSIZE;
		n += wr->sg_list[i].length;
	}
	*total = n;
	return 0;
}

static int ardma_tx_chunk_user_len(bool raw_mode, u32 path_mtu,
				   u32 *chunk_user_len, u32 *slots)
{
	u32 s;

	if (!path_mtu || path_mtu > ARDMA_FRAME_SIZE ||
	    path_mtu % ARDMA_SLOT_WIRE_SIZE)
		return -EINVAL;

	s = path_mtu / ARDMA_SLOT_WIRE_SIZE;
	if (!s)
		return -EINVAL;

	*slots = s;
	*chunk_user_len = s * (raw_mode ? ARDMA_RAW_SLOT_USER_SIZE :
			       ARDMA_FRAME_SLOT_USER_SIZE);
	return 0;
}

static u8 ardma_tx_terminal_eof(void)
{
	unsigned int eof = READ_ONCE(tx_terminal_eof);

	return eof <= 0xf ? eof : 3;
}

static int ardma_send_apple_chunk(struct ardma_peer *peer,
				  struct ardma_send_ctx *ctx,
				  struct ardma_pd *pd,
				  const struct ib_sge *sg_list, int num_sge,
				  u32 chunk, u32 base,
				  u32 chunk_len, unsigned int marker_mode,
				  bool raw_mode, bool group_first,
				  u8 tail_eof, bool append_crc, u32 crc)
{
	u32 slot_user = raw_mode ? ARDMA_RAW_SLOT_USER_SIZE :
		ARDMA_FRAME_SLOT_USER_SIZE;
	u32 split_user = raw_mode ? ARDMA_RAW_SPLIT_USER_SIZE :
		ARDMA_FRAME_SPLIT_USER_SIZE;
	u32 slots = chunk_len / slot_user;
	u32 full_pieces;
	u32 piece;
	u8 sof, eof;
	int ret;

	if (!slots || chunk_len % slot_user)
		return -EINVAL;

	full_pieces = slots - 1;
	for (piece = 0; piece < full_pieces; piece++) {
		ardma_tx_markers(marker_mode,
				 group_first && !piece ? ARDMA_TX_PIECE_FIRST :
							 ARDMA_TX_PIECE_MID,
				 0, &sof, &eof);
		ret = ardma_submit_tx_piece(peer, ctx, pd, sg_list, num_sge,
					    chunk, piece,
					    base + piece * slot_user,
					    slot_user, slot_user, sof, eof,
					    false, 0);
		if (ret)
			return ret;
	}

	ardma_tx_markers(marker_mode,
			 group_first && !full_pieces ? ARDMA_TX_PIECE_FIRST :
			 full_pieces ? ARDMA_TX_PIECE_SPLIT :
				       ARDMA_TX_PIECE_FIRST,
			 0, &sof, &eof);
	ret = ardma_submit_tx_piece(peer, ctx, pd, sg_list, num_sge, chunk,
				    full_pieces, base + full_pieces * slot_user,
				    split_user, split_user, sof, eof, false, 0);
	if (ret)
		return ret;

	ardma_tx_markers(marker_mode, ARDMA_TX_PIECE_TAIL, tail_eof,
			 &sof, &eof);
	ret = ardma_submit_tx_piece(peer, ctx, pd, sg_list, num_sge, chunk,
				    full_pieces + 1,
				    base + full_pieces * slot_user + split_user,
				    ARDMA_TAIL_USER_SIZE,
				    append_crc ? ARDMA_TAIL_USER_SIZE + 4 :
					       ARDMA_TAIL_USER_SIZE,
				    sof, eof, append_crc, crc);
	return ret;
}

static int ardma_send_apple_frame_chunk(struct ardma_peer *peer,
					struct ardma_send_ctx *ctx,
					struct ardma_pd *pd,
					const struct ib_sge *sg_list,
					int num_sge, u32 chunk, u32 base,
					u32 user_len,
					unsigned int marker_mode,
					bool group_first, u8 tail_eof)
{
	u32 max_user = ARDMA_FRAME_SLOT_USER_SIZE *
		       (ARDMA_FRAME_SIZE / ARDMA_SLOT_WIRE_SIZE);
	u32 remaining = user_len;
	u32 pos = 0;
	u32 piece = 0;
	bool first_piece = true;
	u8 sof, eof;
	int ret;

	if (!user_len || user_len > max_user)
		return -EINVAL;

	while (remaining > ARDMA_FRAME_SLOT_USER_SIZE) {
		ardma_tx_markers(marker_mode,
				 group_first && first_piece ?
				 ARDMA_TX_PIECE_FIRST : ARDMA_TX_PIECE_MID,
				 0, &sof, &eof);
		/* FRAME-mode silicon overwrites the last 4 bytes of a
		 * non-EOF wire payload with its per-TLP CRC, so set
		 * wire_len = payload_len + 4 to keep our user data intact.
		 */
		ret = ardma_submit_tx_piece(peer, ctx, pd, sg_list, num_sge,
					    chunk, piece, base + pos,
					    ARDMA_FRAME_SLOT_USER_SIZE,
					    ARDMA_FRAME_SLOT_USER_SIZE + 4,
					    sof, eof, false, 0);
		if (ret)
			return ret;
		pos += ARDMA_FRAME_SLOT_USER_SIZE;
		remaining -= ARDMA_FRAME_SLOT_USER_SIZE;
		piece++;
		first_piece = false;
	}

	if (remaining > ARDMA_TAIL_USER_SIZE) {
		u32 split_len = remaining - ARDMA_TAIL_USER_SIZE;

		ardma_tx_markers(marker_mode,
				 group_first && first_piece ?
				 ARDMA_TX_PIECE_FIRST : ARDMA_TX_PIECE_SPLIT,
				 0, &sof, &eof);
		/* Same per-TLP CRC overwrite for the split (eof=0) piece. */
		ret = ardma_submit_tx_piece(peer, ctx, pd, sg_list, num_sge,
					    chunk, piece, base + pos,
					    split_len, split_len + 4, sof, eof,
					    false, 0);
		if (ret)
			return ret;
		pos += split_len;
		remaining -= split_len;
		piece++;
		first_piece = false;
	}

	ardma_tx_markers(marker_mode,
			 group_first && first_piece ? ARDMA_TX_PIECE_ONLY :
						      ARDMA_TX_PIECE_TAIL,
			 tail_eof, &sof, &eof);
	return ardma_submit_tx_piece(peer, ctx, pd, sg_list, num_sge, chunk,
				     piece, base + pos, remaining, remaining,
				     sof, eof, false, 0);
}

static int ardma_send_apple_frame_single_desc(struct ardma_peer *peer,
					      struct ardma_send_ctx *ctx,
					      struct ardma_pd *pd,
					      const struct ib_sge *sg_list,
					      int num_sge, u32 chunk, u32 base,
					      u32 user_len,
					      unsigned int marker_mode,
					      u8 tail_eof)
{
	u8 sof, eof;

	if (!user_len || user_len > ARDMA_FRAME_SIZE)
		return -EINVAL;

	ardma_tx_markers(marker_mode, ARDMA_TX_PIECE_ONLY, tail_eof,
			 &sof, &eof);
	return ardma_submit_tx_piece(peer, ctx, pd, sg_list, num_sge,
				     chunk, 0, base, user_len, user_len,
				     sof, eof, false, 0);
}

static int ardma_send_apple(struct ardma_qp *qp, const struct ib_send_wr *wr)
{
	struct ardma_pd *pd = container_of(qp->base.pd, struct ardma_pd, base);
	struct ardma_peer *peer = qp->peer;
	struct ardma_send_ctx *ctx;
	u32 total, chunk = 0, chunks;
	u32 path_mtu;
	u32 chunk_len;
	u32 slots;
	unsigned int marker_mode;
	bool raw_mode;
	bool use_single_desc;
	int ret;

	if (!READ_ONCE(tx_enabled)) {
		pr_warn_ratelimited("send_apple[qp=0x%x]: tx_enabled=0 -> EOPNOTSUPP\n",
			qp->base.qp_num);
		return -EOPNOTSUPP;
	}
	if (!peer || !peer->tx_ring) {
		pr_warn_ratelimited("send_apple[qp=0x%x]: peer=%p tx_ring=%p paths=%d -> ENOTCONN\n",
			qp->base.qp_num, peer, peer ? peer->tx_ring : NULL,
			peer ? peer->paths_enabled : -1);
		return -ENOTCONN;
	}
	if (!READ_ONCE(peer->paths_enabled)) {
		ret = ardma_enable_peer_paths(peer);
		if (ret) {
			pr_warn_ratelimited("send_apple[qp=0x%x]: lazy paths enable failed=%d -> ENOTCONN\n",
				qp->base.qp_num, ret);
			return ret;
		}
	}
	if (!READ_ONCE(peer->tx_ring_running)) {
		pr_warn_ratelimited("send_apple[qp=0x%x]: tx_ring_running=0 -> ESHUTDOWN\n",
			qp->base.qp_num);
		return -ESHUTDOWN;
	}
	if (wr->num_sge > ARDMA_MAX_SGE || (wr->num_sge && !wr->sg_list)) {
		pr_warn_ratelimited("send_apple[qp=0x%x]: num_sge=%d sg_list=%p -> EINVAL\n",
			qp->base.qp_num, wr->num_sge, wr->sg_list);
		return -EINVAL;
	}
	if (wr->send_flags & IB_SEND_INLINE) {
		pr_warn_ratelimited("send_apple[qp=0x%x]: IB_SEND_INLINE not supported\n",
			qp->base.qp_num);
		return -EOPNOTSUPP;
	}

	ret = ardma_wr_total_len(wr, &total);
	if (ret) {
		pr_warn_ratelimited("send_apple[qp=0x%x]: total_len failed=%d\n",
			qp->base.qp_num, ret);
		return ret;
	}
	if (!total || total > ARDMA_MAX_MSG_SIZE) {
		pr_warn_ratelimited("send_apple[qp=0x%x]: total=%u (must be nonzero, <= %u) -> EMSGSIZE\n",
			qp->base.qp_num, total, ARDMA_MAX_MSG_SIZE);
		return -EMSGSIZE;
	}

	path_mtu = ardma_qp_path_mtu_bytes(qp);
	if ((int)path_mtu <= 0 ||
	    path_mtu > ardma_active_mtu_bytes()) {
		pr_warn_ratelimited("send_apple[qp=0x%x]: invalid path_mtu=%u active_mtu=%u -> EMSGSIZE\n",
			qp->base.qp_num, path_mtu, ardma_active_mtu_bytes());
		return -EMSGSIZE;
	}

	raw_mode = READ_ONCE(tx_raw_mode);
	if (!raw_mode && !READ_ONCE(tx_honor_path_mtu))
		path_mtu = ardma_active_mtu_bytes();
	use_single_desc = !raw_mode && READ_ONCE(tx_frame_single_desc) &&
		peer->remote_is_apple;

	if (use_single_desc) {
		if (!path_mtu || path_mtu > ARDMA_FRAME_SIZE) {
			pr_warn_ratelimited("send_apple[qp=0x%x]: unsupported single-desc path_mtu=%u -> EMSGSIZE\n",
				qp->base.qp_num, path_mtu);
			return -EMSGSIZE;
		}
		chunk_len = path_mtu;
		slots = 1;
	} else {
		ret = ardma_tx_chunk_user_len(raw_mode, path_mtu, &chunk_len,
					      &slots);
		if (ret) {
			pr_warn_ratelimited("send_apple[qp=0x%x]: unsupported path_mtu=%u raw=%u -> EMSGSIZE\n",
				qp->base.qp_num, path_mtu, raw_mode);
			return -EMSGSIZE;
		}
	}
	if (raw_mode && total % chunk_len) {
		pr_warn_ratelimited("send_apple[qp=0x%x]: raw total=%u is not a multiple of tx_chunk=%u (path_mtu=%u slots=%u) -> EMSGSIZE\n",
			qp->base.qp_num, total, chunk_len, path_mtu, slots);
		return -EMSGSIZE;
	}

	chunks = raw_mode ? total / chunk_len : DIV_ROUND_UP(total, chunk_len);
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->qp = qp;
	ctx->wr_id = wr->wr_id;
	ctx->wr_cqe = wr->wr_cqe;
	ctx->qpn = qp->base.qp_num;
	ctx->dest_qpn = qp->attr.dest_qp_num;
	ctx->total_len = total;
	ctx->blocks = chunks;
	ctx->signaled = qp->sq_sig_all || (wr->send_flags & IB_SEND_SIGNALED);
	atomic_set(&ctx->pending, 1);
	atomic_set(&ctx->failed, 0);
	ardma_qp_get(qp);
	marker_mode = READ_ONCE(tx_marker_mode);
	ardma_log_tx_event(peer, ARDMA_TX_EVT_WR_BEGIN, ctx->qpn,
			   ctx->dest_qpn, ctx->wr_id, total, 0, chunks, 0,
			   0, chunk_len, path_mtu, 0, 0,
			   atomic_read(&ctx->pending), 0,
			   ctx->signaled, false);

	if (!raw_mode) {
		u8 terminal_eof = ardma_tx_terminal_eof();

		for (chunk = 0; chunk < chunks; chunk++) {
			u32 base = chunk * chunk_len;
			u32 this_len = min_t(u32, chunk_len, total - base);
			u8 tail_eof = chunk == chunks - 1 ? terminal_eof : 2;
			unsigned int pace_us;

			pace_us = READ_ONCE(tx_pace_us);
			if (chunk && pace_us)
				usleep_range(pace_us, pace_us + 10);

			if (use_single_desc) {
				ret = ardma_send_apple_frame_single_desc(peer,
					ctx, pd, wr->sg_list, wr->num_sge,
					chunk, base, this_len, marker_mode,
					tail_eof);
			} else {
				ret = ardma_send_apple_frame_chunk(peer, ctx,
					pd, wr->sg_list, wr->num_sge, chunk,
					base, this_len, marker_mode, chunk == 0,
					tail_eof);
			}
			if (ret)
				goto fail;
		}
		goto posted;
	}

	for (chunk = 0; chunk < chunks;) {
		u32 group_max = raw_mode ? ARDMA_FRAME_SIZE :
			ARDMA_FRAME_SLOT_USER_SIZE *
			(ARDMA_FRAME_SIZE / ARDMA_SLOT_WIRE_SIZE);
		u32 group_base = chunk * chunk_len;
		u32 group_len = min_t(u32, group_max, total - group_base);
		u32 group_chunks;
		u32 group_crc = 0;
		u32 i;

		if (group_len % chunk_len) {
			ret = -EMSGSIZE;
			goto fail;
		}

		if (raw_mode) {
			ret = ardma_crc32c_sges(pd, wr->sg_list, wr->num_sge,
						group_base, group_len,
						&group_crc);
			if (ret)
				goto fail;
		}

		group_chunks = group_len / chunk_len;
		for (i = 0; i < group_chunks; i++, chunk++) {
			u32 base = group_base + i * chunk_len;
			bool group_tail = i + 1 == group_chunks;
			bool append_crc = raw_mode && group_tail;
			u8 tail_eof = 0;
			unsigned int pace_us;

			if (group_tail)
				tail_eof = group_base + group_len == total ? 3 : 2;

			/* Diagnostic pacing: sleep between negotiated MTU chunks if asked.
			 * Used to test the overrun hypothesis without changing the
			 * wire path. chunk==0 skips so the first chunk doesn't pay
			 * the latency cost. */
			pace_us = READ_ONCE(tx_pace_us);
			if (chunk && pace_us)
				usleep_range(pace_us, pace_us + 10);

			ret = ardma_send_apple_chunk(peer, ctx, pd, wr->sg_list,
						     wr->num_sge, chunk, base,
						     chunk_len, marker_mode,
						     raw_mode, i == 0,
						     tail_eof, append_crc,
						     group_crc);
			if (ret)
				goto fail;
		}
	}

posted:
	ardma_log_tx_event(peer, ARDMA_TX_EVT_WR_POSTED, ctx->qpn,
			   ctx->dest_qpn, ctx->wr_id, total, 0, chunks, 0,
			   0, chunk_len, path_mtu, 0, 0,
			   atomic_read(&ctx->pending), 0,
			   ctx->signaled, false);
	ardma_tx_ctx_put(ctx, false);
	return 0;

fail:
	atomic64_inc(&peer->tx_errors);
	ardma_log_tx_event(peer, ARDMA_TX_EVT_WR_FAIL, ctx->qpn,
			   ctx->dest_qpn, ctx->wr_id, total, 0, chunk, 0,
			   0, 0, 0, 0, 0, atomic_read(&ctx->pending), ret,
			   ctx->signaled, true);
	ardma_tx_ctx_put(ctx, true);
	return ret;
}

static void ardma_tx_apply_inter_wr_gap(struct ardma_qp *qp)
{
	unsigned int gap_us = READ_ONCE(tx_inter_wr_gap_us);
	u64 gap_ns, now, last, target, delay_ns, delay_us;
	unsigned long flags;

	if (!gap_us)
		return;

	gap_ns = (u64)gap_us * NSEC_PER_USEC;
	for (;;) {
		spin_lock_irqsave(&qp->send_lock, flags);
		now = ktime_get_ns();
		last = qp->last_post_send_ns;
		if (!last || now - last >= gap_ns) {
			qp->last_post_send_ns = now;
			spin_unlock_irqrestore(&qp->send_lock, flags);
			return;
		}
		target = last + gap_ns;
		spin_unlock_irqrestore(&qp->send_lock, flags);

		delay_ns = target - now;
		delay_us = DIV_ROUND_UP_ULL(delay_ns, NSEC_PER_USEC);
		if (delay_us > UINT_MAX - 10)
			delay_us = UINT_MAX - 10;
		usleep_range((unsigned int)delay_us,
			     (unsigned int)delay_us + 10);
	}
}

/* ----- verbs object ops ------------------------------------------ */

static int ardma_alloc_ucontext(struct ib_ucontext *ibuc,
				struct ib_udata *udata)
{
	return 0;
}

static void ardma_dealloc_ucontext(struct ib_ucontext *ibuc)
{
}

static int ardma_alloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	struct ardma_pd *pd = container_of(ibpd, struct ardma_pd, base);

	INIT_LIST_HEAD(&pd->mrs);
	spin_lock_init(&pd->mr_lock);
	return 0;
}

static int ardma_dealloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	return 0;
}

static struct ib_mr *ardma_reg_user_mr(struct ib_pd *ibpd, u64 start,
				       u64 length, u64 virt_addr,
				       int access_flags, struct ib_dmah *dmah,
				       struct ib_udata *udata)
{
	static atomic_t lkey_counter = ATOMIC_INIT(1);
	struct ardma_pd *pd = container_of(ibpd, struct ardma_pd, base);
	struct ardma_ibdev *dev =
		container_of(ibpd->device, struct ardma_ibdev, base);
	struct ardma_mr *mr;
	unsigned long flags;
	u64 page_off, va_aligned;
	long got;
	int npages, err, i;
	u32 lkey;

	page_off = start & ~PAGE_MASK;
	va_aligned = start & PAGE_MASK;
	npages = (page_off + length + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (npages <= 0 || npages > 1024 * 1024)
		return ERR_PTR(-EINVAL);

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return ERR_PTR(-ENOMEM);
	mr->user_va = start;
	mr->length = length;
	mr->npages = npages;
	refcount_set(&mr->refs, 1);
	init_waitqueue_head(&mr->ref_wait);
	INIT_LIST_HEAD(&mr->pd_link);

	mr->pages = kvcalloc(npages, sizeof(*mr->pages), GFP_KERNEL);
	if (!mr->pages) {
		err = -ENOMEM;
		goto err_mr;
	}

	got = pin_user_pages_fast(va_aligned, npages,
				  FOLL_WRITE | FOLL_LONGTERM, mr->pages);
	if (got < 0) {
		err = got;
		goto err_pages;
	}
	if (got < npages) {
		unpin_user_pages(mr->pages, got);
		err = -EFAULT;
		goto err_pages;
	}

	if (READ_ONCE(tx_zcopy) && dev->peer && dev->peer->tx_ring) {
		struct device *dma_dev = tb_ring_dma_device(dev->peer->tx_ring);

		mr->dma_addrs = kvcalloc(npages, sizeof(*mr->dma_addrs),
					 GFP_KERNEL);
		if (mr->dma_addrs) {
			mr->dma_dev = dma_dev;
			for (i = 0; i < npages; i++) {
				mr->dma_addrs[i] = dma_map_page(dma_dev,
								mr->pages[i],
								0, PAGE_SIZE,
								DMA_TO_DEVICE);
				if (dma_mapping_error(dma_dev,
						      mr->dma_addrs[i]))
					goto disable_zcopy;
				dma_sync_single_for_cpu(dma_dev,
							mr->dma_addrs[i],
							PAGE_SIZE,
							DMA_TO_DEVICE);
			}
		}
	}

mapped_or_disabled:
	lkey = atomic_inc_return(&lkey_counter);
	mr->base.lkey = lkey;
	mr->base.rkey = lkey;
	mr->base.length = length;
	mr->base.iova = virt_addr;
	mr->base.pd = ibpd;
	mr->base.device = ibpd->device;

	spin_lock_irqsave(&pd->mr_lock, flags);
	list_add(&mr->pd_link, &pd->mrs);
	spin_unlock_irqrestore(&pd->mr_lock, flags);
	return &mr->base;

disable_zcopy:
	while (i > 0) {
		i--;
		dma_unmap_page(mr->dma_dev, mr->dma_addrs[i], PAGE_SIZE,
			       DMA_TO_DEVICE);
	}
	kvfree(mr->dma_addrs);
	mr->dma_addrs = NULL;
	mr->dma_dev = NULL;
	goto mapped_or_disabled;

err_pages:
	kvfree(mr->pages);
err_mr:
	kfree(mr);
	return ERR_PTR(err);
}

static int ardma_dereg_mr(struct ib_mr *ibmr, struct ib_udata *udata)
{
	struct ardma_mr *mr = container_of(ibmr, struct ardma_mr, base);
	struct ardma_pd *pd = container_of(ibmr->pd, struct ardma_pd, base);
	struct ardma_ibdev *dev =
		container_of(ibmr->device, struct ardma_ibdev, base);
	unsigned long flags;
	unsigned int timeout_ms;

	spin_lock_irqsave(&pd->mr_lock, flags);
	if (!mr->dying) {
		mr->dying = true;
		list_del_init(&mr->pd_link);
	}
	spin_unlock_irqrestore(&pd->mr_lock, flags);

	timeout_ms = READ_ONCE(mr_dereg_timeout_ms);
	if (timeout_ms) {
		unsigned long timeout = msecs_to_jiffies(timeout_ms);

		if (!wait_event_timeout(mr->ref_wait,
					refcount_read(&mr->refs) == 1,
					timeout)) {
			struct ardma_peer *peer = dev->peer;

			pr_warn("dereg_mr lkey=0x%x refs=%u after %u ms; forcing TX ring drain\n",
				mr->base.lkey, refcount_read(&mr->refs),
				timeout_ms);
			if (peer && peer->tx_ring) {
				ardma_stop_tx_ring(peer);
				wait_event_timeout(mr->ref_wait,
						   refcount_read(&mr->refs) == 1,
						   msecs_to_jiffies(2000));
				ardma_restart_tx_ring(peer);
			}
			if (refcount_read(&mr->refs) != 1) {
				pr_warn("dereg_mr lkey=0x%x still busy refs=%u; leaving MR for retry\n",
					mr->base.lkey,
					refcount_read(&mr->refs));
				return -EBUSY;
			}
		}
	} else {
		wait_event(mr->ref_wait, refcount_read(&mr->refs) == 1);
	}
	if (mr->dma_addrs && mr->dma_dev) {
		int i;

		for (i = 0; i < mr->npages; i++)
			dma_unmap_page(mr->dma_dev, mr->dma_addrs[i],
				       PAGE_SIZE, DMA_TO_DEVICE);
		kvfree(mr->dma_addrs);
	}
	if (mr->pages) {
		unpin_user_pages(mr->pages, mr->npages);
		kvfree(mr->pages);
	}
	kfree(mr);
	return 0;
}

static struct ib_mr *ardma_get_dma_mr(struct ib_pd *ibpd, int access_flags)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static int ardma_create_cq(struct ib_cq *ibcq,
			   const struct ib_cq_init_attr *attr,
			   struct uverbs_attr_bundle *attrs)
{
	struct ardma_cq *cq = container_of(ibcq, struct ardma_cq, base);
	int i;

	if (attr->cqe <= 0 || attr->cqe > ARDMA_MAX_CQE)
		return -EINVAL;

	spin_lock_init(&cq->lock);
	INIT_LIST_HEAD(&cq->wc_list);
	INIT_LIST_HEAD(&cq->free_list);
	cq->pool = kvcalloc(attr->cqe, sizeof(*cq->pool), GFP_KERNEL);
	if (!cq->pool)
		return -ENOMEM;
	for (i = 0; i < attr->cqe; i++)
		list_add_tail(&cq->pool[i].list, &cq->free_list);
	cq->cqe_capacity = attr->cqe;
	cq->free_count = attr->cqe;
	return 0;
}

static int ardma_destroy_cq(struct ib_cq *ibcq, struct ib_udata *udata)
{
	struct ardma_cq *cq = container_of(ibcq, struct ardma_cq, base);

	kvfree(cq->pool);
	return 0;
}

static int ardma_poll_cq(struct ib_cq *ibcq, int num_entries,
			 struct ib_wc *wc)
{
	struct ardma_cq *cq = container_of(ibcq, struct ardma_cq, base);
	struct ardma_wc_entry *e, *tmp;
	unsigned long flags;
	int n = 0;

	spin_lock_irqsave(&cq->lock, flags);
	list_for_each_entry_safe(e, tmp, &cq->wc_list, list) {
		if (n >= num_entries)
			break;
		wc[n++] = e->wc;
		list_del(&e->list);
		cq->wc_count--;
		list_add_tail(&e->list, &cq->free_list);
		cq->free_count++;
	}
	spin_unlock_irqrestore(&cq->lock, flags);
	return n;
}

static int ardma_req_notify_cq(struct ib_cq *ibcq,
			       enum ib_cq_notify_flags flags)
{
	struct ardma_cq *cq = container_of(ibcq, struct ardma_cq, base);
	unsigned long irqf;

	spin_lock_irqsave(&cq->lock, irqf);
	cq->notify = flags & IB_CQ_SOLICITED_MASK;
	if ((flags & IB_CQ_REPORT_MISSED_EVENTS) && cq->wc_count) {
		spin_unlock_irqrestore(&cq->lock, irqf);
		return 1;
	}
	spin_unlock_irqrestore(&cq->lock, irqf);
	return 0;
}

static int ardma_create_qp(struct ib_qp *ibqp, struct ib_qp_init_attr *attr,
			   struct ib_udata *udata)
{
	struct ardma_qp *qp = container_of(ibqp, struct ardma_qp, base);
	struct ardma_ibdev *dev =
		container_of(ibqp->device, struct ardma_ibdev, base);
	struct ardma_peer *peer;
	int peer_count = 0;
	int qpn;
	int ret;

	if (attr->qp_type != IB_QPT_UC && attr->qp_type != IB_QPT_GSI)
		return -EOPNOTSUPP;
	if (attr->cap.max_send_sge > ARDMA_MAX_SGE ||
	    attr->cap.max_recv_sge > ARDMA_MAX_SGE)
		return -EINVAL;

	peer = NULL;
	if (attr->qp_type == IB_QPT_UC) {
		peer_count = ardma_peer_count();
		if (!peer_count || !dev->peer || READ_ONCE(dev->peer->closing))
			return -ENOTCONN;
		peer = dev->peer;
		refcount_inc(&peer->refs);
	}

	qp->peer = peer;
	qp->qp_type = attr->qp_type;
	qp->init_attr = *attr;
	qp->init_attr.cap.max_send_wr = min_t(u32, attr->cap.max_send_wr, 4095);
	qp->init_attr.cap.max_recv_wr = min_t(u32, attr->cap.max_recv_wr, 4095);
	qp->init_attr.cap.max_send_sge = min_t(u32, attr->cap.max_send_sge,
					       ARDMA_MAX_SGE);
	qp->init_attr.cap.max_recv_sge = min_t(u32, attr->cap.max_recv_sge,
					       ARDMA_MAX_SGE);
	attr->cap = qp->init_attr.cap;
	qp->state = attr->qp_type == IB_QPT_GSI ? IB_QPS_RTS : IB_QPS_RESET;
	qp->sq_sig_all = attr->sq_sig_type == IB_SIGNAL_ALL_WR;
	refcount_set(&qp->refs, 1);
	init_waitqueue_head(&qp->ref_wait);
	spin_lock_init(&qp->recv_lock);
	spin_lock_init(&qp->send_lock);
	spin_lock_init(&qp->tx_pool_lock);
	INIT_LIST_HEAD(&qp->recv_q);
	INIT_LIST_HEAD(&qp->qps_link);
	INIT_LIST_HEAD(&qp->tx_pool_free);
	qp->rx_pending_active = -1;
	if (attr->qp_type == IB_QPT_UC) {
		int i;

		for (i = 0; i < ARDMA_PENDING_RX_SLOTS; i++) {
			qp->rx_pending[i].buf = kzalloc(ARDMA_PENDING_RX_BYTES,
							GFP_KERNEL);
			if (!qp->rx_pending[i].buf) {
				while (i--)
					kfree(qp->rx_pending[i].buf);
				ardma_peer_put(peer);
				return -ENOMEM;
			}
		}
	}

	if (attr->qp_type == IB_QPT_GSI) {
		ibqp->qp_num = 1;
	} else if (peer_count > 1) {
		qpn = receive_path << ARDMA_QPN_BASE_SHIFT;
		if (qpn < ARDMA_QPN_MIN || qpn > ARDMA_QPN_MAX) {
			int i;

			for (i = 0; i < ARDMA_PENDING_RX_SLOTS; i++) {
				kfree(qp->rx_pending[i].buf);
				qp->rx_pending[i].buf = NULL;
			}
			ardma_peer_put(peer);
			return -ENOSPC;
		}
		ibqp->qp_num = qpn;
	} else {
		qpn = ardma_alloc_qpn();
		if (qpn < 0) {
			int i;

			for (i = 0; i < ARDMA_PENDING_RX_SLOTS; i++) {
				kfree(qp->rx_pending[i].buf);
				qp->rx_pending[i].buf = NULL;
			}
			ardma_peer_put(peer);
			return qpn;
		}
		ibqp->qp_num = qpn;
		qp->qpn_allocated = true;
	}

	if (attr->qp_type == IB_QPT_UC) {
		ret = ardma_tx_pool_init(qp, peer);
		if (ret) {
			int i;

			if (qp->qpn_allocated) {
				ardma_free_qpn(ibqp->qp_num);
				qp->qpn_allocated = false;
			}
			for (i = 0; i < ARDMA_PENDING_RX_SLOTS; i++) {
				kfree(qp->rx_pending[i].buf);
				qp->rx_pending[i].buf = NULL;
			}
			ardma_peer_put(peer);
			return ret;
		}
		ardma_peer_qp_activate(qp, peer);
	}

	pr_info("create_qp %s qpn=0x%x peer=%s peer_count=%d\n",
		attr->qp_type == IB_QPT_UC ? "UC" : "GSI", ibqp->qp_num,
		peer ? dev_name(&peer->svc->dev) : "(deferred)", peer_count);
	ardma_log_rx_event(peer, ARDMA_RX_EVT_QP_CREATE, ibqp->qp_num, 0, 0,
			   qp->init_attr.cap.max_send_wr,
			   qp->init_attr.cap.max_recv_wr, attr->qp_type, 0,
			   0, 0, 0, qp->state, qp->registered,
			   qp->recv_q_depth, qp->rx_pending_ready_count,
			   qp->rx_pending_active, qp->rx_piece, 0);
	return 0;
}

static int ardma_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			   int attr_mask, struct ib_udata *udata)
{
	struct ardma_qp *qp = container_of(ibqp, struct ardma_qp, base);
	enum ib_qp_state old = qp->state;
	unsigned long flags;
	int ret;

	if (attr_mask & IB_QP_PATH_MTU) {
		int mtu = ib_mtu_enum_to_int(attr->path_mtu);

		if (mtu <= 0 || mtu > ardma_active_mtu_bytes())
			return -EINVAL;
	}

	if (qp->qp_type == IB_QPT_UC && !qp->peer &&
	    (attr_mask & IB_QP_STATE) && attr->qp_state == IB_QPS_RTR) {
		const struct ib_global_route *grh = NULL;
		struct ardma_peer *peer = NULL;
		__be32 daddr;

		if (attr_mask & IB_QP_AV)
			grh = rdma_ah_read_grh(&attr->ah_attr);
		if (grh)
			peer = ardma_peer_get_by_gid_map(&grh->dgid);
		if (!peer)
			peer = ardma_peer_get_single();
		if (!peer) {
			if (grh && ardma_gid_ipv4_mapped(&grh->dgid, &daddr))
				pr_warn("modify_qp[0x%x]: no peer for remote GID %pI4; set peer_gid_routes (currently '%s')\n",
					ibqp->qp_num, &daddr,
					peer_gid_routes ? peer_gid_routes : "");
			else
				pr_warn("modify_qp[0x%x]: no peer for RTR; set peer_gid_routes for multi-peer mode\n",
					ibqp->qp_num);
			return -ENOTCONN;
		}
		ret = ardma_bind_qp_peer(qp, peer);
		if (ret)
			return ret;
		pr_info("modify_qp[0x%x]: bound to peer %s route=%d-%llx\n",
			ibqp->qp_num, dev_name(&qp->peer->svc->dev),
			qp->peer->xd->tb->index,
			(unsigned long long)qp->peer->xd->route);
	}

	if (attr_mask & IB_QP_STATE)
		qp->state = attr->qp_state;
	if (attr_mask & IB_QP_DEST_QPN)
		qp->attr.dest_qp_num = attr->dest_qp_num;
	if (attr_mask & IB_QP_AV)
		qp->attr.ah_attr = attr->ah_attr;
	if (attr_mask & IB_QP_PORT)
		qp->attr.port_num = attr->port_num;
	if (attr_mask & IB_QP_PATH_MTU)
		qp->attr.path_mtu = attr->path_mtu;
	if (attr_mask & IB_QP_SQ_PSN)
		qp->attr.sq_psn = attr->sq_psn;
	if (attr_mask & IB_QP_RQ_PSN)
		qp->attr.rq_psn = attr->rq_psn;
	qp->attr_mask |= attr_mask;

	if (qp->qp_type == IB_QPT_UC && !qp->registered &&
	    old == IB_QPS_RESET && qp->state == IB_QPS_INIT) {
		spin_lock_irqsave(&ardma_qp_lock, flags);
		list_add_tail(&qp->qps_link, &ardma_qp_list);
		spin_unlock_irqrestore(&ardma_qp_lock, flags);
		qp->registered = true;
	}
	if (qp->registered &&
	    (qp->state == IB_QPS_RESET || qp->state == IB_QPS_ERR)) {
		spin_lock_irqsave(&ardma_qp_lock, flags);
		if (!list_empty(&qp->qps_link))
			list_del_init(&qp->qps_link);
		spin_unlock_irqrestore(&ardma_qp_lock, flags);
		qp->registered = false;
	}

	pr_info("modify_qp[0x%x]: %d -> %d dest=0x%x\n", ibqp->qp_num, old,
		qp->state, qp->attr.dest_qp_num);
	ardma_log_rx_event(qp->peer, ARDMA_RX_EVT_QP_MODIFY, ibqp->qp_num,
			   qp->attr.dest_qp_num, 0, attr_mask, 0, 0, 0, 0,
			   0, old, qp->state, qp->registered,
			   qp->recv_q_depth, qp->rx_pending_ready_count,
			   qp->rx_pending_active, qp->rx_piece, 0);
	return 0;
}

static int ardma_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			  int attr_mask, struct ib_qp_init_attr *init_attr)
{
	struct ardma_qp *qp = container_of(ibqp, struct ardma_qp, base);

	*attr = qp->attr;
	attr->qp_state = qp->state;
	*init_attr = qp->init_attr;
	return 0;
}

static int ardma_destroy_qp(struct ib_qp *ibqp, struct ib_udata *udata)
{
	struct ardma_qp *qp = container_of(ibqp, struct ardma_qp, base);
	struct ardma_recv_wr *r, *tmp;
	unsigned long flags;
	int i;

	ardma_log_rx_event(qp->peer, ARDMA_RX_EVT_QP_DESTROY, ibqp->qp_num,
			   qp->attr.dest_qp_num, 0, refcount_read(&qp->refs),
			   0, 0, 0, 0, 0, qp->state, IB_QPS_RESET,
			   qp->registered, qp->recv_q_depth,
			   qp->rx_pending_ready_count, qp->rx_pending_active,
			   qp->rx_piece, 0);

	if (qp->registered) {
		spin_lock_irqsave(&ardma_qp_lock, flags);
		if (!list_empty(&qp->qps_link))
			list_del_init(&qp->qps_link);
		spin_unlock_irqrestore(&ardma_qp_lock, flags);
		qp->registered = false;
	}

	if (refcount_read(&qp->refs) > 1 && qp->peer) {
		/* Don't stop the peer's TX ring -- it is shared across all
		 * QPs of this peer, and stopping it would break unrelated
		 * QPs (returning -ESHUTDOWN from their post_send). Wait for
		 * our refs to drain naturally; the timeout below handles
		 * the case where they don't. */
		pr_warn("destroy_qp[0x%x]: %u in-flight refs at destroy; waiting for natural completion\n",
			ibqp->qp_num, refcount_read(&qp->refs));
	}
	if (!wait_event_timeout(qp->ref_wait,
				refcount_read(&qp->refs) == 1,
				msecs_to_jiffies(5000))) {
		bool drained = false;

		if (READ_ONCE(tx_force_drain_on_destroy) && qp->peer &&
		    qp->peer->tx_ring) {
			/* Force-cancel stuck NHI descriptors. tb_ring_stop
			 * triggers ardma_tx_callback(canceled=true) for every
			 * pending tx_frame, which decrements ctx pending counts
			 * and ultimately runs ardma_qp_put() on QPs whose WRs
			 * never naturally completed (e.g., Mac never returned
			 * E2E credits). This kills any in-flight TX on OTHER
			 * QPs sharing the same peer ring -- accept that vs
			 * leaking QP state and wedging the module-unload path. */
			pr_warn("destroy_qp[0x%x]: forcing TX ring drain to clear refs=%u\n",
				ibqp->qp_num, refcount_read(&qp->refs));
			ardma_stop_tx_ring(qp->peer);
			drained = wait_event_timeout(qp->ref_wait,
						     refcount_read(&qp->refs) == 1,
						     msecs_to_jiffies(2000)) > 0;
			/* Restart the ring so future post_sends on other QPs
			 * (or a fresh QP create on this peer) don't get
			 * permanent ESHUTDOWN. */
			ardma_restart_tx_ring(qp->peer);
		}

		if (!drained) {
			pr_warn("destroy_qp[0x%x]: timed out waiting for refs=%u; "
				"releasing peer ref to avoid hot-unplug hang (leaks QP)\n",
				ibqp->qp_num, refcount_read(&qp->refs));
			/* Critical: drop the peer ref even on timeout. Otherwise
			 * ardma_remove's unbounded wait_event(refs==1) hangs the
			 * tb_service remove path forever (D-state kworker on cable
			 * unplug). We accept the QP-level memory leak (recv_q,
			 * rx_wr, rx_pending) over a system-level wedge. */
			if (qp->qpn_allocated) {
				ardma_free_qpn(ibqp->qp_num);
				qp->qpn_allocated = false;
			}
			ardma_peer_qp_deactivate(qp);
			ardma_peer_put(qp->peer);
			qp->peer = NULL;
			return -ETIMEDOUT;
		}
		pr_info("destroy_qp[0x%x]: TX ring drain completed; clean cleanup\n",
			ibqp->qp_num);
	}

	spin_lock_irqsave(&qp->recv_lock, flags);
	list_for_each_entry_safe(r, tmp, &qp->recv_q, list) {
		list_del(&r->list);
		ardma_recv_wr_release(qp, r);
	}
	ardma_recv_wr_release(qp, qp->rx_wr);
	qp->rx_wr = NULL;
	spin_unlock_irqrestore(&qp->recv_lock, flags);
	ardma_tx_pool_destroy(qp);
	for (i = 0; i < ARDMA_PENDING_RX_SLOTS; i++) {
		kfree(qp->rx_pending[i].buf);
		qp->rx_pending[i].buf = NULL;
	}

	if (qp->qpn_allocated) {
		ardma_free_qpn(ibqp->qp_num);
		qp->qpn_allocated = false;
	}
	ardma_peer_qp_deactivate(qp);
	ardma_peer_put(qp->peer);
	qp->peer = NULL;
	return 0;
}

static int ardma_post_recv(struct ib_qp *ibqp, const struct ib_recv_wr *wr,
			   const struct ib_recv_wr **bad_wr)
{
	struct ardma_qp *qp = container_of(ibqp, struct ardma_qp, base);
	struct ardma_pd *pd = container_of(qp->base.pd, struct ardma_pd, base);
	unsigned long flags;
	bool posted = false;

	if (qp->state != IB_QPS_INIT && qp->state != IB_QPS_RTR &&
	    qp->state != IB_QPS_RTS && qp->state != IB_QPS_SQD &&
	    qp->state != IB_QPS_SQE) {
		if (bad_wr)
			*bad_wr = wr;
		return -EINVAL;
	}

	for (; wr; wr = wr->next) {
		struct ardma_recv_wr *r;
		u32 total_len = 0;
		int i;

		if (wr->num_sge > ARDMA_MAX_SGE ||
		    (wr->num_sge && !wr->sg_list)) {
			if (bad_wr)
				*bad_wr = wr;
			return -EINVAL;
		}
		r = kzalloc(sizeof(*r), GFP_KERNEL);
		if (!r) {
			if (bad_wr)
				*bad_wr = wr;
			return -ENOMEM;
		}
		r->wr_id = wr->wr_id;
		r->wr_cqe = wr->wr_cqe;
		r->num_sge = wr->num_sge;
		for (i = 0; i < wr->num_sge; i++) {
			int ret;

			r->sge[i] = wr->sg_list[i];
			if (!r->sge[i].length)
				continue;
			r->mr[i] = ardma_pd_get_mr(pd, r->sge[i].lkey);
			if (!r->mr[i]) {
				ardma_recv_wr_free(r);
				if (bad_wr)
					*bad_wr = wr;
				return -EINVAL;
			}
			ret = ardma_mr_check_range(r->mr[i], r->sge[i].addr,
						   r->sge[i].length);
			if (ret) {
				ardma_recv_wr_free(r);
				if (bad_wr)
					*bad_wr = wr;
				return ret;
			}
		}
		if (ardma_recv_wr_total_len(r, &total_len) ||
		    total_len > ARDMA_MAX_MSG_SIZE) {
			ardma_recv_wr_free(r);
			if (bad_wr)
				*bad_wr = wr;
			return -EMSGSIZE;
		}

		if (READ_ONCE(raw_rx_credit_guard) &&
		    ardma_peer_rx_raw(qp->peer)) {
			u32 credits = ardma_apple_raw_rx_credits(total_len);
			u32 budget = ardma_peer_raw_rx_credit_budget(qp->peer);
			u32 max_recv = READ_ONCE(raw_rx_max_recv_bytes);
			u32 byte_budget = READ_ONCE(raw_rx_window_bytes);
			s64 bytes_reserved;
			int reserved;

			if (max_recv && total_len > max_recv) {
				pr_warn_ratelimited("post_recv[qp=0x%x]: rejecting Apple RAW recv len=%u max=%u\n",
						    ibqp->qp_num, total_len,
						    max_recv);
				ardma_recv_wr_free(r);
				if (bad_wr)
					*bad_wr = wr;
				return -EMSGSIZE;
			}

			if (!budget || credits > budget) {
				pr_warn_ratelimited("post_recv[qp=0x%x]: rejecting Apple RAW recv len=%u credits=%u budget=%u\n",
						    ibqp->qp_num, total_len,
						    credits, budget);
				ardma_recv_wr_free(r);
				if (bad_wr)
					*bad_wr = wr;
				return -EMSGSIZE;
			}

			if (byte_budget) {
				bytes_reserved =
					atomic64_add_return(total_len,
							     &qp->peer->raw_rx_bytes_reserved);
				if (bytes_reserved > byte_budget) {
					atomic64_sub(total_len,
						     &qp->peer->raw_rx_bytes_reserved);
					pr_warn_ratelimited("post_recv[qp=0x%x]: rejecting Apple RAW recv byte window len=%u reserved=%lld budget=%u\n",
							    ibqp->qp_num,
							    total_len,
							    (long long)bytes_reserved,
							    byte_budget);
					ardma_recv_wr_free(r);
					if (bad_wr)
						*bad_wr = wr;
					return -ENOMEM;
				}
				r->raw_rx_bytes = total_len;
			}

			reserved = atomic_add_return(credits,
						     &qp->peer->raw_rx_credits_reserved);
			if (reserved > budget) {
				atomic_sub(credits,
					   &qp->peer->raw_rx_credits_reserved);
				if (r->raw_rx_bytes) {
					atomic64_sub(r->raw_rx_bytes,
						     &qp->peer->raw_rx_bytes_reserved);
					r->raw_rx_bytes = 0;
				}
				pr_warn_ratelimited("post_recv[qp=0x%x]: rejecting Apple RAW recv window len=%u credits=%u reserved=%d budget=%u\n",
						    ibqp->qp_num, total_len,
						    credits, reserved, budget);
				ardma_recv_wr_free(r);
				if (bad_wr)
					*bad_wr = wr;
				return -ENOMEM;
			}
			r->raw_rx_credits = credits;
		}

		spin_lock_irqsave(&qp->recv_lock, flags);
		list_add_tail(&r->list, &qp->recv_q);
		qp->recv_q_depth++;
		ardma_log_rx_event(qp->peer, ARDMA_RX_EVT_POST_RECV,
				   ibqp->qp_num, qp->attr.dest_qp_num,
				   r->wr_id, 0, 0, total_len, 0, 0, 0, 0,
				   qp->state, qp->registered,
				   qp->recv_q_depth,
				   qp->rx_pending_ready_count,
				   qp->rx_pending_active, qp->rx_piece, 0);
		spin_unlock_irqrestore(&qp->recv_lock, flags);
		posted = true;
	}

	if (posted && qp->peer && !READ_ONCE(qp->peer->paths_enabled)) {
		int ret = ardma_enable_peer_paths(qp->peer);

		if (ret) {
			pr_warn_ratelimited("post_recv[qp=0x%x]: lazy paths enable failed=%d\n",
					    ibqp->qp_num, ret);
		}
	}

	for (;;) {
		struct ardma_recv_wr *r;
		enum ib_wc_status status;
		u32 byte_len;

		spin_lock_irqsave(&qp->recv_lock, flags);
		r = ardma_flush_pending_locked(qp, &byte_len, &status);
		spin_unlock_irqrestore(&qp->recv_lock, flags);
		if (!r)
			break;
		ardma_push_rx_wc(qp, r, byte_len, status);
	}
	return 0;
}

static int ardma_post_send(struct ib_qp *ibqp, const struct ib_send_wr *wr,
			   const struct ib_send_wr **bad_wr)
{
	struct ardma_qp *qp = container_of(ibqp, struct ardma_qp, base);

	if (qp->state != IB_QPS_RTS && qp->state != IB_QPS_SQD &&
	    qp->state != IB_QPS_SQE) {
		if (bad_wr)
			*bad_wr = wr;
		return -EINVAL;
	}

	for (; wr; wr = wr->next) {
		int ret;

		if (wr->opcode != IB_WR_SEND) {
			if (bad_wr)
				*bad_wr = wr;
			return -EOPNOTSUPP;
		}
		ardma_tx_apply_inter_wr_gap(qp);
		ret = ardma_send_apple(qp, wr);
		if (ret) {
			if (bad_wr)
				*bad_wr = wr;
			return ret;
		}
	}
	return 0;
}

static int ardma_create_ah(struct ib_ah *ibah,
			   struct rdma_ah_init_attr *init_attr,
			   struct ib_udata *udata)
{
	struct ardma_ah *ah = container_of(ibah, struct ardma_ah, base);

	if (!init_attr || !init_attr->ah_attr)
		return -EINVAL;
	rdma_copy_ah_attr(&ah->attr, init_attr->ah_attr);
	return 0;
}

static int ardma_query_ah(struct ib_ah *ibah, struct rdma_ah_attr *attr)
{
	struct ardma_ah *ah = container_of(ibah, struct ardma_ah, base);

	rdma_copy_ah_attr(attr, &ah->attr);
	return 0;
}

static int ardma_destroy_ah(struct ib_ah *ibah, u32 flags)
{
	struct ardma_ah *ah = container_of(ibah, struct ardma_ah, base);

	rdma_destroy_ah_attr(&ah->attr);
	return 0;
}

/* ----- query/GID ops --------------------------------------------- */

static int ardma_query_device(struct ib_device *ibdev,
			      struct ib_device_attr *attr,
			      struct ib_udata *udata)
{
	memset(attr, 0, sizeof(*attr));
	attr->vendor_id = 0x106b;
	attr->vendor_part_id = APPLE_RDMA_PRTCID;
	attr->hw_ver = 1;
	attr->sys_image_guid = ibdev->node_guid;
	attr->device_cap_flags = IB_DEVICE_CHANGE_PHY_PORT;
	attr->kernel_cap_flags = IBK_LOCAL_DMA_LKEY;
	attr->max_mr_size = ~0ull;
	attr->page_size_cap = SZ_4K | SZ_2M | SZ_1G;
	attr->max_qp = 11;
	attr->max_qp_wr = 4095;
	attr->max_send_sge = ARDMA_MAX_SGE;
	attr->max_recv_sge = ARDMA_MAX_SGE;
	attr->max_cq = 11;
	attr->max_cqe = ARDMA_MAX_CQE;
	attr->max_mr = 1024;
	attr->max_pd = 11;
	attr->max_ah = 1024;
	attr->atomic_cap = IB_ATOMIC_NONE;
	attr->max_pkeys = 1;
	return 0;
}

static int ardma_query_port(struct ib_device *ibdev, u32 port_num,
			    struct ib_port_attr *attr)
{
	struct ardma_ibdev *dev = container_of(ibdev, struct ardma_ibdev, base);
	bool active;

	if (port_num != 1)
		return -EINVAL;

	memset(attr, 0, sizeof(*attr));
	active = dev->peer && !READ_ONCE(dev->peer->closing) &&
		dev->peer->paths_enabled;
	attr->state = active ? IB_PORT_ACTIVE : IB_PORT_DOWN;
	attr->phys_state = active ?
		IB_PORT_PHYS_STATE_LINK_UP : IB_PORT_PHYS_STATE_DISABLED;
	attr->max_mtu = IB_MTU_4096;
	attr->active_mtu = ardma_active_mtu_enum();
	attr->max_msg_sz = ARDMA_MAX_MSG_SIZE;
	attr->gid_tbl_len = 32;
	attr->pkey_tbl_len = 1;
	attr->max_vl_num = 1;
	attr->active_width = IB_WIDTH_4X;
	attr->active_speed = IB_SPEED_FDR10;
	return 0;
}

static int ardma_get_port_immutable(struct ib_device *ibdev, u32 port_num,
				    struct ib_port_immutable *immutable)
{
	struct ib_port_attr attr;
	int ret;

	ret = ardma_query_port(ibdev, port_num, &attr);
	if (ret)
		return ret;
	immutable->pkey_tbl_len = attr.pkey_tbl_len;
	immutable->gid_tbl_len = attr.gid_tbl_len;
	immutable->core_cap_flags = RDMA_CORE_PORT_IBA_ROCE_UDP_ENCAP;
	immutable->max_mad_size = IB_MGMT_MAD_SIZE;
	return 0;
}

static int ardma_query_pkey(struct ib_device *ibdev, u32 port, u16 index,
			    u16 *pkey)
{
	if (port != 1 || index)
		return -EINVAL;
	*pkey = 0xffff;
	return 0;
}

static enum rdma_link_layer ardma_get_link_layer(struct ib_device *ibdev,
						 u32 port_num)
{
	return IB_LINK_LAYER_ETHERNET;
}

static int ardma_add_gid(const struct ib_gid_attr *attr, void **context)
{
	return 0;
}

static int ardma_del_gid(const struct ib_gid_attr *attr, void **context)
{
	return 0;
}

static const struct ib_device_ops ardma_dev_ops = {
	.owner = THIS_MODULE,
	.driver_id = RDMA_DRIVER_UNKNOWN,
	.uverbs_abi_ver = ARDMA_UVERBS_ABI,
	.uverbs_no_driver_id_binding = 1,

	.query_device = ardma_query_device,
	.query_port = ardma_query_port,
	.query_pkey = ardma_query_pkey,
	.add_gid = ardma_add_gid,
	.del_gid = ardma_del_gid,
	.get_port_immutable = ardma_get_port_immutable,
	.get_link_layer = ardma_get_link_layer,

	.alloc_ucontext = ardma_alloc_ucontext,
	.dealloc_ucontext = ardma_dealloc_ucontext,
	.alloc_pd = ardma_alloc_pd,
	.dealloc_pd = ardma_dealloc_pd,
	.create_qp = ardma_create_qp,
	.destroy_qp = ardma_destroy_qp,
	.modify_qp = ardma_modify_qp,
	.query_qp = ardma_query_qp,
	.create_ah = ardma_create_ah,
	.create_user_ah = ardma_create_ah,
	.query_ah = ardma_query_ah,
	.destroy_ah = ardma_destroy_ah,
	.create_cq = ardma_create_cq,
	.destroy_cq = ardma_destroy_cq,
	.post_send = ardma_post_send,
	.post_recv = ardma_post_recv,
	.poll_cq = ardma_poll_cq,
	.req_notify_cq = ardma_req_notify_cq,
	.reg_user_mr = ardma_reg_user_mr,
	.dereg_mr = ardma_dereg_mr,
	.get_dma_mr = ardma_get_dma_mr,

	INIT_RDMA_OBJ_SIZE(ib_ucontext, ardma_ucontext, base),
	INIT_RDMA_OBJ_SIZE(ib_ah, ardma_ah, base),
	INIT_RDMA_OBJ_SIZE(ib_pd, ardma_pd, base),
	INIT_RDMA_OBJ_SIZE(ib_cq, ardma_cq, base),
	INIT_RDMA_OBJ_SIZE(ib_qp, ardma_qp, base),
};

/* ----- ib_device registration ------------------------------------ */

static void ardma_detach_netdev_locked(struct ardma_ibdev *dev)
{
	struct net_device *netdev = dev->netdev;

	if (!netdev)
		return;

	ib_device_set_netdev(&dev->base, NULL, 1);
	dev->netdev = NULL;
	dev_put(netdev);
}

static int ardma_attach_netdev_locked(struct ardma_ibdev *dev,
				      struct net_device *netdev)
{
	int ret;

	if (dev->netdev == netdev)
		return 0;
	if (dev->netdev)
		ardma_detach_netdev_locked(dev);

	ret = ib_device_set_netdev(&dev->base, netdev, 1);
	if (ret)
		return ret;

	dev_hold(netdev);
	dev->netdev = netdev;
	return 0;
}

static int ardma_netdev_event(struct notifier_block *nb,
			      unsigned long event, void *ptr)
{
	struct ardma_ibdev *dev =
		container_of(nb, struct ardma_ibdev, netdev_nb);
	struct net_device *netdev = netdev_notifier_info_to_dev(ptr);
	int ret;

	switch (event) {
	case NETDEV_UNREGISTER:
		mutex_lock(&dev->netdev_lock);
		if (dev->netdev == netdev) {
			pr_info("CM netdev %s unregistering; detaching GID netdev\n",
				netdev_name(netdev));
			ardma_detach_netdev_locked(dev);
			mutex_unlock(&dev->netdev_lock);
			return NOTIFY_OK;
		}
		mutex_unlock(&dev->netdev_lock);
		break;
	case NETDEV_REGISTER:
		if (strcmp(netdev_name(netdev), dev->netdev_name))
			break;
		mutex_lock(&dev->netdev_lock);
		if (!dev->netdev && !dev->shutting_down) {
			ret = ardma_attach_netdev_locked(dev, netdev);
			if (ret)
				pr_warn("reattach CM netdev %s failed: %d\n",
					netdev_name(netdev), ret);
			else
				pr_info("reattached CM netdev %s\n",
					netdev_name(netdev));
		}
		mutex_unlock(&dev->netdev_lock);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static int ardma_register_ibdev(struct ardma_peer *peer)
{
	struct ardma_ibdev *dev;
	u8 mac[ETH_ALEN];
	int ret;

	dev = ib_alloc_device(ardma_ibdev, base);
	if (!dev)
		return -ENOMEM;

	dev->peer = peer;
	mutex_init(&dev->netdev_lock);
	dev->netdev_nb.notifier_call = ardma_netdev_event;
	ardma_peer_netdev_name(peer, dev->netdev_name,
			       sizeof(dev->netdev_name));
	dev->base.phys_port_cnt = 1;
	dev->base.num_comp_vectors = num_possible_cpus();
	dev->base.local_dma_lkey = 0;
	dev->base.node_type = RDMA_NODE_IB_CA;
	dev->base.uverbs_cmd_mask |=
		BIT_ULL(IB_USER_VERBS_CMD_POST_SEND) |
		BIT_ULL(IB_USER_VERBS_CMD_POST_RECV) |
		BIT_ULL(IB_USER_VERBS_CMD_POLL_CQ) |
		BIT_ULL(IB_USER_VERBS_CMD_REQ_NOTIFY_CQ);

	eth_random_addr(mac);
	addrconf_addr_eui48((u8 *)&dev->base.node_guid, mac);

	dev->netdev = dev_get_by_name(&init_net, dev->netdev_name);
	if (!dev->netdev) {
		pr_err("CM netdev '%s' not found for peer %s route=%d-%llx\n",
		       dev->netdev_name, dev_name(&peer->svc->dev),
		       peer->xd->tb->index, (unsigned long long)peer->xd->route);
		ib_dealloc_device(&dev->base);
		return -ENODEV;
	}

	ib_set_device_ops(&dev->base, &ardma_dev_ops);
	ret = ib_device_set_netdev(&dev->base, dev->netdev, 1);
	if (ret)
		goto err_netdev;

	ret = ib_register_device(&dev->base, "usb4_rdma%d", NULL);
	if (ret)
		goto err_clear_netdev;

	ret = register_netdevice_notifier(&dev->netdev_nb);
	if (ret)
		goto err_unregister_ibdev;
	dev->netdev_nb_registered = true;

	peer->ibdev = dev;
	pr_info("registered ib_device %s for peer %s route=%d-%llx using GID netdev %s\n",
		dev_name(&dev->base.dev), dev_name(&peer->svc->dev),
		peer->xd->tb->index, (unsigned long long)peer->xd->route,
		dev->netdev_name);
	return 0;

err_unregister_ibdev:
	ib_unregister_device(&dev->base);
err_clear_netdev:
	ib_device_set_netdev(&dev->base, NULL, 1);
err_netdev:
	dev_put(dev->netdev);
	dev->netdev = NULL;
	ib_dealloc_device(&dev->base);
	return ret;
}

static void ardma_unregister_ibdev(struct ardma_peer *peer)
{
	struct ardma_ibdev *dev = peer ? peer->ibdev : NULL;

	if (!dev)
		return;
	peer->ibdev = NULL;

	/* Detach before unregistering the notifier. unregister_netdevice_notifier()
	 * synthesizes NETDEV_UNREGISTER events for every current netdev; if our
	 * pointer is still armed, the synthetic event is indistinguishable from a
	 * real thunderbolt0 unregister.
	 */
	mutex_lock(&dev->netdev_lock);
	dev->shutting_down = true;
	ardma_detach_netdev_locked(dev);
	mutex_unlock(&dev->netdev_lock);

	if (dev->netdev_nb_registered) {
		unregister_netdevice_notifier(&dev->netdev_nb);
		dev->netdev_nb_registered = false;
	}
	ib_unregister_device(&dev->base);
	ib_dealloc_device(&dev->base);
}

/* ----- Thunderbolt service/rings --------------------------------- */

static bool ardma_ring_depth_valid(unsigned int depth)
{
	if (depth < ARDMA_MIN_RING_DEPTH || depth > ARDMA_MAX_RING_DEPTH)
		return false;
	if (depth & (depth - 1))
		return false;
	if (depth <= ARDMA_RING_RESERVE)
		return false;
	return true;
}

static u32 ardma_rx_frame_capacity(unsigned int depth)
{
	return depth - ARDMA_RING_RESERVE;
}

static int ardma_alloc_rx_frames_for(struct ardma_peer *peer,
				     struct tb_ring *rx_ring,
				     struct ardma_rx_frame **frames_out,
				     u32 *capacity_io, u32 qpn)
{
	struct device *dma_dev = tb_ring_dma_device(rx_ring);
	struct ardma_rx_frame *frames;
	u32 count = *capacity_io;
	u32 i;

	frames = kcalloc(count, sizeof(*frames), GFP_KERNEL);
	if (!frames)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct ardma_rx_frame *rf = &frames[i];

		rf->peer = peer;
		rf->qpn = qpn;
		rf->data = kmalloc(ARDMA_FRAME_SIZE, GFP_KERNEL);
		if (!rf->data)
			goto err;
		rf->dma = dma_map_single(dma_dev, rf->data, ARDMA_FRAME_SIZE,
					 DMA_FROM_DEVICE);
		if (dma_mapping_error(dma_dev, rf->dma)) {
			kfree(rf->data);
			rf->data = NULL;
			goto err;
		}
		rf->frame.buffer_phy = rf->dma;
		rf->frame.size = 0;
		rf->frame.callback = ardma_rx_callback;
		INIT_LIST_HEAD(&rf->frame.list);
	}
	*frames_out = frames;
	return 0;

err:
	while (i > 0) {
		struct ardma_rx_frame *rf;

		i--;
		rf = &frames[i];

		if (rf->data) {
			dma_unmap_single(dma_dev, rf->dma, ARDMA_FRAME_SIZE,
					 DMA_FROM_DEVICE);
			kfree(rf->data);
		}
	}
	kfree(frames);
	*frames_out = NULL;
	*capacity_io = 0;
	return -ENOMEM;
}

static int ardma_alloc_rx_frames(struct ardma_peer *peer)
{
	u32 qpn = (u32)receive_path << ARDMA_QPN_BASE_SHIFT;

	return ardma_alloc_rx_frames_for(peer, peer->rx_ring, &peer->rx_frames,
					 &peer->rx_frame_capacity, qpn);
}

static u32 ardma_rx_post_count_for(struct ardma_peer *peer, u32 capacity)
{
	unsigned int requested = READ_ONCE(rx_post_frames);
	u32 max = capacity;

	if (!requested || requested > max)
		return peer->rx_raw_wire ? max : min_t(u32, 64, max);
	return requested;
}

static u32 ardma_rx_post_count(struct ardma_peer *peer)
{
	return ardma_rx_post_count_for(peer, peer->rx_frame_capacity);
}

static void ardma_free_rx_frames_for(struct tb_ring *rx_ring,
				     struct ardma_rx_frame **frames_io,
				     u32 *capacity_io)
{
	struct device *dma_dev;
	struct ardma_rx_frame *frames = *frames_io;
	u32 i;

	if (!frames || !rx_ring)
		return;
	dma_dev = tb_ring_dma_device(rx_ring);
	for (i = 0; i < *capacity_io; i++) {
		struct ardma_rx_frame *rf = &frames[i];

		if (rf->data) {
			dma_unmap_single(dma_dev, rf->dma, ARDMA_FRAME_SIZE,
					 DMA_FROM_DEVICE);
			kfree(rf->data);
		}
	}
	kfree(frames);
	*frames_io = NULL;
	*capacity_io = 0;
}

static void ardma_free_rx_frames(struct ardma_peer *peer)
{
	ardma_free_rx_frames_for(peer->rx_ring, &peer->rx_frames,
				 &peer->rx_frame_capacity);
}

/* RX polling support (rx_poll_mode=1).
 *
 * NHI invokes start_poll from interrupt context whenever frames arrive on a
 * polled ring. We just kick the per-peer kthread, which then drains the ring
 * via tb_ring_poll() until empty and re-arms IRQs via tb_ring_poll_complete().
 *
 * The drain reuses ardma_rx_callback unchanged. That path runs from process
 * context here instead of softirq, so we wrap it in local_bh_disable() to keep
 * the RX-event/QP spin_lock_irqsave consumers happy and to preserve a bottom-
 * half-like execution profile for any code that might use _bh primitives.
 */
static void ardma_rx_start_poll(void *data)
{
	struct ardma_peer *peer = data;

	complete(&peer->rx_poll_kick);
}

static int ardma_rx_poll_thread(void *data)
{
	struct ardma_peer *peer = data;

	while (!kthread_should_stop() && !atomic_read(&peer->rx_poll_stop)) {
		if (wait_for_completion_interruptible(&peer->rx_poll_kick))
			continue;
		reinit_completion(&peer->rx_poll_kick);

		if (atomic_read(&peer->rx_poll_stop) || kthread_should_stop())
			break;

		local_bh_disable();
		for (;;) {
			struct ring_frame *frame = tb_ring_poll(peer->rx_ring);

			if (!frame)
				break;
			ardma_rx_callback(peer->rx_ring, frame, false);
		}
		local_bh_enable();

		tb_ring_poll_complete(peer->rx_ring);
	}
	return 0;
}

static void ardma_teardown_extra_lane(struct ardma_peer *peer,
				      struct ardma_path_lane *lane)
{
	if (lane->paths_enabled) {
		tb_xdomain_disable_paths(peer->xd, lane->local_out_hop,
					 lane->tx_ring ? lane->tx_ring->hop : -1,
					 lane->local_in_hop,
					 lane->rx_ring ? lane->rx_ring->hop : -1);
		lane->paths_enabled = false;
	}
	if (lane->rx_ring) {
		tb_ring_stop(lane->rx_ring);
		ardma_free_rx_frames_for(lane->rx_ring, &lane->rx_frames,
					 &lane->rx_frame_capacity);
		tb_ring_free(lane->rx_ring);
		lane->rx_ring = NULL;
	}
	if (lane->tx_ring) {
		if (lane->tx_ring_running)
			tb_ring_stop(lane->tx_ring);
		lane->tx_ring_running = false;
		tb_ring_free(lane->tx_ring);
		lane->tx_ring = NULL;
	}
	if (lane->local_in_hop >= 0) {
		tb_xdomain_release_in_hopid(peer->xd, lane->local_in_hop);
		lane->local_in_hop = -1;
	}
	if (lane->local_out_hop >= 0) {
		tb_xdomain_release_out_hopid(peer->xd, lane->local_out_hop);
		lane->local_out_hop = -1;
	}
	lane->rx_posted_frames = 0;
	lane->qpn = 0;
}

static int ardma_setup_extra_lane(struct ardma_peer *peer, unsigned int lane_idx,
				  unsigned int depth, unsigned int tx_ring_flags,
				  unsigned int rx_ring_flags, u16 rx_sof_mask,
				  u16 rx_eof_mask)
{
	struct tb_xdomain *xd = peer->xd;
	struct ardma_path_lane *lane = &peer->extra_lanes[lane_idx - 1];
	int path = receive_path + lane_idx;
	u32 post_count;
	u32 i;
	int ret;

	lane->local_in_hop = -1;
	lane->local_out_hop = -1;
	lane->qpn = (u32)path << ARDMA_QPN_BASE_SHIFT;

	ret = tb_xdomain_alloc_in_hopid(xd, path);
	if (ret != path) {
		if (ret >= 0) {
			tb_xdomain_release_in_hopid(xd, ret);
			ret = -EINVAL;
		}
		pr_warn("lane %u: alloc_in_hopid(%d) failed: %d\n",
			lane_idx, path, ret);
		return ret;
	}
	lane->local_in_hop = ret;

	ret = tb_xdomain_alloc_out_hopid(xd, path);
	if (ret != path) {
		if (ret >= 0) {
			tb_xdomain_release_out_hopid(xd, ret);
			ret = -EINVAL;
		}
		pr_warn("lane %u: alloc_out_hopid(%d) failed: %d\n",
			lane_idx, path, ret);
		goto err;
	}
	lane->local_out_hop = ret;

	lane->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, -1, depth,
					 tx_ring_flags);
	if (!lane->tx_ring) {
		ret = -ENOMEM;
		pr_warn("lane %u: TX ring allocation failed\n", lane_idx);
		goto err;
	}

	lane->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, -1, depth,
					 rx_ring_flags, lane->tx_ring->hop,
					 rx_sof_mask, rx_eof_mask, NULL, NULL);
	if (!lane->rx_ring) {
		ret = -ENOMEM;
		pr_warn("lane %u: RX ring allocation failed\n", lane_idx);
		goto err;
	}

	lane->rx_frame_capacity = ardma_rx_frame_capacity(depth);
	ret = ardma_alloc_rx_frames_for(peer, lane->rx_ring, &lane->rx_frames,
					&lane->rx_frame_capacity, lane->qpn);
	if (ret)
		goto err;
	post_count = ardma_rx_post_count_for(peer, lane->rx_frame_capacity);
	lane->rx_posted_frames = post_count;

	tb_ring_start(lane->tx_ring);
	lane->tx_ring_running = true;
	tb_ring_start(lane->rx_ring);

	for (i = 0; i < post_count; i++) {
		ret = tb_ring_rx(lane->rx_ring, &lane->rx_frames[i].frame);
		if (ret) {
			pr_warn("lane %u: post RX frame %u failed: %d\n",
				lane_idx, i, ret);
			goto err;
		}
	}

	ret = tb_xdomain_enable_paths(xd, lane->local_out_hop,
				      lane->tx_ring->hop, lane->local_in_hop,
				      lane->rx_ring->hop);
	if (ret) {
		pr_warn("lane %u: enable_paths failed: %d\n", lane_idx, ret);
		goto err;
	}
	lane->paths_enabled = true;
	pr_info("lane %u active: in_path=%d rx_hop=%d out_path=%d tx_hop=%d qpn=0x%x posted=%u/%u\n",
		lane_idx, lane->local_in_hop, lane->rx_ring->hop,
		lane->local_out_hop, lane->tx_ring->hop, lane->qpn,
		lane->rx_posted_frames, lane->rx_frame_capacity);
	return 0;

err:
	ardma_teardown_extra_lane(peer, lane);
	return ret;
}

static int ardma_setup_rings(struct ardma_peer *peer)
{
	struct tb_xdomain *xd = peer->xd;
	unsigned int tx_ring_flags = READ_ONCE(tx_e2e) ? RING_FLAG_E2E : 0;
	unsigned int rx_ring_flags = READ_ONCE(rx_e2e) ? RING_FLAG_E2E : 0;
	unsigned int depth = READ_ONCE(ring_depth);
	unsigned int lanes = READ_ONCE(path_lanes);
	int requested_out_hop = READ_ONCE(transmit_path);
	u16 rx_sof_mask = ARDMA_RX_RAW_SOF_MASK;
	u16 rx_eof_mask = ARDMA_RX_RAW_EOF_MASK;
	int e2e_tx_hop;
	u32 i;
	int ret;

	if (!ardma_ring_depth_valid(depth)) {
		pr_warn("invalid ring_depth=%u (must be power-of-two %u..%u)\n",
			depth, ARDMA_MIN_RING_DEPTH, ARDMA_MAX_RING_DEPTH);
		return -EINVAL;
	}
	if (!lanes)
		lanes = 1;
	if (lanes > ARDMA_MAX_PATH_LANES)
		lanes = ARDMA_MAX_PATH_LANES;
	if (lanes > 1 && READ_ONCE(rx_poll_mode)) {
		pr_warn("path_lanes=%u is not supported with rx_poll_mode=1\n",
			lanes);
		return -EOPNOTSUPP;
	}
	if (!READ_ONCE(tx_raw_mode))
		tx_ring_flags |= RING_FLAG_FRAME;
	if (!ardma_peer_rx_raw(peer)) {
		rx_ring_flags |= RING_FLAG_FRAME;
		rx_sof_mask = ARDMA_RX_FRAME_SOF_MASK;
		rx_eof_mask = ARDMA_RX_FRAME_EOF_MASK;
	}

	peer->local_in_hop = -1;
	peer->local_out_hop = -1;
	peer->reserved_out_hop = -1;

	ret = tb_xdomain_alloc_in_hopid(xd, receive_path);
	if (ret != receive_path) {
		if (ret >= 0) {
			tb_xdomain_release_in_hopid(xd, ret);
			ret = -EINVAL;
		}
		pr_warn("alloc_in_hopid(%d) failed: %d\n", receive_path, ret);
		return ret;
	}
	peer->local_in_hop = ret;

	peer->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, local_tx_hop,
					 depth, tx_ring_flags);
	if (!peer->tx_ring) {
		pr_warn("tb_ring_alloc_tx(hop=%d) failed\n", local_tx_hop);
		ret = -ENOMEM;
		goto err_in_hop;
	}
	e2e_tx_hop = peer->tx_ring->hop;

	if (lanes > 1 && requested_out_hop < 0)
		requested_out_hop = receive_path;

	if (requested_out_hop < 0 && READ_ONCE(reserve_low_out_hops)) {
		ret = tb_xdomain_alloc_out_hopid(xd, -1);
		if (ret < 0)
			goto err_tx_ring;
		peer->reserved_out_hop = ret;
		pr_info("reserved diagnostic low out_hop=%d before real TX path allocation\n",
			peer->reserved_out_hop);
	}

	ret = tb_xdomain_alloc_out_hopid(xd, requested_out_hop);
	if (ret < 0)
		goto err_tx_ring;
	if (requested_out_hop >= 0 && ret != requested_out_hop) {
		if (ret >= 0) {
			tb_xdomain_release_out_hopid(xd, ret);
			ret = -EINVAL;
		}
		pr_warn("alloc_out_hopid(%d) failed: %d\n",
			requested_out_hop, ret);
		goto err_tx_ring;
	}
	peer->local_out_hop = ret;

	if (READ_ONCE(rx_poll_mode)) {
		init_completion(&peer->rx_poll_kick);
		atomic_set(&peer->rx_poll_stop, 0);
		peer->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, local_rx_hop,
						 depth,
						 rx_ring_flags, e2e_tx_hop,
						 rx_sof_mask, rx_eof_mask,
						 ardma_rx_start_poll, peer);
	} else {
		peer->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, local_rx_hop,
						 depth,
						 rx_ring_flags, e2e_tx_hop,
						 rx_sof_mask, rx_eof_mask,
						 NULL, NULL);
	}
	if (!peer->rx_ring) {
		ret = -ENOMEM;
		goto err_out_hop;
	}

	peer->rx_frame_capacity = ardma_rx_frame_capacity(depth);
	ret = ardma_alloc_rx_frames(peer);
	if (ret)
		goto err_rx_ring;
	peer->rx_posted_frames = ardma_rx_post_count(peer);

	if (READ_ONCE(rx_poll_mode)) {
		peer->rx_poll_task = kthread_run(ardma_rx_poll_thread, peer,
						 "ardma_rxpoll/%s",
						 dev_name(&peer->svc->dev));
		if (IS_ERR(peer->rx_poll_task)) {
			ret = PTR_ERR(peer->rx_poll_task);
			peer->rx_poll_task = NULL;
			pr_warn("rx poll kthread spawn failed: %d\n", ret);
			goto err_rx_frames;
		}
		pr_info("rx_poll_mode=1: spawned ardma_rxpoll kthread\n");
	}

	tb_ring_start(peer->tx_ring);
	peer->tx_ring_running = true;
	tb_ring_start(peer->rx_ring);

	for (i = 0; i < peer->rx_posted_frames; i++) {
		ret = tb_ring_rx(peer->rx_ring, &peer->rx_frames[i].frame);
		if (ret) {
			pr_warn("post RX frame %d failed: %d\n", i, ret);
			goto err_started;
		}
	}

	if (READ_ONCE(enable_paths_on_setup)) {
		ret = tb_xdomain_enable_paths(xd, peer->local_out_hop,
					      peer->tx_ring->hop,
					      peer->local_in_hop,
					      peer->rx_ring->hop);
		if (ret) {
			pr_warn("enable_paths failed: %d\n", ret);
			goto err_started;
		}
		peer->paths_enabled = true;
	} else {
		pr_info("enable_paths_on_setup=0: skipping tb_xdomain_enable_paths\n");
	}

	pr_info("rings active: in_path=%d rx_hop=%d out_path=%d tx_hop=%d (paths_enabled=%d)\n",
		peer->local_in_hop, peer->rx_ring->hop,
		peer->local_out_hop, peer->tx_ring->hop,
		peer->paths_enabled);
	pr_info("ring_depth=%u posted %u/%u RX descriptors\n", depth,
		peer->rx_posted_frames, peer->rx_frame_capacity);

	for (i = 1; i < lanes; i++) {
		ret = ardma_setup_extra_lane(peer, i, depth, tx_ring_flags,
					     rx_ring_flags, rx_sof_mask,
					     rx_eof_mask);
		if (ret)
			goto err_extra_lanes;
		peer->extra_lane_count = i;
	}
	return 0;

err_extra_lanes:
	while (peer->extra_lane_count) {
		ardma_teardown_extra_lane(peer,
			&peer->extra_lanes[peer->extra_lane_count - 1]);
		peer->extra_lane_count--;
	}
err_started:
	if (peer->rx_poll_task) {
		atomic_set(&peer->rx_poll_stop, 1);
		complete(&peer->rx_poll_kick);
		kthread_stop(peer->rx_poll_task);
		peer->rx_poll_task = NULL;
	}
	tb_ring_stop(peer->rx_ring);
	ardma_stop_tx_ring(peer);
	ardma_free_rx_frames(peer);
	goto err_rx_ring;
err_rx_frames:
	ardma_free_rx_frames(peer);
err_rx_ring:
	tb_ring_free(peer->rx_ring);
	peer->rx_ring = NULL;
err_out_hop:
	if (peer->local_out_hop >= 0)
		tb_xdomain_release_out_hopid(xd, peer->local_out_hop);
	peer->local_out_hop = -1;
err_tx_ring:
	if (peer->reserved_out_hop >= 0) {
		tb_xdomain_release_out_hopid(xd, peer->reserved_out_hop);
		peer->reserved_out_hop = -1;
	}
	tb_ring_free(peer->tx_ring);
	peer->tx_ring = NULL;
err_in_hop:
	if (peer->local_in_hop >= 0)
		tb_xdomain_release_in_hopid(xd, peer->local_in_hop);
	peer->local_in_hop = -1;
	return ret;
}

static void ardma_teardown_rings(struct ardma_peer *peer)
{
	while (peer->extra_lane_count) {
		ardma_teardown_extra_lane(peer,
			&peer->extra_lanes[peer->extra_lane_count - 1]);
		peer->extra_lane_count--;
	}
	if (peer->paths_enabled) {
		tb_xdomain_disable_paths(peer->xd, peer->local_out_hop,
					 peer->tx_ring ? peer->tx_ring->hop : -1,
					 peer->local_in_hop,
					 peer->rx_ring ? peer->rx_ring->hop : -1);
		peer->paths_enabled = false;
	}
	if (peer->rx_poll_task) {
		atomic_set(&peer->rx_poll_stop, 1);
		complete(&peer->rx_poll_kick);
		kthread_stop(peer->rx_poll_task);
		peer->rx_poll_task = NULL;
	}
	if (peer->rx_ring) {
		tb_ring_stop(peer->rx_ring);
		ardma_free_rx_frames(peer);
		tb_ring_free(peer->rx_ring);
		peer->rx_ring = NULL;
	}
	if (peer->tx_ring) {
		ardma_stop_tx_ring(peer);
		tb_ring_free(peer->tx_ring);
		peer->tx_ring = NULL;
	}
	if (peer->local_in_hop >= 0) {
		tb_xdomain_release_in_hopid(peer->xd, peer->local_in_hop);
		peer->local_in_hop = -1;
	}
	if (peer->local_out_hop >= 0) {
		tb_xdomain_release_out_hopid(peer->xd, peer->local_out_hop);
		peer->local_out_hop = -1;
	}
	if (peer->reserved_out_hop >= 0) {
		tb_xdomain_release_out_hopid(peer->xd, peer->reserved_out_hop);
		peer->reserved_out_hop = -1;
	}
}

static int ardma_stats_show(struct seq_file *m, void *unused)
{
	struct ardma_peer *peer = m->private;

	seq_printf(m, "receive_path: %d\n", receive_path);
	seq_printf(m, "paths_enabled: %u\n", peer->paths_enabled);
	seq_printf(m, "active_uc_qps: %u\n", READ_ONCE(peer->active_uc_qps));
	seq_printf(m, "raw_rx_credits_reserved: %d\n",
		   atomic_read(&peer->raw_rx_credits_reserved));
	seq_printf(m, "raw_rx_credit_budget: %u\n",
		   ardma_peer_raw_rx_credit_budget(peer));
	seq_printf(m, "raw_rx_bytes_reserved: %lld\n",
		   (long long)atomic64_read(&peer->raw_rx_bytes_reserved));
	seq_printf(m, "raw_rx_window_bytes: %u\n",
		   READ_ONCE(raw_rx_window_bytes));
	seq_printf(m, "local_in_hop: %d\n", peer->local_in_hop);
	seq_printf(m, "rx_hop: %d\n", peer->rx_ring ? peer->rx_ring->hop : -1);
	seq_printf(m, "local_out_hop: %d\n", peer->local_out_hop);
	seq_printf(m, "reserved_out_hop: %d\n", peer->reserved_out_hop);
	seq_printf(m, "tx_hop: %d\n", peer->tx_ring ? peer->tx_ring->hop : -1);
	seq_printf(m, "tx_marker_mode: %u\n", READ_ONCE(tx_marker_mode));
	seq_printf(m, "rx_frames: %lld\n",
		   (long long)atomic64_read(&peer->rx_frame_count));
	seq_printf(m, "rx_eof0: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[0]));
	seq_printf(m, "rx_eof1: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[1]));
	seq_printf(m, "rx_eof2: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[2]));
	seq_printf(m, "rx_eof3: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[3]));
	seq_printf(m, "rx_eof_other: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof_other));
	seq_printf(m, "rx_messages: %lld\n",
		   (long long)atomic64_read(&peer->rx_messages));
	seq_printf(m, "rx_drops: %lld\n",
		   (long long)atomic64_read(&peer->rx_drops));
	seq_printf(m, "rx_bad_shape: %lld\n",
		   (long long)atomic64_read(&peer->rx_bad_shape));
	seq_printf(m, "rx_no_qp: %lld\n",
		   (long long)atomic64_read(&peer->rx_no_qp));
	seq_printf(m, "tx_frames: %lld\n",
		   (long long)atomic64_read(&peer->tx_frames));
	seq_printf(m, "tx_completions: %lld\n",
		   (long long)atomic64_read(&peer->tx_completions));
	seq_printf(m, "tx_errors: %lld\n",
		   (long long)atomic64_read(&peer->tx_errors));
	seq_printf(m, "tx_zcopy_frames: %lld\n",
		   (long long)atomic64_read(&peer->tx_zcopy_frames));
	seq_printf(m, "tx_pool_frames: %lld\n",
		   (long long)atomic64_read(&peer->tx_pool_frames));
	seq_printf(m, "tx_dynamic_frames: %lld\n",
		   (long long)atomic64_read(&peer->tx_dynamic_frames));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_stats);

static u32 ardma_ring_reg_base(const struct tb_ring *ring)
{
	return ring->is_tx ? ARDMA_NHI_REG_TX_RING_BASE :
			     ARDMA_NHI_REG_RX_RING_BASE;
}

static u32 ardma_options_reg_base(const struct tb_ring *ring)
{
	return ring->is_tx ? ARDMA_NHI_REG_TX_OPTIONS_BASE :
			     ARDMA_NHI_REG_RX_OPTIONS_BASE;
}

static void __iomem *ardma_ring_mmio(struct tb_ring *ring)
{
	if (!ring || !ring->nhi || !ring->nhi->iobase)
		return NULL;
	return ring->nhi->iobase + ardma_ring_reg_base(ring) +
	       ring->hop * ARDMA_NHI_RING_SLOT_SIZE;
}

static void __iomem *ardma_options_mmio(struct tb_ring *ring)
{
	if (!ring || !ring->nhi || !ring->nhi->iobase)
		return NULL;
	return ring->nhi->iobase + ardma_options_reg_base(ring) +
	       ring->hop * ARDMA_NHI_OPTIONS_SLOT_SIZE;
}

static void ardma_count_ring_lists(struct tb_ring *ring,
				   unsigned int *queued,
				   unsigned int *inflight)
{
	struct ring_frame *frame;

	*queued = 0;
	*inflight = 0;
	list_for_each_entry(frame, &ring->queue, list)
		(*queued)++;
	list_for_each_entry(frame, &ring->in_flight, list)
		(*inflight)++;
}

static unsigned int ardma_ring_distance(unsigned int from, unsigned int to,
					unsigned int size)
{
	if (!size)
		return 0;
	if (to >= from)
		return to - from;
	return size - from + to;
}

static void ardma_dump_ring_summary(struct seq_file *m, const char *name,
				    struct tb_ring *ring, u64 app_posted,
				    u64 app_completed, u64 app_errors)
{
	struct ardma_ring_desc *descs;
	unsigned int queued, inflight;
	void __iomem *ring_mmio;
	void __iomem *opt_mmio;
	const char *lo_name;
	const char *hi_name;
	unsigned long flags;
	u32 index = 0xffffffff;
	u32 desc_cfg = 0xffffffff;
	u32 opt[ARDMA_NHI_OPTIONS_DWORDS];
	u32 meta = 0;
	u32 time = 0;
	u32 reg_lo;
	u32 reg_hi;
	int head = 0;
	int tail = 0;
	int size = 0;
	int i;

	if (!ring) {
		seq_printf(m, "%s_summary: none\n", name);
		return;
	}

	descs = (struct ardma_ring_desc *)ring->descriptors;
	ring_mmio = ardma_ring_mmio(ring);
	opt_mmio = ardma_options_mmio(ring);
	lo_name = ring->is_tx ? "hw_tail" : "host_head";
	hi_name = ring->is_tx ? "host_head" : "hw_tail";

	spin_lock_irqsave(&ring->lock, flags);
	head = ring->head;
	tail = ring->tail;
	size = ring->size;
	ardma_count_ring_lists(ring, &queued, &inflight);

	if (ring_mmio) {
		index = ioread32(ring_mmio + 8);
		desc_cfg = ioread32(ring_mmio + 12);
	}
	for (i = 0; i < ARDMA_NHI_OPTIONS_DWORDS; i++)
		opt[i] = opt_mmio ? ioread32(opt_mmio + i * sizeof(u32)) :
				    0xffffffff;
	if (descs && size > 0) {
		meta = READ_ONCE(descs[tail].meta);
		time = READ_ONCE(descs[tail].time);
	}

	reg_lo = index & 0xffff;
	reg_hi = index >> 16;
	seq_printf(m,
		   "%s%-2d run=%u size=%d irq=%d vec=%u flags=0x%x sw_head=%d sw_tail=%d sw_queued=%u reg=0x%08x %s=%u %s=%u reg_distance=%u desc_cfg=0x%08x opt0=0x%08x opt4=0x%08x tail_desc=len:%u flags:0x%x sof:%u eof:%u time=0x%08x queued=%u inflight=%u\n",
		   ring->is_tx ? "tx" : "rx", ring->hop, ring->running,
		   size, ring->irq, ring->vector, ring->flags, head, tail,
		   ardma_ring_distance(tail, head, size), index, lo_name,
		   reg_lo, hi_name, reg_hi,
		   ardma_ring_distance(reg_lo, reg_hi, size), desc_cfg,
		   opt[0], opt[1], meta & 0xfff, meta >> ARDMA_DESC_FLAGS_SHIFT,
		   (meta >> 16) & 0xf, (meta >> 12) & 0xf, time, queued,
		   inflight);
	seq_printf(m,
		   "    app_posted=%llu app_completed=%llu app_errors=%llu opt_enable=%u opt_raw=%u opt_e2e=%u opt_no_snoop=%u opt_e2e_peer_tx_hop=%u\n",
		   (unsigned long long)app_posted,
		   (unsigned long long)app_completed,
		   (unsigned long long)app_errors,
		   !!(opt[0] & ARDMA_NHI_OPT_ENABLE),
		   !!(opt[0] & ARDMA_NHI_OPT_RAW),
		   !!(opt[0] & ARDMA_NHI_OPT_E2E_FLOW),
		   !!(opt[0] & ARDMA_NHI_OPT_NO_SNOOP),
		   (unsigned int)((opt[0] & ARDMA_NHI_OPT_E2E_HOP_MASK) >>
				  ARDMA_NHI_OPT_E2E_HOP_SHIFT));
	seq_printf(m,
		   "    options[0..31]=0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
		   opt[0], opt[1], opt[2], opt[3],
		   opt[4], opt[5], opt[6], opt[7]);
	spin_unlock_irqrestore(&ring->lock, flags);
}

static void ardma_dump_ring_state(struct seq_file *m, const char *name,
				  struct tb_ring *ring)
{
	struct ardma_ring_desc *descs;
	unsigned int queued, inflight;
	void __iomem *ring_mmio;
	void __iomem *opt_mmio;
	unsigned long flags;
	int head, tail, size;
	int i;

	if (!ring) {
		seq_printf(m, "%s: none\n\n", name);
		return;
	}

	descs = (struct ardma_ring_desc *)ring->descriptors;
	ring_mmio = ardma_ring_mmio(ring);
	opt_mmio = ardma_options_mmio(ring);

	spin_lock_irqsave(&ring->lock, flags);
	head = ring->head;
	tail = ring->tail;
	size = ring->size;
	ardma_count_ring_lists(ring, &queued, &inflight);

	seq_printf(m,
		   "%s: hop=%d is_tx=%u running=%u flags=0x%x e2e_tx_hop=%d size=%d head=%d tail=%d queued=%u inflight=%u\n",
		   name, ring->hop, ring->is_tx, ring->running, ring->flags,
		   ring->e2e_tx_hop, size, head, tail, queued, inflight);
	seq_printf(m, "%s: descriptors=%px descriptors_dma=%pad\n",
		   name, ring->descriptors, &ring->descriptors_dma);

	if (ring_mmio) {
		seq_printf(m,
			   "%s: ring_regs[0..15]=0x%08x 0x%08x 0x%08x 0x%08x\n",
			   name, ioread32(ring_mmio + 0),
			   ioread32(ring_mmio + 4), ioread32(ring_mmio + 8),
			   ioread32(ring_mmio + 12));
	} else {
		seq_printf(m, "%s: ring_regs unavailable\n", name);
	}

	if (opt_mmio) {
		seq_printf(m,
			   "%s: options[0..31]=0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
			   name, ioread32(opt_mmio + 0), ioread32(opt_mmio + 4),
			   ioread32(opt_mmio + 8), ioread32(opt_mmio + 12),
			   ioread32(opt_mmio + 16), ioread32(opt_mmio + 20),
			   ioread32(opt_mmio + 24), ioread32(opt_mmio + 28));
	} else {
		seq_printf(m, "%s: options unavailable\n", name);
	}

	if (!descs || size <= 0) {
		seq_puts(m, "\n");
		spin_unlock_irqrestore(&ring->lock, flags);
		return;
	}

	seq_printf(m, "%s: next descriptors from tail\n", name);
	for (i = 0; i < min(size, 8); i++) {
		int idx = (tail + i) % size;
		u32 meta = READ_ONCE(descs[idx].meta);
		u32 time = READ_ONCE(descs[idx].time);

		seq_printf(m,
			   "  desc[%03d]: phys=0x%016llx len=%u eof=%u sof=%u flags=0x%03x meta=0x%08x time=0x%08x%s%s\n",
			   idx, (unsigned long long)READ_ONCE(descs[idx].phys),
			   meta & 0xfff, (meta >> 12) & 0xf,
			   (meta >> 16) & 0xf, meta >> ARDMA_DESC_FLAGS_SHIFT,
			   meta, time, idx == tail ? " <tail>" : "",
			   idx == head ? " <head>" : "");
	}
	seq_puts(m, "\n");
	spin_unlock_irqrestore(&ring->lock, flags);
}

static int ardma_nhi_stall_dump_show(struct seq_file *m, void *unused)
{
	struct ardma_peer *peer = m->private;
	u64 tx_completions;
	u64 tx_errors;

	if (!peer || !peer->tx_event_log)
		return -ENODEV;

	tx_completions = atomic64_read(&peer->tx_completions);
	tx_errors = atomic64_read(&peer->tx_errors);

	seq_printf(m, "receive_path: %d\n", receive_path);
	seq_printf(m, "paths_enabled: %u\n", peer->paths_enabled);
	seq_printf(m, "active_uc_qps: %u\n", READ_ONCE(peer->active_uc_qps));
	seq_printf(m, "raw_rx_credits_reserved: %d\n",
		   atomic_read(&peer->raw_rx_credits_reserved));
	seq_printf(m, "raw_rx_credit_budget: %u\n",
		   ardma_peer_raw_rx_credit_budget(peer));
	seq_printf(m, "raw_rx_bytes_reserved: %lld\n",
		   (long long)atomic64_read(&peer->raw_rx_bytes_reserved));
	seq_printf(m, "raw_rx_window_bytes: %u\n",
		   READ_ONCE(raw_rx_window_bytes));
	seq_printf(m, "local_in_hop: %d\n", peer->local_in_hop);
	seq_printf(m, "local_out_hop: %d\n", peer->local_out_hop);
	seq_printf(m, "remote_is_apple: %u\n", peer->remote_is_apple);
	seq_printf(m, "rx_raw_wire: %u\n", peer->rx_raw_wire);
	seq_printf(m, "tx_marker_mode: %u\n", READ_ONCE(tx_marker_mode));
	seq_printf(m, "ring_depth: %zu\n", tb_ring_size(peer->rx_ring));
	seq_printf(m, "rx_posted_frames: %u\n", peer->rx_posted_frames);
	seq_printf(m, "rx_frame_capacity: %u\n", peer->rx_frame_capacity);
	seq_printf(m, "rx_frames: %lld\n",
		   (long long)atomic64_read(&peer->rx_frame_count));
	seq_printf(m, "rx_eof0: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[0]));
	seq_printf(m, "rx_eof1: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[1]));
	seq_printf(m, "rx_eof2: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[2]));
	seq_printf(m, "rx_eof3: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[3]));
	seq_printf(m, "rx_eof_other: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof_other));
	seq_printf(m, "rx_messages: %lld\n",
		   (long long)atomic64_read(&peer->rx_messages));
	seq_printf(m, "tx_frames: %lld\n",
		   (long long)atomic64_read(&peer->tx_frames));
	seq_printf(m, "tx_completions: %lld\n",
		   (long long)atomic64_read(&peer->tx_completions));
	seq_printf(m, "tx_errors: %lld\n\n",
		   (long long)atomic64_read(&peer->tx_errors));

	seq_puts(m, "# Compact NHI ring summary. TX reg low16=controller tail, high16=host producer.\n");
	seq_puts(m, "# RX reg low16=host consumer, high16=controller tail.\n");
	ardma_dump_ring_summary(m, "tx_ring", peer->tx_ring,
				atomic64_read(&peer->tx_frames),
				tx_completions, tx_errors);
	ardma_dump_ring_summary(m, "rx_ring", peer->rx_ring,
				peer->rx_posted_frames,
				atomic64_read(&peer->rx_frame_count), 0);
	seq_puts(m, "\n");

	ardma_dump_ring_state(m, "tx_ring", peer->tx_ring);
	ardma_dump_ring_state(m, "rx_ring", peer->rx_ring);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_nhi_stall_dump);

static int ardma_nhi_rings_show(struct seq_file *m, void *unused)
{
	struct ardma_peer *peer = m->private;
	u64 tx_completions;
	u64 tx_errors;

	if (!peer)
		return -ENODEV;

	tx_completions = atomic64_read(&peer->tx_completions);
	tx_errors = atomic64_read(&peer->tx_errors);

	seq_printf(m, "peer=%s receive_path=%d paths_enabled=%u remote_is_apple=%u tx_raw_mode=%u tx_e2e=%u rx_raw_wire=%u rx_poll_mode=%u tx_marker_mode=%u ring_depth=%zu rx_posted_frames=%u rx_frame_capacity=%u\n",
		   dev_name(&peer->svc->dev), receive_path, peer->paths_enabled,
		   peer->remote_is_apple, READ_ONCE(tx_raw_mode), READ_ONCE(tx_e2e),
		   peer->rx_raw_wire, READ_ONCE(rx_poll_mode),
		   READ_ONCE(tx_marker_mode), tb_ring_size(peer->rx_ring),
		   peer->rx_posted_frames, peer->rx_frame_capacity);
	seq_puts(m, "# TX reg low16=controller tail, high16=host producer.\n");
	seq_puts(m, "# RX reg low16=host consumer, high16=controller tail.\n");
	ardma_dump_ring_summary(m, "tx_ring", peer->tx_ring,
				atomic64_read(&peer->tx_frames),
				tx_completions, tx_errors);
	ardma_dump_ring_summary(m, "rx_ring", peer->rx_ring,
				peer->rx_posted_frames,
				atomic64_read(&peer->rx_frame_count), 0);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_nhi_rings);

static int ardma_nhi_all_rings_show(struct seq_file *m, void *unused)
{
	struct ardma_peer *peer = m->private;
	struct tb_nhi *nhi;
	u32 hop_count;
	int active;
	u32 i;

	if (!peer || !peer->xd || !peer->xd->tb || !peer->xd->tb->nhi)
		return -ENODEV;

	nhi = peer->xd->tb->nhi;
	hop_count = READ_ONCE(nhi->hop_count);

	seq_printf(m,
		   "peer=%s receive_path=%d paths_enabled=%u local_in_hop=%d local_out_hop=%d hop_count=%u remote_is_apple=%u tx_raw_mode=%u tx_e2e=%u rx_raw_wire=%u rx_poll_mode=%u tx_marker_mode=%u ring_depth=%zu rx_posted_frames=%u rx_frame_capacity=%u\n",
		   dev_name(&peer->svc->dev), receive_path, peer->paths_enabled,
		   peer->local_in_hop, peer->local_out_hop, hop_count,
		   peer->remote_is_apple, READ_ONCE(tx_raw_mode), READ_ONCE(tx_e2e),
		   peer->rx_raw_wire, READ_ONCE(rx_poll_mode),
		   READ_ONCE(tx_marker_mode), tb_ring_size(peer->rx_ring),
		   peer->rx_posted_frames, peer->rx_frame_capacity);
	seq_puts(m, "# All active NHI rings on this controller, including thunderbolt-net.\n");
	seq_puts(m, "# TX reg low16=controller tail, high16=host producer.\n");
	seq_puts(m, "# RX reg low16=host consumer, high16=controller tail.\n");
	seq_puts(m, "# opt0 bits: enable=31 raw=30 no_snoop=29 e2e=28 e2e_peer_tx_hop=22:12.\n\n");

	seq_puts(m, "tx_rings:\n");
	active = 0;
	for (i = 0; i < hop_count; i++) {
		struct tb_ring *ring = READ_ONCE(nhi->tx_rings[i]);
		u64 posted = 0;
		u64 completed = 0;
		u64 errors = 0;

		if (!ring)
			continue;

		if (ring == peer->tx_ring) {
			posted = atomic64_read(&peer->tx_frames);
			completed = atomic64_read(&peer->tx_completions);
			errors = atomic64_read(&peer->tx_errors);
			seq_puts(m, "  owner=apple_rdma\n");
		} else {
			seq_puts(m, "  owner=other\n");
		}
		ardma_dump_ring_summary(m, "tx", ring, posted, completed,
					errors);
		active++;
	}
	seq_printf(m, "active_tx_rings=%d\n\n", active);

	seq_puts(m, "rx_rings:\n");
	active = 0;
	for (i = 0; i < hop_count; i++) {
		struct tb_ring *ring = READ_ONCE(nhi->rx_rings[i]);
		u64 posted = 0;
		u64 completed = 0;

		if (!ring)
			continue;

		if (ring == peer->rx_ring) {
			posted = peer->rx_posted_frames;
			completed = atomic64_read(&peer->rx_frame_count);
			seq_puts(m, "  owner=apple_rdma\n");
		} else {
			seq_puts(m, "  owner=other\n");
		}
		ardma_dump_ring_summary(m, "rx", ring, posted, completed, 0);
		active++;
	}
	seq_printf(m, "active_rx_rings=%d\n", active);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_nhi_all_rings);

/*
 * Dump every NHI MMIO register that the upstream Linux NHI driver
 * names, plus a wide non-zero scan of the rest of BAR0. Used to diff
 * NHI state between "one E2E ring" vs "two E2E rings" to find AMD-
 * specific credit registers that change unexpectedly across that
 * boundary. Read-only -- no writes performed.
 *
 * NHI register offsets per linux/drivers/thunderbolt/nhi_regs.h:
 *   0x00000  TX descriptor ring base (16 B per hop)
 *   0x08000  RX descriptor ring base (16 B per hop)
 *   0x19800  TX options (32 B per hop)
 *   0x29800  RX options (32 B per hop)
 *   0x37800  RING_NOTIFY_BASE
 *   0x37808  RING_INT_CLEAR
 *   0x38200  RING_INTERRUPT_BASE
 *   0x38208  RING_INTERRUPT_MASK_CLEAR_BASE
 *   0x38c00  INT_THROTTLING_RATE
 *   0x38c40  INT_VEC_ALLOC_BASE
 *   0x39640  CAPS (low 11 bits = hop_count, bits 16-23 = version)
 *   0x39864  DMA_MISC
 *   0x39898  RESET
 *   0x39900  INMAIL_DATA
 *   0x39904  INMAIL_CMD
 *   0x3990c  OUTMAIL_CMD
 *   0x39944  FW_STS
 */
static int ardma_nhi_regs_show(struct seq_file *m, void *unused)
{
	struct ardma_peer *peer = m->private;
	struct tb_nhi *nhi;
	void __iomem *iobase;
	u32 hop_count;
	u32 i, j;

	if (!peer || !peer->xd || !peer->xd->tb || !peer->xd->tb->nhi)
		return -ENODEV;

	nhi = peer->xd->tb->nhi;
	iobase = nhi->iobase;
	hop_count = READ_ONCE(nhi->hop_count);

	if (!iobase) {
		seq_puts(m, "ERROR: nhi->iobase is NULL\n");
		return -EIO;
	}

	seq_printf(m,
		   "# AMD Strix Halo NHI register dump. peer=%s hop_count=%u\n",
		   dev_name(&peer->svc->dev), hop_count);
	seq_printf(m, "# pci=%s vendor=0x%04x device=0x%04x\n\n",
		   pci_name(nhi->pdev), nhi->pdev->vendor, nhi->pdev->device);

	/* Global named registers */
	seq_puts(m, "[global named registers]\n");
	seq_printf(m, "0x39640 CAPS                 = 0x%08x  (hop_count=%u version=0x%02x)\n",
		   ioread32(iobase + 0x39640),
		   ioread32(iobase + 0x39640) & 0x7ff,
		   (ioread32(iobase + 0x39640) >> 16) & 0xff);
	seq_printf(m, "0x37808 RING_INT_CLEAR       = 0x%08x\n",
		   ioread32(iobase + 0x37808));
	seq_printf(m, "0x38c00 INT_THROTTLING_RATE  = 0x%08x\n",
		   ioread32(iobase + 0x38c00));
	seq_printf(m, "0x39864 DMA_MISC             = 0x%08x\n",
		   ioread32(iobase + 0x39864));
	seq_printf(m, "0x39898 RESET                = 0x%08x\n",
		   ioread32(iobase + 0x39898));
	seq_printf(m, "0x39900 INMAIL_DATA          = 0x%08x\n",
		   ioread32(iobase + 0x39900));
	seq_printf(m, "0x39904 INMAIL_CMD           = 0x%08x\n",
		   ioread32(iobase + 0x39904));
	seq_printf(m, "0x3990c OUTMAIL_CMD          = 0x%08x\n",
		   ioread32(iobase + 0x3990c));
	seq_printf(m, "0x39944 FW_STS               = 0x%08x\n",
		   ioread32(iobase + 0x39944));
	seq_puts(m, "\n");

	/* Per-ring interrupt enable / notify / int-vec-alloc.
	 * Each is a bitmap with one bit per hop, packed into u32 words. */
	seq_puts(m, "[per-ring interrupt registers]\n");
	for (i = 0; i < (31 + 3 * hop_count) / 32 && i < 8; i++)
		seq_printf(m, "0x%05x RING_NOTIFY[%u]        = 0x%08x\n",
			   0x37800 + i * 4, i,
			   ioread32(iobase + 0x37800 + i * 4));
	for (i = 0; i < (31 + 2 * hop_count) / 32 && i < 8; i++) {
		seq_printf(m, "0x%05x RING_INTERRUPT[%u]     = 0x%08x\n",
			   0x38200 + i * 4, i,
			   ioread32(iobase + 0x38200 + i * 4));
		seq_printf(m, "0x%05x RING_INT_MASK_CLR[%u]  = 0x%08x\n",
			   0x38208 + i * 4, i,
			   ioread32(iobase + 0x38208 + i * 4));
	}
	for (i = 0; i < (hop_count + 7) / 8 && i < 8; i++)
		seq_printf(m, "0x%05x INT_VEC_ALLOC[%u]      = 0x%08x\n",
			   0x38c40 + i * 4, i,
			   ioread32(iobase + 0x38c40 + i * 4));
	seq_puts(m, "\n");

	/* Per-ring TX/RX descriptor pointer regs (16 bytes per hop). */
	seq_puts(m, "[per-ring TX descriptor regs (REG_TX_RING_BASE + hop*16)]\n");
	seq_puts(m, "# off    hop  phys_lo    phys_hi    tail/head   size_descs\n");
	for (i = 0; i < hop_count && i < 32; i++) {
		u32 off = 0x00000 + i * 16;
		u32 lo = ioread32(iobase + off);
		u32 hi = ioread32(iobase + off + 4);
		u32 tailhead = ioread32(iobase + off + 8);
		u32 size = ioread32(iobase + off + 12);
		if (!lo && !hi && !tailhead && !size)
			continue;
		seq_printf(m,
			   "0x%05x %2u   0x%08x 0x%08x 0x%08x  0x%08x\n",
			   off, i, lo, hi, tailhead, size);
	}
	seq_puts(m, "\n");

	seq_puts(m, "[per-ring RX descriptor regs (REG_RX_RING_BASE + hop*16)]\n");
	for (i = 0; i < hop_count && i < 32; i++) {
		u32 off = 0x08000 + i * 16;
		u32 lo = ioread32(iobase + off);
		u32 hi = ioread32(iobase + off + 4);
		u32 tailhead = ioread32(iobase + off + 8);
		u32 size = ioread32(iobase + off + 12);
		if (!lo && !hi && !tailhead && !size)
			continue;
		seq_printf(m,
			   "0x%05x %2u   0x%08x 0x%08x 0x%08x  0x%08x\n",
			   off, i, lo, hi, tailhead, size);
	}
	seq_puts(m, "\n");

	/* Per-ring TX options (32 bytes per hop, 8 dwords). */
	seq_puts(m, "[per-ring TX options (REG_TX_OPTIONS_BASE + hop*32)]\n");
	for (i = 0; i < hop_count && i < 32; i++) {
		u32 off = 0x19800 + i * 32;
		u32 dw[8];
		bool any = false;

		for (j = 0; j < 8; j++) {
			dw[j] = ioread32(iobase + off + j * 4);
			if (dw[j])
				any = true;
		}
		if (!any)
			continue;
		seq_printf(m,
			   "0x%05x hop%2u: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			   off, i, dw[0], dw[1], dw[2], dw[3],
			   dw[4], dw[5], dw[6], dw[7]);
	}
	seq_puts(m, "\n");

	seq_puts(m, "[per-ring RX options (REG_RX_OPTIONS_BASE + hop*32)]\n");
	for (i = 0; i < hop_count && i < 32; i++) {
		u32 off = 0x29800 + i * 32;
		u32 dw[8];
		bool any = false;

		for (j = 0; j < 8; j++) {
			dw[j] = ioread32(iobase + off + j * 4);
			if (dw[j])
				any = true;
		}
		if (!any)
			continue;
		seq_printf(m,
			   "0x%05x hop%2u: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			   off, i, dw[0], dw[1], dw[2], dw[3],
			   dw[4], dw[5], dw[6], dw[7]);
	}
	seq_puts(m, "\n");

	/* Wide scan of full BAR0 (0x80000 = 512 KiB on Strix Halo) for
	 * non-zero u32s, skipping the ring banks we've already named to
	 * keep output tractable. Coverage = everything outside:
	 *   0x00000-0x07FFF  TX descriptor regs
	 *   0x08000-0x0FFFF  RX descriptor regs
	 *   0x10000-0x197FF  unmapped / unused
	 *   0x19800-0x1A7FF  TX options (32 hops * 32 B)
	 *   0x29800-0x2A7FF  RX options (32 hops * 32 B)
	 * The interesting candidates for AMD-private credit registers are
	 * anywhere else with a non-zero default.
	 */
	seq_puts(m, "[wide non-zero scan: full BAR0 0x00000-0x80000, skipping known ring banks]\n");
	for (i = 0; i < 0x80000; i += 4) {
		u32 v;

		/* Skip already-dumped per-ring banks. */
		if (i < 0x10000)
			continue;
		if (i >= 0x19800 && i < 0x1a800)
			continue;
		if (i >= 0x29800 && i < 0x2a800)
			continue;

		v = ioread32(iobase + i);
		if (v)
			seq_printf(m, "0x%05x = 0x%08x\n", i, v);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_nhi_regs);

/*
 * nhi_poke debugfs writer for AMD silicon experiments. Read returns help.
 * Write commands (one command per write):
 *   "throttle_all <hex32>"   write same value to all 16 INT_THROTTLING slots
 *   "throttle_one <slot> <hex32>"  write one slot
 *   "reset_hrr"              pulse REG_RESET bit 0 (HRR). DANGEROUS.
 *   "pci_reset"              reset the backing NHI PCI function. DANGEROUS.
 *   "ring_int_clear <hex32>" write to REG_RING_INT_CLEAR (clears NOTIFY bits)
 *
 * No safety net beyond peer/iobase null checks. For experiments only.
 */
static int ardma_nhi_poke_show(struct seq_file *m, void *unused)
{
	seq_puts(m,
		 "Write commands (echo into this file):\n"
		 "  throttle_all <hex32>          write same value to all 16 throttle slots\n"
		 "  throttle_one <slot> <hex32>   write one slot (0..15)\n"
		 "  reset_hrr                     pulse REG_RESET bit 0 (DANGEROUS)\n"
		 "  pci_reset                     call pci_reset_function() on the NHI (DANGEROUS)\n"
		 "  ring_int_clear <hex32>        write REG_RING_INT_CLEAR\n");
	return 0;
}
static int ardma_nhi_poke_open(struct inode *inode, struct file *file)
{
	return single_open(file, ardma_nhi_poke_show, inode->i_private);
}
static ssize_t ardma_nhi_poke_write(struct file *file, const char __user *ubuf,
				    size_t len, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct ardma_peer *peer = m->private;
	struct tb_nhi *nhi;
	void __iomem *iobase;
	char buf[64];
	char cmd[32];
	u32 v0, v1;
	int n;

	if (!peer || !peer->xd || !peer->xd->tb || !peer->xd->tb->nhi)
		return -ENODEV;
	nhi = peer->xd->tb->nhi;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;

	v0 = v1 = 0;
	if (sscanf(buf, "%31s %x %x", cmd, &v0, &v1) >= 1) {
		/* For throttle_one, slot was meant to be decimal — re-parse */
		if (strcmp(cmd, "throttle_one") == 0)
			sscanf(buf, "%31s %u %x", cmd, &v0, &v1);
		if (strcmp(cmd, "pci_reset") == 0) {
			int ret;

			if (!nhi->pdev)
				return -ENODEV;
			pr_warn("apple_rdma: resetting NHI PCI function %s; active rings/module state are invalid after this\n",
				pci_name(nhi->pdev));
			ret = pci_reset_function(nhi->pdev);
			pr_warn("apple_rdma: pci_reset_function(%s) returned %d\n",
				pci_name(nhi->pdev), ret);
			return ret ? ret : len;
		}

		iobase = nhi->iobase;
		if (!iobase)
			return -EIO;

		if (strcmp(cmd, "throttle_all") == 0) {
			u32 slot;
			for (slot = 0; slot < 16; slot++)
				iowrite32(v0, iobase + 0x38c00 + slot * 4);
			pr_info("apple_rdma: wrote 0x%08x to all 16 throttle slots\n", v0);
			return len;
		}
		if (strcmp(cmd, "throttle_one") == 0) {
			if (v0 >= 16) return -EINVAL;
			iowrite32(v1, iobase + 0x38c00 + v0 * 4);
			pr_info("apple_rdma: wrote 0x%08x to throttle slot %u\n", v1, v0);
			return len;
		}
		if (strcmp(cmd, "reset_hrr") == 0) {
			iowrite32(1, iobase + 0x39898);
			pr_info("apple_rdma: pulsed REG_RESET HRR bit\n");
			return len;
		}
		if (strcmp(cmd, "ring_int_clear") == 0) {
			iowrite32(v0, iobase + 0x37808);
			pr_info("apple_rdma: wrote 0x%08x to RING_INT_CLEAR\n", v0);
			return len;
		}
		if (strcmp(cmd, "wr") == 0) {
			/* wr <hex_off> <hex_val> -- arbitrary u32 write */
			if (v0 >= 0x80000) return -EINVAL;
			if (v0 & 3) return -EINVAL;
			iowrite32(v1, iobase + v0);
			pr_info("apple_rdma: wrote 0x%08x to BAR0+0x%05x\n", v1, v0);
			return len;
		}
		if (strcmp(cmd, "rd") == 0) {
			/* rd <hex_off> -- arbitrary u32 read, result in dmesg */
			u32 r;
			if (v0 >= 0x80000) return -EINVAL;
			if (v0 & 3) return -EINVAL;
			r = ioread32(iobase + v0);
			pr_info("apple_rdma: read BAR0+0x%05x = 0x%08x\n", v0, r);
			return len;
		}
		if (strcmp(cmd, "or") == 0) {
			/* or <hex_off> <hex_mask> -- read-modify-write set bits */
			u32 r;
			if (v0 >= 0x80000 || (v0 & 3)) return -EINVAL;
			r = ioread32(iobase + v0);
			iowrite32(r | v1, iobase + v0);
			pr_info("apple_rdma: BAR0+0x%05x: 0x%08x |= 0x%08x -> 0x%08x\n",
				v0, r, v1, r | v1);
			return len;
		}
		if (strcmp(cmd, "and") == 0) {
			/* and <hex_off> <hex_mask> -- read-modify-write clear bits */
			u32 r;
			if (v0 >= 0x80000 || (v0 & 3)) return -EINVAL;
			r = ioread32(iobase + v0);
			iowrite32(r & v1, iobase + v0);
			pr_info("apple_rdma: BAR0+0x%05x: 0x%08x &= 0x%08x -> 0x%08x\n",
				v0, r, v1, r & v1);
			return len;
		}
	}
	(void)n;
	return -EINVAL;
}
static const struct file_operations ardma_nhi_poke_fops = {
	.owner = THIS_MODULE,
	.open = ardma_nhi_poke_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = ardma_nhi_poke_write,
};

static int ardma_rx_marker_log_show(struct seq_file *m, void *unused)
{
	struct ardma_rx_marker_log_entry *entries;
	struct ardma_peer *peer = m->private;
	unsigned long flags;
	u64 total;
	u32 count;
	u32 start;
	u32 pos;
	int i, j;

	if (!peer || !peer->rx_event_log)
		return -ENODEV;

	entries = kcalloc(ARDMA_RX_MARKER_LOG_ENTRIES, sizeof(*entries),
			  GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	spin_lock_irqsave(&peer->rx_marker_lock, flags);
	total = peer->rx_marker_seq;
	pos = peer->rx_marker_pos;
	count = min_t(u64, total, ARDMA_RX_MARKER_LOG_ENTRIES);
	start = (pos + ARDMA_RX_MARKER_LOG_ENTRIES - count) %
		ARDMA_RX_MARKER_LOG_ENTRIES;
	for (i = 0; i < count; i++)
		entries[i] = peer->rx_marker_log[
			(start + i) % ARDMA_RX_MARKER_LOG_ENTRIES];
	spin_unlock_irqrestore(&peer->rx_marker_lock, flags);

	seq_printf(m, "rx marker entries logged: %llu (showing %u)\n\n",
		   total, count);
	for (i = 0; i < count; i++) {
		struct ardma_rx_marker_log_entry *e = &entries[i];

		seq_printf(m,
			   "seq=%llu len=%u sof=%u eof=%u flags=0x%03x meta=0x%08x prefix=",
			   e->seq, e->len, e->sof, e->eof, e->flags, e->meta);
		for (j = 0; j < e->prefix_len; j++)
			seq_printf(m, "%02x%s", e->prefix[j],
				   j + 1 == e->prefix_len ? "" : " ");
		seq_putc(m, '\n');
	}

	kfree(entries);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_rx_marker_log);

static int ardma_tx_event_log_show(struct seq_file *m, void *unused)
{
	struct ardma_tx_event_entry *entries;
	struct ardma_peer *peer = m->private;
	unsigned long flags;
	u64 total;
	u32 count;
	u32 start;
	u32 pos;
	u64 prev_ns = 0;
	int i;

	if (!peer)
		return -ENODEV;

	entries = kvcalloc(ARDMA_EVENT_LOG_ENTRIES, sizeof(*entries),
			   GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	spin_lock_irqsave(&peer->tx_event_lock, flags);
	total = peer->tx_event_seq;
	pos = peer->tx_event_pos;
	count = min_t(u64, total, ARDMA_EVENT_LOG_ENTRIES);
	start = (pos + ARDMA_EVENT_LOG_ENTRIES - count) %
		ARDMA_EVENT_LOG_ENTRIES;
	for (i = 0; i < count; i++)
		entries[i] = peer->tx_event_log[
			(start + i) % ARDMA_EVENT_LOG_ENTRIES];
	spin_unlock_irqrestore(&peer->tx_event_lock, flags);

	seq_printf(m, "tx events logged: %llu (showing %u) event_log=%u\n\n",
		   (unsigned long long)total, count, READ_ONCE(event_log));
	for (i = 0; i < count; i++) {
		struct ardma_tx_event_entry *e = &entries[i];
		u64 delta_us = prev_ns ? div_u64(e->ns - prev_ns, 1000) : 0;

		seq_printf(m,
			   "seq=%llu +%lluus %s frame=%llu qpn=0x%x dest=0x%x wr_id=%llu total=%u block=%u piece=%u app_off=0x%x payload=%u wire=%u sof=%u eof=%u pending=%u ret=%d raw=%u e2e=%u marker=%u signaled=%u canceled=%u\n",
			   (unsigned long long)e->seq,
			   (unsigned long long)delta_us,
			   ardma_tx_event_name(e->type),
			   (unsigned long long)e->frame_id, e->qpn,
			   e->dest_qpn, (unsigned long long)e->wr_id,
			   e->total_len, e->block, e->piece, e->app_off,
			   e->payload_len, e->wire_len, e->sof, e->eof,
			   e->pending, e->ret, e->raw_mode, e->e2e,
			   e->marker_mode, e->signaled, e->canceled);
		prev_ns = e->ns;
	}

	kvfree(entries);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_tx_event_log);

static int ardma_rx_event_log_show(struct seq_file *m, void *unused)
{
	struct ardma_rx_event_entry *entries;
	struct ardma_peer *peer = m->private;
	unsigned long flags;
	u64 total;
	u32 count;
	u32 start;
	u32 pos;
	u64 prev_ns = 0;
	int i;

	if (!peer)
		return -ENODEV;

	entries = kvcalloc(ARDMA_EVENT_LOG_ENTRIES, sizeof(*entries),
			   GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	spin_lock_irqsave(&peer->rx_event_lock, flags);
	total = peer->rx_event_seq;
	pos = peer->rx_event_pos;
	count = min_t(u64, total, ARDMA_EVENT_LOG_ENTRIES);
	start = (pos + ARDMA_EVENT_LOG_ENTRIES - count) %
		ARDMA_EVENT_LOG_ENTRIES;
	for (i = 0; i < count; i++)
		entries[i] = peer->rx_event_log[
			(start + i) % ARDMA_EVENT_LOG_ENTRIES];
	spin_unlock_irqrestore(&peer->rx_event_lock, flags);

	seq_printf(m, "rx events logged: %llu (showing %u) event_log=%u\n\n",
		   (unsigned long long)total, count, READ_ONCE(event_log));
	for (i = 0; i < count; i++) {
		struct ardma_rx_event_entry *e = &entries[i];
		u64 delta_us = prev_ns ? div_u64(e->ns - prev_ns, 1000) : 0;

		seq_printf(m,
			   "seq=%llu +%lluus %s qpn=0x%x dest=0x%x wr_id=%llu len=%u byte_len=%u expected=%u sof=%u eof=%u status=%u state=%u->%u registered=%u recv_q=%u pending_ready=%u pending_active=%u rx_piece=%u ret=%d\n",
			   (unsigned long long)e->seq,
			   (unsigned long long)delta_us,
			   ardma_rx_event_name(e->type), e->qpn, e->dest_qpn,
			   (unsigned long long)e->wr_id, e->len,
			   e->byte_len, e->expected_len, e->sof, e->eof,
			   e->status, e->old_state, e->new_state,
			   e->registered, e->recv_q_depth, e->pending_ready,
			   e->pending_active, e->rx_piece, e->ret);
		prev_ns = e->ns;
	}

	kvfree(entries);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_rx_event_log);

/*
 * Debug helper: send an arbitrary xdomain control packet to the peer and log
 * the reply (if any). Layout mirrors drivers/net/thunderbolt/main.c
 * struct thunderbolt_ip_header so the same shape can be reused to probe
 * Apple AD/FA57 LOGIN-style exchanges without rebuilding the module.
 */
struct ardma_xdomain_ctrl_hdr {
	u32 route_hi;
	u32 route_lo;
	u32 length_sn;
	uuid_t uuid;
	uuid_t initiator_uuid;
	uuid_t target_uuid;
	u32 type;
	u32 command_id;
} __packed;

static atomic_t ardma_login_command_id = ATOMIC_INIT(0);

static int ardma_send_xdomain_probe(struct ardma_peer *peer,
				    const uuid_t *dispatcher_uuid,
				    u32 type, const u8 *body, size_t body_len,
				    size_t reply_size)
{
	struct ardma_xdomain_ctrl_hdr *hdr;
	struct tb_xdomain *xd = peer ? peer->xd : NULL;
	u8 *buf;
	u8 *reply;
	size_t total;
	u32 cmd_id;
	int ret;

	if (!xd)
		return -ENODEV;
	if (body_len > 1024)
		return -EINVAL;
	if (reply_size < sizeof(*hdr) || reply_size > 4096)
		return -EINVAL;

	total = sizeof(*hdr) + body_len;
	buf = kzalloc(total, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	reply = kzalloc(reply_size, GFP_KERNEL);
	if (!reply) {
		kfree(buf);
		return -ENOMEM;
	}

	hdr = (struct ardma_xdomain_ctrl_hdr *)buf;
	hdr->route_hi = upper_32_bits(xd->route);
	hdr->route_lo = lower_32_bits(xd->route);
	/* length_sn: low 6 bits = (size - 12)/4, sequence bits 27..28 = 0. */
	hdr->length_sn = ((u32)(total - 12) / 4) & 0x3f;
	uuid_copy(&hdr->uuid, dispatcher_uuid);
	if (xd->local_uuid)
		uuid_copy(&hdr->initiator_uuid, xd->local_uuid);
	if (xd->remote_uuid)
		uuid_copy(&hdr->target_uuid, xd->remote_uuid);
	hdr->type = type;
	cmd_id = (u32)atomic_inc_return(&ardma_login_command_id);
	hdr->command_id = cmd_id;

	if (body && body_len)
		memcpy(buf + sizeof(*hdr), body, body_len);

	pr_info("login_send -> uuid=%pUb type=0x%x cmd=0x%x route=0x%llx req=%zu rsp=%zu timeout=%ums\n",
		dispatcher_uuid, type, cmd_id,
		(unsigned long long)xd->route, total, reply_size,
		login_timeout_ms);
	print_hex_dump(KERN_INFO, "login_send req: ", DUMP_PREFIX_OFFSET,
		       16, 1, buf, min_t(size_t, total, 96), false);

	ret = tb_xdomain_request(xd, buf, total,
				 TB_CFG_PKG_XDOMAIN_RESP,
				 reply, reply_size,
				 TB_CFG_PKG_XDOMAIN_RESP,
				 login_timeout_ms);
	if (ret < 0) {
		pr_info("login_send <- error %d (likely timeout/no reply for rsp_size=%zu)\n",
			ret, reply_size);
	} else {
		const struct ardma_xdomain_ctrl_hdr *rh = (void *)reply;

		pr_info("login_send <- reply uuid=%pUb type=0x%x cmd=0x%x rsp_size=%zu (matched)\n",
			&rh->uuid, rh->type, rh->command_id, reply_size);
		print_hex_dump(KERN_INFO, "login_send rsp: ",
			       DUMP_PREFIX_OFFSET, 16, 1,
			       reply, reply_size, false);
	}

	kfree(reply);
	kfree(buf);
	return ret;
}

static ssize_t ardma_login_send_write(struct file *file,
				      const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	struct ardma_peer *peer = file->private_data;
	char *kbuf, *cur, *uuid_s, *type_s, *body_s, *rsp_s;
	uuid_t dispatcher_uuid;
	u32 type;
	u8 *body = NULL;
	size_t body_len = 0;
	size_t reply_size = 256;
	u32 rsp_arg;
	int ret;

	if (!peer)
		return -ENODEV;
	if (count == 0 || count > 4096)
		return -EINVAL;

	kbuf = memdup_user_nul(ubuf, count);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	cur = strim(kbuf);
	uuid_s = cur;
	type_s = strchr(cur, ' ');
	if (!type_s) {
		ret = -EINVAL;
		goto out;
	}
	*type_s++ = '\0';
	while (*type_s == ' ')
		type_s++;
	body_s = strchr(type_s, ' ');
	if (body_s) {
		*body_s++ = '\0';
		while (*body_s == ' ')
			body_s++;
	}
	rsp_s = body_s ? strchr(body_s, ' ') : NULL;
	if (rsp_s) {
		*rsp_s++ = '\0';
		while (*rsp_s == ' ')
			rsp_s++;
		if (kstrtou32(rsp_s, 0, &rsp_arg) < 0) {
			pr_info("login_send: bad reply size '%s'\n", rsp_s);
			ret = -EINVAL;
			goto out;
		}
		reply_size = rsp_arg;
	}

	if (uuid_parse(uuid_s, &dispatcher_uuid) < 0) {
		pr_info("login_send: bad uuid '%s'\n", uuid_s);
		ret = -EINVAL;
		goto out;
	}
	if (kstrtou32(type_s, 0, &type) < 0) {
		pr_info("login_send: bad type '%s'\n", type_s);
		ret = -EINVAL;
		goto out;
	}

	if (body_s && *body_s) {
		size_t hexlen = strlen(body_s);

		if (hexlen % 2) {
			pr_info("login_send: body hex length must be even\n");
			ret = -EINVAL;
			goto out;
		}
		body_len = hexlen / 2;
		body = kzalloc(body_len, GFP_KERNEL);
		if (!body) {
			ret = -ENOMEM;
			goto out;
		}
		if (hex2bin(body, body_s, body_len) < 0) {
			pr_info("login_send: bad body hex\n");
			ret = -EINVAL;
			goto out_body;
		}
	}

	ret = ardma_send_xdomain_probe(peer, &dispatcher_uuid, type,
				       body, body_len, reply_size);

out_body:
	kfree(body);
out:
	kfree(kbuf);
	return ret < 0 ? ret : (ssize_t)count;
}

static const struct file_operations ardma_login_send_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ardma_login_send_write,
	.llseek = noop_llseek,
};

static int ardma_ctrl_log_show(struct seq_file *m, void *unused)
{
	struct ardma_ctrl_log_entry *e;
	unsigned long flags;
	int i = 0;

	seq_printf(m, "control messages logged: %d (received total: %d)\n\n",
		   atomic_read(&ardma_ctrl_logged),
		   atomic_read(&ardma_ctrl_received));

	spin_lock_irqsave(&ardma_ctrl_lock, flags);
	list_for_each_entry(e, &ardma_ctrl_list, list) {
		seq_printf(m,
			   "==== ctrl[%d] source=%s size=%u (showing first %u bytes) ====\n",
			   i++, e->source, e->size, e->dump_len);
		seq_hex_dump(m, "  ", DUMP_PREFIX_OFFSET, 16, 1,
			     e->dump, e->dump_len, true);
		seq_puts(m, "\n");
	}
	spin_unlock_irqrestore(&ardma_ctrl_lock, flags);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_ctrl_log);

/* Diagnostic: dump everything Apple advertises in xd->remote_properties.
 * The peer's directory may contain sub-services with their own UUIDs we'd
 * need to register additional tb_protocol_handler entries for. One-shot
 * logging during probe; no behavioral change. */
static const char *ardma_property_type_name(enum tb_property_type t)
{
	switch (t) {
	case TB_PROPERTY_TYPE_UNKNOWN:   return "unknown";
	case TB_PROPERTY_TYPE_DIRECTORY: return "dir";
	case TB_PROPERTY_TYPE_DATA:      return "data";
	case TB_PROPERTY_TYPE_TEXT:      return "text";
	case TB_PROPERTY_TYPE_VALUE:     return "value";
	default:                         return "?";
	}
}

static void ardma_dump_property_dir(struct tb_property_dir *dir, int depth)
{
	struct tb_property *p;
	char indent[16];
	int n;

	if (!dir || depth > 4) {
		if (depth > 4)
			pr_info("%*s(max recursion depth reached)\n", depth * 2, "");
		return;
	}

	n = min_t(int, depth * 2, (int)sizeof(indent) - 1);
	memset(indent, ' ', n);
	indent[n] = '\0';

	if (dir->uuid)
		pr_info("%sdir uuid=%pUb\n", indent, dir->uuid);
	else
		pr_info("%sdir uuid=(none)\n", indent);

	tb_property_for_each(dir, p) {
		switch (p->type) {
		case TB_PROPERTY_TYPE_DIRECTORY:
			pr_info("%s  '%s' (dir, len=%zu)\n",
				indent, p->key, p->length);
			ardma_dump_property_dir(p->value.dir, depth + 1);
			break;
		case TB_PROPERTY_TYPE_TEXT:
			pr_info("%s  '%s' (text): \"%s\"\n",
				indent, p->key,
				p->value.text ? p->value.text : "");
			break;
		case TB_PROPERTY_TYPE_VALUE:
			pr_info("%s  '%s' (value): 0x%08x (%u)\n",
				indent, p->key, p->value.immediate,
				p->value.immediate);
			break;
		case TB_PROPERTY_TYPE_DATA:
			print_hex_dump(KERN_INFO, indent, DUMP_PREFIX_OFFSET,
				       16, 1, p->value.data,
				       min_t(size_t, p->length * 4, 64), true);
			pr_info("%s  '%s' (data, %zu dwords)\n",
				indent, p->key, p->length);
			break;
		default:
			pr_info("%s  '%s' (type=%s, len=%zu)\n",
				indent, p->key,
				ardma_property_type_name(p->type), p->length);
			break;
		}
	}
}

static void ardma_dump_remote_properties(struct tb_xdomain *xd)
{
	if (!xd) {
		pr_info("xd=NULL, no remote properties\n");
		return;
	}
	if (!xd->remote_properties) {
		pr_info("xd remote_properties=NULL (peer published no directory yet)\n");
		return;
	}
	pr_info("Apple peer remote_properties (gen=%u):\n",
		xd->remote_property_block_gen);
	ardma_dump_property_dir(xd->remote_properties, 0);
}

static const uuid_t *ardma_remote_ad_dir_uuid(struct tb_xdomain *xd)
{
	struct tb_property *p;

	if (!xd || !xd->remote_properties)
		return NULL;

	p = tb_property_find(xd->remote_properties, apple_rdma_key,
			     TB_PROPERTY_TYPE_DIRECTORY);
	if (!p || !p->value.dir || !p->value.dir->uuid)
		return NULL;

	return p->value.dir->uuid;
}

static void ardma_register_remote_ad_protocol(struct ardma_peer *peer)
{
	const uuid_t *uuid;
	int ret;

	uuid = ardma_remote_ad_dir_uuid(peer->xd);
	if (!uuid)
		return;

	if (uuid_equal(uuid, &apple_rdma_service_uuid)) {
		dev_info(&peer->svc->dev,
			 "peer AD/FA57 dir uuid %pUb matches primary handler\n",
			 uuid);
		return;
	}

	uuid_copy(&peer->remote_ad_uuid, uuid);
	INIT_LIST_HEAD(&peer->remote_ad_protocol_handler.list);
	peer->remote_ad_protocol_handler.uuid = &peer->remote_ad_uuid;
	peer->remote_ad_protocol_handler.callback = ardma_ctrl_callback;
	peer->remote_ad_protocol_handler.data = "peer_ad_dir_uuid";

	ret = tb_register_protocol_handler(&peer->remote_ad_protocol_handler);
	if (ret) {
		dev_warn(&peer->svc->dev,
			 "failed to register peer AD/FA57 ctrl handler uuid %pUb: %d\n",
			 &peer->remote_ad_uuid, ret);
		return;
	}

	peer->remote_ad_protocol_registered = true;
	dev_info(&peer->svc->dev,
		 "registered peer AD/FA57 ctrl handler uuid %pUb\n",
		 &peer->remote_ad_uuid);
}

static void ardma_unregister_remote_ad_protocol(struct ardma_peer *peer)
{
	if (!peer->remote_ad_protocol_registered)
		return;

	tb_unregister_protocol_handler(&peer->remote_ad_protocol_handler);
	peer->remote_ad_protocol_registered = false;
}

static int ardma_probe(struct tb_service *svc, const struct tb_service_id *id)
{
	struct tb_xdomain *xd = tb_service_parent(svc);
	struct ardma_peer *peer;
	int ret;

	if (!xd)
		return -ENODEV;
	if (apple_vendor_only &&
	    (!xd->vendor_name || strcmp(xd->vendor_name, "Apple Inc."))) {
		dev_info(&svc->dev,
			 "skipping non-Apple AD/FA57 peer vendor='%s'\n",
			 xd->vendor_name ? xd->vendor_name : "(null)");
		return -ENODEV;
	}
	if (peer_device_name && *peer_device_name &&
	    (!xd->device_name || strcmp(xd->device_name, peer_device_name))) {
		dev_info(&svc->dev,
			 "skipping AD/FA57 peer device='%s' wanted='%s'\n",
			 xd->device_name ? xd->device_name : "(null)",
			 peer_device_name);
		return -ENODEV;
	}
	if (peer_route && *peer_route) {
		/* Format: "<domain>-<route>" e.g. "0-2" or "1-2". Matches
		 * the XDomain device dir name under /sys/bus/thunderbolt. */
		int wanted_domain = -1;
		u64 wanted_route_v = 0;
		const char *dash = strchr(peer_route, '-');
		bool ok = false;
		if (dash) {
			char dom_buf[8] = {0};
			size_t dom_len = dash - peer_route;
			if (dom_len > 0 && dom_len < sizeof(dom_buf)) {
				memcpy(dom_buf, peer_route, dom_len);
				if (kstrtoint(dom_buf, 10, &wanted_domain) == 0 &&
				    kstrtou64(dash + 1, 16, &wanted_route_v) == 0)
					ok = true;
			}
		}
		if (!ok) {
			dev_warn(&svc->dev, "peer_route '%s' must be <domain>-<route> e.g. 1-2\n",
				 peer_route);
			return -EINVAL;
		}
		if (xd->tb->index != wanted_domain || xd->route != wanted_route_v) {
			dev_info(&svc->dev,
				 "skipping AD/FA57 peer %d-%llx wanted %d-%llx\n",
				 xd->tb->index, (unsigned long long)xd->route,
				 wanted_domain, (unsigned long long)wanted_route_v);
			return -ENODEV;
		}
	}

	peer = devm_kzalloc(&svc->dev, sizeof(*peer), GFP_KERNEL);
	if (!peer)
		return -ENOMEM;
	INIT_LIST_HEAD(&peer->peers_link);
	peer->svc = svc;
	peer->xd = xd;
	peer->remote_is_apple = ardma_xdomain_is_apple(xd);
	peer->rx_raw_wire = peer->remote_is_apple;
	refcount_set(&peer->refs, 1);
	init_waitqueue_head(&peer->ref_wait);
	mutex_init(&peer->tx_lock);
	spin_lock_init(&peer->rx_marker_lock);
	spin_lock_init(&peer->tx_event_lock);
	spin_lock_init(&peer->rx_event_lock);
	atomic64_set(&peer->tx_frame_ids, 0);
	peer->local_in_hop = -1;
	peer->local_out_hop = -1;

	peer->tx_event_log = kvcalloc(ARDMA_EVENT_LOG_ENTRIES,
				      sizeof(*peer->tx_event_log),
				      GFP_KERNEL);
	if (!peer->tx_event_log)
		return -ENOMEM;
	peer->rx_event_log = kvcalloc(ARDMA_EVENT_LOG_ENTRIES,
				      sizeof(*peer->rx_event_log),
				      GFP_KERNEL);
	if (!peer->rx_event_log) {
		kvfree(peer->tx_event_log);
		peer->tx_event_log = NULL;
		return -ENOMEM;
	}

	dev_info(&svc->dev,
		 "Apple RDMA peer route=0x%llx link_speed=%u service_uuid=%pUb vendor='%s' rx_wire=%s\n",
		 xd->route, xd->link_speed, &apple_rdma_service_uuid,
		 xd->vendor_name ? xd->vendor_name : "(null)",
		 peer->rx_raw_wire ? "RAW" : "FRAME");

	ardma_dump_remote_properties(xd);

	ret = ardma_setup_rings(peer);
	if (ret) {
		kvfree(peer->rx_event_log);
		peer->rx_event_log = NULL;
		kvfree(peer->tx_event_log);
		peer->tx_event_log = NULL;
		return ret;
	}

	ardma_register_remote_ad_protocol(peer);

	tb_service_set_drvdata(svc, peer);
	mutex_lock(&ardma_peer_lock);
	list_add_tail(&peer->peers_link, &ardma_peer_list);
	mutex_unlock(&ardma_peer_lock);

	ret = ardma_register_ibdev(peer);
	if (ret) {
		mutex_lock(&ardma_peer_lock);
		if (!list_empty(&peer->peers_link))
			list_del_init(&peer->peers_link);
		mutex_unlock(&ardma_peer_lock);
		tb_service_set_drvdata(svc, NULL);
		ardma_unregister_remote_ad_protocol(peer);
		ardma_teardown_rings(peer);
		kvfree(peer->rx_event_log);
		peer->rx_event_log = NULL;
		kvfree(peer->tx_event_log);
		peer->tx_event_log = NULL;
		return ret;
	}

	peer->debugfs_dir = debugfs_create_dir(dev_name(&svc->dev),
					       ardma_debugfs_root);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("stats", 0444, peer->debugfs_dir, peer,
				    &ardma_stats_fops);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("nhi_stall_dump", 0444, peer->debugfs_dir,
				    peer, &ardma_nhi_stall_dump_fops);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("nhi_rings", 0444, peer->debugfs_dir,
				    peer, &ardma_nhi_rings_fops);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("nhi_all_rings", 0444, peer->debugfs_dir,
				    peer, &ardma_nhi_all_rings_fops);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("nhi_regs", 0444, peer->debugfs_dir,
				    peer, &ardma_nhi_regs_fops);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("nhi_poke", 0644, peer->debugfs_dir,
				    peer, &ardma_nhi_poke_fops);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("rx_marker_log", 0444, peer->debugfs_dir,
				    peer, &ardma_rx_marker_log_fops);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("tx_event_log", 0444, peer->debugfs_dir,
				    peer, &ardma_tx_event_log_fops);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("rx_event_log", 0444, peer->debugfs_dir,
				    peer, &ardma_rx_event_log_fops);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("login_send", 0200, peer->debugfs_dir,
				    peer, &ardma_login_send_fops);

	return 0;
}

static void ardma_remove(struct tb_service *svc)
{
	struct ardma_peer *peer = tb_service_get_drvdata(svc);

	if (!peer)
		return;

	WRITE_ONCE(peer->closing, true);
	mutex_lock(&ardma_peer_lock);
	if (!list_empty(&peer->peers_link))
		list_del_init(&peer->peers_link);
	mutex_unlock(&ardma_peer_lock);
	ardma_unregister_ibdev(peer);
	ardma_unregister_remote_ad_protocol(peer);
	if (!wait_event_timeout(peer->ref_wait,
				refcount_read(&peer->refs) == 1,
				msecs_to_jiffies(5000))) {
		/* Stuck refs at module unload typically come from leaked QPs
		 * (destroy_qp's tx_force_drain_on_destroy=false path). The
		 * tx ring still holds in-flight tx_frame callbacks that will
		 * never naturally complete (Mac never returned credits). Force
		 * a ring stop here -- whole module is going away so collateral
		 * damage to other in-flight TX is irrelevant. tb_ring_stop()
		 * synchronously fires all pending callbacks with canceled=true,
		 * which decrements ctx->pending and finally drops the QP refs
		 * that hold the peer ref. */
		pr_warn("ardma_remove: peer refs=%u after 5s; force-stopping TX ring to drain stuck descriptors\n",
			refcount_read(&peer->refs));
		ardma_stop_tx_ring(peer);
		if (!wait_event_timeout(peer->ref_wait,
					refcount_read(&peer->refs) == 1,
					msecs_to_jiffies(2000)))
			pr_warn("ardma_remove: peer refs=%u still nonzero after drain; proceeding (callback-vs-free race possible)\n",
				refcount_read(&peer->refs));
	}
	ardma_teardown_rings(peer);
	debugfs_remove_recursive(peer->debugfs_dir);
	kvfree(peer->tx_event_log);
	peer->tx_event_log = NULL;
	kvfree(peer->rx_event_log);
	peer->rx_event_log = NULL;
	tb_service_set_drvdata(svc, NULL);
}

static const struct tb_service_id ardma_ids[] = {
	{
		.match_flags = TBSVC_MATCH_PROTOCOL_KEY |
			       TBSVC_MATCH_PROTOCOL_ID |
			       TBSVC_MATCH_PROTOCOL_VERSION |
			       TBSVC_MATCH_PROTOCOL_REVISION,
		.protocol_key = {
			(char)0xff, (char)0xff, (char)0xff, (char)0xff,
			(char)0xff, (char)0xff, 'A', 'D', '\0',
		},
		.protocol_id = APPLE_RDMA_PRTCID,
		.protocol_version = APPLE_RDMA_PRTCVERS,
		.protocol_revision = APPLE_RDMA_PRTCREVS,
	},
	{ },
};
MODULE_DEVICE_TABLE(tbsvc, ardma_ids);

static struct tb_service_driver ardma_service_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = ARDMA_DRV_NAME,
	},
	.probe = ardma_probe,
	.remove = ardma_remove,
	.id_table = ardma_ids,
};

/* ----- property dir / control handler ---------------------------- */

static int ardma_select_service_uuid(void)
{
	apple_rdma_service_uuid = apple_rdma_default_service_uuid;
	if (service_uuid && *service_uuid)
		return uuid_parse(service_uuid, &apple_rdma_service_uuid);
	return 0;
}

static int ardma_register_property_dir(void)
{
	struct tb_property_dir *dir;
	int ret = 0;

	if (!advertise_service)
		return 0;

	dir = tb_property_create_dir(&apple_rdma_service_uuid);
	if (!dir)
		return -ENOMEM;
	ret = ret ?: tb_property_add_immediate(dir, "prtcid",
					       APPLE_RDMA_PRTCID);
	ret = ret ?: tb_property_add_immediate(dir, "prtcvers",
					       APPLE_RDMA_PRTCVERS);
	ret = ret ?: tb_property_add_immediate(dir, "prtcrevs",
					       APPLE_RDMA_PRTCREVS);
	ret = ret ?: tb_property_add_immediate(dir, "prtcstns",
					       advertise_prtcstns);
	ret = ret ?: tb_property_add_immediate(dir, apple_rdma_ca_key, 1);
	if (ret) {
		tb_property_free_dir(dir);
		return ret;
	}
	ret = tb_register_property_dir(apple_rdma_key, dir);
	if (ret) {
		tb_property_free_dir(dir);
		return ret;
	}
	ardma_property_dir = dir;
	pr_info("advertising AD/FA57 service uuid %pUb prtcstns=0x%x\n",
		&apple_rdma_service_uuid, advertise_prtcstns);
	return 0;
}

static void ardma_unregister_property_dir(void)
{
	if (!ardma_property_dir)
		return;
	tb_unregister_property_dir(apple_rdma_key, ardma_property_dir);
	tb_property_free_dir(ardma_property_dir);
	ardma_property_dir = NULL;
}

static int ardma_ctrl_callback(const void *buf, size_t size, void *data)
{
	const char *source = data ? data : "primary_service_uuid";
	int logged;

	atomic_inc(&ardma_ctrl_received);

	logged = atomic_read(&ardma_ctrl_logged);
	if (logged < ARDMA_MAX_CTRL_MSGS) {
		struct ardma_ctrl_log_entry *e;

		e = kmalloc(sizeof(*e), GFP_ATOMIC);
		if (e) {
			unsigned long flags;

			e->size = size;
			e->dump_len = min_t(u32, size, ARDMA_MAX_CTRL_BYTES);
			strscpy(e->source, source, sizeof(e->source));
			memcpy(e->dump, buf, e->dump_len);

			spin_lock_irqsave(&ardma_ctrl_lock, flags);
			list_add_tail(&e->list, &ardma_ctrl_list);
			spin_unlock_irqrestore(&ardma_ctrl_lock, flags);

			atomic_inc(&ardma_ctrl_logged);
			pr_info("ctrl[%d]: source=%s size=%zu for AD/FA57 uuid\n",
				logged, source, size);
		}
	}

	return 1;
}

static void ardma_free_ctrl_log(void)
{
	struct ardma_ctrl_log_entry *e, *tmp;
	unsigned long flags;
	LIST_HEAD(free_list);

	spin_lock_irqsave(&ardma_ctrl_lock, flags);
	list_splice_init(&ardma_ctrl_list, &free_list);
	spin_unlock_irqrestore(&ardma_ctrl_lock, flags);

	list_for_each_entry_safe(e, tmp, &free_list, list) {
		list_del(&e->list);
		kfree(e);
	}
}

/* ----- module init/exit ------------------------------------------ */

static int __init ardma_init(void)
{
	int ret;

	ret = ardma_select_service_uuid();
	if (ret) {
		pr_err("invalid service_uuid '%s'\n", service_uuid);
		return ret;
	}
	if (receive_path <= 0 || receive_path > (ARDMA_QPN_MAX >> 8)) {
		pr_err("invalid receive_path %d for Apple-shaped QPNs\n",
		       receive_path);
		return -EINVAL;
	}
	if (!path_lanes)
		path_lanes = 1;
	if (path_lanes > ARDMA_MAX_PATH_LANES)
		path_lanes = ARDMA_MAX_PATH_LANES;
	if (receive_path + path_lanes - 1 > (ARDMA_QPN_MAX >> 8)) {
		pr_err("receive_path=%d path_lanes=%u exceeds Apple-shaped QPN range\n",
		       receive_path, path_lanes);
		return -EINVAL;
	}

	ardma_debugfs_root = debugfs_create_dir(ARDMA_DRV_NAME, NULL);
	if (IS_ERR(ardma_debugfs_root))
		ardma_debugfs_root = NULL;
	if (ardma_debugfs_root)
		debugfs_create_file("ctrl_log", 0444, ardma_debugfs_root, NULL,
				    &ardma_ctrl_log_fops);

	ret = ardma_register_property_dir();
	if (ret)
		goto err_debugfs;

	INIT_LIST_HEAD(&ardma_protocol_handler.list);
	ardma_protocol_handler.uuid = &apple_rdma_service_uuid;
	ardma_protocol_handler.callback = ardma_ctrl_callback;
	ardma_protocol_handler.data = "primary_service_uuid";
	ret = tb_register_protocol_handler(&ardma_protocol_handler);
	if (ret)
		goto err_property;

	ret = tb_register_service_driver(&ardma_service_driver);
	if (ret)
		goto err_protocol;

	pr_info("loaded, matching Apple AD/FA57 receive_path=%d\n",
		receive_path);
	return 0;

err_protocol:
	tb_unregister_protocol_handler(&ardma_protocol_handler);
err_property:
	ardma_unregister_property_dir();
err_debugfs:
	debugfs_remove_recursive(ardma_debugfs_root);
	return ret;
}

static void __exit ardma_exit(void)
{
	tb_unregister_service_driver(&ardma_service_driver);
	tb_unregister_protocol_handler(&ardma_protocol_handler);
	ardma_unregister_property_dir();
	ardma_free_ctrl_log();
	debugfs_remove_recursive(ardma_debugfs_root);
	ida_destroy(&ardma_qpn_slots);
	pr_info("unloaded\n");
}

module_init(ardma_init);
module_exit(ardma_exit);

MODULE_AUTHOR("usb4-rdma project");
MODULE_DESCRIPTION("Experimental Apple RDMA-over-Thunderbolt verbs peer");
MODULE_LICENSE("GPL v2");
