/*
 * Netmap passthrough interface driver for Linux
 * Copyright(c) 2015 Vincenzo Maffione <v.maffione@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/virtio_net.h>

#define WITH_PTNETMAP_GUEST
#include "../bsd_glue.h"
#include "net/netmap.h"
#include "dev/netmap/netmap_kern.h"
#include "dev/netmap/netmap_virt.h"


/* Enable to debug RX-side hangs */
//#define HANGCTRL

#if 0  /* Switch to 1 to enable per-packet logs. */
#define DBG D
#else
#define DBG ND
#endif

#define DRV_NAME "ptnet"
#define DRV_VERSION "0.1"

#define PTNET_MSIX_VECTORS	2

struct ptnet_info {
	struct net_device *netdev;
	struct pci_dev *pdev;

	/* Mirrors PTFEAT register. */
	uint32_t ptfeatures;

	/* Access to device memory. */
	int bars;
	u8* __iomem ioaddr;
#ifndef PTNET_CSB_ALLOC
	u8* __iomem csbaddr;
#endif  /* !PTNET_CSB_ALLOC */

	/* MSI-X interrupt data structures. */
	struct msix_entry msix_entries[PTNET_MSIX_VECTORS];
	char msix_names[PTNET_MSIX_VECTORS][64];
	cpumask_var_t msix_affinity_masks[PTNET_MSIX_VECTORS];

	/* CSB memory to be used for producer/consumer state
	 * syncrhonization. */
	struct paravirt_csb *csb;

	struct netmap_priv_d *nm_priv;
	struct netmap_pt_guest_adapter *ptna;

	struct napi_struct napi;

#ifdef HANGCTRL
#define HANG_INTVAL_MS		3000
	struct timer_list hang_timer;
#endif
};

#ifdef HANGCTRL
static void
hang_tmr_callback(unsigned long arg)
{
	struct ptnet_info *pi = (struct ptnet_info *)arg;
	struct netmap_adapter *na = &pi->ptna->hwup.up;
	struct netmap_kring *kring = &na->rx_rings[0];
	struct netmap_ring *ring = kring->ring;
	volatile struct paravirt_csb *csb = pi->csb;

	pr_info("HANG RX: hwc %u h %u c %u hwt %u t %u guest_need_rxkick %u\n",
		kring->nr_hwcur, ring->head, ring->cur,
		kring->nr_hwtail, ring->tail, csb->guest_need_rxkick);

	if (mod_timer(&pi->hang_timer,
		      jiffies + msecs_to_jiffies(HANG_INTVAL_MS))) {
		pr_err("%s: mod_timer() failed\n", __func__);
	}
}
#endif

static inline void
ptnet_sync_tail(struct pt_ring *ptring, struct netmap_kring *kring)
{
	struct netmap_ring *ring = kring->ring;

	/* Update hwcur and hwtail as known by the host. */
        ptnetmap_guest_read_kring_csb(ptring, kring);

	/* nm_sync_finalize */
	ring->tail = kring->rtail = kring->nr_hwtail;
}

static netdev_tx_t
ptnet_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct ptnet_info *pi = netdev_priv(netdev);
	struct netmap_adapter *na = NA(netdev);
	/* Alternative way:
	 *	struct netmap_adapter *na = &pi->ptna->hwup.up;
	*/
	struct netmap_kring *kring = &na->tx_rings[0];
	struct netmap_ring *ring = kring->ring;
	unsigned int const lim = kring->nkr_num_slots - 1;
	int nfrags = skb_shinfo(skb)->nr_frags;
	struct paravirt_csb *csb = pi->csb;
	struct netmap_slot *slot;
	int nmbuf_bytes;
	int skbdata_len;
	void *skbdata;
	void *nmbuf;
	int f;

	DBG("TX skb len=%d", skb->len);

	/* Update hwcur and hwtail (completed TX slots) as known by the host,
	 * by reading from CSB. */
	ptnet_sync_tail(&csb->tx_ring, kring);

	if (unlikely(ring->head == ring->tail)) {
		RD(1, "TX ring unexpected overflow, dropping");
		dev_kfree_skb_any(skb);

		return NETDEV_TX_OK;
	}

	/* Grab the next available TX slot. */
	slot = &ring->slot[ring->head];
	nmbuf = NMB(na, slot);
	nmbuf_bytes = 0;

	/* First step: Setup the virtio-net header at the beginning of th
	 *  first slot. */
	if (pi->ptfeatures & NET_PTN_FEATURES_VNET_HDR) {
		struct virtio_net_hdr_v1 *vh = nmbuf;

		if (likely(skb->ip_summed == CHECKSUM_PARTIAL)) {
			vh->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
			vh->csum_start = skb_checksum_start_offset(skb);
			vh->csum_offset = skb->csum_offset;
		} else {
			vh->flags = 0;
			vh->csum_start = vh->csum_offset = 0;
		}

		if (skb_is_gso(skb)) {
			vh->hdr_len = skb_headlen(skb);
			vh->gso_size = skb_shinfo(skb)->gso_size;
			if (skb_shinfo(skb)->gso_size & SKB_GSO_TCPV4) {
				vh->gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
			} else if (skb_shinfo(skb)->gso_size & SKB_GSO_UDP) {
				vh->gso_type = VIRTIO_NET_HDR_GSO_UDP;
			} else if (skb_shinfo(skb)->gso_size & SKB_GSO_TCPV6) {
				vh->gso_type = VIRTIO_NET_HDR_GSO_TCPV6;
			}

			if (skb_shinfo(skb)->gso_size & SKB_GSO_TCP_ECN) {
				vh->gso_type |= VIRTIO_NET_HDR_GSO_ECN;
			}

		} else {
			vh->hdr_len = vh->gso_size = 0;
			vh->gso_type = VIRTIO_NET_HDR_GSO_NONE;
		}

		vh->num_buffers = 0;

		nmbuf += sizeof(*vh);
		nmbuf_bytes += sizeof(*vh);
	}

	/* Second step: Copy in the linear part of the sk_buff. */

	skbdata = skb->data;
	skbdata_len = skb_headlen(skb);

	for (;;) {
		int copy = skbdata_len;

		if (unlikely(copy > ring->nr_buf_size - nmbuf_bytes)) {
			copy = ring->nr_buf_size - nmbuf_bytes;
		}

		memcpy(nmbuf, skbdata, copy);
		skbdata += copy;
		skbdata_len -= copy;
		nmbuf += copy;
		nmbuf_bytes += copy;

		if (likely(!skbdata_len)) {
			break;
		}

		slot->len = nmbuf_bytes;
		slot->flags = NS_MOREFRAG;
		ring->head = ring->cur = nm_next(ring->head, lim);
		slot = &ring->slot[ring->head];
		nmbuf = NMB(na, slot);
		nmbuf_bytes = 0;
	}

	/* Third step: Copy in the sk_buffs frags. */

	for (f = 0; f < nfrags; f++) {
		const struct skb_frag_struct *frag;

		frag = &skb_shinfo(skb)->frags[f];
		skbdata = skb_frag_address(frag);
		skbdata_len = skb_frag_size(frag);

		for (;;) {
			int copy = skbdata_len;

			if (copy > ring->nr_buf_size - nmbuf_bytes) {
				copy = ring->nr_buf_size - nmbuf_bytes;
			}

			memcpy(nmbuf, skbdata, copy);
			skbdata += copy;
			skbdata_len -= copy;
			nmbuf += copy;
			nmbuf_bytes += copy;

			if (!skbdata_len) {
				break;
			}

			slot->len = nmbuf_bytes;
			slot->flags = NS_MOREFRAG;
			ring->head = ring->cur = nm_next(ring->head, lim);
			slot = &ring->slot[ring->head];
			nmbuf = NMB(na, slot);
			nmbuf_bytes = 0;
		}
	}

	/* Prepare the last slot. */
	slot->len = nmbuf_bytes;
	slot->flags = 0;
	ring->head = ring->cur = nm_next(ring->head, lim);

	/* nm_txsync_prologue */
	kring->rcur = ring->cur;
	kring->rhead = ring->head;

	/* Tell the host to process the new packets, updating cur and head in
	 * the CSB. */
	ptnetmap_guest_write_kring_csb(&csb->tx_ring, kring->rcur, kring->rhead);

        /* Ask for a kick from a guest to the host if needed. */
	if (NM_ACCESS_ONCE(csb->host_need_txkick) && !skb->xmit_more) {
		csb->tx_ring.sync_flags = NAF_FORCE_RECLAIM;
		iowrite32(0, pi->ioaddr + PTNET_IO_TXKICK);
	}

        /* No more TX slots for further transmissions. We have to stop the
	 * qdisc layer and enable notifications. */
	if (ring->head == ring->tail) {
		netif_stop_queue(netdev);
		csb->guest_need_txkick = 1;

                /* Double check. */
		ptnet_sync_tail(&csb->tx_ring, kring);
		if (unlikely(ring->head != ring->tail)) {
			/* More TX space came in the meanwhile. */
			netif_start_queue(netdev);
			csb->guest_need_txkick = 0;
		}
	}

	pi->netdev->stats.tx_bytes += skb->len;
	pi->netdev->stats.tx_packets ++;

	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

/*
 * ptnet_get_stats - Get System Network Statistics
 * @netdev: network interface device structure
 *
 * Returns the address of the device statistics structure.
 */
static struct net_device_stats *
ptnet_get_stats(struct net_device *netdev)
{
	return &netdev->stats;
}

/*
 * ptnet_change_mtu - Change the Maximum Transfer Unit
 * @netdev: network interface device structure
 * @new_mtu: new value for maximum frame size
 *
 * Returns 0 on success, negative on failure
 */
static int
ptnet_change_mtu(struct net_device *netdev, int new_mtu)
{
	pr_info("%s changing MTU from %d to %d\n",
		netdev->name, netdev->mtu, new_mtu);
	netdev->mtu = new_mtu;

	return 0;
}

/*
 * ptnet_tx_intr - Interrupt Handler
 * @irq: interrupt number
 * @data: pointer to a network interface device structure
 */
static irqreturn_t
ptnet_tx_intr(int irq, void *data)
{
	struct net_device *netdev = data;
	struct ptnet_info *pi = netdev_priv(netdev);

	//printk("%s\n", __func__);

	if (!pi->nm_priv && netmap_tx_irq(netdev, 0)) {
		return IRQ_HANDLED;
	}

	/* Just wake up the qdisc layer, it will flush pending transmissions,
	 * with the side effect of reclaiming completed TX slots. */
	netif_wake_queue(netdev);

	return IRQ_HANDLED;
}

/*
 * ptnet_rx_intr - Interrupt Handler
 * @irq: interrupt number
 * @data: pointer to a network interface device structure
 */
static irqreturn_t
ptnet_rx_intr(int irq, void *data)
{
	struct net_device *netdev = data;
	struct ptnet_info *pi = netdev_priv(netdev);
	unsigned int unused;

	//printk("%s\n", __func__);

	if (!pi->nm_priv && netmap_rx_irq(netdev, 0, &unused)) {
		(void)unused;
		return IRQ_HANDLED;
	}

	/* Disable interrupts and schedule NAPI. */
	if (likely(napi_schedule_prep(&pi->napi))) {
		/* It's good thing to reset guest_need_rxkick as soon as
		 * possible. */
		pi->csb->guest_need_rxkick = 0;
		__napi_schedule(&pi->napi);
	} else {
		/* This should not happen, probably. */
		pi->csb->guest_need_rxkick = 1;
	}

	return IRQ_HANDLED;
}

/*
 * ptnet_rx_poll - NAPI Rx polling callback
 * @pi: NIC private structure
 */
static int
ptnet_rx_poll(struct napi_struct *napi, int budget)
{
	struct ptnet_info *pi = container_of(napi, struct ptnet_info,
					     napi);
	struct netmap_adapter *na = &pi->ptna->hwup.up;
	struct netmap_kring *kring = &na->rx_rings[0];
	struct netmap_ring *ring = kring->ring;
	unsigned int const lim = kring->nkr_num_slots - 1;
	struct paravirt_csb *csb = pi->csb;
	bool have_vnet_hdr = pi->ptfeatures & NET_PTN_FEATURES_VNET_HDR;
	int work_done = 0;

#ifdef HANGCTRL
	del_timer(&pi->hang_timer);
#endif

	/* Update hwtail, rtail, tail and hwcur to what is known from the host,
	 * reading from CSB. */
	ptnet_sync_tail(&csb->rx_ring, kring);

	kring->nr_kflags &= ~NKR_PENDINTR;

	/* Import completed RX slots. */
	while (work_done < budget && ring->head != ring->tail) {
		struct virtio_net_hdr_v1 *vh;
		struct netmap_slot *slot;
		struct sk_buff *skb;
		unsigned int len;
		void *nmbuf;

		slot = &ring->slot[ring->head];
		ring->head = ring->cur = nm_next(ring->head, lim);

		nmbuf = NMB(na, slot);
		len = slot->len;

		vh = nmbuf;
		if (likely(have_vnet_hdr)) {
			nmbuf += sizeof(*vh);
			len -= sizeof(*vh);
		}

		skb = napi_alloc_skb(napi, len);
		if (!skb) {
			pr_err("napi_alloc_skb() failed\n");
			break;
		}

		memcpy(skb_put(skb, len), nmbuf, len);

		DBG("RX SKB len=%d", skb->len);

		pi->netdev->stats.rx_bytes += skb->len;
		pi->netdev->stats.rx_packets ++;

		if (likely(have_vnet_hdr && (vh->flags & VIRTIO_NET_HDR_F_NEEDS_CSUM))) {
			if (unlikely(!skb_partial_csum_set(skb, vh->csum_start,
							   vh->csum_offset))) {
				dev_kfree_skb_any(skb);
				work_done ++;
				continue;
			}

		} else if (have_vnet_hdr && (vh->flags & VIRTIO_NET_HDR_F_DATA_VALID)) {
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		}

		skb->protocol = eth_type_trans(skb, pi->netdev);

		if (likely(have_vnet_hdr && vh->gso_type != VIRTIO_NET_HDR_GSO_NONE)) {
			switch (vh->gso_type & ~VIRTIO_NET_HDR_GSO_ECN) {

			case VIRTIO_NET_HDR_GSO_TCPV4:
				skb_shinfo(skb)->gso_type = SKB_GSO_TCPV4;
				break;

			case VIRTIO_NET_HDR_GSO_UDP:
				skb_shinfo(skb)->gso_type = SKB_GSO_UDP;
				break;

			case VIRTIO_NET_HDR_GSO_TCPV6:
				skb_shinfo(skb)->gso_type = SKB_GSO_TCPV6;
				break;
			}

			if (vh->gso_type & VIRTIO_NET_HDR_GSO_ECN) {
				skb_shinfo(skb)->gso_type |= SKB_GSO_TCP_ECN;
			}

			skb_shinfo(skb)->gso_size = vh->gso_size;
			skb_shinfo(skb)->gso_type |= SKB_GSO_DODGY;
			skb_shinfo(skb)->gso_segs = 0;
		}

		napi_gro_receive(napi, skb);

		work_done ++;
	}

	if (work_done < budget) {
		/* Budget was not fully consumed, since we have no more
		 * completed RX slots. We can enable notifications and
		 * exit polling mode. */
                csb->guest_need_rxkick = 1;
		napi_complete_done(napi, work_done);

                /* Double check for more completed RX slots. */
		ptnet_sync_tail(&csb->rx_ring, kring);
		if (ring->head != ring->tail && napi_schedule_prep(napi)) {
			/* If there is more work to do, disable notifications
			 * and go ahead. */
                        csb->guest_need_rxkick = 0;
			__napi_schedule(napi);
                }
#ifdef HANGCTRL
		if (mod_timer(&pi->hang_timer,
			      jiffies + msecs_to_jiffies(HANG_INTVAL_MS))) {
			pr_err("%s: mod_timer failed\n", __func__);
		}
#endif
	}

	if (work_done) {
		/* Tell the host (through the CSB) about the updated ring->cur and
		 * ring->head (RX buffer refill).
		 */
		kring->rcur = ring->cur;
		kring->rhead = ring->head;
		ptnetmap_guest_write_kring_csb(&csb->rx_ring, kring->rcur,
					       kring->rhead);
		/* Kick the host if needed. */
		if (NM_ACCESS_ONCE(csb->host_need_rxkick)) {
			csb->rx_ring.sync_flags = NAF_FORCE_READ;
			iowrite32(0, pi->ioaddr + PTNET_IO_RXKICK);
		}
	}

	return work_done;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
/* Polling 'interrupt' - used by things like netconsole to send skbs
 * without having to re-enable interrupts. It's not called while
 * the interrupt routine is executing.
 */
static void
ptnet_netpoll(struct net_device *netdev)
{
	struct ptnet_info *pi = netdev_priv(netdev);

	disable_irq(pi->pdev->irq);
	ptnet_tx_intr(pi->msix_entries[0].vector, netdev);
	ptnet_rx_intr(pi->msix_entries[1].vector, netdev);
	enable_irq(pi->pdev->irq);
}
#endif

static int
ptnet_irqs_init(struct ptnet_info *pi)
{
	char *names[PTNET_MSIX_VECTORS] = {"TX", "RX"};
	irq_handler_t handlers[PTNET_MSIX_VECTORS] = {
		ptnet_tx_intr,
		ptnet_rx_intr,
	};
	int ret;
	int i;

	/* Allocate the MSI-X interrupt vectors we need. */
	memset(pi->msix_affinity_masks, 0, sizeof(pi->msix_affinity_masks));

	for (i=0; i<PTNET_MSIX_VECTORS; i++) {
		if (!alloc_cpumask_var(&pi->msix_affinity_masks[i],
				       GFP_KERNEL)) {
			pr_err("Failed to alloc cpumask var\n");
			goto err_masks;
		}
		pi->msix_entries[i].entry = i;
	}

	ret = pci_enable_msix_exact(pi->pdev, pi->msix_entries,
				    PTNET_MSIX_VECTORS);
	if (ret) {
		pr_err("Failed to enable msix vectors (%d)\n", ret);
		goto err_masks;
	}

	for (i=0; i<PTNET_MSIX_VECTORS; i++) {
		snprintf(pi->msix_names[i], sizeof(pi->msix_names[i]),
			 "ptnet-%s", names[i]);
		ret = request_irq(pi->msix_entries[i].vector, handlers[i],
				  0, pi->msix_names[i], pi->netdev);
		if (ret) {
			pr_err("Unable to allocate interrupt (%d)\n", ret);
			goto err_irqs;
		}
		pr_info("IRQ for %s --> %u \n", names[i],
			pi->msix_entries[i].vector);
	}

	/* Tell the hypervisor that we have allocated the MSI-X vectors,
	 * so that it can do its own setup. */
	iowrite32(PTNET_CTRL_IRQINIT, pi->ioaddr + PTNET_IO_CTRL);

	return 0;

err_irqs:
	for (; i>=0; i--) {
		free_irq(pi->msix_entries[i].vector, pi->netdev);
	}
	i = PTNET_MSIX_VECTORS-1;
err_masks:
	for (; i>=0; i--) {
		free_cpumask_var(pi->msix_affinity_masks[i]);
	}

	return ret;
}

static void
ptnet_irqs_fini(struct ptnet_info *pi)
{
	int i;

	/* Tell the hypervisor that we are going to deallocate the
	 * MSI-X vectors, so that it can do its own setup. */
	iowrite32(PTNET_CTRL_IRQFINI, pi->ioaddr + PTNET_IO_CTRL);

	for (i=0; i<PTNET_MSIX_VECTORS; i++) {
		free_irq(pi->msix_entries[i].vector, pi->netdev);
		if (pi->msix_affinity_masks[i]) {
			free_cpumask_var(pi->msix_affinity_masks[i]);
		}
	}
	pci_disable_msix(pi->pdev);
}

static void
ptnet_ioregs_dump(struct ptnet_info *pi)
{
	char *regnames[PTNET_IO_END >> 2] = {
		"PTFEAT",
		"PTCTL",
		"PTSTS",
		"CTRL",
		"MAC_LO",
		"MAC_HI",
		"TXKICK",
		"RXKICK",
	}; // remove this ; to drive the compiler crazy !
	uint32_t val;
	int i;

	for (i=0; i<PTNET_IO_END; i+=4) {
		val = ioread32(pi->ioaddr + i);
		pr_info("PTNET_IO_%s = %u\n", regnames[i >> 2], val);
	}
}

static int ptnet_nm_register_netif(struct netmap_adapter *na, int onoff);
static int ptnet_nm_register_native(struct netmap_adapter *na, int onoff);

/*
 * ptnet_open - Called when a network interface is made active
 * @netdev: network interface device structure
 *
 * Returns 0 on success, negative value on failure
 *
 * The open entry point is called when a network interface is made
 * active by the system (IFF_UP).  At this point all resources needed
 * for transmit and receive operations are allocated, the interrupt
 * handler is registered with the OS, the watchdog task is started,
 * and the stack is notified that the interface is ready.
 */
static int
ptnet_open(struct net_device *netdev)
{
	struct ptnet_info *pi = netdev_priv(netdev);
	struct netmap_adapter *na = NA(netdev);
	enum txrx t;
	int ret;

	netmap_adapter_get(na);

	pi->nm_priv = netmap_priv_new();
	if (!pi->nm_priv) {
		pr_err("Failed to alloc netmap priv\n");
		return -ENOMEM;
	}

	NMG_LOCK();

	/* Replace nm_register method on the fly. */
	na->nm_register = ptnet_nm_register_netif;

	/* Put the device in netmap mode. */
	ret = netmap_do_regif(pi->nm_priv, na, 0, NR_REG_ALL_NIC |
			      NR_EXCLUSIVE);
	if (ret) {
		pr_err("netmap_do_regif() failed\n");
		netmap_adapter_put(na);
		NMG_UNLOCK();
		return -ret;
	}

	NMG_UNLOCK();

	/* Init np_si[t], this should have not effect on Linux. */
	for_rx_tx(t) {
		pi->nm_priv->np_si[t] = NULL;
	}


	napi_enable(&pi->napi);
	netif_start_queue(netdev);

	if (0) ptnet_ioregs_dump(pi);

	pi->csb->guest_csb_on = 1;

#ifdef HANGCTRL
	setup_timer(&pi->hang_timer, &hang_tmr_callback, (unsigned long)pi);
	if (mod_timer(&pi->hang_timer,
		      jiffies + msecs_to_jiffies(HANG_INTVAL_MS))) {
		pr_err("%s: mod_timer failed\n", __func__);
	}
#endif

	pr_info("%s: %p\n", __func__, pi);

	return 0;
}

/*
 * ptnet_close - Disables a network interface
 * @netdev: network interface device structure
 *
 * Returns 0, this is not allowed to fail
 *
 * The close entry point is called when an interface is de-activated
 * by the OS.  The hardware is still under the drivers control, but
 * needs to be disabled.  A global MAC reset is issued to stop the
 * hardware, and all transmit and receive resources are freed.
 */
static int
ptnet_close(struct net_device *netdev)
{
	struct ptnet_info *pi = netdev_priv(netdev);
	struct netmap_adapter *na = NA(netdev);

#ifdef HANGCTRL
	del_timer(&pi->hang_timer);
#endif

	pi->csb->guest_csb_on = 0;

	netif_tx_disable(netdev);
	napi_disable(&pi->napi);
	//synchronize_irq(pi->pdev->irq);

	NMG_LOCK();
	netmap_do_unregif(pi->nm_priv);
	na->nm_register = ptnet_nm_register_native;
	NMG_UNLOCK();

	netmap_priv_delete(pi->nm_priv);
	pi->nm_priv = NULL;
	netmap_adapter_put(na);

	pr_info("%s: %p\n", __func__, pi);

	return 0;
}

static const struct net_device_ops ptnet_netdev_ops = {
	.ndo_open		= ptnet_open,
	.ndo_stop		= ptnet_close,
	.ndo_start_xmit		= ptnet_start_xmit,
	.ndo_get_stats		= ptnet_get_stats,
	.ndo_change_mtu		= ptnet_change_mtu,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= ptnet_netpoll,
#endif
};

static uint32_t
ptnet_nm_ptctl(struct net_device *netdev, uint32_t cmd)
{
	struct ptnet_info *pi = netdev_priv(netdev);
	int ret;

	iowrite32(cmd, pi->ioaddr + PTNET_IO_PTCTL);
	ret = ioread32(pi->ioaddr + PTNET_IO_PTSTS);
	pr_info("PTCTL %u, ret %u\n", cmd, ret);

	return ret;
}

static struct netmap_pt_guest_ops ptnet_nm_pt_guest_ops = {
	.nm_ptctl = ptnet_nm_ptctl,
};

static int
ptnet_nm_register_common(struct netmap_adapter *na, int onoff,
			 int native)
{
	struct netmap_pt_guest_adapter *ptna =
			(struct netmap_pt_guest_adapter *)na;

	/* device-specific */
	struct net_device *netdev = na->ifp;
	struct paravirt_csb *csb = ptna->csb;
	enum txrx t;
	int ret = 0;
	int i;

	if (na->active_fds > 0) {
		/* This cannot happen since we have NR_EXCLUSIVE. */
		BUG_ON(!native);

		/* Nothing to do. */
		return 0;
	}

	if (onoff) {
		/* Make sure the host adapter passed through is ready
		 * for txsync/rxsync. This also initializes the CSB. */
		ret = ptnet_nm_ptctl(netdev, NET_PARAVIRT_PTCTL_REGIF);
		if (ret) {
			return ret;
		}

		for_rx_tx(t) {
			for (i=0; i<nma_get_nrings(na, t); i++) {
				struct netmap_kring *kring = &NMR(na, t)[i];
				struct pt_ring *ptring;

				if (!nm_kring_pending_on(kring)) {
					continue;
				}

				/* Sync krings from the host, reading from
				 * CSB. */
				ptring = (t == NR_TX ? &csb->tx_ring : &csb->rx_ring);
				kring->rhead = kring->ring->head = ptring->head;
				kring->rcur = kring->ring->cur = ptring->cur;
				kring->nr_hwcur = ptring->hwcur;
				kring->nr_hwtail = kring->rtail =
					kring->ring->tail = ptring->hwtail;
				kring->nr_mode = NKR_NETMAP_ON;
			}
		}

		if (native) {
			nm_set_native_flags(na);
		} else {
			/* Don't call nm_set_native_flags, since we don't want
			 * to replace ndo_start_xmit method. */
			na->na_flags |= NAF_NETMAP_ON;
		}
	} else {
		if (native) {
			nm_clear_native_flags(na);
		} else {
			na->na_flags &= NAF_NETMAP_ON;
		}

		for_rx_tx(t) {
			for (i=0; i<nma_get_nrings(na, t); i++) {
				struct netmap_kring *kring = &NMR(na, t)[i];

				if (!nm_kring_pending_off(kring)) {
					continue;
				}

				kring->nr_mode = NKR_NETMAP_OFF;
			}
		}

		ret = ptnet_nm_ptctl(netdev, NET_PARAVIRT_PTCTL_UNREGIF);
	}

	return ret;
}

static int
ptnet_nm_register_netif(struct netmap_adapter *na, int onoff)
{
	return ptnet_nm_register_common(na, onoff, 0);
}

static int
ptnet_nm_register_native(struct netmap_adapter *na, int onoff)
{
	return ptnet_nm_register_common(na, onoff, 1);
}

static int
ptnet_nm_config(struct netmap_adapter *na, unsigned *txr, unsigned *txd,
		unsigned *rxr, unsigned *rxd)
{
	struct netmap_pt_guest_adapter *ptna =
		(struct netmap_pt_guest_adapter *)na;
	int ret;

	if (ptna->csb == NULL) {
		pr_err("%s: NULL CSB pointer\n", __func__);
		return EINVAL;
	}

	ret = ptnet_nm_ptctl(na->ifp, NET_PARAVIRT_PTCTL_CONFIG);
	if (ret) {
		return ret;
	}

	*txr = ptna->csb->num_tx_rings;
	*rxr = ptna->csb->num_rx_rings;
#if 1
	*txr = 1;
	*rxr = 1;
#endif
	*txd = ptna->csb->num_tx_slots;
	*rxd = ptna->csb->num_rx_slots;

	pr_info("txr %u, rxr %u, txd %u, rxd %u\n",
		*txr, *rxr, *txd, *rxd);

	return 0;
}

static int
ptnet_nm_txsync(struct netmap_kring *kring, int flags)
{
	struct netmap_adapter *na = kring->na;
	struct net_device *netdev = na->ifp;
	struct ptnet_info *pi = netdev_priv(netdev);
	bool notify;

	notify = netmap_pt_guest_txsync(kring, flags);
	if (notify) {
		iowrite32(0, pi->ioaddr + PTNET_IO_TXKICK);
	}

	return 0;
}

static int
ptnet_nm_rxsync(struct netmap_kring *kring, int flags)
{
	struct netmap_adapter *na = kring->na;
	struct net_device *netdev = na->ifp;
	struct ptnet_info *pi = netdev_priv(netdev);
	bool notify;

	notify = netmap_pt_guest_rxsync(kring, flags);
	if (notify) {
		iowrite32(0, pi->ioaddr + PTNET_IO_RXKICK);
	}

	return 0;
}

static struct netmap_adapter ptnet_nm_ops = {
	.num_tx_desc = 1024,
	.num_rx_desc = 1024,
	.num_tx_rings = 1,
	.num_rx_rings = 1,
	.nm_register = ptnet_nm_register_native,
	.nm_config = ptnet_nm_config,
	.nm_txsync = ptnet_nm_txsync,
	.nm_rxsync = ptnet_nm_rxsync,
};

/*
 * ptnet_probe - Device Initialization Routine
 * @pdev: PCI device information struct
 * @ent: entry in ptnet_pci_table
 *
 * Returns 0 on success, negative on failure
 *
 * ptnet_probe initializes an pi identified by a pci_dev structure.
 * The OS initialization, configuring of the pi private structure,
 * and a hardware reset occur.
 */
static int
ptnet_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct net_device *netdev;
	struct netmap_adapter na_arg;
	struct ptnet_info *pi;
	uint8_t macaddr[6];
	uint32_t macreg;
	int bars;
	int err;

	bars = pci_select_bars(pdev, IORESOURCE_MEM | IORESOURCE_IO);
	err = pci_enable_device(pdev);
	if (err) {
		return err;
	}

	err = pci_request_selected_regions(pdev, bars, DRV_NAME);
	if (err) {
		goto err_pci_reg;
	}

	pci_set_master(pdev);
	err = pci_save_state(pdev);
	if (err) {
		goto err_alloc_etherdev;
	}

	err = -ENOMEM;
	netdev = alloc_etherdev(sizeof(struct ptnet_info));
	if (!netdev) {
		goto err_alloc_etherdev;
	}

	/* Cross-link data structures. */
	SET_NETDEV_DEV(netdev, &pdev->dev);
	pci_set_drvdata(pdev, netdev);
	pi = netdev_priv(netdev);
	pi->netdev = netdev;
	pi->pdev = pdev;
	pi->bars = bars;

	err = -EIO;
	pr_info("IO BAR (registers): start 0x%llx, len %llu, flags 0x%lx\n",
		pci_resource_start(pdev, PTNETMAP_IO_PCI_BAR),
		pci_resource_len(pdev, PTNETMAP_IO_PCI_BAR),
		pci_resource_flags(pdev, PTNETMAP_IO_PCI_BAR));

	pi->ioaddr = pci_iomap(pdev, PTNETMAP_IO_PCI_BAR, 0);
	if (!pi->ioaddr) {
		goto err_ptfeat;
	}

	/* Check if we are supported by the hypervisor. If not,
	 * bail out immediately. */
	iowrite32(NET_PTN_FEATURES_BASE | NET_PTN_FEATURES_VNET_HDR,
		  pi->ioaddr + PTNET_IO_PTFEAT);
	pi->ptfeatures = ioread32(pi->ioaddr + PTNET_IO_PTFEAT);
	if (!(pi->ptfeatures & NET_PTN_FEATURES_BASE)) {
		pr_err("Hypervisor doesn't support netmap passthrough\n");
		goto err_ptfeat;
	}

#ifndef PTNET_CSB_ALLOC
	/* Map the CSB memory exposed by the device. We don't use
	 * pci_ioremap_bar(), since we want the ioremap_cache() function
	 * to be called internally, rather than ioremap_nocache(). */
	pr_info("MEMORY BAR (CSB): start 0x%llx, len %llu, flags 0x%lx\n",
		pci_resource_start(pdev, PTNETMAP_MEM_PCI_BAR),
		pci_resource_len(pdev, PTNETMAP_MEM_PCI_BAR),
		pci_resource_flags(pdev, PTNETMAP_MEM_PCI_BAR));
	pi->csbaddr = ioremap_cache(pci_resource_start(pdev, PTNETMAP_MEM_PCI_BAR),
				    pci_resource_len(pdev, PTNETMAP_MEM_PCI_BAR));
	if (!pi->csbaddr)
		goto err_ptfeat;
	pi->csb = (struct paravirt_csb *)pi->csbaddr;

#else  /* PTNET_CSB_ALLOC */

	/* Alloc the CSB here and tell the hypervisor its physical address. */
	pi->csb = kzalloc(sizeof(struct paravirt_csb), GFP_KERNEL);
	if (!pi->csb) {
		goto err_ptfeat;
	}

	{
		phys_addr_t paddr = virt_to_phys(pi->csb);

		/* CSB allocation protocol. Write CSBBAH first, then
		 * CSBBAL. */
		iowrite32((paddr >> 32) & 0xffffffff,
			  pi->ioaddr + PTNET_IO_CSBBAH);
		iowrite32(paddr & 0xffffffff,
			  pi->ioaddr + PTNET_IO_CSBBAL);
	}
#endif /* PTNET_CSB_ALLOC */

	/* useless, to be removed */
	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		goto err_irqs;
	}

	netdev->netdev_ops = &ptnet_netdev_ops;
	netif_napi_add(netdev, &pi->napi, ptnet_rx_poll, NAPI_POLL_WEIGHT);

	strncpy(netdev->name, pci_name(pdev), sizeof(netdev->name) - 1);

	/* Read MAC address from device and put it into the netdev struct. */
	macreg = ioread32(pi->ioaddr + PTNET_IO_MAC_HI);
	macaddr[0] = (macreg >> 8) & 0xff;
	macaddr[1] = macreg & 0xff;
	macreg = ioread32(pi->ioaddr + PTNET_IO_MAC_LO);
	macaddr[2] = (macreg >> 24) & 0xff;
	macaddr[3] = (macreg >> 16) & 0xff;
	macaddr[4] = (macreg >> 8) & 0xff;
	macaddr[5] = macreg & 0xff;
	memcpy(netdev->dev_addr, macaddr, netdev->addr_len);

	netdev->features = NETIF_F_HIGHDMA;

	if (pi->ptfeatures & NET_PTN_FEATURES_VNET_HDR) {
		netdev->hw_features |= NETIF_F_HW_CSUM
				       | NETIF_F_SG
				       | NETIF_F_TSO
				       | NETIF_F_UFO
				       | NETIF_F_TSO_ECN
				       | NETIF_F_TSO6;
		netdev->features |= netdev->hw_features
				    | NETIF_F_RXCSUM
				    | NETIF_F_GSO_ROBUST;
	}

	device_set_wakeup_enable(&pi->pdev->dev, 0);

	//synchronize_irq(pi->pdev->irq);
	err = ptnet_irqs_init(pi);
	if (err) {
		goto err_irqs;
	}

	strcpy(netdev->name, "eth%d");
	err = register_netdev(netdev);
	if (err)
		goto err_netreg;

	/* Attach a guest pass-through netmap adapter to this device. */
	na_arg = ptnet_nm_ops;
	na_arg.ifp = pi->netdev;
	netmap_pt_guest_attach(&na_arg, &ptnet_nm_pt_guest_ops);
	/* Now a netmap adapter for this device has been allocated, and it
	 * can be accessed through NA(ifp). We have to initialize the CSB
	 * pointer. */
	pi->ptna = (struct netmap_pt_guest_adapter *)NA(pi->netdev);
	pi->ptna->csb = pi->csb;

	/* This is not-NULL when the network interface is up, ready to be
	 * used by the kernel stack. When NULL, the interface can be
	 * opened in netmap mode. */
	pi->nm_priv = NULL;

	netif_carrier_on(netdev);

	pr_info("%s: %p\n", __func__, pi);

	return 0;

	pr_info("%s: failed\n", __func__);
err_netreg:
	ptnet_irqs_fini(pi);
err_irqs:
#ifndef  PTNET_CSB_ALLOC
	iounmap(pi->csbaddr);
#else  /* PTNET_CSB_ALLOC */
	kfree(pi->csb);
#endif /* PTNET_CSB_ALLOC */
err_ptfeat:
	iounmap(pi->ioaddr);
	free_netdev(netdev);
err_alloc_etherdev:
	pci_release_selected_regions(pdev, bars);
err_pci_reg:
	pci_disable_device(pdev);
	return err;
}

/*
 * ptnet_remove - Device Removal Routine
 * @pdev: PCI device information struct
 *
 * ptnet_remove is called by the PCI subsystem to alert the driver
 * that it should release a PCI device.  The could be caused by a
 * Hot-Plug event, or because the driver is going to be removed from
 * memory.
 */
static void
ptnet_remove(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct ptnet_info *pi = netdev_priv(netdev);

	netif_carrier_off(netdev);

	netmap_detach(netdev);

	unregister_netdev(netdev);

	ptnet_irqs_fini(pi);

	iounmap(pi->ioaddr);
#ifndef  PTNET_CSB_ALLOC
	iounmap(pi->csbaddr);
#else  /* !PTNET_CSB_ALLOC */
	iowrite32(0, pi->ioaddr + PTNET_IO_CSBBAH);
	iowrite32(0, pi->ioaddr + PTNET_IO_CSBBAL);
	kfree(pi->csb);
#endif /* !PTNET_CSB_ALLOC */
	pci_release_selected_regions(pdev, pi->bars);
	free_netdev(netdev);
	pci_disable_device(pdev);

	pr_info("%s: %p\n", __func__, pi);
}

static void
ptnet_shutdown(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);

	netif_device_detach(netdev);

	if (netif_running(netdev)) {
		ptnet_close(netdev);
	}

	pci_disable_device(pdev);
}

/* PCI Device ID Table */
static const struct pci_device_id ptnet_pci_table[] = {
        {PCI_DEVICE(PTNETMAP_PCI_VENDOR_ID, PTNETMAP_PCI_NETIF_ID), 0, 0, 0},
	/* required last entry */
	{0,}
};

MODULE_DEVICE_TABLE(pci, ptnet_pci_table);

static struct pci_driver ptnet_driver = {
	.name     = DRV_NAME,
	.id_table = ptnet_pci_table,
	.probe    = ptnet_probe,
	.remove   = ptnet_remove,
	.shutdown = ptnet_shutdown,
};

/*
 * ptnet_init - Driver Registration Routine
 *
 * ptnet_init is the first routine called when netmap is
 * loaded. All it does is register with the PCI subsystem.
 */
int
ptnet_init(void)
{
	pr_info("%s - version %s\n", "Passthrough netmap interface driver",
		DRV_VERSION);
	pr_info("%s\n", "Copyright (c) 2015 Vincenzo Maffione");

	return pci_register_driver(&ptnet_driver);
}

/*
 * ptnet_exit - Driver Exit Cleanup Routine
 *
 * ptnet_exit is called just before netmap module is removed
 * from memory.
 */
void
ptnet_fini(void)
{
	pci_unregister_driver(&ptnet_driver);
}
