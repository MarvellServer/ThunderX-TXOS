/*
 * hacks for Vulcan
 * needed for bootwrapper boot
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/platform_device.h>
#include <linux/pci-ecam.h>

static bool VLCN = false;
static bool DBG = false;
static bool FWF = false;
static int maxnode;

struct vulcan_node_pci {
	bool USB;
	bool SATA;
	bool PCIE;
	bool PCISB0;
	bool PCISB1;
	bool PCISB2;
	bool PCISB3;
	unsigned int nd_bus;
	void *pci;
	DECLARE_BITMAP(rcbus_map, 256);
	DECLARE_BITMAP(epdev_map, 256);
};

static struct vulcan_node_pci ndata[2] = {
	{false, false, false, false, false, false, false, 0x0},
	{false, false, false, false, false, false, false, 0x80},
};

static inline int bus_to_node(unsigned int bus)
{
	return bus / 0x80;
}

static inline int node_to_bus(unsigned int n)
{
	return 0x80 * n;
}

void __iomem *vulcan_pci_config_base(void *pci, unsigned int bus, unsigned int devfn, unsigned int off)
{
	struct pci_config_window *cfg = pci;

	bus = bus % 0x80;
	return cfg->win + ((bus << 20) | (devfn << 12) | off);
}

static inline struct vulcan_node_pci *bus_to_ndata(unsigned int bus)
{
	return &ndata[bus_to_node(bus)];
}

static bool vulcan_whitelist(unsigned int busn, unsigned int devfn)
{
	struct vulcan_node_pci *n = bus_to_ndata(busn);
	int d = PCI_SLOT(devfn);

	if (maxnode == 1 && busn >= node_to_bus(1))
		return false;

	if (busn == n->nd_bus && devfn == 0)
		return true;
	if (busn == n->nd_bus && n->SATA && (devfn == 0x90 || devfn == 0x91))
		return true;
	if (busn == n->nd_bus && n->USB && (devfn == 0x88 || devfn == 0x89))
		return true;
	if (busn == n->nd_bus && n->PCIE && test_bit(d, n->rcbus_map))
		return true;
	if (n->PCIE && test_bit(busn, n->epdev_map))
		return true;

	return false;
}

static int vulcan_pcie_read_config(struct pci_bus *bus, unsigned int devfn,
				   int offset, int len, u32 *val)
{
	void __iomem *addr;
	unsigned int roff = offset & 0xffc;
	unsigned int busn = bus->number;
	struct vulcan_node_pci *n = bus_to_ndata(busn);

	if (!vulcan_whitelist(busn, devfn)) {
		if (DBG)
			pr_info("%x:%x: skipped\n", busn, devfn);
		goto baddr;
	}

	addr = vulcan_pci_config_base(n->pci, busn, devfn, roff);


	if (DBG) pr_info("%x:%x [%x] =>\n", busn, devfn, offset);
	*val = readl(addr);

	/* fixup loopback EP dev class - 0604 causes linux to fail */
	if (test_bit(busn, n->epdev_map) && devfn == 0 && roff == 8)
		if ((*val & 0xffff0000) == 0x06040000)
			*val = 0xfe000000 | (*val & 0xffff);

	if (DBG) pr_info("\t\t => %08x\n", *val);
	if (len == 1)
		*val = (*val >> (8 * (offset & 3))) & 0xff;
	else if (len == 2)
		*val = (*val >> (8 * (offset & 3))) & 0xffff;

	return PCIBIOS_SUCCESSFUL;

baddr:
	*val = 0xffffffff;
	return PCIBIOS_DEVICE_NOT_FOUND;
}

static int vulcan_pcie_write_config(struct pci_bus *bus, unsigned int devfn,
				    int offset, int len, u32 val)
{
	void __iomem *addr;
	u32 mask, tmp;
	unsigned int woff = offset & 0xffc;
	unsigned int busn = bus->number;
	struct vulcan_node_pci *n = bus_to_ndata(busn);

	addr = vulcan_pci_config_base(n->pci, busn, devfn, woff);
	if (DBG) pr_info("%x:%x [%x] <=\n", busn, devfn, offset);
	if (len == 4) {
		tmp = val;
		mask = 0xffffffff;
	} else {
		if (len == 2)
			mask = ~(0xffff << ((offset & 0x3) * 8));
		else if (len == 1)
			mask = ~(0xff << ((offset & 0x3) * 8));
		else
			return PCIBIOS_BAD_REGISTER_NUMBER;

		tmp = readl(addr) & mask;
		if (DBG) pr_info("\t\t|=> %08x\n", tmp);
		tmp |= val << ((offset & 0x3) * 8);
	}
	if (DBG) pr_info("\t\t<= %08x %08x\n", tmp, mask);
	writel(tmp, addr);
	if (DBG) pr_info("\t\t   %08x\n", readl(addr));

	return PCIBIOS_SUCCESSFUL;
}

static unsigned int vulcan_get_pci_cap(void __iomem *q, unsigned int cap)
{
	unsigned int off;
	u32 val, n = 48;

	off = readl(q + 0x34) & 0xff;
	while (off && --n) {
		val = readl(q + off);
		if ((val & 0xff) == cap)
			return off;
		off = (val >> 8) & 0xff;
	}
	return 0;
}

static void vulcan_pcie_setup_mps(int b, int d, int f)
{
	void __iomem *q;
	unsigned int off;
	u32 val, devcap, devctrl, devctrl2;
	struct vulcan_node_pci *n = bus_to_ndata(b);

	pr_info("%02x:%02x:%02x setup mps\n", b, d, f);
	q = vulcan_pci_config_base(n->pci, b, PCI_DEVFN(d, f), 0);
	pr_info("q: %08x\n", q);
	val = readl(q);
	pr_info("val: %08x\n", val);
	if (val == 0xffffffffu) {
		pr_info("%x:%x:%x: no device!\n", b, d, f);
		return;
	}
	off = vulcan_get_pci_cap(q, 0x10);
	pr_info("off: %08x\n", off);
	if (off == 0) {
		pr_err("no pci cap on %x:%x:%x\n", b, d, f);
		return;
	}
	q += off;
	pr_info("q: %08x\n", q);

	devcap = readl(q + 4);
	pr_info("devcap addr: %08x, Data: %08x\n", q+4, devcap);
	devctrl = readl(q + 8);
	pr_info("devctrlp addr: %08x, Data: %08x\n", q+8, devctrl);
	pr_info("PCIe cap %02x %08x %08x, mps = %d max = %d "
		"mrrs = %d max = %d\n", off, devcap, devctrl,
		(devctrl >> 5) & 0x7, devcap & 0x07,
		(devctrl >> 12) & 0x7, (devcap >> 15) & 0x07);
	devctrl &= ~(0x7 << 5);
	devctrl &= ~(0x7 << 12);
	writel(devctrl, q + 8);
	pr_info("WRITE devctrlp addr: %08x, Data: %08x\n", q+8, devctrl);
	devctrl = readl(q + 8);
	pr_info("READ devctrlp addr: %08x, Data: %08x\n", q+8, devctrl);
	pr_info("PCIe after at %02x %08x, mps = %d mrs = %d\n",
		off, devctrl, (devctrl >> 5) & 0x7,
		(devctrl >> 12) & 0x7);

	devctrl2 = readl(q + 0x28);
	pr_info("devctrl2 addr: %08x, Data: %08x\n", q+0x28, devctrl2);
	pr_info("PCIe DEV_CTRL2 %08x", devctrl2);
	devctrl2 |= (1 << 4); /* Completion Timeout Disabled */
	writel(devctrl2, q + 0x28);
	pr_info("WRITE devctrl2 addr: %08x, Data: %08x\n", q+0x28, devctrl2);
	devctrl2 = readl(q + 0x28);
	pr_info(" -> %08x (Completion Timeout %sabled)\n", devctrl2,
		((devctrl2 >> 4) & 1) ? "Dis" : "En");
}

static int vulcan_pcie_reset(int n, int rc, int link)
{
	void __iomem *p;
	u64 rcpa = 0;
	u32 val;
	int b = node_to_bus(n);
	int i;

	pr_info("%x:%x:%x reset\n", b, rc, 0);
	rcpa = 0x402800000 + (1u << 30) * n + 0x20000 * (rc - 1);
	p = ioremap(rcpa, 0x20000);

	if (!p) {
		pr_err("ioremap failed\n");
		return -ENOMEM;
	} else
		pr_info("Mapped %lx to %p\n", (unsigned long)rcpa, p);

	val = readl(p+0x8280);
	pr_info("before reset 0x8280 : %08x\n", val);
	writel(val & ~8u, p+0x8280);

	/* Wait for reset complete */
	for (i = 0; i < 2000; i++) {
		val = readl(p+0x8280);
		pr_info("read back 0x8280 : %08x\n", val);
		if ((val & 8) == 0)
			break;
	}
	pr_info("rc reset %s\n", (i==2000) ? "fail" : "done");

	if (!link)
		goto done;

	/* wait for link up */
	for (i = 0; i < 2000; i++) {
		val = readl(p + 0x8e80);
		pr_info("%x.%x.%x: state1: 0x%x 30 %s 29 %s 28 %s\n",
					b, rc, 0, val,
					(((val >> 30) & 1) ? "UP" : "DOWN"),
					(((val >> 29) & 1) ? "UP" : "DOWN"),
					(((val >> 28) & 1) ? "UP" : "DOWN"));
		if ((val >> 28) & 1)
			break;
	}
	pr_info("rc linkup %s\n", (i==2000)?"fail":"done");

done:
	iounmap(p);
	return 0;
}

static int vulcan_pcie_setup_msi(int b, int rc, int f)
{
	void __iomem *p;
	u64 rcpa;
	u32 val;
	const unsigned long smmubase = 0x402300000ul;
	int n = bus_to_node(b);

	rcpa = 0x402800000 + (1u << 30) * n + 0x20000 * (rc - 1);
	p = ioremap(rcpa, 0x20000);
	if (!p)
		return -ENOMEM;
	pr_info("Mapped %lx to %p\n", (unsigned long)rcpa, p);

	if (0) {
		/* WCU disable - not used */
		val = readl(p + 0x8000);
		writel(val | (1 << 9), p + 0x8000);
		pr_info("0x8000: %08x -> %08x\n", val, readl(p + 0x8000));
	} else {
		pr_info("0x8000: %08x\n", readl(p + 0x8000));
	}

	writel(0x03020000 + (1u << 30) * n, p + 0x8b00); /* msi base lo */
	writel(0x00000004, p + 0x8b40); /* msi base hi */
	writel(0x03120000 + (1u << 30) * n, p + 0x8b80); /* msi limit lo */
	writel(0x00000004, p + 0x8bc0); /* msi limit hi */

	pr_info("0x8b00: %08x %08x %08x %08x\n",
		readl(p+0x8b00), readl(p+0x8b40),
		readl(p+0x8b80), readl(p+0x8bc0));
	iounmap(p);

	/* Disable SMMU */
	/* rajkumar
        p = ioremap(smmubase + n * 0x40000000, 0x10000);
	pr_info("Mapped SMMU %lx to %p\n", smmubase, p);
	val = readl(p + 0x1040);
	writel(0x400, p + 0x1040);
	pr_info("smmu:1040 %08x -> %08x\n", val, readl(p + 0x1040));
	iounmap(p);
        */

	return 0;
}

static void vulcan_setup_rc(unsigned int n, unsigned int rc,
			unsigned int ep, unsigned int rb, int reset)
{
	u8 __iomem *q;
	u32 val, conf;
	int b = node_to_bus(n);
	struct vulcan_node_pci *nd = bus_to_ndata(b);

	rb += b;
	if (ep)
		ep += b;

	q = vulcan_pci_config_base(nd->pci, b, rc, 0);
	val = readl(q);
	pr_info("%x:%x:%x: %08x\n", b, rc, 0, val);

	pr_info("Setup rc %x %x : 0x%x-0x%x\n", n, rc, ep, rb);
	if (!FWF && reset)
		vulcan_pcie_reset(n, rc, reset >> 1);
	if (!ep) {
		pr_info("rc disabled\n");
		return;
	}

	q = vulcan_pci_config_base(nd->pci, b, PCI_DEVFN(rc, 0), 0);
	pr_info("%x:%x:%x: %08x\n", b, rc, 0, readl(q));
	if (!FWF) {
		conf = (rb << 16) | (ep << 8) | b;
		writel(conf, q + 0x18);		/* buses */
		readl(q + 0x18);
		writel(0x6, q + 0x4);		/* cmd enable */
	}

	conf = readl(q + 0x18);
	val = readl(q + 0x4);
	rb = (conf >> 16) & 0xff;
	ep = (conf >> 8) & 0xff;
	pr_info("%x:%x:%x: bus %08x cmd %08x ep %x rb %x b %x\n",
				b, rc, 0, conf, val, ep, rb, conf & 0xff);
	__set_bit(rc, nd->rcbus_map);

	q = vulcan_pci_config_base(nd->pci, ep, PCI_DEVFN(0, 0), 0);
	pr_info("%x:%x:%x: %08x\n", ep, 0, 0, readl(q));

	if (!FWF)
		writel(0x6, q + 0x4);		/* cmd enable */
	val = readl(q + 0x4);
	pr_info("%x:%x:%x: ep cmd %08x\n", ep, 0, 0, val);
	__set_bit(ep, nd->epdev_map);
}

extern int acpi_disabled, acpi_pci_disabled;
static int vulcan_iomask = 0;

static void vulcan_config_node(int iomask, int node)
{
	u8 __iomem *p;
	u32 conf;
	struct vulcan_node_pci *n = &ndata[node];
	unsigned long cmupa = 0x401000000ul + (node << 30);

	if (iomask & 0x1)
		n->PCIE = true;
	if (iomask & 0x2)
		n->PCISB0 = true;
	if (iomask & 0x4)
		n->PCISB1 = true;
	if (iomask & 0x8)
		n->PCISB2 = true;
	if (iomask & 0x10)
		n->PCISB3 = true;
	if (iomask & 0x20)
		n->USB = true;
	if (iomask & 0x40)
		n->SATA = true;

	pr_info("\t node %d: IO mask %cSATA %cUSB %cPCISB3 %cPCISB2 %cPCISB1 %cPCISB0 %cPCIe\n", node,
		(n->SATA?'+':'-'), (n->USB?'+':'-'), (n->PCISB3?'+':'-'),
		(n->PCISB2?'+':'-'), (n->PCISB1?'+':'-'), (n->PCISB0?'+':'-'), (n->PCIE?'+':'-'));

	p = ioremap(cmupa, 0x10000);
	if (p == NULL)  {
		pr_info("cmu pa failed %lx \n", cmupa);
		return;
	} else {
		pr_info("mapped %lx to %p\n", cmupa, p);
	}
	/* init CMU */
	if (!FWF) {
		u64 nbase64 = 0x4000000000 + 0x1000000000 * node;
		u64 nlim64 =  nbase64 + 0x1000000000 - 1;
		u32 nbase32 = 0x40000000 + 0x10000000 * node;
		u32 nlim32 =  nbase32 + 0x10000000 - 1;

		/* buses */
		conf = ((n->nd_bus + 0x7f) << 16) | ((n->nd_bus + 1) << 8) | n->nd_bus;
		writel(conf, p + 0x420);
		pr_info("%d: readback buses %08x %08x", node, conf, readl(p + 0x420));

		/* mem ranges */
		writel((nlim32 & 0xfff00000u) | ((nbase32 >> 16) & 0xfff0), p + 0x428);
		writel((nlim64 & 0xfff00000u) | (1 << 16) | ((nbase64 >> 16) & 0xfff0) | 1, p + 0x42c);
		writel((nbase64  >> 32), p + 0x430);
		writel((nlim64  >> 32), p + 0x434);
	}

	pr_info("CMU config [%d] : buses %08x", node, readl(p + 0x420));
	pr_info("CMU config [%d] : base/lim32 %08x\n", node, readl(p + 0x428));
	pr_info("CMU config [%d] : base/lim64 %08x\n", node, readl(p + 0x42c));
	pr_info("CMU config [%d] : base64 %08x lim64 %08x\n", node, readl(p + 0x430), readl(p + 0x434));
	iounmap(p);
}

static void vulcan_init_node(int iomask, int node)
{
	u8 __iomem *p, *q;
	u32 conf, val, v1;
	struct vulcan_node_pci *n = &ndata[node];

	p = vulcan_pci_config_base(n->pci, n->nd_bus, 0, 0);
	pr_info("CMU [%d:0.0] : %08x %08x %08x", n->nd_bus, readl(p), iomask, p);
	conf = readl(p + 0x4);
	pr_info("CMU config [%d] : cmd %08x", node, conf);
	writel(conf | 0x6, p + 0x4);
	pr_info("CMU config [%d] : cmd %08x", node, readl(p + 0x4));

	if (n->PCIE) {
		if (n->PCISB0) {
	                pr_info("Controller-0 rc setup. bus=0, dev=1, sec_bus=1, sub_bus=8\n");
			vulcan_setup_rc(node, 1, 0x1, 0x8, 3);
		} else
			vulcan_setup_rc(node, 1, 0, 0, 0);
		if (n->PCISB1) {
	                pr_info("Controller-1 rc setup. bus=0, dev=5, sec_bus=9, sub_bus=16\n");
			vulcan_setup_rc(node, 5, 0x9, 0x10, 3);
		} else
			vulcan_setup_rc(node, 5, 0, 0, 0);
		if (n->PCISB2) {
	                pr_info("Controller-2 rc setup. bus=0, dev=9, sec_bus=17, sub_bus=24\n");
			vulcan_setup_rc(node, 9, 0x11, 0x18, 3);
		} else
			vulcan_setup_rc(node, 9, 0, 0, 0);
		if (n->PCISB3) {
	                pr_info("Controller-3 rc setup. bus=0, dev=13, sec_bus=25, sub_bus=32\n");
			vulcan_setup_rc(node, 13, 0x19, 0x20, 3);
		} else
			vulcan_setup_rc(node, 13, 0, 0, 0);
	}
	if (n->USB) {
		//tx2 q = vulcan_pci_config_base(n->pci, n->nd_bus, PCI_DEVFN(0xf, 0), 0);
		q = vulcan_pci_config_base(n->pci, n->nd_bus, PCI_DEVFN(0x11, 0), 0);
		pr_info("RAJ: USB 0.17.0 config base %x\n", q);
		val = readl(q + 0x18);
		v1 = readl(q + 0x1c);
		pr_info("BAR 2/3: %08x %08x\n", val, v1);
		val = readl(q + 0x20);
		v1 = readl(q + 0x24);
		pr_info("BAR 4/5: %08x %08x\n", val, v1);
		if (!FWF) {
		        pr_info("USB 0:17:0 cmd write\n");
			writel(0x0404, q + 0x4);	/* cmd enable, int disable */
			writel(0x0151, q+ 0x3c);
		}
		pr_info("USB 0:16:0 cmd %x\n", readl(q + 0x4));

		q = vulcan_pci_config_base(n->pci, n->nd_bus, PCI_DEVFN(0x11, 1), 0);
		pr_info("RAJ: USB 0.17.1 config base %x\n", q);
		val = readl(q + 0x18);
		v1 = readl(q + 0x1c);
		pr_info("BAR 2/3: %08x %08x\n", val, v1);
		val = readl(q + 0x20);
		v1 = readl(q + 0x24);
		pr_info("BAR 4/5: %08x %08x\n", val, v1);
		if (!FWF) {
		        pr_info("USB 0:17:1 cmd write\n");
			writel(0x0404, q + 0x4);	/* cmd enable, int disable */
			writel(0x0152, q+ 0x3c);
		}
		pr_info("USB 0:17:1 cmd %x\n", readl(q + 0x4));
		pr_info("USB Config done\n");
        }
	if (n->SATA) {
		q = vulcan_pci_config_base(n->pci, n->nd_bus, PCI_DEVFN(0x12, 0), 0);
		pr_info("RAJ: SATA 0.18.0 config base %x\n", q);
		val = readl(q + 0x18);
		v1 = readl(q + 0x1c);
		pr_info("BAR 2/3: %08x %08x\n", val, v1);
		val = readl(q + 0x20);
		v1 = readl(q + 0x24);
		pr_info("BAR 4/5: %08x %08x\n", val, v1);
		if (!FWF) {
		        pr_info("SATA 0:18:0 cmd write\n");
			writel(0x0404, q + 0x4);	/* cmd enable, int disable */
			writel(0x0151, q+ 0x3c);
		}
		pr_info("SATA 0:18:0 cmd read1%x\n", readl(q + 0x4));

		q = vulcan_pci_config_base(n->pci, n->nd_bus, PCI_DEVFN(0x12, 1), 0);
		pr_info("RAJ: SATA 0.18.1 config base %x\n", q);
		val = readl(q + 0x18);
		v1 = readl(q + 0x1c);
		pr_info("BAR 2/3: %08x %08x\n", val, v1);
		val = readl(q + 0x20);
		v1 = readl(q + 0x24);
		pr_info("BAR 4/5: %08x %08x\n", val, v1);
		if (!FWF) {
		        pr_info("SATA 0:18:1 cmd write\n");
			writel(0x0404, q + 0x4);	/* cmd enable, int disable */
			writel(0x0152, q+ 0x3c);
		}
		pr_info("SATA 0:18:1 cmd %x\n", readl(q + 0x4));
		pr_info("SATA Config done\n");
	}
	if (n->PCIE && n->PCISB0) {
		unsigned int ep;

		vulcan_pcie_setup_msi(n->nd_bus, 1, 0);

		q = vulcan_pci_config_base(n->pci, n->nd_bus, PCI_DEVFN(1, 0), 0);
		val  = readl(q + 0x18);
		ep = (val >> 8) & 0xff;

		vulcan_pcie_setup_mps(n->nd_bus, 1, 0);
		pr_info("MPS setup for ep %x\n", ep);
		vulcan_pcie_setup_mps(ep, 0, 0);
		pr_info("MPS setup done\n");
	}
        if (n->PCIE && n->PCISB1) {
                unsigned int ep;

                vulcan_pcie_setup_msi(n->nd_bus, 5, 0);

                q = vulcan_pci_config_base(n->pci, n->nd_bus, PCI_DEVFN(5, 0), 0);
                val  = readl(q + 0x18);
                ep = (val >> 8) & 0xff;

                vulcan_pcie_setup_mps(n->nd_bus, 5, 0);
                pr_info("MPS setup for ep %x\n", ep);
                vulcan_pcie_setup_mps(ep, 0, 0);
                pr_info("MPS setup done\n");
        }
        if (n->PCIE && n->PCISB2) {
                unsigned int ep;

                vulcan_pcie_setup_msi(n->nd_bus, 9, 0);

                q = vulcan_pci_config_base(n->pci, n->nd_bus, PCI_DEVFN(9, 0), 0);
                val  = readl(q + 0x18);
                ep = (val >> 8) & 0xff;

                vulcan_pcie_setup_mps(n->nd_bus, 9, 0);
                pr_info("MPS setup for ep %x\n", ep);
                vulcan_pcie_setup_mps(ep, 0, 0);
                pr_info("MPS setup done\n");
        }
	if (n->PCIE && n->PCISB3) {
		unsigned int ep;

		vulcan_pcie_setup_msi(n->nd_bus, 13, 0);

		q = vulcan_pci_config_base(n->pci, n->nd_bus, PCI_DEVFN(13, 0), 0);
		val  = readl(q + 0x18);
		ep = (val >> 8) & 0xff;

		vulcan_pcie_setup_mps(n->nd_bus, 13, 0);
		pr_info("MPS setup for ep %x\n", ep);
		vulcan_pcie_setup_mps(ep, 0, 0);
		pr_info("MPS setup done\n");
	}
}

int vulcan_pci_init(void *pci, struct pci_ops *pops)
{
	struct pci_config_window *cfg = pci;
	int node;

	node = bus_to_node(cfg->busr.start);
	pr_info("%d Configuring Vulcan PCIe controller %cFWF.\n", node, (FWF?'+':'-'));

	if (vulcan_iomask == 0)
		return -ENODEV;

	if (node == 1 && maxnode == 1)
		return -ENODEV;

	ndata[node].pci = pci;

	pr_info("Vulcan PCI setup!\n");
#if 1
	/* Vulcan host-like bridge */
	pops->read = vulcan_pcie_read_config;
	pops->write = vulcan_pcie_write_config;
#endif

	if (node == 0)
		vulcan_init_node(vulcan_iomask & 0xff, 0);
	else
		vulcan_init_node((vulcan_iomask >> 8) & 0xff, 1);
	return 0;
}

static int vulcan_pci_early(void)
{

	if (vulcan_iomask == 0) {
		pr_info("Skipping PCI setup!\n");
		return 0;
	}

	/* Vulcan host-like bridge */
	pr_info("Vulcan PCI setup!\n");

	VLCN = true;
	if (vulcan_iomask & 0x10000)
		FWF = true;

	pr_info("Configuring Vulcan PCIe controller %cFWF.\n", (FWF?'+':'-'));
	maxnode = 1;
	if ((vulcan_iomask >> 8) & 0xff) {
		vulcan_config_node((vulcan_iomask >> 8) & 0xff, 1);
		maxnode = 2;
	}
	vulcan_config_node(vulcan_iomask & 0xff, 0);

	return 0;
}


static int __init set_iomask(char *str)
{
	int val;

	get_option(&str, &val);
	vulcan_iomask = val;
	return 0;
}

subsys_initcall(vulcan_pci_early);
early_param("vulcan_iomask", set_iomask);
