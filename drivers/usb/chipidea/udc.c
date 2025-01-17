/*
 * udc.c - ChipIdea UDC driver
 *
 * Copyright (C) 2008 Chipidea - MIPS Technologies, Inc. All rights reserved.
 *
 * Author: David Lopo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmapool.h>
#include <linux/err.h>
#include <linux/irqreturn.h>
#include <linux/ratelimit.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/otg.h>
#include <linux/usb/chipidea.h>
#include <linux/usb/msm_hsusb.h>
#include <linux/tracepoint.h>
#include <linux/qcom/usb_trace.h>

#include "ci.h"
#include "udc.h"
#include "bits.h"
#include "debug.h"

#define USB_MAX_TIMEOUT		25 /* 25msec timeout */
#define REMOTE_WAKEUP_DELAY	msecs_to_jiffies(200)
#define EP_PRIME_CHECK_DELAY	(jiffies + msecs_to_jiffies(1000))
#define MAX_PRIME_CHECK_RETRY	3 /*Wait for 3sec for EP prime failure */

/* Turns on streaming. overrides CI13XXX_DISABLE_STREAMING */
static unsigned int streaming;
module_param(streaming, uint, S_IRUGO | S_IWUSR);

/* control endpoint description */
static const struct usb_endpoint_descriptor
ctrl_endpt_out_desc = {
	.bLength         = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_OUT,
	.bmAttributes    = USB_ENDPOINT_XFER_CONTROL,
	.wMaxPacketSize  = cpu_to_le16(CTRL_PAYLOAD_MAX),
};

static const struct usb_endpoint_descriptor
ctrl_endpt_in_desc = {
	.bLength         = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes    = USB_ENDPOINT_XFER_CONTROL,
	.wMaxPacketSize  = cpu_to_le16(CTRL_PAYLOAD_MAX),
};

static struct ci13xxx_ebi_err_data *ebi_err_data;

/**
 * hw_ep_bit: calculates the bit number
 * @num: endpoint number
 * @dir: endpoint direction
 *
 * This function returns bit number
 */
static inline int hw_ep_bit(int num, int dir)
{
	return num + (dir ? 16 : 0);
}

static inline int ep_to_bit(struct ci_hdrc *ci, int n)
{
	int fill = 16 - ci->hw_ep_max / 2;

	if (n >= ci->hw_ep_max / 2)
		n += fill;

	return n;
}

/**
 * hw_device_state: enables/disables interrupts (execute without interruption)
 * @dma: 0 => disable, !0 => enable and set dma engine
 *
 * This function returns an error code
 */
static int hw_device_state(struct ci_hdrc *ci, u32 dma)
{
	if (dma) {
		if (streaming ||
		    !(ci->platdata->flags & CI13XXX_DISABLE_STREAMING))
			hw_write(ci, OP_USBMODE, USBMODE_CI_SDIS, 0);
		else
			hw_write(ci, OP_USBMODE, USBMODE_CI_SDIS,
					USBMODE_CI_SDIS);

		hw_write(ci, OP_ENDPTLISTADDR, ~0, dma);

		if (ci->ci_driver->notify_event)
			ci->udc_driver->notify_event(ci,
				CI13XXX_CONTROLLER_CONNECT_EVENT);

		/* interrupt, error, port change, reset, sleep/suspend */
		hw_write(ci, OP_USBINTR, ~0,
			     USBi_UI|USBi_UEI|USBi_PCI|USBi_URI|USBi_SLI);
	} else {
		hw_write(ci, OP_USBINTR, ~0, 0);
	}
	return 0;
}

static void debug_ept_flush_info(struct ci13xxx *ci, int ep_num, int dir)
{
	struct ci13xxx_ep *mep;

	if (dir)
		mep = &ci->ci13xxx_ep[ep_num + hw_ep_max/2];
	else
		mep = &ci->ci13xxx_ep[ep_num];

	pr_err_ratelimited("USB Registers\n");
	pr_err_ratelimited("USBCMD:%x\n", hw_read(ci, OP_USBCMD, ~0));
	pr_err_ratelimited("USBSTS:%x\n", hw_read(ci, OP_USBSTS, ~0));
	pr_err_ratelimited("ENDPTLISTADDR:%x\n",
			hw_cread(CAP_ENDPTLISTADDR, ~0));
	pr_err_ratelimited("PORTSC:%x\n", hw_read(ci, OP_PORTSC, ~0));
	pr_err_ratelimited("USBMODE:%x\n", hw_read(ci, OP_USBMODE, ~0));
	pr_err_ratelimited("ENDPTSTAT:%x\n", hw_read(ci, OP_ENDPTSTAT, ~0));

	dbg_usb_op_fail(0xFF, "FLUSHF", mep);
}

/**
 * hw_ep_flush: flush endpoint fifo (execute without interruption)
 * @num: endpoint number
 * @dir: endpoint direction
 *
 * This function returns an error code
 */
static int hw_ep_flush(struct ci_hdrc *ci, int num, int dir)
{
	ktime_t start, diff;
	int n = hw_ep_bit(num, dir);
	struct ci13xxx_ep *mEp = &ci->ci13xxx_ep[n];

	/* Flush ep0 even when queue is empty */
	if (ci->skip_flush || (num && list_empty(&mEp->qh.queue)))
		return 0;

	start = ktime_get();
	do {
		/* flush any pending transfer */
		hw_write(ci, OP_ENDPTFLUSH, BIT(n), BIT(n));
		while (hw_read(ci, OP_ENDPTFLUSH, BIT(n))) {
			cpu_relax();
			diff = ktime_sub(ktime_get(), start);
			if (ktime_to_ms(diff) > USB_MAX_TIMEOUT) {
				printk_ratelimited(KERN_ERR
					"%s: Failed to flush ep#%d %s\n",
					__func__, num,
					dir ? "IN" : "OUT");
				debug_ept_flush_info(num, dir);
				ci->skip_flush = true;
				return 0;
			}
	} while (hw_read(ci, OP_ENDPTSTAT, BIT(n)));

	return 0;
}

/**
 * hw_ep_disable: disables endpoint (execute without interruption)
 * @num: endpoint number
 * @dir: endpoint direction
 *
 * This function returns an error code
 */
static int hw_ep_disable(struct ci_hdrc *ci, int num, int dir)
{
	hw_write(ci, OP_ENDPTCTRL + num,
		 dir ? ENDPTCTRL_TXE : ENDPTCTRL_RXE, 0);
	return 0;
}

/**
 * hw_ep_enable: enables endpoint (execute without interruption)
 * @num:  endpoint number
 * @dir:  endpoint direction
 * @type: endpoint type
 *
 * This function returns an error code
 */
static int hw_ep_enable(struct ci_hdrc *ci, int num, int dir, int type)
{
	u32 mask, data;

	if (dir) {
		mask  = ENDPTCTRL_TXT;  /* type    */
		data  = type << __ffs(mask);

		mask |= ENDPTCTRL_TXS;  /* unstall */
		mask |= ENDPTCTRL_TXR;  /* reset data toggle */
		data |= ENDPTCTRL_TXR;
		mask |= ENDPTCTRL_TXE;  /* enable  */
		data |= ENDPTCTRL_TXE;
	} else {
		mask  = ENDPTCTRL_RXT;  /* type    */
		data  = type << __ffs(mask);

		mask |= ENDPTCTRL_RXS;  /* unstall */
		mask |= ENDPTCTRL_RXR;  /* reset data toggle */
		data |= ENDPTCTRL_RXR;
		mask |= ENDPTCTRL_RXE;  /* enable  */
		data |= ENDPTCTRL_RXE;
	}
	hw_write(ci, OP_ENDPTCTRL + num, mask, data);

	/* make sure endpoint is enabled before returning */
	mb();
	return 0;
}

/**
 * hw_ep_get_halt: return endpoint halt status
 * @num: endpoint number
 * @dir: endpoint direction
 *
 * This function returns 1 if endpoint halted
 */
static int hw_ep_get_halt(struct ci_hdrc *ci, int num, int dir)
{
	u32 mask = dir ? ENDPTCTRL_TXS : ENDPTCTRL_RXS;

	return hw_read(ci, OP_ENDPTCTRL + num, mask) ? 1 : 0;
}

/**
 * hw_test_and_clear_setup_status: test & clear setup status (execute without
 *                                 interruption)
 * @n: endpoint number
 *
 * This function returns setup status
 */
static int hw_test_and_clear_setup_status(struct ci_hdrc *ci, int n)
{
	n = ep_to_bit(ci, n);
	return hw_test_and_clear(ci, OP_ENDPTSETUPSTAT, BIT(n));
}

/**
 * hw_ep_prime: primes endpoint (execute without interruption)
 * @num:     endpoint number
 * @dir:     endpoint direction
 * @is_ctrl: true if control endpoint
 *
 * This function returns an error code
 */
static int hw_ep_prime(struct ci_hdrc *ci, int num, int dir, int is_ctrl)
{
	int n = hw_ep_bit(num, dir);

	if (is_ctrl && dir == RX && hw_read(ci, OP_ENDPTSETUPSTAT, BIT(num)))
		return -EAGAIN;

	hw_write(ci, OP_ENDPTPRIME, BIT(n), BIT(n));

	if (is_ctrl && dir == RX && hw_read(ci, OP_ENDPTSETUPSTAT, BIT(num)))
		return -EAGAIN;

	/* status shoult be tested according with manual but it doesn't work */
	return 0;
}

/**
 * hw_ep_set_halt: configures ep halt & resets data toggle after clear (execute
 *                 without interruption)
 * @num:   endpoint number
 * @dir:   endpoint direction
 * @value: true => stall, false => unstall
 *
 * This function returns an error code
 */
static int hw_ep_set_halt(struct ci_hdrc *ci, int num, int dir, int value)
{
	if (value != 0 && value != 1)
		return -EINVAL;

	do {
		enum ci_hw_regs reg = OP_ENDPTCTRL + num;
		u32 mask_xs = dir ? ENDPTCTRL_TXS : ENDPTCTRL_RXS;
		u32 mask_xr = dir ? ENDPTCTRL_TXR : ENDPTCTRL_RXR;

		if (hw_read(ci, OP_ENDPTSETUPSTAT, BIT(num)))
			return 0;

		/* data toggle - reserved for EP0 but it's in ESS */
		hw_write(ci, reg, mask_xs|mask_xr,
			  value ? mask_xs : mask_xr);
	} while (value != hw_ep_get_halt(ci, num, dir));

	return 0;
}

/**
 * hw_is_port_high_speed: test if port is high speed
 *
 * This function returns true if high speed port
 */
static int hw_port_is_high_speed(struct ci_hdrc *ci)
{
	return ci->hw_bank.lpm ? hw_read(ci, OP_DEVLC, DEVLC_PSPD) :
		hw_read(ci, OP_PORTSC, PORTSC_HSP);
}

/**
 * hw_read_intr_enable: returns interrupt enable register
 *
 * This function returns register data
 */
static u32 hw_read_intr_enable(struct ci_hdrc *ci)
{
	return hw_read(ci, OP_USBINTR, ~0);
}

/**
 * hw_read_intr_status: returns interrupt status register
 *
 * This function returns register data
 */
static u32 hw_read_intr_status(struct ci_hdrc *ci)
{
	return hw_read(ci, OP_USBSTS, ~0);
}

/**
 * hw_test_and_clear_complete: test & clear complete status (execute without
 *                             interruption)
 * @n: endpoint number
 *
 * This function returns complete status
 */
static int hw_test_and_clear_complete(struct ci_hdrc *ci, int n)
{
	n = ep_to_bit(ci, n);
	return hw_test_and_clear(ci, OP_ENDPTCOMPLETE, BIT(n));
}

/**
 * hw_test_and_clear_intr_active: test & clear active interrupts (execute
 *                                without interruption)
 *
 * This function returns active interrutps
 */
static u32 hw_test_and_clear_intr_active(struct ci_hdrc *ci)
{
	u32 reg = hw_read_intr_status(ci) & hw_read_intr_enable(ci);

	hw_write(ci, OP_USBSTS, ~0, reg);
	return reg;
}

/**
 * hw_test_and_clear_setup_guard: test & clear setup guard (execute without
 *                                interruption)
 *
 * This function returns guard value
 */
static int hw_test_and_clear_setup_guard(struct ci_hdrc *ci)
{
	return hw_test_and_write(ci, OP_USBCMD, USBCMD_SUTW, 0);
}

/**
 * hw_test_and_set_setup_guard: test & set setup guard (execute without
 *                              interruption)
 *
 * This function returns guard value
 */
static int hw_test_and_set_setup_guard(struct ci_hdrc *ci)
{
	return hw_test_and_write(ci, OP_USBCMD, USBCMD_SUTW, USBCMD_SUTW);
}

/**
 * hw_usb_set_address: configures USB address (execute without interruption)
 * @value: new USB address
 *
 * This function explicitly sets the address, without the "USBADRA" (advance)
 * feature, which is not supported by older versions of the controller.
 */
static void hw_usb_set_address(struct ci_hdrc *ci, u8 value)
{
	hw_write(ci, OP_DEVICEADDR, DEVICEADDR_USBADR,
		 value << __ffs(DEVICEADDR_USBADR));
}

/**
 * hw_usb_reset: restart device after a bus reset (execute without
 *               interruption)
 *
 * This function returns an error code
 */
static int hw_usb_reset(struct ci_hdrc *ci)
{
	int delay_count = 10; /* 100 usec delay */

	hw_usb_set_address(ci, 0);

	/* ESS flushes only at end?!? */
	hw_write(ci, OP_ENDPTFLUSH,    ~0, ~0);

	/* clear setup token semaphores */
	hw_write(ci, OP_ENDPTSETUPSTAT, 0,  0);

	/* clear complete status */
	hw_write(ci, OP_ENDPTCOMPLETE,  0,  0);

	/* wait until all bits cleared */
	while (delay_count-- && hw_read(ci, OP_ENDPTPRIME, ~0))
		udelay(10);
	if (delay_count < 0)
		pr_err("ENDPTPRIME is not cleared during bus reset\n");

	/* reset all endpoints ? */

	/* reset internal status and wait for further instructions
	   no need to verify the port reset status (ESS does it) */

	return 0;
}

static void dump_usb_info(void *ignore, unsigned int ebi_addr,
	unsigned int ebi_apacket0, unsigned int ebi_apacket1)
{
	struct ci13xxx *udc = _udc;
	unsigned long flags;
	struct list_head   *ptr = NULL;
	struct ci13xxx_req *req = NULL;
	struct ci13xxx_ep *mEp;
	unsigned i;
	struct ci13xxx_ebi_err_entry *temp_dump;
	static int count;
	u32 epdir = 0;

	if (count)
		return;
	count++;

	pr_info("%s: USB EBI error detected\n", __func__);

	ebi_err_data = kmalloc(sizeof(struct ci13xxx_ebi_err_data),
				 GFP_ATOMIC);
	if (!ebi_err_data) {
		pr_err("%s: memory alloc failed for ebi_err_data\n", __func__);
		return;
	}

	ebi_err_data->ebi_err_entry = kmalloc(
					sizeof(struct ci13xxx_ebi_err_entry),
					GFP_ATOMIC);
	if (!ebi_err_data->ebi_err_entry) {
		kfree(ebi_err_data);
		pr_err("%s: memory alloc failed for ebi_err_entry\n", __func__);
		return;
	}

	ebi_err_data->ebi_err_addr = ebi_addr;
	ebi_err_data->apkt0 = ebi_apacket0;
	ebi_err_data->apkt1 = ebi_apacket1;

	temp_dump = ebi_err_data->ebi_err_entry;
	pr_info("\n DUMPING USB Requests Information\n");
	spin_lock_irqsave(udc->lock, flags);
	for (i = 0; i < hw_ep_max; i++) {
		list_for_each(ptr, &udc->ci13xxx_ep[i].qh.queue) {
			mEp = &udc->ci13xxx_ep[i];
			req = list_entry(ptr, struct ci13xxx_req, queue);

			temp_dump->usb_req_buf = req->req.buf;
			temp_dump->usb_req_length = req->req.length;
			epdir = mEp->dir;
			temp_dump->ep_info = mEp->num | (epdir << 15);

			temp_dump->next = kmalloc(
					  sizeof(struct ci13xxx_ebi_err_entry),
					  GFP_ATOMIC);
			if (!temp_dump->next) {
				pr_err("%s: memory alloc failed\n", __func__);
				spin_unlock_irqrestore(udc->lock, flags);
				return;
			}
			temp_dump = temp_dump->next;
		}
	}
	spin_unlock_irqrestore(udc->lock, flags);
}

/******************************************************************************
 * UTIL block
 *****************************************************************************/

static int add_td_to_list(struct ci_hw_ep *hwep, struct ci_hw_req *hwreq,
			  unsigned length)
{
	int i;
	u32 temp;
	struct td_node *lastnode, *node = kzalloc(sizeof(struct td_node),
						  GFP_ATOMIC);

	if (node == NULL)
		return -ENOMEM;

	node->ptr = dma_pool_alloc(hwep->td_pool, GFP_ATOMIC,
				   &node->dma);
	if (node->ptr == NULL) {
		kfree(node);
		return -ENOMEM;
	}

	memset(node->ptr, 0, sizeof(struct ci_hw_td));
	node->ptr->token = cpu_to_le32(length << __ffs(TD_TOTAL_BYTES));
	node->ptr->token &= cpu_to_le32(TD_TOTAL_BYTES);
	node->ptr->token |= cpu_to_le32(TD_STATUS_ACTIVE);

	temp = (u32) (hwreq->req.dma + hwreq->req.actual);
	if (length) {
		node->ptr->page[0] = cpu_to_le32(temp);
		for (i = 1; i < TD_PAGE_COUNT; i++) {
			u32 page = temp + i * CI_HDRC_PAGE_SIZE;
			page &= ~TD_RESERVED_MASK;
			node->ptr->page[i] = cpu_to_le32(page);
		}
	}

	hwreq->req.actual += length;

	if (!list_empty(&hwreq->tds)) {
		/* get the last entry */
		lastnode = list_entry(hwreq->tds.prev,
				struct td_node, td);
		lastnode->ptr->next = cpu_to_le32(node->dma);
	}

	INIT_LIST_HEAD(&node->td);
	list_add_tail(&node->td, &hwreq->tds);

	return 0;
}

/**
 * _usb_addr: calculates endpoint address from direction & number
 * @ep:  endpoint
 */
static inline u8 _usb_addr(struct ci_hw_ep *ep)
{
	return ((ep->dir == TX) ? USB_ENDPOINT_DIR_MASK : 0) | ep->num;
}

static void ep_prime_timer_func(unsigned long data)
{
	struct ci13xxx_ep *mEp = (struct ci13xxx_ep *)data;
	struct ci13xxx_req *req;
	struct list_head *ptr = NULL;
	int n = hw_ep_bit(mEp->num, mEp->dir);
	unsigned long flags;

	spin_lock_irqsave(mEp->lock, flags);

	if (_udc && (!_udc->vbus_active || _udc->suspended)) {
		pr_debug("ep%d%s prime timer when vbus_active=%d,suspend=%d\n",
			mep->num, mep->dir ? "IN" : "OUT",
			_udc->vbus_active, _udc->suspended);
		goto out;
	}

	if (!hw_cread(CAP_ENDPTPRIME, BIT(n)))
		goto out;

	if (list_empty(&mEp->qh.queue))
		goto out;

	req = list_entry(mEp->qh.queue.next, struct ci13xxx_req, queue);

	mb();
	if (!(TD_STATUS_ACTIVE & req->ptr->token))
		goto out;

	mEp->prime_timer_count++;
	if (mEp->prime_timer_count == MAX_PRIME_CHECK_RETRY) {
		mEp->prime_timer_count = 0;
		pr_info("ep%d dir:%s QH:cap:%08x cur:%08x next:%08x tkn:%08x\n",
				mEp->num, mEp->dir ? "IN" : "OUT",
				mEp->qh.ptr->cap, mEp->qh.ptr->curr,
				mEp->qh.ptr->td.next, mEp->qh.ptr->td.token);
		list_for_each(ptr, &mEp->qh.queue) {
			req = list_entry(ptr, struct ci13xxx_req, queue);
			pr_info("\treq:%08xnext:%08xtkn:%08xpage0:%08xsts:%d\n",
					req->dma, req->ptr->next,
					req->ptr->token, req->ptr->page[0],
					req->req.status);
		}
		dbg_usb_op_fail(0xFF, "PRIMEF", mEp);
		mEp->prime_fail_count++;
	} else {
		mod_timer(&mEp->prime_timer, EP_PRIME_CHECK_DELAY);
	}

	spin_unlock_irqrestore(mEp->lock, flags);
	return;

out:
	mEp->prime_timer_count = 0;
	spin_unlock_irqrestore(mEp->lock, flags);

}

/**
 * _hardware_queue: configures a request at hardware level
 * @gadget: gadget
 * @hwep:    endpoint
 *
 * This function returns an error code
 */
static int _hardware_enqueue(struct ci_hw_ep *hwep, struct ci_hw_req *hwreq)
{
	struct ci_hdrc *ci = hwep->ci;
	int ret = 0;
	unsigned rest = hwreq->req.length;
	int pages = TD_PAGE_COUNT;
	struct td_node *firstnode, *lastnode;

	/* don't queue twice */
	if (hwreq->req.status == -EALREADY)
		return -EALREADY;

	hwreq->req.status = -EALREADY;
	ret = usb_gadget_map_request(&ci->gadget, &hwreq->req, hwep->dir);
	if (ret)
		return ret;

	/*
	 * The first buffer could be not page aligned.
	 * In that case we have to span into one extra td.
	 */
	if (hwreq->req.dma % PAGE_SIZE)
		pages--;

	if (rest == 0)
		add_td_to_list(hwep, hwreq, 0);

	while (rest > 0) {
		unsigned count = min(hwreq->req.length - hwreq->req.actual,
					(unsigned)(pages * CI_HDRC_PAGE_SIZE));
		add_td_to_list(hwep, hwreq, count);
		rest -= count;
	}
	/* MSM Specific: updating the request as required for
	 * SPS mode. Enable MSM proprietary DMA engine acording
	 * to the UDC private data in the request.
	 */
	if (CI13XX_REQ_VENDOR_ID(hwreq->req.udc_priv) == MSM_VENDOR_ID) {	// TODO: djStolen; MSM specific propriatery DMA
		if (hwreq->req.udc_priv & MSM_SPS_MODE) {
			hwreq->ptr->token = TD_STATUS_ACTIVE;
			if (hwreq->req.udc_priv & MSM_IS_FINITE_TRANSFER)
				hwreq->ptr->next = TD_TERMINATE;
			else
				//hwreq->ptr->next = MSM_ETD_TYPE | hwreq->dma;
				hwreq->ptr->next = MSM_ETD_TYPE | hwreq->req.dma;		// TODO: djStolen; MSM specific propriatery DMA; ->dma is now missing directly in hwreq; it was previously there in mReq
			if (!hwreq->req.no_interrupt)
				hwreq->ptr->token |= MSM_ETD_IOC;
		}
		hwreq->req.dma = 0;
	}

	if (hwreq->req.zero && hwreq->req.length
	    && (hwreq->req.length % hwep->ep.maxpacket == 0))
		add_td_to_list(hwep, hwreq, 0);

	firstnode = list_first_entry(&hwreq->tds, struct td_node, td);

	lastnode = list_entry(hwreq->tds.prev,
		struct td_node, td);

	lastnode->ptr->next = cpu_to_le32(TD_TERMINATE);
	if (!hwreq->req.no_interrupt)
		lastnode->ptr->token |= cpu_to_le32(TD_IOC);

	wmb();

	/* Remote Wakeup */		// TODO: djStolen; This whole chunk is possibly also MSM specific
	if (ci->suspended) {
		if (!ci->remote_wakeup) {
			hwreq->req.status = -EAGAIN;
			dev_dbg(mEp->device, "%s: queue failed (suspend) ept #%d\n",
				__func__, mEp->num);
			return -EAGAIN;
		}
		usb_phy_set_suspend(ci->transceiver, 0);
		schedule_delayed_work(&ci->rw_work, REMOTE_WAKEUP_DELAY);
	}

	hwreq->req.actual = 0;
	if (!list_empty(&hwep->qh.queue)) {
		struct ci_hw_req *hwreqprev;
		int n = hw_ep_bit(hwep->num, hwep->dir);
		int tmp_stat;
		struct td_node *prevlastnode;
		u32 next = firstnode->dma & TD_ADDR_MASK;

		hwreqprev = list_entry(hwep->qh.queue.prev,
				struct ci_hw_req, queue);
		prevlastnode = list_entry(hwreqprev->tds.prev,
				struct td_node, td);

		prevlastnode->ptr->next = cpu_to_le32(next);
		wmb();
		if (hw_read(ci, OP_ENDPTPRIME, BIT(n)))
			goto done;
		start = ktime_get();
		do {
			hw_write(ci, OP_USBCMD, USBCMD_ATDTW, USBCMD_ATDTW);
			tmp_stat = hw_read(ci, OP_ENDPTSTAT, BIT(n));
			diff = ktime_sub(ktime_get(), start);
			/* poll for max. 100ms */
			if (ktime_to_ms(diff) > USB_MAX_TIMEOUT) {
				if (hw_read(ci, OP_USBCMD, USBCMD_ATDTW))
					break;
				printk_ratelimited(KERN_ERR
				"%s:queue failed ep#%d %s\n",
				 __func__, mEp->num, mEp->dir ? "IN" : "OUT");
				return -EAGAIN;
			}
		} while (!hw_read(ci, OP_USBCMD, USBCMD_ATDTW));
		hw_write(ci, OP_USBCMD, USBCMD_ATDTW, 0);
		if (tmp_stat)
			goto done;
	}

	/*  QH configuration */				// TODO: djStolen; this whole chunk till function end is completely different
	if (!list_empty(&mEp->qh.queue)) {
		struct ci13xxx_req *hwreq = \
			list_entry(mEp->qh.queue.next,
				   struct ci13xxx_req, queue);

		if (TD_STATUS_ACTIVE & hwreq->ptr->token) {
			mEp->qh.ptr->td.next   = hwreq->req.dma;
			mEp->qh.ptr->td.token &= ~TD_STATUS;
			goto prime;
		}
	}

	if (CI13XX_REQ_VENDOR_ID(hwreq->req.udc_priv) == MSM_VENDOR_ID) {
		if (hwreq->req.udc_priv & MSM_SPS_MODE) {
			mEp->qh.ptr->td.next   |= MSM_ETD_TYPE;
			i = hw_read(ci, OP_ENDPTPIPEID +
						 mEp->num * sizeof(u32), ~0);
			/* Read current value of this EPs pipe id */
			i = (mEp->dir == TX) ?
				((i >> MSM_TX_PIPE_ID_OFS) & MSM_PIPE_ID_MASK) :
					(i & MSM_PIPE_ID_MASK);
			/* If requested pipe id is different from current,
			   then write it */
			if (i != (hwreq->req.udc_priv & MSM_PIPE_ID_MASK)) {
				if (mEp->dir == TX)
					hw_write(ci, OP_ENDPTPIPEID +
							mEp->num * sizeof(u32),
						MSM_PIPE_ID_MASK <<
							MSM_TX_PIPE_ID_OFS,
						(hwreq->req.udc_priv &
						 MSM_PIPE_ID_MASK)
							<< MSM_TX_PIPE_ID_OFS);
				else
					hw_write(ci, OP_ENDPTPIPEID +
							mEp->num * sizeof(u32),
						MSM_PIPE_ID_MASK,
						hwreq->req.udc_priv &
							MSM_PIPE_ID_MASK);
			}
		}
	}

	mEp->qh.ptr->td.next   = cpu_to_le32(hwreq->req.dma);    /* TERMINATE = 0 */
	mEp->qh.ptr->td.token &=
		cpu_to_le32(~(TD_STATUS_HALTED|TD_STATUS_ACTIVE));

prime:
	wmb();   /* synchronize before ep prime */

	ret = hw_ep_prime(ci, hwep->num, hwep->dir,
			   hwep->type == USB_ENDPOINT_XFER_CONTROL);
	if (!ret)
		mod_timer(&mEp->prime_timer, EP_PRIME_CHECK_DELAY);
done:
	return ret;
}

/*
 * free_pending_td: remove a pending request for the endpoint
 * @hwep: endpoint
 */
static void free_pending_td(struct ci_hw_ep *hwep)
{
	struct td_node *pending = hwep->pending_td;

	dma_pool_free(hwep->td_pool, pending->ptr, pending->dma);
	hwep->pending_td = NULL;
	kfree(pending);
}

/**
 * _hardware_dequeue: handles a request at hardware level
 * @gadget: gadget
 * @hwep:   endpoint
 *
 * This function returns an error code
 */
static int _hardware_dequeue(struct ci_hw_ep *hwep, struct ci_hw_req *hwreq)
{
	u32 tmptoken;
	struct td_node *node, *tmpnode;
	unsigned remaining_length;
	unsigned actual = hwreq->req.length;

	if (hwreq->req.status != -EALREADY)
		return -EINVAL;

	/* clean speculative fetches on req->ptr->token */
	mb();

	hwreq->req.status = 0;

	if (CI13XX_REQ_VENDOR_ID(hwreq->req.udc_priv) == MSM_VENDOR_ID)
		if ((hwreq->req.udc_priv & MSM_SPS_MODE) &&
			(hwreq->req.udc_priv & MSM_IS_FINITE_TRANSFER))
			return -EBUSY;
	list_for_each_entry_safe(node, tmpnode, &hwreq->tds, td) {
		tmptoken = le32_to_cpu(node->ptr->token);
		if ((TD_STATUS_ACTIVE & tmptoken) != 0) {
			hwreq->req.status = -EALREADY;
			return -EBUSY;
		}

		remaining_length = (tmptoken & TD_TOTAL_BYTES);
		remaining_length >>= __ffs(TD_TOTAL_BYTES);
		actual -= remaining_length;

		hwreq->req.status = tmptoken & TD_STATUS;
		if ((TD_STATUS_HALTED & hwreq->req.status)) {
			hwreq->req.status = -EPIPE;
			break;
		} else if ((TD_STATUS_DT_ERR & hwreq->req.status)) {
			hwreq->req.status = -EPROTO;
			break;
		} else if ((TD_STATUS_TR_ERR & hwreq->req.status)) {
			hwreq->req.status = -EILSEQ;
			break;
		}

		if (remaining_length) {
			if (hwep->dir) {
				hwreq->req.status = -EPROTO;
				break;
			}
		}
		/*
		 * As the hardware could still address the freed td
		 * which will run the udc unusable, the cleanup of the
		 * td has to be delayed by one.
		 */
		if (hwep->pending_td)
			free_pending_td(hwep);

		hwep->pending_td = node;
		list_del_init(&node->td);
	}

	usb_gadget_unmap_request(&hwep->ci->gadget, &hwreq->req, hwep->dir);

	hwreq->req.actual += actual;

	if (hwreq->req.status)
		return hwreq->req.status;

	return hwreq->req.actual;
}

/**
 * restore_original_req: Restore original req's attributes
 * @mReq: Request
 *
 * This function restores original req's attributes.  Call
 * this function before completing the large req (>16K).
 */
static void restore_original_req(struct ci13xxx_req *mReq)
{
	mReq->req.buf = mReq->multi.buf;
	mReq->req.length = mReq->multi.len;
	if (!mReq->req.status)
		mReq->req.actual = mReq->multi.actual;

	mReq->multi.len = 0;
	mReq->multi.actual = 0;
	mReq->multi.buf = NULL;
}

/**
 * _ep_nuke: dequeues all endpoint requests
 * @hwep: endpoint
 *
 * This function returns an error code
 * Caller must hold lock
 */
static int _ep_nuke(struct ci_hw_ep *hwep)
__releases(hwep->lock)
__acquires(hwep->lock)
{
	struct ci13xxx_ep *mEpTemp = mEp;
	unsigned val;
	struct td_node *node, *tmpnode;
	if (hwep == NULL)
		return -EINVAL;

	del_timer(&hwep->prime_timer);
	hwep->prime_timer_count = 0;

	hw_ep_flush(hwep->ci, hwep->num, hwep->dir);

	while (!list_empty(&hwep->qh.queue)) {

		/* pop oldest request */
		struct ci_hw_req *hwreq = list_entry(hwep->qh.queue.next,
						     struct ci_hw_req, queue);

		list_for_each_entry_safe(node, tmpnode, &hwreq->tds, td) {
			dma_pool_free(hwep->td_pool, node->ptr, node->dma);
			list_del_init(&node->td);
			node->ptr = NULL;
			kfree(node);
		}

		list_del_init(&hwreq->queue);

		/* MSM Specific: Clear end point proprietary register */
		if (CI13XX_REQ_VENDOR_ID(hwreq->req.udc_priv) == MSM_VENDOR_ID) {
			if (hwreq->req.udc_priv & MSM_SPS_MODE) {
				val = hw_read(hwep->ci, OP_ENDPTPIPEID +
						hwep->num * sizeof(u32), ~0);

				if (val != MSM_EP_PIPE_ID_RESET_VAL)
					hw_write(hwep->ci, OP_ENDPTPIPEID +
							hwep->num * sizeof(u32),
						~0, MSM_EP_PIPE_ID_RESET_VAL);
			}
		}
		hwreq->req.status = -ESHUTDOWN;

		usb_gadget_map_request(&hwep->ci->gadget, &hwreq->req, hwep->dir);

		if (hwep->multi_req) {
			restore_original_req(hwreq);
			hwep->multi_req = false;
		}

		if (hwreq->req.complete != NULL) {
			spin_unlock(hwep->lock);
			hwreq->req.complete(&hwep->ep, &hwreq->req);
			spin_lock(hwep->lock);
	}
	
	if (hwep->pending_td)
		free_pending_td(hwep);

	return 0;
}

/**
 * _gadget_stop_activity: stops all USB activity, flushes & disables all endpts
 * @gadget: gadget
 *
 * This function returns an error code
 */
static int _gadget_stop_activity(struct usb_gadget *gadget)
{
	struct ci_hdrc    *ci = container_of(gadget, struct ci_hdrc, gadget);
	unsigned long flags;

	spin_lock_irqsave(&ci->lock, flags);
	ci->gadget.speed = USB_SPEED_UNKNOWN;
	ci->remote_wakeup = 0;
	ci->suspended = 0;
	ci->configured = 0;
	spin_unlock_irqrestore(&ci->lock, flags);

	gadget->b_hnp_enable = 0;
	gadget->a_hnp_support = 0;
	gadget->host_request = 0;
	gadget->otg_srp_reqd = 0;

	if (ci->driver)
		ci->driver->disconnect(gadget);
	spin_lock_irqsave(ci->lock, flags);
	_ep_nuke(&ci->ep0out);
	_ep_nuke(&ci->ep0in);
	spin_unlock_irqrestore(ci->lock, flags);

	if (ci->ep0in.last_zptr) {
		dma_pool_free(ci->ep0in.td_pool, ci->ep0in.last_zptr,
				ci->ep0in.last_zdma);
		ci->ep0in.last_zptr = NULL;
	}

	return 0;
}

/******************************************************************************
 * ISR block
 *****************************************************************************/
/**
 * isr_reset_handler: USB reset interrupt handler
 * @ci: UDC device
 *
 * This function resets USB engine after a bus reset occurred
 */
static void isr_reset_handler(struct ci_hdrc *ci)
__releases(ci->lock)
__acquires(ci->lock)
{
	int retval;

	spin_unlock(&ci->lock);

	if (ci->suspended) {
		if (ci->platdata->notify_event)
			ci->platdata->notify_event(ci,
			CI13XXX_CONTROLLER_RESUME_EVENT);
		if (ci->transceiver)
			usb_phy_set_suspend(ci->transceiver, 0);
		ci->driver->resume(&ci->gadget);
		ci->suspended = 0;
	}

	/*stop charging upon reset */
	if (ci->transceiver)
		usb_phy_set_power(ci->transceiver, 100);

	retval = _gadget_stop_activity(&ci->gadget);
	if (retval)
		goto done;

	ci->skip_flush = false;
	retval = hw_usb_reset(ci);
	if (retval)
		goto done;

done:
	spin_lock(&ci->lock);

	if (retval)
		dev_err(ci->dev, "error: %i\n", retval);
}

/**
 * isr_get_status_complete: get_status request complete function
 * @ep:  endpoint
 * @req: request handled
 *
 * Caller must release lock
 */
static void isr_get_status_complete(struct usb_ep *ep, struct usb_request *req)
{
	if (ep == NULL || req == NULL)
		return;

	if (req->status)
		err("GET_STATUS failed");
}

/**
 * _ep_queue: queues (submits) an I/O request to an endpoint
 *
 * Caller must hold lock
 */
static int _ep_queue(struct usb_ep *ep, struct usb_request *req,
		    gfp_t __maybe_unused gfp_flags)
{
	struct ci_hw_ep  *hwep  = container_of(ep,  struct ci_hw_ep, ep);
	struct ci_hw_req *hwreq = container_of(req, struct ci_hw_req, req);
	struct ci_hdrc *ci = hwep->ci;
	int retval = 0;

	if (ep == NULL || req == NULL || hwep->ep.desc == NULL)
		return -EINVAL;

	if (hwep->type == USB_ENDPOINT_XFER_CONTROL) {
		if (req->length)
			hwep = (ci->ep0_dir == RX) ?
			       ci->ep0out : ci->ep0in;
		if (!list_empty(&hwep->qh.queue)) {
			_ep_nuke(hwep);
			retval = -EOVERFLOW;
			dev_warn(hwep->ci->dev, "endpoint ctrl %X nuked\n",
				 _usb_addr(hwep));
		}
	}

	if (usb_endpoint_xfer_isoc(hwep->ep.desc) &&
	    hwreq->req.length > (1 + hwep->ep.mult) * hwep->ep.maxpacket) {
		dev_err(hwep->ci->dev, "request length too big for isochronous\n");
		return -EMSGSIZE;
	}
	if (hwep->multi_req) {
		dev_err(hwep->ci->dev, "Large request is in progress. come again");
		return -EAGAIN;
	}

	/* first nuke then test link, e.g. previous status has not sent */
	if (!list_empty(&hwreq->queue)) {
		dev_err(hwep->ci->dev, "request already in queue\n");
		return -EBUSY;
	}

	hwep->multi_req = true;
	hwreq->multi.len = req->length;
	hwreq->multi.buf = req->buf;

	/* push request */
	hwreq->req.status = -EINPROGRESS;
	hwreq->req.actual = 0;

	retval = _hardware_enqueue(hwep, hwreq);

	if (retval == -EALREADY)
		retval = 0;
	if (!retval)
		list_add_tail(&hwreq->queue, &hwep->qh.queue);
	else if (hwep->multi_req)
		hwep->multi_req = false;

	return retval;
}

/**
 * isr_get_status_response: get_status request response
 * @ci: ci struct
 * @setup: setup request packet
 *
 * This function returns an error code
 */
static int isr_get_status_response(struct ci_hdrc *ci,
				   struct usb_ctrlrequest *setup)
__releases(hwep->lock)
__acquires(hwep->lock)
{
	struct ci_hw_ep *hwep = ci->ep0in;
	struct usb_request *req = ci->status;
	int dir, num, retval;

	if (hwep == NULL || setup == NULL)
		return -EINVAL;

	req->complete = isr_get_status_complete;
	req->length   = 2;
	req->buf      = ci->status_buf;

	if ((setup->bRequestType & USB_RECIP_MASK) == USB_RECIP_DEVICE) {
		if (setup->wIndex == OTG_STATUS_SELECTOR) {
			*((u8 *)req->buf) = ci->gadget.host_request <<
						HOST_REQUEST_FLAG;
			req->length = 1;
		} else {
			/* Assume that device is bus powered for now. */
			*((u16 *)req->buf) = ci->remote_wakeup << 1;
		}
		/* TODO: D1 - Remote Wakeup; D0 - Self Powered */
		retval = 0;
	} else if ((setup->bRequestType & USB_RECIP_MASK) \
		   == USB_RECIP_ENDPOINT) {
		dir = (le16_to_cpu(setup->wIndex) & USB_ENDPOINT_DIR_MASK) ?
			TX : RX;
		num =  le16_to_cpu(setup->wIndex) & USB_ENDPOINT_NUMBER_MASK;
		*(u16 *)req->buf = hw_ep_get_halt(ci, num, dir);
	}
	/* else do nothing; reserved for future use */

	retval = usb_ep_queue(&hwep->ep, req, GFP_ATOMIC);
	spin_lock(hwep->lock);
	return retval;
}

/**
 * isr_setup_status_complete: setup_status request complete function
 * @ep:  endpoint
 * @req: request handled
 *
 * Caller must release lock. Put the port in test mode if test mode
 * feature is selected.
 */
static void
isr_setup_status_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct ci_hdrc *ci = req->context;
	unsigned long flags;

	if (ci->setaddr) {
		hw_usb_set_address(ci, ci->address);
		ci->setaddr = false;
	}

	spin_lock_irqsave(&ci->lock, flags);
	if (ci->test_mode)
		hw_port_test_set(ci, ci->test_mode);
	spin_unlock_irqrestore(&ci->lock, flags);
}

/**
 * isr_setup_status_phase: queues the status phase of a setup transation
 * @ci: ci struct
 *
 * This function returns an error code
 */
static int isr_setup_status_phase(struct ci_hdrc *ci)
{
	int retval;
	struct ci_hw_ep *hwep;

	hwep = (ci->ep0_dir == TX) ? ci->ep0out : ci->ep0in;
	ci->status->context = ci;
	ci->status->complete = isr_setup_status_complete;
	ci->status->length = 0;

	retval = _ep_queue(&hwep->ep, ci->status, GFP_ATOMIC);

	return retval;
}

/**
 * isr_tr_complete_low: transaction complete low level handler
 * @hwep: endpoint
 *
 * This function returns an error code
 * Caller must hold lock
 */
static int isr_tr_complete_low(struct ci_hw_ep *hwep)
__releases(hwep->lock)
__acquires(hwep->lock)
{
	struct ci_hw_req *hwreq, *hwreqtemp;
	struct ci_hw_ep *hweptemp = hwep;
	int retval = 0;
	int req_dequeue = 1;
	struct ci13xxx *ci = hwep->ci;

	del_timer(&hwep->prime_timer);
	hwep->prime_timer_count = 0;
	list_for_each_entry_safe(hwreq, hwreqtemp, &hwep->qh.queue,
			queue) {
dequeue:
		retval = _hardware_dequeue(hwep, hwreq);
		if (retval < 0) {
			/*
			 * FIXME: don't know exact delay
			 * required for HW to update dTD status
			 * bits. This is a temporary workaround till
			 * HW designers come back on this.
			 */
			if (retval == -EBUSY && req_dequeue &&
				(hwep->dir == 0 || hwep->num == 0)) {
				req_dequeue = 0;
				ci->dTD_update_fail_count++;
				hwep->dTD_update_fail_count++;
				udelay(10);
				goto dequeue;
			}
			break;
		}
		req_dequeue = 0;

		if (hwep->multi_req) { /* Large request in progress */
			unsigned remain_len;

			hwreq->multi.actual += hwreq->req.actual;
			remain_len = hwreq->multi.len - hwreq->multi.actual;
			if (hwreq->req.status || !remain_len ||
				(hwreq->req.actual != hwreq->req.length)) {
				restore_original_req(hwreq);
				hwep->multi_req = false;
			} else {
				hwreq->req.buf = hwreq->multi.buf +
						hwreq->multi.actual;
				hwreq->req.length = min_t(unsigned, remain_len,
						(4 * CI13XXX_PAGE_SIZE));

				hwreq->req.status = -EINPROGRESS;
				hwreq->req.actual = 0;
				list_del_init(&hwreq->queue);
				retval = _hardware_enqueue(hwep, hwreq);
				if (retval) {
					err("Large req failed in middle");
					hwreq->req.status = retval;
					restore_original_req(hwreq);
					hwep->multi_req = false;
					goto done;
				} else {
					list_add_tail(&hwreq->queue,
						&hwep->qh.queue);
					return 0;
				}
			}
		}
		list_del_init(&hwreq->queue);
done:
		if (hwreq->req.complete != NULL) {
			spin_unlock(hwep->lock);
			if ((hwep->type == USB_ENDPOINT_XFER_CONTROL) &&
					hwreq->req.length)
				hweptemp = hwep->ci->ep0in;
			hwreq->req.complete(&hweptemp->ep, &hwreq->req);
			spin_lock(hwep->lock);
		}
	}

	if (retval == -EBUSY)
		retval = 0;

	return retval;
}

/**
 * isr_tr_complete_handler: transaction complete interrupt handler
 * @ci: UDC descriptor
 *
 * This function handles traffic events
 */
static void isr_tr_complete_handler(struct ci_hdrc *ci)
__releases(ci->lock)
__acquires(ci->lock)
{
	unsigned i;
	u8 tmode = 0;

	for (i = 0; i < ci->hw_ep_max; i++) {
		struct ci_hw_ep *hwep  = &ci->ci_hw_ep[i];
		int type, num, dir, err = -EINVAL;
		struct usb_ctrlrequest req;

		if (hwep->ep.desc == NULL)
			continue;   /* not configured */

		if (hw_test_and_clear_complete(ci, i)) {
			err = isr_tr_complete_low(hwep);
			if (hwep->type == USB_ENDPOINT_XFER_CONTROL) {
				if (err > 0)   /* needs status phase */
					err = isr_setup_status_phase(ci);
				if (err < 0) {
					spin_unlock(&ci->lock);
					if (usb_ep_set_halt(&hwep->ep))
						dev_err(ci->dev,
							"error: ep_set_halt\n");
					spin_lock(&ci->lock);
				}
			}
		}

		if (hwep->type != USB_ENDPOINT_XFER_CONTROL ||
		    !hw_test_and_clear_setup_status(ci, i))
			continue;

		if (i != 0) {
			dev_warn(ci->dev, "ctrl traffic at endpoint %d\n", i);
			continue;
		}

		/*
		 * Flush data and handshake transactions of previous
		 * setup packet.
		 */
		_ep_nuke(ci->ep0out);
		_ep_nuke(ci->ep0in);

		/* read_setup_packet */
		do {
			hw_test_and_set_setup_guard(ci);
			memcpy(&req, &hwep->qh.ptr->setup, sizeof(req));
			/* Ensure buffer is read before acknowledging to h/w */
			mb();
		} while (!hw_test_and_clear_setup_guard(ci));

		type = req.bRequestType;

		ci->ep0_dir = (type & USB_DIR_IN) ? TX : RX;

		switch (req.bRequest) {
		case USB_REQ_CLEAR_FEATURE:
			if (type == (USB_DIR_OUT|USB_RECIP_ENDPOINT) &&
					le16_to_cpu(req.wValue) ==
					USB_ENDPOINT_HALT) {
				if (req.wLength != 0)
					break;
				num  = le16_to_cpu(req.wIndex);
				dir = num & USB_ENDPOINT_DIR_MASK;
				num &= USB_ENDPOINT_NUMBER_MASK;
				if (dir) /* TX */
					num += ci->hw_ep_max/2;
				if (!ci->ci_hw_ep[num].wedge) {
					spin_unlock(&ci->lock);
					err = usb_ep_clear_halt(
						&ci->ci_hw_ep[num].ep);
					spin_lock(&ci->lock);
					if (err)
						break;
				}
				err = isr_setup_status_phase(ci);
			} else if (type == (USB_DIR_OUT|USB_RECIP_DEVICE) &&
					le16_to_cpu(req.wValue) ==
					USB_DEVICE_REMOTE_WAKEUP) {
				if (req.wLength != 0)
					break;
				ci->remote_wakeup = 0;
				err = isr_setup_status_phase(ci);
			} else {
				goto delegate;
			}
			break;
		case USB_REQ_GET_STATUS:
			if (type != (USB_DIR_IN|USB_RECIP_DEVICE)   &&
			    type != (USB_DIR_IN|USB_RECIP_ENDPOINT) &&
			    type != (USB_DIR_IN|USB_RECIP_INTERFACE))
				goto delegate;
			if (le16_to_cpu(req.wValue)  != 0)
				break;
			err = isr_get_status_response(ci, &req);
			break;
		case USB_REQ_SET_ADDRESS:
			if (type != (USB_DIR_OUT|USB_RECIP_DEVICE))
				goto delegate;
			if (le16_to_cpu(req.wLength) != 0 ||
			    le16_to_cpu(req.wIndex)  != 0)
				break;
			ci->address = (u8)le16_to_cpu(req.wValue);
			ci->setaddr = true;
			err = isr_setup_status_phase(ci);
			break;
		case USB_REQ_SET_CONFIGURATION:
			if (type == (USB_DIR_OUT|USB_TYPE_STANDARD))
				ci->configured = !!req.wValue;
			goto delegate;
		case USB_REQ_SET_FEATURE:
			if (type == (USB_DIR_OUT|USB_RECIP_ENDPOINT) &&
					le16_to_cpu(req.wValue) ==
					USB_ENDPOINT_HALT) {
				if (req.wLength != 0)
					break;
				num  = le16_to_cpu(req.wIndex);
				dir = num & USB_ENDPOINT_DIR_MASK;
				num &= USB_ENDPOINT_NUMBER_MASK;
				if (dir) /* TX */
					num += ci->hw_ep_max/2;

				spin_unlock(&ci->lock);
				err = usb_ep_set_halt(&ci->ci_hw_ep[num].ep);
				spin_lock(&ci->lock);
				if (!err)
					isr_setup_status_phase(ci);
			} else if (type == (USB_DIR_OUT|USB_RECIP_DEVICE)) {
				if (req.wLength != 0)
					break;
				switch (le16_to_cpu(req.wValue)) {
				case USB_DEVICE_REMOTE_WAKEUP:
					ci->remote_wakeup = 1;
					err = isr_setup_status_phase(ci);
					break;
				case USB_DEVICE_B_HNP_ENABLE:
					ci->gadget.b_hnp_enable = 1;
					err = isr_setup_status_phase(ci);
					break;
				case USB_DEVICE_A_HNP_SUPPORT:
					ci->gadget.a_hnp_support = 1;
					err = isr_setup_status_phase(ci);
					break;
				case USB_DEVICE_A_ALT_HNP_SUPPORT:
					break;
				case USB_DEVICE_TEST_MODE:
					tmode = le16_to_cpu(req.wIndex) >> 8;
					switch (tmode) {
					case TEST_J:
					case TEST_K:
					case TEST_SE0_NAK:
					case TEST_PACKET:
					case TEST_FORCE_EN:
						ci->test_mode = tmode;
						err = isr_setup_status_phase(
								ci);
						break;
					case TEST_OTG_SRP_REQD:
						ci->gadget.otg_srp_reqd = 1;
						err = isr_setup_status_phase(
								ci);
						break;
					case TEST_OTG_HNP_REQD:
						ci->gadget.host_request = 1;
						err = isr_setup_status_phase(
								ci);
					default:
						break;
					}
				default:
					break;
				}
			} else {
				goto delegate;
			}
			break;
		default:
delegate:
			if (req.wLength == 0)   /* no data phase */
				ci->ep0_dir = TX;

			spin_unlock(&ci->lock);
			err = ci->driver->setup(&ci->gadget, &req);
			spin_lock(&ci->lock);
			break;
		}

		if (err < 0) {
			spin_unlock(&ci->lock);
			if (usb_ep_set_halt(&hwep->ep))
				dev_err(ci->dev, "error: ep_set_halt\n");
			spin_lock(&ci->lock);
		}
	}
}

/******************************************************************************
 * ENDPT block
 *****************************************************************************/
/**
 * ep_enable: configure endpoint, making it usable
 *
 * Check usb_ep_enable() at "usb_gadget.h" for details
 */
static int ep_enable(struct usb_ep *ep,
		     const struct usb_endpoint_descriptor *desc)
{
	struct ci_hw_ep *hwep = container_of(ep, struct ci_hw_ep, ep);
	int retval = 0;
	unsigned long flags;
	u32 cap = 0;
	unsigned mult = 0;

	if (ep == NULL || desc == NULL)
		return -EINVAL;

	spin_lock_irqsave(hwep->lock, flags);

	/* only internal SW should enable ctrl endpts */

	hwep->ep.desc = desc;

	if (!list_empty(&hwep->qh.queue))
		dev_warn(hwep->ci->dev, "enabling a non-empty endpoint!\n");

	hwep->dir  = usb_endpoint_dir_in(desc) ? TX : RX;
	hwep->num  = usb_endpoint_num(desc);
	hwep->type = usb_endpoint_type(desc);

	hwep->ep.maxpacket = usb_endpoint_maxp(desc) & 0x07ff;
	hwep->ep.mult = QH_ISO_MULT(usb_endpoint_maxp(desc));

	if (hwep->type == USB_ENDPOINT_XFER_CONTROL)
		cap |=  QH_IOS;
	} else if (mEhwepp->type == USB_ENDPOINT_XFER_ISOC) {
		cap &= ~QH_MULT;
		mult = ((mEhwepp->ep.maxpacket >> QH_MULT_SHIFT) + 1) & 0x03;
		cap |= (mult << ffs_nr(QH_MULT));
	} else {
		cap |= QH_ZLT;
	}
	cap |= (hwep->ep.maxpacket << __ffs(QH_MAX_PKT)) & QH_MAX_PKT;

	hwep->qh.ptr->cap = cpu_to_le32(cap);

	hwep->qh.ptr->td.next |= cpu_to_le32(TD_TERMINATE);   /* needed? */

	/* complete all the updates to ept->head before enabling endpoint */
	mb();

	/*
	 * Enable endpoints in the HW other than ep0 as ep0
	 * is always enabled
	 */
	if (hwep->num)
		retval |= hw_ep_enable(hwep->ci, hwep->num, hwep->dir,
				       hwep->type);

	spin_unlock_irqrestore(hwep->lock, flags);
	return retval;
}

/**
 * ep_disable: endpoint is no longer usable
 *
 * Check usb_ep_disable() at "usb_gadget.h" for details
 */
static int ep_disable(struct usb_ep *ep)
{
	struct ci_hw_ep *hwep = container_of(ep, struct ci_hw_ep, ep);
	int direction, retval = 0;
	unsigned long flags;

	if (ep == NULL)
		return -EINVAL;
	else if (hwep->ep.desc == NULL)
		return -EBUSY;

	spin_lock_irqsave(hwep->lock, flags);

	/* only internal SW should disable ctrl endpts */

	direction = hwep->dir;
	do {
		retval |= _ep_nuke(hwep);
		retval |= hw_ep_disable(hwep->ci, hwep->num, hwep->dir);

		if (hwep->type == USB_ENDPOINT_XFER_CONTROL)
			hwep->dir = (hwep->dir == TX) ? RX : TX;

	} while (hwep->dir != direction);

	if (hwep->last_zptr) {					// TODO: djStolen; Possibly MSM specific
		dma_pool_free(hwep->td_pool, hwep->last_zptr,
				hwep->last_zdma);
		hwep->last_zptr = NULL;
	}

	hwep->ep.desc = NULL;

	spin_unlock_irqrestore(hwep->lock, flags);
	return retval;
}

/**
 * ep_alloc_request: allocate a request object to use with this endpoint
 *
 * Check usb_ep_alloc_request() at "usb_gadget.h" for details
 */
static struct usb_request *ep_alloc_request(struct usb_ep *ep, gfp_t gfp_flags)
{
	struct ci_hw_req *hwreq = NULL;

	if (ep == NULL)
		return NULL;

	hwreq = kzalloc(sizeof(struct ci_hw_req), gfp_flags);
	if (hwreq != NULL) {
		INIT_LIST_HEAD(&hwreq->queue);
		INIT_LIST_HEAD(&hwreq->tds);
	}

	return (hwreq == NULL) ? NULL : &hwreq->req;
}

/**
 * ep_free_request: frees a request object
 *
 * Check usb_ep_free_request() at "usb_gadget.h" for details
 */
static void ep_free_request(struct usb_ep *ep, struct usb_request *req)
{
	struct ci_hw_ep  *hwep  = container_of(ep,  struct ci_hw_ep, ep);
	struct ci_hw_req *hwreq = container_of(req, struct ci_hw_req, req);
	struct td_node *node, *tmpnode;
	unsigned long flags;

	if (ep == NULL || req == NULL) {
		return;
	} else if (!list_empty(&hwreq->queue)) {
		dev_err(hwep->ci->dev, "freeing queued request\n");
		return;
	}

	spin_lock_irqsave(hwep->lock, flags);

	list_for_each_entry_safe(node, tmpnode, &hwreq->tds, td) {
		dma_pool_free(hwep->td_pool, node->ptr, node->dma);
		list_del_init(&node->td);
		node->ptr = NULL;
		kfree(node);
	}

	kfree(hwreq);

	spin_unlock_irqrestore(hwep->lock, flags);
}

/**
 * ep_queue: queues (submits) an I/O request to an endpoint
 *
 * Check usb_ep_queue()* at usb_gadget.h" for details
 */
static int ep_queue(struct usb_ep *ep, struct usb_request *req,
		    gfp_t __maybe_unused gfp_flags)
{
	struct ci_hw_ep  *hwep  = container_of(ep,  struct ci_hw_ep, ep);
	struct ci13xxx *ci = hwep->ci;
	int retval = 0;
	unsigned long flags;

	if (ep == NULL || req == NULL || hwep->ep.desc == NULL)
 		return -EINVAL;

	spin_lock_irqsave(hwep->lock, flags);
	if (!ci->configured && hwep->type !=			// TODO: djStolen; MSM specific
		USB_ENDPOINT_XFER_CONTROL) {
		retval = -ESHUTDOWN;
		goto done;
	}
	retval = _ep_queue(ep, req, gfp_flags);
	spin_unlock_irqrestore(hwep->lock, flags);
	return retval;
}

/**
 * ep_dequeue: dequeues (cancels, unlinks) an I/O request from an endpoint
 *
 * Check usb_ep_dequeue() at "usb_gadget.h" for details
 */
static int ep_dequeue(struct usb_ep *ep, struct usb_request *req)
{
	struct ci_hw_ep  *hwep  = container_of(ep,  struct ci_hw_ep, ep);
	struct ci_hw_ep  *hwepTemp = hwep;
	struct ci_hw_req *hwreq = container_of(req, struct ci_hw_req, req);
	unsigned long flags;

	if (ep == NULL || req == NULL || hwreq->req.status != -EALREADY ||
		hwep->ep.desc == NULL || list_empty(&hwreq->queue) ||
		list_empty(&hwep->qh.queue))
 		return -EINVAL;

	spin_lock_irqsave(hwep->lock, flags);
	/*
	 * Only ep0 IN is exposed to composite.  When a req is dequeued
	 * on ep0, check both ep0 IN and ep0 OUT queues.
	 */
	if (ep == NULL || req == NULL || hwreq->req.status != -EALREADY ||
		hwep->desc == NULL || list_empty(&hwreq->queue) ||
		(list_empty(&hwep->qh.queue) && ((hwep->type !=
			USB_ENDPOINT_XFER_CONTROL) ||
			list_empty(&hwep->ci->ep0out.qh.queue)))) {
		spin_unlock_irqrestore(hwep->lock, flags);
		return -EINVAL;
	}

	if ((hwep->type == USB_ENDPOINT_XFER_CONTROL)) {
		hw_ep_flush(hwep->ci, hwep->num, RX);
		hw_ep_flush(hwep->ci, hwep->num, TX);
	} else {
		hw_ep_flush(hwep->ci, hwep->num, hwep->dir);
	}

	/* pop request */
	list_del_init(&hwreq->queue);

	usb_gadget_unmap_request(&hwep->ci->gadget, req, hwep->dir);

	req->status = -ECONNRESET;
	if (hwep->multi_req) {
		restore_original_req(hwreq);
		hwep->multi_req = false;
	}

	if (hwreq->req.complete != NULL) {
		spin_unlock(hwep->lock);
		hwreq->req.complete(&hwep->ep, &hwreq->req);
		spin_lock(hwep->lock);
	}

	spin_unlock_irqrestore(hwep->lock, flags);
	return 0;
}

static int is_sps_req(struct ci13xxx_req *mReq)
{
	return (CI13XX_REQ_VENDOR_ID(mReq->req.udc_priv) == MSM_VENDOR_ID &&
			mReq->req.udc_priv & MSM_SPS_MODE);
}

/**
 * ep_set_halt: sets the endpoint halt feature
 *
 * Check usb_ep_set_halt() at "usb_gadget.h" for details
 */
static int ep_set_halt(struct usb_ep *ep, int value)
{
	struct ci13xxx_ep *mEp = container_of(ep, struct ci13xxx_ep, ep);
	int direction, retval = 0;
	unsigned long flags;

	if (ep == NULL || mEp->ep.desc == NULL)
		return -EINVAL;

	spin_lock_irqsave(mEp->lock, flags);

#ifndef STALL_IN
	/* g_file_storage MS compliant but g_zero fails chapter 9 compliance */
	if (value && mEp->type == USB_ENDPOINT_XFER_BULK && mEp->dir == TX &&
		!list_empty(&mEp->qh.queue) &&
		!is_sps_req(list_entry(mEp->qh.queue.next, struct ci13xxx_req,
							   queue))){
		spin_unlock_irqrestore(mEp->lock, flags);
		return -EAGAIN;
	}
#endif

	direction = mEp->dir;
	do {
		retval |= hw_ep_set_halt(mEp->ci, mEp->num, mEp->dir, value);

		if (!value)
			mEp->wedge = 0;

		if (mEp->type == USB_ENDPOINT_XFER_CONTROL)
			mEp->dir = (mEp->dir == TX) ? RX : TX;

	} while (mEp->dir != direction);

	spin_unlock_irqrestore(mEp->lock, flags);
	return retval;
}

/**
 * ep_set_wedge: sets the halt feature and ignores clear requests
 *
 * Check usb_ep_set_wedge() at "usb_gadget.h" for details
 */
static int ep_set_wedge(struct usb_ep *ep)
{
	struct ci_hw_ep *hwep = container_of(ep, struct ci_hw_ep, ep);
	unsigned long flags;

	if (ep == NULL || hwep->ep.desc == NULL)
		return -EINVAL;

	spin_lock_irqsave(hwep->lock, flags);
	hwep->wedge = 1;
	spin_unlock_irqrestore(hwep->lock, flags);

	return usb_ep_set_halt(ep);
}

/**
 * ep_fifo_flush: flushes contents of a fifo
 *
 * Check usb_ep_fifo_flush() at "usb_gadget.h" for details
 */
static void ep_fifo_flush(struct usb_ep *ep)
{
	struct ci13xxx_ep *mEp = container_of(ep, struct ci13xxx_ep, ep);
	unsigned long flags;

	if (ep == NULL) {
		dev_err(mEp->ci->dev, "%02X: -EINVAL\n", _usb_addr(mEp));
		return;
	}

	spin_lock_irqsave(mEp->lock, flags);

	/*
	 * _ep_nuke() takes care of flushing the endpoint.
	 * some function drivers expect udc to retire all
	 * pending requests upon flushing an endpoint.  There
	 * is no harm in doing it.
	 */
	_ep_nuke(mEp);


	spin_unlock_irqrestore(mEp->lock, flags);
}

/**
 * Endpoint-specific part of the API to the USB controller hardware
 * Check "usb_gadget.h" for details
 */
static const struct usb_ep_ops usb_ep_ops = {
	.enable	       = ep_enable,
	.disable       = ep_disable,
	.alloc_request = ep_alloc_request,
	.free_request  = ep_free_request,
	.queue	       = ep_queue,
	.dequeue       = ep_dequeue,
	.set_halt      = ep_set_halt,
	.set_wedge     = ep_set_wedge,
	.fifo_flush    = ep_fifo_flush,
};

/******************************************************************************
 * GADGET block
 *****************************************************************************/
static int ci_udc_vbus_session(struct usb_gadget *_gadget, int is_active)
{
	struct ci_hdrc *ci = container_of(_gadget, struct ci_hdrc, gadget);
	unsigned long flags;
	int gadget_ready = 0;

	if (!(ci->platdata->flags & CI_HDRC_PULLUP_ON_VBUS))
		return -EOPNOTSUPP;

	spin_lock_irqsave(&ci->lock, flags);
	ci->vbus_active = is_active;
	if (ci->driver)
		gadget_ready = 1;
	spin_unlock_irqrestore(&ci->lock, flags);

	if (gadget_ready) {
		if (is_active) {
			pm_runtime_get_sync(&_gadget->dev);
			hw_device_reset(ci, USBMODE_CM_DC);
			hw_device_state(ci, ci->ep0out->qh.dma);
		} else {
			hw_device_state(ci, 0);
			_gadget_stop_activity(&ci->gadget);
			if (ci->platdata->notify_event)
				ci->platdata->notify_event(ci,
					CI13XXX_CONTROLLER_DISCONNECT_EVENT);
			pm_runtime_put_sync(&_gadget->dev);
		}
	}

	return 0;
}

int ci13xxx_wakeup(struct usb_gadget *_gadget)
{
	struct ci13xxx *ci = container_of(_gadget, struct ci13xxx, gadget);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&ci->lock, flags);
	if (!ci->remote_wakeup) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	spin_unlock_irqrestore(&ci->lock, flags);

	ci->platdata->notify_event(ci,
			CI13XXX_CONTROLLER_REMOTE_WAKEUP_EVENT);

	if (ci->transceiver)
		usb_phy_set_suspend(ci->transceiver, 0);

	spin_lock_irqsave(&ci->lock, flags);
	if (!hw_read(ci, OP_PORTSC, PORTSC_SUSP)) {
		ret = -EINVAL;
		goto out;
	}
	hw_write(ci, OP_PORTSC, PORTSC_FPR, PORTSC_FPR);
out:
	spin_unlock_irqrestore(&ci->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(ci13xxx_wakeup);

static void usb_do_remote_wakeup(struct work_struct *w)
{
	struct ci13xxx *ci;
	unsigned long flags;
	bool do_wake;

	ci = container_of(to_delayed_work(w), struct ci13xxx, rw_work);
	/*
	 * This work can not be canceled from interrupt handler. Check
	 * if wakeup conditions are still met.
	 */
	spin_lock_irqsave(udc->lock, flags);
	do_wake = udc->suspended && udc->remote_wakeup;
	spin_unlock_irqrestore(udc->lock, flags);

	if (do_wake)
		ci13xxx_wakeup(&udc->gadget);
}

static int ci_udc_vbus_draw(struct usb_gadget *_gadget, unsigned ma)
{
	struct ci_hdrc *ci = container_of(_gadget, struct ci_hdrc, gadget);

	if (ci->transceiver)
		return usb_phy_set_power(ci->transceiver, ma);
	return -ENOTSUPP;
}

/* Change Data+ pullup status
 * this func is used by usb_gadget_connect/disconnet
 */
static int ci_udc_pullup(struct usb_gadget *_gadget, int is_on)
{
	struct ci_hdrc *ci = container_of(_gadget, struct ci_hdrc, gadget);

	if (is_on)
		hw_write(ci, OP_USBCMD, USBCMD_RS, USBCMD_RS);
	else
		hw_write(ci, OP_USBCMD, USBCMD_RS, 0);

	return 0;
}

static int ci_udc_start(struct usb_gadget *gadget,
			 struct usb_gadget_driver *driver);
static int ci_udc_stop(struct usb_gadget *gadget,
			struct usb_gadget_driver *driver);
/**
 * Device operations part of the API to the USB controller hardware,
 * which don't involve endpoints (or i/o)
 * Check  "usb_gadget.h" for details
 */
static const struct usb_gadget_ops usb_gadget_ops = {
	.vbus_session	= ci13xxx_vbus_session,
	.wakeup		= ci13xxx_wakeup,
	.pullup		= ci13xxx_pullup,
	.vbus_draw	= ci13xxx_vbus_draw,
	.udc_start	= ci13xxx_start,
	.udc_stop	= ci13xxx_stop,
};

static int init_eps(struct ci13xxx *ci)
{
	int retval = 0, i, j;

	for (i = 0; i < ci->hw_ep_max/2; i++)
		for (j = RX; j <= TX; j++) {
			int k = i + j * ci->hw_ep_max/2;
			struct ci13xxx_ep *mEp = &ci->ci13xxx_ep[k];

			scnprintf(mEp->name, sizeof(mEp->name), "ep%i%s", i,
					(j == TX)  ? "in" : "out");

			mEp->ci          = ci;
			mEp->lock         = &ci->lock;
			mEp->td_pool      = ci->td_pool;

			mEp->ep.name      = mEp->name;
			mEp->ep.ops       = &usb_ep_ops;
			/*
			 * for ep0: maxP defined in desc, for other
			 * eps, maxP is set by epautoconfig() called
			 * by gadget layer
			 */
			mEp->ep.maxpacket = (unsigned short)~0;

			INIT_LIST_HEAD(&mEp->qh.queue);
			setup_timer(&mEp->prime_timer, ep_prime_timer_func,
				(unsigned long) mEp);
			mEp->qh.ptr = dma_pool_alloc(ci->qh_pool, GFP_KERNEL,
						     &mEp->qh.dma);
			if (mEp->qh.ptr == NULL)
				retval = -ENOMEM;
			else
				memset(mEp->qh.ptr, 0, sizeof(*mEp->qh.ptr));

			/*
			 * set up shorthands for ep0 out and in endpoints,
			 * don't add to gadget's ep_list
			 */
			if (i == 0) {
				if (j == RX)
					ci->ep0out = hwep;
				else
					ci->ep0in = hwep;

				hwep->ep.maxpacket = CTRL_PAYLOAD_MAX;
				continue;
			}

			list_add_tail(&hwep->ep.ep_list, &ci->gadget.ep_list);
		}

	return retval;
}

static void destroy_eps(struct ci_hdrc *ci)
{
	int i;

	for (i = 0; i < ci->hw_ep_max; i++) {
		struct ci_hw_ep *hwep = &ci->ci_hw_ep[i];

		dma_pool_free(ci->qh_pool, hwep->qh.ptr, hwep->qh.dma);
	}
}

/**
 * ci_udc_start: register a gadget driver
 * @gadget: our gadget
 * @driver: the driver being registered
 *
 * Interrupts are enabled here.
 */
static int ci_udc_start(struct usb_gadget *gadget,
			 struct usb_gadget_driver *driver)
{
	struct ci_hdrc *ci = container_of(gadget, struct ci_hdrc, gadget);
	unsigned long flags;
	int retval = -ENOMEM;
	bool put = false;

	if (driver->disconnect == NULL)
		return -EINVAL;


	ci->ep0out->ep.desc = &ctrl_endpt_out_desc;
	retval = usb_ep_enable(&ci->ep0out->ep);
	if (retval)
		return retval;

	ci->ep0in->ep.desc = &ctrl_endpt_in_desc;
	retval = usb_ep_enable(&ci->ep0in->ep);
	if (retval)
		return retval;
	ci->status = usb_ep_alloc_request(&ci->ep0in.ep, GFP_KERNEL);
	if (!ci->status)
		return -ENOMEM;
	ci->status_buf = kzalloc(2, GFP_KERNEL); /* for GET_STATUS */
	if (!ci->status_buf) {
		usb_ep_free_request(&ci->ep0in.ep, ci->status);
		return -ENOMEM;
	}

	pm_runtime_get_sync(&ci->gadget.dev);
	spin_lock_irqsave(&ci->lock, flags);

	ci->driver = driver;
	if (ci->platdata->flags & CI13XXX_PULLUP_ON_VBUS) {
		if (ci->vbus_active) {
			if (ci->platdata->flags & CI13XXX_REGS_SHARED)
				hw_device_reset(ci, USBMODE_CM_DC);
		} else {
			put = true;
			goto done;
		}
	}

	retval = hw_device_state(ci, ci->ep0out->qh.dma);

 done:
	spin_unlock_irqrestore(&ci->lock, flags);
	if (retval || put)
		pm_runtime_put_sync(&ci->gadget.dev);

	if (ci->platadata->notify_event)
			ci->platadata->notify_event(ci,
				CI13XXX_CONTROLLER_UDC_STARTED_EVENT);

	return retval;
}

/**
 * ci13xxx_stop: unregister a gadget driver
 */
static int ci13xxx_stop(struct usb_gadget *gadget,
			struct usb_gadget_driver *driver)
{
	struct ci13xxx *ci = container_of(gadget, struct ci13xxx, gadget);
	unsigned long flags;

	spin_lock_irqsave(&ci->lock, flags);

	if (!(ci->platdata->flags & CI13XXX_PULLUP_ON_VBUS) ||
			ci->vbus_active) {
		hw_device_state(ci, 0);
		ci->driver = NULL;
		spin_unlock_irqrestore(&ci->lock, flags);
		_gadget_stop_activity(&ci->gadget);
		spin_lock_irqsave(&ci->lock, flags);
		pm_runtime_put(&ci->gadget.dev);
	}

	spin_unlock_irqrestore(&ci->lock, flags);

	usb_ep_free_request(&udc->ep0in.ep, udc->status);
	kfree(udc->status_buf);

	return 0;
}

/******************************************************************************
 * BUS block
 *****************************************************************************/
/**
 * udc_irq: ci interrupt handler
 *
 * This function returns IRQ_HANDLED if the IRQ has been handled
 * It locks access to registers
 */
static irqreturn_t udc_irq(struct ci_hdrc *ci)
{
	irqreturn_t retval;
	u32 intr;

	if (ci == NULL)
		return IRQ_HANDLED;

	spin_lock(&ci->lock);

	if (ci->platdata->flags & CI_HDRC_REGS_SHARED) {
		if (hw_read(ci, OP_USBMODE, USBMODE_CM) !=
				USBMODE_CM_DC) {
			spin_unlock(&ci->lock);
			return IRQ_NONE;
		}
	}
	intr = hw_test_and_clear_intr_active(ci);

	if (intr) {
		/* order defines priority - do NOT change it */
		if (USBi_URI & intr)
			isr_reset_handler(ci);

		if (USBi_PCI & intr) {
			ci->gadget.speed = hw_port_is_high_speed(ci) ?
				USB_SPEED_HIGH : USB_SPEED_FULL;
			if (ci->suspended && ci->driver->resume) {
				spin_unlock(&ci->lock);
				if (ci->platdata->notify_event)
					ci->platdata->notify_event(ci,
					  CI13XXX_CONTROLLER_RESUME_EVENT);
				if (ci->transceiver)
					usb_phy_set_suspend(ci->transceiver, 0);
				ci->driver->resume(&ci->gadget);
				spin_lock(&ci->lock);
				ci->suspended = 0;
			}
		}

		if (USBi_UI  & intr)
			isr_tr_complete_handler(ci);

		if (USBi_SLI & intr) {
			if (ci->gadget.speed != USB_SPEED_UNKNOWN &&
			    ci->driver->suspend) {
				ci->suspended = 1;
				spin_unlock(&ci->lock);
				ci->driver->suspend(&ci->gadget);
				if (ci->platdata->notify_event)
					ci->platdata->notify_event(ci,
					  CI13XXX_CONTROLLER_SUSPEND_EVENT);
				if (ci->transceiver)
					usb_phy_set_suspend(ci->transceiver, 1);
				spin_lock(&ci->lock);
			}
		}
		retval = IRQ_HANDLED;
	} else {
		retval = IRQ_NONE;
	}
	spin_unlock(&ci->lock);

	return retval;
}

/**
 * udc_start: initialize gadget role
 * @ci: chipidea controller
 */
static int udc_start(struct ci_hdrc *ci)
{
	struct device *dev = ci->dev;
	int retval = 0;

	spin_lock_init(&ci->lock);

	ci->gadget.ops          = &usb_gadget_ops;
	ci->gadget.speed        = USB_SPEED_UNKNOWN;
	ci->gadget.max_speed    = USB_SPEED_HIGH;
	if (ci->platdata->flags & CI13XXX_IS_OTG)
		ci->gadget.is_otg       = 1;
	else
		ci->gadget.is_otg       = 0;
	ci->gadget.name         = ci->platdata->name;

	INIT_LIST_HEAD(&ci->gadget.ep_list);

	/* alloc resources */
	ci->qh_pool = dma_pool_create("ci_hw_qh", dev,
				       sizeof(struct ci_hw_qh),
				       64, CI_HDRC_PAGE_SIZE);
	if (ci->qh_pool == NULL)
		return -ENOMEM;

	ci->td_pool = dma_pool_create("ci_hw_td", dev,
				       sizeof(struct ci_hw_td),
				       64, CI_HDRC_PAGE_SIZE);
	if (ci->td_pool == NULL) {
		retval = -ENOMEM;
		goto free_qh_pool;
	}

	INIT_DELAYED_WORK(&ci->rw_work, usb_do_remote_wakeup);

	retval = init_eps(ci);
	if (retval)
		goto free_pools;

	ci->gadget.ep0 = &ci->ep0in->ep;

	if (ci->global_phy) {
		ci->transceiver = usb_get_phy(USB_PHY_TYPE_USB2);
		if (IS_ERR(ci->transceiver))
			ci->transceiver = NULL;
	}

	if (ci->platdata->flags & CI_HDRC_REQUIRE_TRANSCEIVER) {
		if (ci->transceiver == NULL) {
			retval = -ENODEV;
			goto destroy_eps;
		}
	}

	if (!(ci->platdata->flags & CI_HDRC_REGS_SHARED)) {
		retval = hw_device_reset(ci, USBMODE_CM_DC);
		if (retval)
			goto put_transceiver;
	}

	if (ci->transceiver) {
		retval = otg_set_peripheral(ci->transceiver->otg,
						&ci->gadget);
		if (retval)
			goto put_transceiver;
	}

	retval = usb_add_gadget_udc(dev, &ci->gadget);
	if (retval)
		goto remove_trans;

	pm_runtime_no_callbacks(&ci->gadget.dev);
	pm_runtime_enable(&ci->gadget.dev);

	if (register_trace_usb_daytona_invalid_access(dump_usb_info, NULL))
		pr_err("Registering trace failed\n");

	return retval;

remove_trans:
	if (ci->transceiver) {
		otg_set_peripheral(ci->transceiver->otg, NULL);
		if (ci->global_phy)
			usb_put_phy(ci->transceiver);
	}

	dev_err(dev, "error = %i\n", retval);
put_transceiver:
	if (ci->transceiver && ci->global_phy)
		usb_put_phy(ci->transceiver);
destroy_eps:
	destroy_eps(ci);
free_pools:
	dma_pool_destroy(ci->td_pool);
free_qh_pool:
	dma_pool_destroy(ci->qh_pool);
	return retval;
}

/**
 * udc_remove: parent remove must call this to remove UDC
 *
 * No interrupts active, the IRQ has been released
 */
static void udc_stop(struct ci_hdrc *ci)
{
	if (ci == NULL)
		return;

	if (unregister_trace_usb_daytona_invalid_access(dump_usb_info, NULL))
		pr_err("Unregistering trace failed\n");

	usb_del_gadget_udc(&ci->gadget);

	destroy_eps(ci);

	dma_pool_destroy(ci->td_pool);
	dma_pool_destroy(ci->qh_pool);

	if (ci->transceiver) {
		otg_set_peripheral(ci->transceiver->otg, NULL);
		if (ci->global_phy)
			usb_put_phy(ci->transceiver);
	}
	/* my kobject is dynamic, I swear! */
	memset(&ci->gadget, 0, sizeof(ci->gadget));
}

/**
 * ci_hdrc_gadget_init - initialize device related bits
 * ci: the controller
 *
 * This function enables the gadget role, if the device is "device capable".
 */
int ci_hdrc_gadget_init(struct ci_hdrc *ci)
{
	struct ci_role_driver *rdrv;

	if (!hw_read(ci, CAP_DCCPARAMS, DCCPARAMS_DC))
		return -ENXIO;

	rdrv = devm_kzalloc(ci->dev, sizeof(struct ci_role_driver), GFP_KERNEL);
	if (!rdrv)
		return -ENOMEM;

	rdrv->start	= udc_start;
	rdrv->stop	= udc_stop;
	rdrv->irq	= udc_irq;
	rdrv->name	= "gadget";
	ci->roles[CI_ROLE_GADGET] = rdrv;

	return 0;
}
