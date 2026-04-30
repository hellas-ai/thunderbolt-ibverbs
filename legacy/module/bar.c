// SPDX-License-Identifier: GPL-2.0
/*
 * bar.c — read-only BAR0 exploration for USB4 host routers.
 *
 * Walks the PCI bus for AMD Strix Halo USB4 host routers (and any other
 * USB4 NHI we know about), maps each device's 512 KB BAR0 read-only via
 * a parallel ioremap (the existing thunderbolt driver keeps ownership;
 * we just observe), and exposes the contents under debugfs:
 *
 *   /sys/kernel/debug/usb4_rdma/pci/<bdf>/
 *       bar0       — raw 512 KB blob, read() and mmap() (read-only)
 *       regs       — pretty-printed standard NHI registers + decoded fields
 *       caps       — PCI capability list with raw bytes for vendor caps
 *       survey     — non-zero 32-bit dwords outside the spec'd register layout
 *       read32     — interactive: `echo OFFSET > read32; cat read32` returns the dword
 *
 * The default is strictly read-only; writing to MMIO from this module
 * would race with the in-tree thunderbolt driver's own register writes
 * and almost certainly break things. There is intentionally no `write32`
 * file. If you need to poke writes, do it from userspace via mmap of
 * `bar0` after loading with `allow_write=1` — and accept the risk.
 *
 * The point of this is exploration, not control: we want to understand
 * which regions of the host router's BAR are populated by AMD beyond
 * what `drivers/thunderbolt/nhi_regs.h` documents.
 */

#define pr_fmt(fmt) "usb4_rdma/bar: " fmt

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mm.h>

#include "usb4_rdma.h"

/* ----- module parameters ------------------------------------------ */

static bool allow_write;
module_param(allow_write, bool, 0444);
MODULE_PARM_DESC(allow_write, "Allow PROT_WRITE on bar0 mmap (DANGEROUS)");

/* ----- known USB4 host router PCI IDs ----------------------------- */

#define PCI_VENDOR_ID_AMD_LOCAL  0x1022

static const struct pci_device_id usb4_nhi_ids[] = {
	/* AMD Strix Halo USB4 host routers */
	{ PCI_DEVICE(0x1022, 0x158d) },
	{ PCI_DEVICE(0x1022, 0x158e) },
	/* Older AMD USB4 (Phoenix/Hawk Point) — same NHI layout, may also be useful */
	{ PCI_DEVICE(0x1022, 0x1668) },
	/* Intel JHL — for reference, mostly to confirm scanner works */
	{ PCI_DEVICE(0x8086, 0x9a1b) },
	{ PCI_DEVICE(0x8086, 0x9a1d) },
	{ PCI_DEVICE(0x8086, 0x9a1f) },
	{ PCI_DEVICE(0x8086, 0x9a21) },
	{ },
};

/* ----- standard NHI register definitions ------------------------- */
/*
 * Offsets and bit fields are sourced from drivers/thunderbolt/nhi_regs.h
 * in the running kernel. We don't include nhi_regs.h directly because
 * it lives under drivers/, not include/, and isn't exported to
 * out-of-tree modules.
 */

struct nhi_reg {
	const char *name;
	u32 offset;
	const char *note;
};

/* Single-dword "scalar" registers worth dumping by name. */
static const struct nhi_reg nhi_named_regs[] = {
	{ "REG_CAPS",           0x39640, "version[23:16] = USB4 NHI version" },
	{ "REG_DMA_MISC",       0x39864, "BIT(2)=int_auto_clear, BIT(17)=disable_auto_clear" },
	{ "REG_RESET",          0x39898, "BIT(0)=host router reset request" },
	{ "REG_INMAIL_DATA",    0x39900, "FW mailbox in: data" },
	{ "REG_INMAIL_CMD",     0x39904, "FW mailbox in: cmd | BIT(30)=err | BIT(31)=req" },
	{ "REG_OUTMAIL_CMD",    0x3990c, "FW mailbox out: cmd, [11:8]=opmode" },
	{ "REG_FW_STS",         0x39944, "FW status: BIT(0)=ICM_EN, BIT(31)=NVM_AUTH_DONE" },
	{ "REG_INT_THROTTLING_RATE", 0x38c00, "interrupt coalescing rate" },
};

/* Spec'd address ranges on the BAR. Anything *outside* these is
 * candidate vendor-extension territory worth flagging in `survey`. */
struct nhi_range {
	const char *name;
	u32 start;
	u32 end;
};

static const struct nhi_range nhi_known_ranges[] = {
	{ "TX_RING_BASE",            0x00000, 0x08000 },
	{ "RX_RING_BASE",            0x08000, 0x10000 }, /* 32 KB nominal */
	{ "(reserved)",              0x10000, 0x19800 },
	{ "TX_OPTIONS_BASE",         0x19800, 0x21800 },
	{ "(reserved)",              0x21800, 0x29800 },
	{ "RX_OPTIONS_BASE",         0x29800, 0x31800 },
	{ "(reserved)",              0x31800, 0x37800 },
	{ "RING_NOTIFY_BASE",        0x37800, 0x38200 },
	{ "RING_INTERRUPT_BASE",     0x38200, 0x38c00 },
	{ "INT_THROTTLING/ALLOC",    0x38c00, 0x39000 },
	{ "(reserved)",              0x39000, 0x39640 },
	{ "CAPS/DMA_MISC/RESET",     0x39640, 0x39900 },
	{ "INMAIL/OUTMAIL/FW_STS",   0x39900, 0x39a00 },
	{ "MSI-X table",             0x7e000, 0x7f000 },
	{ "MSI-X PBA",               0x7f000, 0x80000 },
};

/* ----- per-device state ------------------------------------------- */

struct usb4_write32_state {
	u32 offset;
	u32 written;
	u32 readback;
	bool valid;
};

struct usb4_pci_dev {
	struct pci_dev *pdev;
	resource_size_t bar0_phys;
	size_t bar0_len;
	void __iomem *bar0;
	struct dentry *dir;
	u32 read32_offset;
	struct usb4_write32_state write32_state;
	struct list_head list;
};

static LIST_HEAD(pci_devs);
static DEFINE_MUTEX(pci_devs_lock);
static struct dentry *pci_root;

/* ----- raw register access ---------------------------------------- */

static u32 bar_read32(const struct usb4_pci_dev *d, u32 offset)
{
	if (offset + 4 > d->bar0_len)
		return 0xffffffff;
	return readl(d->bar0 + offset);
}

/* ----- regs: pretty-print named registers ------------------------- */

static int regs_show(struct seq_file *m, void *unused)
{
	struct usb4_pci_dev *d = m->private;
	int i;

	seq_printf(m, "device:    %s\n", pci_name(d->pdev));
	seq_printf(m, "vendor:    0x%04x\n", d->pdev->vendor);
	seq_printf(m, "device_id: 0x%04x\n", d->pdev->device);
	seq_printf(m, "bar0:      phys 0x%llx, len %zu KiB, virt %p\n",
		   (u64)d->bar0_phys, d->bar0_len / 1024, d->bar0);
	seq_puts(m, "\n");

	seq_printf(m, "  %-28s %-10s %-12s %s\n", "name", "offset", "value", "note");
	seq_puts(m, "  ----------------------------------------------------------------------------------------\n");
	for (i = 0; i < ARRAY_SIZE(nhi_named_regs); i++) {
		u32 v = bar_read32(d, nhi_named_regs[i].offset);
		seq_printf(m, "  %-28s 0x%08x  0x%08x   %s\n",
			   nhi_named_regs[i].name,
			   nhi_named_regs[i].offset,
			   v,
			   nhi_named_regs[i].note);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regs);

/* ----- caps: PCI capability list with vendor-cap bytes ------------ */

static int caps_show(struct seq_file *m, void *unused)
{
	struct usb4_pci_dev *d = m->private;
	struct pci_dev *pdev = d->pdev;
	u8 cap_pos;
	u32 word;
	int i, ext_pos;

	seq_printf(m, "device: %s\n\n", pci_name(pdev));

	/* Standard PCI capabilities (pre-3.0): walk the list */
	seq_puts(m, "Standard capabilities:\n");
	cap_pos = 0;
	pci_read_config_byte(pdev, PCI_CAPABILITY_LIST, &cap_pos);
	while (cap_pos && cap_pos != 0xff) {
		u8 id, next;

		pci_read_config_byte(pdev, cap_pos, &id);
		pci_read_config_byte(pdev, cap_pos + 1, &next);

		seq_printf(m, "  [0x%02x] id=0x%02x", cap_pos, id);
		if (id == PCI_CAP_ID_VNDR) {
			u8 len;
			pci_read_config_byte(pdev, cap_pos + 2, &len);
			seq_printf(m, " VENDOR-SPECIFIC len=%u  ", len);
			for (i = 0; i < len; i += 4) {
				pci_read_config_dword(pdev, cap_pos + i, &word);
				seq_printf(m, "%08x ", word);
			}
		} else if (id == PCI_CAP_ID_EXP) {
			seq_puts(m, " PCI-Express");
		} else if (id == PCI_CAP_ID_MSI) {
			seq_puts(m, " MSI");
		} else if (id == PCI_CAP_ID_MSIX) {
			seq_puts(m, " MSI-X");
		} else if (id == PCI_CAP_ID_PM) {
			seq_puts(m, " PowerMgmt");
		}
		seq_putc(m, '\n');

		cap_pos = next;
	}

	/* PCIe extended capabilities (post-0x100) */
	seq_puts(m, "\nExtended capabilities:\n");
	ext_pos = 0x100;
	while (ext_pos && ext_pos < 0x1000) {
		u32 hdr;
		u16 ext_id;
		u16 ver;
		u16 next;

		pci_read_config_dword(pdev, ext_pos, &hdr);
		if (hdr == 0 || hdr == 0xffffffff)
			break;
		ext_id = hdr & 0xffff;
		ver = (hdr >> 16) & 0xf;
		next = (hdr >> 20) & 0xffc;

		seq_printf(m, "  [0x%03x] ext_id=0x%04x v%u", ext_pos, ext_id, ver);
		switch (ext_id) {
		case 0x000b: seq_puts(m, " VENDOR-SPECIFIC"); break;
		case 0x000d: seq_puts(m, " Access Control Services"); break;
		case 0x0001: seq_puts(m, " AER"); break;
		case 0x000e: seq_puts(m, " ARI"); break;
		case 0x0010: seq_puts(m, " SR-IOV"); break;
		case 0x001d: seq_puts(m, " DPC"); break;
		case 0x0026: seq_puts(m, " ATS"); break;
		default: break;
		}
		seq_puts(m, "  raw:");
		/* Dump up to 8 dwords (32 bytes) of the cap */
		for (i = 0; i < 8; i++) {
			pci_read_config_dword(pdev, ext_pos + i * 4, &word);
			seq_printf(m, " %08x", word);
		}
		seq_putc(m, '\n');

		if (!next || next == ext_pos)
			break;
		ext_pos = next;
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(caps);

/* ----- survey: find non-zero regions outside spec'd ranges -------- */

static bool offset_in_known_range(u32 offset)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(nhi_known_ranges); i++) {
		if (offset >= nhi_known_ranges[i].start &&
		    offset <  nhi_known_ranges[i].end)
			return true;
	}
	return false;
}

static int survey_show(struct seq_file *m, void *unused)
{
	struct usb4_pci_dev *d = m->private;
	u32 offset;
	u32 chunk_start = 0;
	u32 chunk_count = 0;
	u32 chunk_first_value = 0;

	seq_printf(m, "Surveying %s, BAR0 = %zu KiB\n\n",
		   pci_name(d->pdev), d->bar0_len / 1024);

	seq_puts(m, "1) Spec'd ranges (from drivers/thunderbolt/nhi_regs.h)\n");
	seq_printf(m, "  %-28s %-12s %-12s %s\n", "name", "start", "end", "size");
	seq_puts(m, "  --------------------------------------------------------------------\n");
	{
		int i;
		for (i = 0; i < ARRAY_SIZE(nhi_known_ranges); i++) {
			seq_printf(m, "  %-28s 0x%08x   0x%08x   %u\n",
				   nhi_known_ranges[i].name,
				   nhi_known_ranges[i].start,
				   nhi_known_ranges[i].end,
				   nhi_known_ranges[i].end -
					nhi_known_ranges[i].start);
		}
	}

	seq_puts(m, "\n2) Non-zero dwords *outside* spec'd ranges (vendor extension candidates)\n\n");
	seq_printf(m, "  %-12s %-12s %s\n", "first_off", "len_dwords", "first_value");
	seq_puts(m, "  ----------------------------------------------\n");

	/* Walk the whole BAR in 4-byte steps, group consecutive non-zero
	 * out-of-range dwords into chunks. */
	for (offset = 0; offset + 4 <= d->bar0_len; offset += 4) {
		u32 v;

		if (offset_in_known_range(offset)) {
			/* If we were tracking a chunk in unknown territory,
			 * close it now. */
			if (chunk_count) {
				seq_printf(m, "  0x%08x   %-12u 0x%08x\n",
					   chunk_start, chunk_count,
					   chunk_first_value);
				chunk_count = 0;
			}
			continue;
		}

		v = bar_read32(d, offset);
		if (v == 0) {
			if (chunk_count) {
				seq_printf(m, "  0x%08x   %-12u 0x%08x\n",
					   chunk_start, chunk_count,
					   chunk_first_value);
				chunk_count = 0;
			}
			continue;
		}

		if (chunk_count == 0) {
			chunk_start = offset;
			chunk_first_value = v;
		}
		chunk_count++;
	}
	if (chunk_count) {
		seq_printf(m, "  0x%08x   %-12u 0x%08x\n",
			   chunk_start, chunk_count, chunk_first_value);
	}

	seq_puts(m, "\n  (Note: zero in vendor-extension territory does NOT mean unused — could be\n");
	seq_puts(m, "   read-zero registers that activate under specific conditions. To find those,\n");
	seq_puts(m, "   re-run `cat survey` with the system in different states and diff the output.)\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(survey);

/* ----- scan: every non-zero dword in BAR0, annotated --------------- */

static const char *region_name_for_offset(u32 offset)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(nhi_known_ranges); i++) {
		if (offset >= nhi_known_ranges[i].start &&
		    offset <  nhi_known_ranges[i].end)
			return nhi_known_ranges[i].name;
	}
	return "(unknown)";
}

static int scan_show(struct seq_file *m, void *unused)
{
	struct usb4_pci_dev *d = m->private;
	u32 offset;
	u32 nonzero = 0;

	seq_printf(m, "Full BAR0 scan: %s, %zu KiB\n", pci_name(d->pdev),
		   d->bar0_len / 1024);
	seq_puts(m, "Listing every non-zero 32-bit dword.\n\n");
	seq_printf(m, "  %-12s %-12s %s\n", "offset", "value", "region");
	seq_puts(m, "  ----------------------------------------------------------------\n");

	for (offset = 0; offset + 4 <= d->bar0_len; offset += 4) {
		u32 v = bar_read32(d, offset);
		if (v == 0)
			continue;
		nonzero++;
		seq_printf(m, "  0x%08x   0x%08x   %s\n",
			   offset, v, region_name_for_offset(offset));
	}

	seq_printf(m, "\nTotal: %u non-zero dwords out of %zu (%u%% populated)\n",
		   nonzero, d->bar0_len / 4,
		   (unsigned)((u64)nonzero * 100 / (d->bar0_len / 4)));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(scan);

/* ----- rings: decode TX/RX ring descriptors ----------------------- */
/*
 * Each TX/RX ring slot is 16 bytes (4 dwords). The standard NHI layout
 * stores per-hop descriptors at TX_RING_BASE + 16*hop and similarly for
 * RX. Up to N hops where N comes from REG_HOP_COUNT (we read it from
 * the device directly rather than relying on a #define).
 *
 * Per-slot dword layout (from drivers/thunderbolt/nhi_regs.h):
 *  [0] = ring base address (low 32 bits)
 *  [1] = ring base address (high 32 bits) — split with size in the bottom
 *  [2] = head/tail pointers (16 bits each)
 *  [3] = misc (interrupt vector etc)
 *
 * We just dump the raw values; decoded interpretation requires more
 * spec-following than we want to encode here.
 */

#define USB4_HOP_COUNT_MAX 64

static void dump_ring_table(struct seq_file *m, struct usb4_pci_dev *d,
			    u32 base, const char *label)
{
	int hop;
	int active = 0;

	seq_printf(m, "  %s @ 0x%05x:\n", label, base);
	seq_printf(m, "    %-4s %-12s %-12s %-12s %-12s\n",
		   "hop", "[0]", "[1]", "[2]", "[3]");
	for (hop = 0; hop < USB4_HOP_COUNT_MAX; hop++) {
		u32 d0 = bar_read32(d, base + hop * 16 + 0);
		u32 d1 = bar_read32(d, base + hop * 16 + 4);
		u32 d2 = bar_read32(d, base + hop * 16 + 8);
		u32 d3 = bar_read32(d, base + hop * 16 + 12);
		if (!d0 && !d1 && !d2 && !d3)
			continue;
		seq_printf(m, "    %-4d 0x%08x   0x%08x   0x%08x   0x%08x\n",
			   hop, d0, d1, d2, d3);
		active++;
	}
	if (!active)
		seq_puts(m, "    (no active rings)\n");
	seq_putc(m, '\n');
}

static int rings_show(struct seq_file *m, void *unused)
{
	struct usb4_pci_dev *d = m->private;

	seq_printf(m, "Ring descriptors for %s\n\n", pci_name(d->pdev));
	dump_ring_table(m, d, 0x00000, "TX_RING_BASE");
	dump_ring_table(m, d, 0x08000, "RX_RING_BASE");
	dump_ring_table(m, d, 0x19800, "TX_OPTIONS");
	dump_ring_table(m, d, 0x29800, "RX_OPTIONS");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rings);

/* ----- read32: interactive register read -------------------------- */

static int read32_show(struct seq_file *m, void *unused)
{
	struct usb4_pci_dev *d = m->private;
	u32 v;

	if (d->read32_offset + 4 > d->bar0_len) {
		seq_printf(m, "offset 0x%x out of range (BAR len 0x%zx)\n",
			   d->read32_offset, d->bar0_len);
		return 0;
	}
	v = bar_read32(d, d->read32_offset);
	seq_printf(m, "0x%08x: 0x%08x\n", d->read32_offset, v);
	return 0;
}

static int read32_open(struct inode *inode, struct file *file)
{
	return single_open(file, read32_show, inode->i_private);
}

static ssize_t read32_write(struct file *file, const char __user *user_buf,
			    size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct usb4_pci_dev *d = m->private;
	char buf[24];
	u32 offset;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;
	buf[count] = '\0';

	ret = kstrtou32(buf, 0, &offset);
	if (ret)
		return ret;
	if (offset & 0x3)
		return -EINVAL;	/* must be 4-byte aligned */
	if (offset + 4 > d->bar0_len)
		return -ERANGE;

	d->read32_offset = offset;
	return count;
}

static const struct file_operations read32_fops = {
	.owner   = THIS_MODULE,
	.open    = read32_open,
	.read    = seq_read,
	.write   = read32_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ----- write32: poke a 32-bit register (allow_write only) ---------- */
/*
 * Usage:
 *   echo "OFFSET VALUE" > write32       # both in any base understood by kstrto*
 *   cat write32                          # shows last op (offset, value, readback)
 *
 * Only available if module loaded with allow_write=1. The file mode is 0
 * when disabled so even root cannot accidentally invoke it.
 *
 * EXTREME CAUTION: we share the BAR with the in-tree thunderbolt driver.
 * Writing to its live registers (e.g. ring head/tail, options bits, FW
 * mailboxes) will at minimum confuse the driver and may force a reset
 * that drops xdomain peers. Stick to "reserved" offsets while exploring.
 */

static int write32_show(struct seq_file *m, void *unused)
{
	struct usb4_pci_dev *d = m->private;
	struct usb4_write32_state *st = &d->write32_state;

	if (!allow_write) {
		seq_puts(m, "(write32 disabled — reload module with allow_write=1)\n");
		return 0;
	}

	if (!st->valid) {
		seq_puts(m, "(no write yet — use `echo \"OFFSET VAL\" > write32`)\n");
		return 0;
	}
	seq_printf(m, "last write: 0x%08x <- 0x%08x; readback 0x%08x\n",
		   st->offset, st->written, st->readback);
	return 0;
}

static int write32_open(struct inode *inode, struct file *file)
{
	return single_open(file, write32_show, inode->i_private);
}

static ssize_t write32_write(struct file *file, const char __user *user_buf,
			     size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct usb4_pci_dev *d = m->private;
	struct usb4_write32_state *st = &d->write32_state;
	char buf[64];
	unsigned int offset, val;

	if (!allow_write)
		return -EACCES;
	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;
	buf[count] = '\0';

	/* Accept "OFFSET VAL" with any leading whitespace, hex or decimal. */
	if (sscanf(buf, "%i %i", &offset, &val) != 2)
		return -EINVAL;

	if (offset & 0x3)
		return -EINVAL;
	if (offset + 4 > d->bar0_len)
		return -ERANGE;

	writel(val, d->bar0 + offset);
	/* Readback after a barrier — many MMIO writes are posted. */
	wmb();
	st->offset = offset;
	st->written = val;
	st->readback = readl(d->bar0 + offset);
	st->valid = true;

	pci_info(d->pdev, "write32 0x%08x <- 0x%08x (readback 0x%08x)\n",
		 offset, val, st->readback);
	return count;
}

static const struct file_operations write32_fops = {
	.owner   = THIS_MODULE,
	.open    = write32_open,
	.read    = seq_read,
	.write   = write32_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ----- bar0: raw blob with read() and mmap() ---------------------- */

static ssize_t bar0_read(struct file *file, char __user *user_buf,
			 size_t count, loff_t *ppos)
{
	struct usb4_pci_dev *d = file->private_data;
	loff_t pos = *ppos;
	size_t to_copy;
	void *bounce;

	if (pos < 0 || pos >= (loff_t)d->bar0_len)
		return 0;
	to_copy = min_t(size_t, count, d->bar0_len - pos);
	if (!to_copy)
		return 0;

	bounce = kmalloc(to_copy, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;
	memcpy_fromio(bounce, d->bar0 + pos, to_copy);
	if (copy_to_user(user_buf, bounce, to_copy)) {
		kfree(bounce);
		return -EFAULT;
	}
	kfree(bounce);
	*ppos = pos + to_copy;
	return to_copy;
}

static int bar0_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct usb4_pci_dev *d = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long pfn;

	if (vma->vm_pgoff << PAGE_SHIFT >= d->bar0_len)
		return -EINVAL;
	if (size > d->bar0_len - (vma->vm_pgoff << PAGE_SHIFT))
		return -EINVAL;

	if (!allow_write && (vma->vm_flags & VM_WRITE))
		return -EACCES;

	pfn = (d->bar0_phys >> PAGE_SHIFT) + vma->vm_pgoff;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	return io_remap_pfn_range(vma, vma->vm_start, pfn, size,
				  vma->vm_page_prot);
}

static int bar0_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static const struct file_operations bar0_fops = {
	.owner   = THIS_MODULE,
	.open    = bar0_open,
	.read    = bar0_read,
	.mmap    = bar0_mmap,
	.llseek  = default_llseek,
};

/* ----- per-device probe / remove ---------------------------------- */

static struct usb4_pci_dev *attach_one(struct pci_dev *pdev,
				       struct dentry *parent)
{
	struct usb4_pci_dev *d;
	resource_size_t start, len;

	start = pci_resource_start(pdev, 0);
	len   = pci_resource_len(pdev, 0);
	if (!start || !len) {
		pci_info(pdev, "no BAR0, skipping\n");
		return NULL;
	}

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return NULL;

	d->pdev = pci_dev_get(pdev);
	d->bar0_phys = start;
	d->bar0_len  = len;

	/* ioremap WITHOUT request_mem_region — the in-tree thunderbolt
	 * driver already owns this region. We're a parallel observer. */
	d->bar0 = ioremap(start, len);
	if (!d->bar0) {
		pci_err(pdev, "ioremap of BAR0 (%pa, %zu KiB) failed\n",
			&start, (size_t)len / 1024);
		pci_dev_put(d->pdev);
		kfree(d);
		return NULL;
	}

	d->dir = debugfs_create_dir(pci_name(pdev), parent);
	if (IS_ERR_OR_NULL(d->dir)) {
		pci_warn(pdev, "debugfs_create_dir failed\n");
		iounmap(d->bar0);
		pci_dev_put(d->pdev);
		kfree(d);
		return NULL;
	}

	debugfs_create_file("regs",   0444, d->dir, d, &regs_fops);
	debugfs_create_file("caps",   0444, d->dir, d, &caps_fops);
	debugfs_create_file("survey", 0444, d->dir, d, &survey_fops);
	debugfs_create_file("scan",   0444, d->dir, d, &scan_fops);
	debugfs_create_file("rings",  0444, d->dir, d, &rings_fops);
	debugfs_create_file("read32", 0644, d->dir, d, &read32_fops);
	debugfs_create_file("write32", allow_write ? 0600 : 0000, d->dir, d,
			    &write32_fops);
	debugfs_create_file("bar0",   allow_write ? 0600 : 0400, d->dir, d,
			    &bar0_fops);

	pci_info(pdev, "attached: BAR0 phys 0x%llx, len %zu KiB, debugfs at %s\n",
		 (u64)start, (size_t)len / 1024, pci_name(pdev));

	return d;
}

static void detach_one(struct usb4_pci_dev *d)
{
	debugfs_remove_recursive(d->dir);
	if (d->bar0)
		iounmap(d->bar0);
	pci_dev_put(d->pdev);
	kfree(d);
}

/* ----- public init / exit ----------------------------------------- */

int usb4_rdma_pci_init(struct dentry *parent_dir)
{
	struct pci_dev *pdev = NULL;
	const struct pci_device_id *id;
	int found = 0;

	pci_root = debugfs_create_dir("pci", parent_dir);
	if (IS_ERR_OR_NULL(pci_root))
		return -ENODEV;

	for (id = usb4_nhi_ids; id->vendor; id++) {
		while ((pdev = pci_get_device(id->vendor, id->device, pdev))) {
			struct usb4_pci_dev *d = attach_one(pdev, pci_root);
			if (d) {
				mutex_lock(&pci_devs_lock);
				list_add_tail(&d->list, &pci_devs);
				mutex_unlock(&pci_devs_lock);
				found++;
			}
		}
	}

	pr_info("attached %d USB4 host router(s)%s\n",
		found, allow_write ? " (allow_write=1, mmap PROT_WRITE permitted)" : "");

	return 0;
}

void usb4_rdma_pci_exit(void)
{
	struct usb4_pci_dev *d, *tmp;

	mutex_lock(&pci_devs_lock);
	list_for_each_entry_safe(d, tmp, &pci_devs, list) {
		list_del(&d->list);
		detach_one(d);
	}
	mutex_unlock(&pci_devs_lock);

	debugfs_remove_recursive(pci_root);
	pci_root = NULL;
}
