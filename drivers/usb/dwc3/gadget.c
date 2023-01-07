// SPDX-License-Identifier: GPL-2.0
/*
 * gadget.c - DesignWare USB3 DRD Controller Gadget Framework Link
 *
 * Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors: Felipe Balbi <balbi@ti.com>,
 *	    Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/dma-mapping.h>
#include <linux/usb/composite.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
#include <linux/usb/composite.h>
#endif
#include <linux/usb/samsung_usb.h>
#include <linux/phy/phy.h>
#include <linux/usb_notify.h>
#include <linux/workqueue.h>
#ifdef CONFIG_USB_TYPEC_MANAGER_NOTIFIER
#include <linux/of_platform.h>
#include <linux/usb/typec/manager/usb_typec_manager_notifier.h>
#endif

#include "debug.h"
#include "core.h"
#include "otg.h"
#include "gadget.h"
#include "io.h"

#define DWC3_ALIGN_FRAME(d)	(((d)->frame_number + (d)->interval) \
					& ~((d)->interval - 1))

extern bool acc_dev_status;

#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
static void dwc3_disconnect_gadget(struct dwc3 *dwc);
static void dwc3_gadget_cable_connect(struct dwc3 *dwc, bool connect)
{
	static bool last_connect;
	struct usb_composite_dev *cdev;

	if (last_connect != connect) {
		if (!connect) {
			cdev = get_gadget_data(&dwc->gadget);
			if (cdev != NULL) {
				cdev->mute_switch = 0;
				cdev->force_disconnect = 1;
				dev_info(dwc->dev, "Force Disconnect set to 1\n");
			}
		}
		last_connect = connect;
	}
}
#endif

#ifdef CONFIG_ARGOS
extern int argos_irq_affinity_setup_label(unsigned int irq, const char *label,
		struct cpumask *affinity_cpu_mask,
		struct cpumask *default_cpu_mask);
#ifdef CONFIG_SCHED_HMP
extern struct cpumask hmp_slow_cpu_mask;
static inline struct cpumask *get_default_cpu_mask(void)
{
	return &hmp_slow_cpu_mask;
}
#else
static inline struct cpumask *get_default_cpu_mask(void)
{
	return cpu_all_mask;
}
#endif
cpumask_var_t affinity_cpu_mask;
cpumask_var_t default_cpu_mask;
#endif

/**
 * dwc3_gadget_set_test_mode - enables usb2 test modes
 * @dwc: pointer to our context structure
 * @mode: the mode to set (J, K SE0 NAK, Force Enable)
 *
 * Caller should take care of locking. This function will return 0 on
 * success or -EINVAL if wrong Test Selector is passed.
 */
int dwc3_gadget_set_test_mode(struct dwc3 *dwc, int mode)
{
	u32		reg;

	reg = dwc3_readl(dwc->regs, DWC3_DCTL);
	reg &= ~DWC3_DCTL_TSTCTRL_MASK;

	switch (mode) {
	case TEST_J:
	case TEST_K:
	case TEST_SE0_NAK:
	case TEST_PACKET:
	case TEST_FORCE_EN:
		reg |= mode << 1;
		break;
	default:
		return -EINVAL;
	}

	dwc3_writel(dwc->regs, DWC3_DCTL, reg);

	return 0;
}


#ifdef CONFIG_USB_TYPEC_MANAGER_NOTIFIER
/**
 * dwc3_gadget_get_cmply_link_state - Gets current state of USB Link
 * @dwc: pointer to our context structure
 *
 * extern module can check dwc3 core link state  This function will
 * return 1 link is on compliance of loopback mode else 0.
 */
static int dwc3_gadget_get_cmply_link_state(void)
{
	struct device_node	*np = NULL;
	struct platform_device	*pdev = NULL;
	struct dwc3	*dwc;
	u32		reg = 0;
	u32		ret = -ENODEV;

	np = of_find_compatible_node(NULL, NULL, "synopsys,dwc3");

	if (np) {
		pdev = of_find_device_by_node(np);
		of_node_put(np);
		if (pdev)
			dwc = pdev->dev.driver_data;
	}

	 if (!dwc)
		return ret;

	if (dwc->pullups_connected) {
		reg = dwc3_gadget_get_link_state(dwc);

		dev_info(dwc->dev, "%s: link state = %d\n", __func__, reg);
		if ((reg == DWC3_LINK_STATE_CMPLY) || (reg == DWC3_LINK_STATE_LPBK))
			ret = 1;
		else
			ret = 0;
	} else
		dev_info(dwc->dev, "%s: udc not enabled \n", __func__);

	return ret;
}

static struct typec_manager_gadget_ops manager_dwc3_gadget_ops = {
	.gadget_get_cmply_link_state = dwc3_gadget_get_cmply_link_state,
};
#endif

/**
 * dwc3_gadget_get_link_state - gets current state of usb link
 * @dwc: pointer to our context structure
 *
 * Caller should take care of locking. This function will
 * return the link state on success (>= 0) or -ETIMEDOUT.
 */
int dwc3_gadget_get_link_state(struct dwc3 *dwc)
{
	u32		reg;

	reg = dwc3_readl(dwc->regs, DWC3_DSTS);

	return DWC3_DSTS_USBLNKST(reg);
}

/**
 * dwc3_gadget_set_link_state - sets usb link to a particular state
 * @dwc: pointer to our context structure
 * @state: the state to put link into
 *
 * Caller should take care of locking. This function will
 * return 0 on success or -ETIMEDOUT.
 */
int dwc3_gadget_set_link_state(struct dwc3 *dwc, enum dwc3_link_state state)
{
	int		retries = 10000;
	u32		reg;

	/*
	 * Wait until device controller is ready. Only applies to 1.94a and
	 * later RTL.
	 */
	if (dwc->revision >= DWC3_REVISION_194A) {
		while (--retries) {
			reg = dwc3_readl(dwc->regs, DWC3_DSTS);
			if (reg & DWC3_DSTS_DCNRD)
				udelay(5);
			else
				break;
		}

		if (retries <= 0)
			return -ETIMEDOUT;
	}

	reg = dwc3_readl(dwc->regs, DWC3_DCTL);
	reg &= ~DWC3_DCTL_ULSTCHNGREQ_MASK;

	/* set requested state */
	reg |= DWC3_DCTL_ULSTCHNGREQ(state);
	dwc3_writel(dwc->regs, DWC3_DCTL, reg);

	/*
	 * The following code is racy when called from dwc3_gadget_wakeup,
	 * and is not needed, at least on newer versions
	 */
	if (dwc->revision >= DWC3_REVISION_194A)
		return 0;

	/* wait for a change in DSTS */
	retries = 10000;
	while (--retries) {
		reg = dwc3_readl(dwc->regs, DWC3_DSTS);

		if (DWC3_DSTS_USBLNKST(reg) == state)
			return 0;

		udelay(5);
	}

	return -ETIMEDOUT;
}

static void dwc3_gadget_link_state_print (struct dwc3 *dwc, char* fname)
{
	int i;
	char buf[DWC3_LINK_STATE_LAST_INFO_MEM*5+1] = {0,};

	for (i=0; i<DWC3_LINK_STATE_LAST_INFO_MEM; i++) {
		sprintf(buf+(i*5), "%02d > ",
			dwc->linkstate_record[(dwc->linkstate_ai+i)%DWC3_LINK_STATE_LAST_INFO_MEM]);
	}
	pr_info("usb: %s (%s%s)\n", __func__, buf, fname);
}

/**
 * dwc3_ep_inc_trb - increment a trb index.
 * @index: Pointer to the TRB index to increment.
 *
 * The index should never point to the link TRB. After incrementing,
 * if it is point to the link TRB, wrap around to the beginning. The
 * link TRB is always at the last TRB entry.
 */
static void dwc3_ep_inc_trb(u8 *index)
{
	(*index)++;
	if (*index == (DWC3_TRB_NUM - 1))
		*index = 0;
}

/**
 * dwc3_ep_inc_enq - increment endpoint's enqueue pointer
 * @dep: The endpoint whose enqueue pointer we're incrementing
 */
static void dwc3_ep_inc_enq(struct dwc3_ep *dep)
{
	dwc3_ep_inc_trb(&dep->trb_enqueue);
}

/**
 * dwc3_ep_inc_deq - increment endpoint's dequeue pointer
 * @dep: The endpoint whose enqueue pointer we're incrementing
 */
static void dwc3_ep_inc_deq(struct dwc3_ep *dep)
{
	dwc3_ep_inc_trb(&dep->trb_dequeue);
}

void dwc3_gadget_del_and_unmap_request(struct dwc3_ep *dep,
		struct dwc3_request *req, int status)
{
	struct dwc3			*dwc = dep->dwc;

	req->started = false;
	/* Only delete from the list if the item isn't poisoned. */
	if (req->list.next != LIST_POISON1)
		list_del(&req->list);
	req->remaining = 0;
	req->needs_extra_trb = false;

	if (req->request.status == -EINPROGRESS)
		req->request.status = status;

	if (req->trb)
		usb_gadget_unmap_request_by_dev(dwc->sysdev,
				&req->request, req->direction);

	req->trb = NULL;
	trace_dwc3_gadget_giveback(req);

	if (dep->number > 1)
		pm_runtime_put(dwc->dev);
}

/**
 * dwc3_gadget_giveback - call struct usb_request's ->complete callback
 * @dep: The endpoint to whom the request belongs to
 * @req: The request we're giving back
 * @status: completion code for the request
 *
 * Must be called with controller's lock held and interrupts disabled. This
 * function will unmap @req and call its ->complete() callback to notify upper
 * layers that it has completed.
 */
void dwc3_gadget_giveback(struct dwc3_ep *dep, struct dwc3_request *req,
		int status)
{
	struct dwc3			*dwc = dep->dwc;
	struct usb_gadget		*gadget = &dwc->gadget;
	struct usb_composite_dev	*cdev = get_gadget_data(gadget);

	dwc3_gadget_del_and_unmap_request(dep, req, status);
	
	if (cdev && cdev->gadget) {
		spin_unlock(&dwc->lock);
		usb_gadget_giveback_request(&dep->endpoint, &req->request);
		spin_lock(&dwc->lock);
	}
}

/**
 * dwc3_send_gadget_generic_command - issue a generic command for the controller
 * @dwc: pointer to the controller context
 * @cmd: the command to be issued
 * @param: command parameter
 *
 * Caller should take care of locking. Issue @cmd with a given @param to @dwc
 * and wait for its completion.
 */
int dwc3_send_gadget_generic_command(struct dwc3 *dwc, unsigned cmd, u32 param)
{
	u32		timeout = 500;
	int		status = 0;
	int		ret = 0;
	u32		reg;

	dwc3_writel(dwc->regs, DWC3_DGCMDPAR, param);
	dwc3_writel(dwc->regs, DWC3_DGCMD, cmd | DWC3_DGCMD_CMDACT);

	do {
		reg = dwc3_readl(dwc->regs, DWC3_DGCMD);
		if (!(reg & DWC3_DGCMD_CMDACT)) {
			status = DWC3_DGCMD_STATUS(reg);
			if (status)
				ret = -EINVAL;
			break;
		}
	} while (--timeout);

	if (!timeout) {
		ret = -ETIMEDOUT;
		status = -ETIMEDOUT;
	}

	trace_dwc3_gadget_generic_cmd(cmd, param, status);

	return ret;
}

static int __dwc3_gadget_wakeup(struct dwc3 *dwc);

/**
 * dwc3_send_gadget_ep_cmd - issue an endpoint command
 * @dep: the endpoint to which the command is going to be issued
 * @cmd: the command to be issued
 * @params: parameters to the command
 *
 * Caller should handle locking. This function will issue @cmd with given
 * @params to @dep and wait for its completion.
 */
int dwc3_send_gadget_ep_cmd(struct dwc3_ep *dep, unsigned cmd,
		struct dwc3_gadget_ep_cmd_params *params)
{
	const struct usb_endpoint_descriptor *desc = dep->endpoint.desc;
	struct dwc3		*dwc = dep->dwc;
	u32			timeout = 1000;
	u32			saved_config = 0;
	u32			reg;

	int			cmd_status = 0;
	int			ret = -EINVAL;

	/*
	 * When operating in USB 2.0 speeds (HS/FS), if GUSB2PHYCFG.ENBLSLPM or
	 * GUSB2PHYCFG.SUSPHY is set, it must be cleared before issuing an
	 * endpoint command.
	 *
	 * Save and clear both GUSB2PHYCFG.ENBLSLPM and GUSB2PHYCFG.SUSPHY
	 * settings. Restore them after the command is completed.
	 *
	 * DWC_usb3 3.30a and DWC_usb31 1.90a programming guide section 3.2.2
	 */
	if (dwc->gadget.speed <= USB_SPEED_HIGH) {
		reg = dwc3_readl(dwc->regs, DWC3_GUSB2PHYCFG(0));
		if (unlikely(reg & DWC3_GUSB2PHYCFG_SUSPHY)) {
			saved_config |= DWC3_GUSB2PHYCFG_SUSPHY;
			reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
		}

		if (reg & DWC3_GUSB2PHYCFG_ENBLSLPM) {
			saved_config |= DWC3_GUSB2PHYCFG_ENBLSLPM;
			reg &= ~DWC3_GUSB2PHYCFG_ENBLSLPM;
		}

		if (saved_config)
			dwc3_writel(dwc->regs, DWC3_GUSB2PHYCFG(0), reg);
	}

#if defined(CONFIG_SOC_EXYNOS9830) /* To support L1*/
	if (DWC3_DEPCMD_CMD(cmd) == DWC3_DEPCMD_STARTTRANSFER ||
			DWC3_DEPCMD_CMD(cmd) == DWC3_DEPCMD_UPDATETRANSFER) {
#else
	if (DWC3_DEPCMD_CMD(cmd) == DWC3_DEPCMD_STARTTRANSFER) {
#endif
		int		needs_wakeup;

		needs_wakeup = (dwc->link_state == DWC3_LINK_STATE_U1 ||
				dwc->link_state == DWC3_LINK_STATE_U2 ||
				dwc->link_state == DWC3_LINK_STATE_U3);

		if (unlikely(needs_wakeup)) {
			ret = __dwc3_gadget_wakeup(dwc);
			/*
			 * disable warning when muic/ccic wasn't merged
			 *dev_WARN_ONCE(dwc->dev, ret, "wakeup failed --> %d\n",
			 *		ret);
			 */
		}
	}

	dwc3_writel(dep->regs, DWC3_DEPCMDPAR0, params->param0);
	dwc3_writel(dep->regs, DWC3_DEPCMDPAR1, params->param1);
	dwc3_writel(dep->regs, DWC3_DEPCMDPAR2, params->param2);

	/*
	 * Synopsys Databook 2.60a states in section 6.3.2.5.6 of that if we're
	 * not relying on XferNotReady, we can make use of a special "No
	 * Response Update Transfer" command where we should clear both CmdAct
	 * and CmdIOC bits.
	 *
	 * With this, we don't need to wait for command completion and can
	 * straight away issue further commands to the endpoint.
	 *
	 * NOTICE: We're making an assumption that control endpoints will never
	 * make use of Update Transfer command. This is a safe assumption
	 * because we can never have more than one request at a time with
	 * Control Endpoints. If anybody changes that assumption, this chunk
	 * needs to be updated accordingly.
	 */
	if (DWC3_DEPCMD_CMD(cmd) == DWC3_DEPCMD_UPDATETRANSFER &&
			!usb_endpoint_xfer_isoc(desc))
		cmd &= ~(DWC3_DEPCMD_CMDIOC | DWC3_DEPCMD_CMDACT);
	else
		cmd |= DWC3_DEPCMD_CMDACT;

	dwc3_writel(dep->regs, DWC3_DEPCMD, cmd);
	do {
		reg = dwc3_readl(dep->regs, DWC3_DEPCMD);
		if (!(reg & DWC3_DEPCMD_CMDACT)) {
			cmd_status = DWC3_DEPCMD_STATUS(reg);

			switch (cmd_status) {
			case 0:
				ret = 0;
				break;
			case DEPEVT_TRANSFER_NO_RESOURCE:
				ret = -EINVAL;
				break;
			case DEPEVT_TRANSFER_BUS_EXPIRY:
				/*
				 * SW issues START TRANSFER command to
				 * isochronous ep with future frame interval. If
				 * future interval time has already passed when
				 * core receives the command, it will respond
				 * with an error status of 'Bus Expiry'.
				 *
				 * Instead of always returning -EINVAL, let's
				 * give a hint to the gadget driver that this is
				 * the case by returning -EAGAIN.
				 */
				/* skip this status when isochrouns works */
				if (usb_endpoint_xfer_isoc(dep->endpoint.desc))
					ret = 0;
				else
					ret = -EAGAIN;
				break;
			default:
#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
				dev_WARN(dwc->dev, "UNKNOWN cmd status\n");
#endif
				break;
			}

			break;
		}
	} while (--timeout);

	if (timeout == 0) {
		ret = -ETIMEDOUT;
		cmd_status = -ETIMEDOUT;
	}

	trace_dwc3_gadget_ep_cmd(dep, cmd, params, cmd_status);

	if (ret == 0 && DWC3_DEPCMD_CMD(cmd) == DWC3_DEPCMD_STARTTRANSFER) {
		dep->flags |= DWC3_EP_TRANSFER_STARTED;
		dwc3_gadget_ep_get_transfer_index(dep);
	}

	if (saved_config) {
		reg = dwc3_readl(dwc->regs, DWC3_GUSB2PHYCFG(0));
		reg |= saved_config;
		dwc3_writel(dwc->regs, DWC3_GUSB2PHYCFG(0), reg);
	}

	return ret;
}

static int dwc3_send_clear_stall_ep_cmd(struct dwc3_ep *dep)
{
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_gadget_ep_cmd_params params;
	u32 cmd = DWC3_DEPCMD_CLEARSTALL;

	/*
	 * As of core revision 2.60a the recommended programming model
	 * is to set the ClearPendIN bit when issuing a Clear Stall EP
	 * command for IN endpoints. This is to prevent an issue where
	 * some (non-compliant) hosts may not send ACK TPs for pending
	 * IN transfers due to a mishandled error condition. Synopsys
	 * STAR 9000614252.
	 */
	if (dep->direction && (dwc->revision >= DWC3_REVISION_260A) &&
	    (dwc->gadget.speed >= USB_SPEED_SUPER))
		cmd |= DWC3_DEPCMD_CLEARPENDIN;

	memset(&params, 0, sizeof(params));

	return dwc3_send_gadget_ep_cmd(dep, cmd, &params);
}

static dma_addr_t dwc3_trb_dma_offset(struct dwc3_ep *dep,
		struct dwc3_trb *trb)
{
	u32		offset = (char *) trb - (char *) dep->trb_pool;

	return dep->trb_pool_dma + offset;
}

static int dwc3_alloc_trb_pool(struct dwc3_ep *dep)
{
	struct dwc3		*dwc = dep->dwc;

	if (dep->trb_pool)
		return 0;

	dep->trb_pool = dma_alloc_coherent(dwc->sysdev,
			sizeof(struct dwc3_trb) * DWC3_TRB_NUM,
			&dep->trb_pool_dma, GFP_KERNEL);
	if (!dep->trb_pool) {
		dev_err(dep->dwc->dev, "failed to allocate trb pool for %s\n",
				dep->name);
		return -ENOMEM;
	}

	return 0;
}

static void dwc3_free_trb_pool(struct dwc3_ep *dep)
{
	struct dwc3		*dwc = dep->dwc;

	dma_free_coherent(dwc->sysdev, sizeof(struct dwc3_trb) * DWC3_TRB_NUM,
			dep->trb_pool, dep->trb_pool_dma);

	dep->trb_pool = NULL;
	dep->trb_pool_dma = 0;
}

static int dwc3_gadget_set_xfer_resource(struct dwc3_ep *dep)
{
	struct dwc3_gadget_ep_cmd_params params;

	memset(&params, 0x00, sizeof(params));

	params.param0 = DWC3_DEPXFERCFG_NUM_XFER_RES(1);

	return dwc3_send_gadget_ep_cmd(dep, DWC3_DEPCMD_SETTRANSFRESOURCE,
			&params);
}

/**
 * dwc3_gadget_start_config - configure ep resources
 * @dep: endpoint that is being enabled
 *
 * Issue a %DWC3_DEPCMD_DEPSTARTCFG command to @dep. After the command's
 * completion, it will set Transfer Resource for all available endpoints.
 *
 * The assignment of transfer resources cannot perfectly follow the data book
 * due to the fact that the controller driver does not have all knowledge of the
 * configuration in advance. It is given this information piecemeal by the
 * composite gadget framework after every SET_CONFIGURATION and
 * SET_INTERFACE. Trying to follow the databook programming model in this
 * scenario can cause errors. For two reasons:
 *
 * 1) The databook says to do %DWC3_DEPCMD_DEPSTARTCFG for every
 * %USB_REQ_SET_CONFIGURATION and %USB_REQ_SET_INTERFACE (8.1.5). This is
 * incorrect in the scenario of multiple interfaces.
 *
 * 2) The databook does not mention doing more %DWC3_DEPCMD_DEPXFERCFG for new
 * endpoint on alt setting (8.1.6).
 *
 * The following simplified method is used instead:
 *
 * All hardware endpoints can be assigned a transfer resource and this setting
 * will stay persistent until either a core reset or hibernation. So whenever we
 * do a %DWC3_DEPCMD_DEPSTARTCFG(0) we can go ahead and do
 * %DWC3_DEPCMD_DEPXFERCFG for every hardware endpoint as well. We are
 * guaranteed that there are as many transfer resources as endpoints.
 *
 * This function is called for each endpoint when it is being enabled but is
 * triggered only when called for EP0-out, which always happens first, and which
 * should only happen in one of the above conditions.
 */
static int dwc3_gadget_start_config(struct dwc3_ep *dep)
{
	struct dwc3_gadget_ep_cmd_params params;
	struct dwc3		*dwc;
	u32			cmd;
	int			i;
	int			ret;

	if (dep->number)
		return 0;

	memset(&params, 0x00, sizeof(params));
	cmd = DWC3_DEPCMD_DEPSTARTCFG;
	dwc = dep->dwc;

	ret = dwc3_send_gadget_ep_cmd(dep, cmd, &params);
	if (ret)
		return ret;

	for (i = 0; i < DWC3_ENDPOINTS_NUM; i++) {
		struct dwc3_ep *dep = dwc->eps[i];

		if (!dep)
			continue;

		ret = dwc3_gadget_set_xfer_resource(dep);
		if (ret)
			return ret;
	}

	return 0;
}

static int dwc3_gadget_set_ep_config(struct dwc3_ep *dep, unsigned int action)
{
	const struct usb_ss_ep_comp_descriptor *comp_desc;
	const struct usb_endpoint_descriptor *desc;
	struct dwc3_gadget_ep_cmd_params params;
	struct dwc3 *dwc = dep->dwc;

	comp_desc = dep->endpoint.comp_desc;
	desc = dep->endpoint.desc;

	memset(&params, 0x00, sizeof(params));

	params.param0 = DWC3_DEPCFG_EP_TYPE(usb_endpoint_type(desc))
		| DWC3_DEPCFG_MAX_PACKET_SIZE(usb_endpoint_maxp(desc));

	/* Burst size is only needed in SuperSpeed mode */
	if (dwc->gadget.speed >= USB_SPEED_SUPER) {
		u32 burst = dep->endpoint.maxburst;
		params.param0 |= DWC3_DEPCFG_BURST_SIZE(burst - 1);
	}

	params.param0 |= action;
	if (action == DWC3_DEPCFG_ACTION_RESTORE)
		params.param2 |= dep->saved_state;

	if (usb_endpoint_xfer_control(desc))
		params.param1 = DWC3_DEPCFG_XFER_COMPLETE_EN;

	if (dep->number <= 1 || usb_endpoint_xfer_isoc(desc))
		params.param1 |= DWC3_DEPCFG_XFER_NOT_READY_EN;

	if (usb_ss_max_streams(comp_desc) && usb_endpoint_xfer_bulk(desc)) {
		params.param1 |= DWC3_DEPCFG_STREAM_CAPABLE
			| DWC3_DEPCFG_STREAM_EVENT_EN;
		dep->stream_capable = true;
	}

	if (!usb_endpoint_xfer_control(desc))
		params.param1 |= DWC3_DEPCFG_XFER_IN_PROGRESS_EN;

	/*
	 * We are doing 1:1 mapping for endpoints, meaning
	 * Physical Endpoints 2 maps to Logical Endpoint 2 and
	 * so on. We consider the direction bit as part of the physical
	 * endpoint number. So USB endpoint 0x81 is 0x03.
	 */
	params.param1 |= DWC3_DEPCFG_EP_NUMBER(dep->number);

	/*
	 * We must use the lower 16 TX FIFOs even though
	 * HW might have more
	 */
	if (dep->direction)
		params.param0 |= DWC3_DEPCFG_FIFO_NUMBER(dep->number >> 1);

	if (desc->bInterval) {
		params.param1 |= DWC3_DEPCFG_BINTERVAL_M1(desc->bInterval - 1);
		dep->interval = 1 << (desc->bInterval - 1);
	}

	return dwc3_send_gadget_ep_cmd(dep, DWC3_DEPCMD_SETEPCONFIG, &params);
}

/**
 * __dwc3_gadget_ep_enable - initializes a hw endpoint
 * @dep: endpoint to be initialized
 * @action: one of INIT, MODIFY or RESTORE
 *
 * Caller should take care of locking. Execute all necessary commands to
 * initialize a HW endpoint so it can be used by a gadget driver.
 */
static int __dwc3_gadget_ep_enable(struct dwc3_ep *dep, unsigned int action)
{
	const struct usb_endpoint_descriptor *desc = dep->endpoint.desc;
	struct dwc3		*dwc = dep->dwc;

	u32			reg;
	int			ret;

	if (!(dep->flags & DWC3_EP_ENABLED)) {
		ret = dwc3_gadget_start_config(dep);
		if (ret)
			return ret;
	}

	ret = dwc3_gadget_set_ep_config(dep, action);
	if (ret)
		return ret;

	if (!(dep->flags & DWC3_EP_ENABLED)) {
		struct dwc3_trb	*trb_st_hw;
		struct dwc3_trb	*trb_link;

		dep->type = usb_endpoint_type(desc);
		dep->flags |= DWC3_EP_ENABLED;
		dep->flags &= ~DWC3_EP_END_TRANSFER_PENDING;

		reg = dwc3_readl(dwc->regs, DWC3_DALEPENA);
		reg |= DWC3_DALEPENA_EP(dep->number);
		dwc3_writel(dwc->regs, DWC3_DALEPENA, reg);

		if (usb_endpoint_xfer_control(desc))
			goto out;

		/* Initialize the TRB ring */
		dep->trb_dequeue = 0;
		dep->trb_enqueue = 0;
		memset(dep->trb_pool, 0,
		       sizeof(struct dwc3_trb) * DWC3_TRB_NUM);

		/* Link TRB. The HWO bit is never reset */
		trb_st_hw = &dep->trb_pool[0];

		trb_link = &dep->trb_pool[DWC3_TRB_NUM - 1];
		trb_link->bpl = lower_32_bits(dwc3_trb_dma_offset(dep, trb_st_hw));
		trb_link->bph = upper_32_bits(dwc3_trb_dma_offset(dep, trb_st_hw));
		trb_link->ctrl |= DWC3_TRBCTL_LINK_TRB;
		trb_link->ctrl |= DWC3_TRB_CTRL_HWO;
	}

	/*
	 * Issue StartTransfer here with no-op TRB so we can always rely on No
	 * Response Update Transfer command.
	 */
	if (usb_endpoint_xfer_bulk(desc) ||
			usb_endpoint_xfer_int(desc)) {
		struct dwc3_gadget_ep_cmd_params params;
		struct dwc3_trb	*trb;
		dma_addr_t trb_dma;
		u32 cmd;

		memset(&params, 0, sizeof(params));
		trb = &dep->trb_pool[0];
		trb_dma = dwc3_trb_dma_offset(dep, trb);

		params.param0 = upper_32_bits(trb_dma);
		params.param1 = lower_32_bits(trb_dma);

		cmd = DWC3_DEPCMD_STARTTRANSFER;

		ret = dwc3_send_gadget_ep_cmd(dep, cmd, &params);
		if (ret < 0)
			return ret;
	}

out:
	trace_dwc3_gadget_ep_enable(dep);

	return 0;
}

static void dwc3_stop_active_transfer(struct dwc3_ep *dep, bool force,
		bool interrupt);
static void dwc3_remove_requests(struct dwc3 *dwc, struct dwc3_ep *dep)
{
	struct dwc3_request		*req;

	dwc3_stop_active_transfer(dep, true, false);

	/* - giveback all requests to gadget driver */
	while (!list_empty(&dep->started_list)) {
		req = next_request(&dep->started_list);

		dwc3_gadget_giveback(dep, req, -ESHUTDOWN);
	}

	while (!list_empty(&dep->pending_list)) {
		req = next_request(&dep->pending_list);

		dwc3_gadget_giveback(dep, req, -ESHUTDOWN);
	}

	while (!list_empty(&dep->cancelled_list)) {
		req = next_request(&dep->cancelled_list);

		dwc3_gadget_giveback(dep, req, -ESHUTDOWN);
	}
}

/**
 * __dwc3_gadget_ep_disable - disables a hw endpoint
 * @dep: the endpoint to disable
 *
 * This function undoes what __dwc3_gadget_ep_enable did and also removes
 * requests which are currently being processed by the hardware and those which
 * are not yet scheduled.
 *
 * Caller should take care of locking.
 */
static int __dwc3_gadget_ep_disable(struct dwc3_ep *dep)
{
	struct dwc3		*dwc = dep->dwc;
	u32			reg;

	trace_dwc3_gadget_ep_disable(dep);

	dwc3_remove_requests(dwc, dep);

	/* make sure HW endpoint isn't stalled */
	if (dep->flags & DWC3_EP_STALL)
		__dwc3_gadget_ep_set_halt(dep, 0, false);

	reg = dwc3_readl(dwc->regs, DWC3_DALEPENA);
	reg &= ~DWC3_DALEPENA_EP(dep->number);
	dwc3_writel(dwc->regs, DWC3_DALEPENA, reg);

	dep->stream_capable = false;
	dep->type = 0;
	dep->flags &= DWC3_EP_END_TRANSFER_PENDING;

	/* Clear out the ep descriptors for non-ep0 */
	if (dep->number > 1) {
		dep->endpoint.comp_desc = NULL;
		dep->endpoint.desc = NULL;
	}

	return 0;
}

/* -------------------------------------------------------------------------- */

static int dwc3_gadget_ep0_enable(struct usb_ep *ep,
		const struct usb_endpoint_descriptor *desc)
{
	return -EINVAL;
}

static int dwc3_gadget_ep0_disable(struct usb_ep *ep)
{
	return -EINVAL;
}

/* -------------------------------------------------------------------------- */

static int dwc3_gadget_ep_enable(struct usb_ep *ep,
		const struct usb_endpoint_descriptor *desc)
{
	struct dwc3_ep			*dep;
	struct dwc3			*dwc;
	unsigned long			flags;
	int				ret;

	if (!ep || !desc || desc->bDescriptorType != USB_DT_ENDPOINT) {
		pr_debug("dwc3: invalid parameters\n");
		return -EINVAL;
	}

	if (!desc->wMaxPacketSize) {
		pr_debug("dwc3: missing wMaxPacketSize\n");
		return -EINVAL;
	}

	dep = to_dwc3_ep(ep);
	dwc = dep->dwc;
#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
	if (dev_WARN_ONCE(dwc->dev, dep->flags & DWC3_EP_ENABLED,
					"%s is already enabled\n",
					dep->name))
#else
	if (dep->flags & DWC3_EP_ENABLED)
#endif
		return 0;

	spin_lock_irqsave(&dwc->lock, flags);
	ret = __dwc3_gadget_ep_enable(dep, DWC3_DEPCFG_ACTION_INIT);
	spin_unlock_irqrestore(&dwc->lock, flags);

	return ret;
}

static int dwc3_gadget_ep_disable(struct usb_ep *ep)
{
	struct dwc3_ep			*dep;
	struct dwc3			*dwc;
	unsigned long			flags;
	int				ret;

	if (!ep) {
		pr_debug("dwc3: invalid parameters\n");
		return -EINVAL;
	}

	dep = to_dwc3_ep(ep);
	dwc = dep->dwc;
#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
	if (dev_WARN_ONCE(dwc->dev, !(dep->flags & DWC3_EP_ENABLED),
					"%s is already disabled\n",
					dep->name))
#else
	if (!(dep->flags & DWC3_EP_ENABLED))
#endif
		return 0;

	spin_lock_irqsave(&dwc->lock, flags);
	ret = __dwc3_gadget_ep_disable(dep);
	spin_unlock_irqrestore(&dwc->lock, flags);

	return ret;
}

static struct usb_request *dwc3_gadget_ep_alloc_request(struct usb_ep *ep,
		gfp_t gfp_flags)
{
	struct dwc3_request		*req;
	struct dwc3_ep			*dep = to_dwc3_ep(ep);

	req = kzalloc(sizeof(*req), gfp_flags);
	if (!req)
		return NULL;

	req->direction	= dep->direction;
	req->epnum	= dep->number;
	req->dep	= dep;

	trace_dwc3_alloc_request(req);

	return &req->request;
}

static void dwc3_gadget_ep_free_request(struct usb_ep *ep,
		struct usb_request *request)
{
	struct dwc3_request		*req = to_dwc3_request(request);
	struct dwc3_ep			*dep = to_dwc3_ep(ep);
	struct dwc3				*dwc = dep->dwc;
	unsigned long			flags;

	trace_dwc3_free_request(req);

	spin_lock_irqsave(&dwc->lock, flags);
	if (req->list.next != LIST_POISON1) {
		if (req->list.next != NULL)
			list_del(&req->list);
	}
	spin_unlock_irqrestore(&dwc->lock, flags);

	kfree(req);
}

/**
 * dwc3_ep_prev_trb - returns the previous TRB in the ring
 * @dep: The endpoint with the TRB ring
 * @index: The index of the current TRB in the ring
 *
 * Returns the TRB prior to the one pointed to by the index. If the
 * index is 0, we will wrap backwards, skip the link TRB, and return
 * the one just before that.
 */
static struct dwc3_trb *dwc3_ep_prev_trb(struct dwc3_ep *dep, u8 index)
{
	u8 tmp = index;

	if (!tmp)
		tmp = DWC3_TRB_NUM - 1;

	return &dep->trb_pool[tmp - 1];
}

static u32 dwc3_calc_trbs_left(struct dwc3_ep *dep)
{
	struct dwc3_trb		*tmp;
	u8			trbs_left;

	/*
	 * If enqueue & dequeue are equal than it is either full or empty.
	 *
	 * One way to know for sure is if the TRB right before us has HWO bit
	 * set or not. If it has, then we're definitely full and can't fit any
	 * more transfers in our ring.
	 */
	if (dep->trb_enqueue == dep->trb_dequeue) {
		tmp = dwc3_ep_prev_trb(dep, dep->trb_enqueue);
		if (tmp->ctrl & DWC3_TRB_CTRL_HWO)
			return 0;

		return DWC3_TRB_NUM - 1;
	}

	trbs_left = dep->trb_dequeue - dep->trb_enqueue;
	trbs_left &= (DWC3_TRB_NUM - 1);

	if (dep->trb_dequeue < dep->trb_enqueue)
		trbs_left--;

	return trbs_left;
}

static void __dwc3_prepare_one_trb(struct dwc3_ep *dep, struct dwc3_trb *trb,
		dma_addr_t dma, unsigned length, unsigned chain, unsigned node,
		unsigned stream_id, unsigned short_not_ok, unsigned no_interrupt)
{
	struct dwc3		*dwc = dep->dwc;
	struct usb_gadget	*gadget = &dwc->gadget;
	enum usb_device_speed	speed = gadget->speed;

	trb->size = DWC3_TRB_SIZE_LENGTH(length);
	trb->bpl = lower_32_bits(dma);
	trb->bph = upper_32_bits(dma);

	switch (usb_endpoint_type(dep->endpoint.desc)) {
	case USB_ENDPOINT_XFER_CONTROL:
		trb->ctrl = DWC3_TRBCTL_CONTROL_SETUP;
		break;

	case USB_ENDPOINT_XFER_ISOC:
		if (!node) {
			trb->ctrl = DWC3_TRBCTL_ISOCHRONOUS_FIRST;

			/*
			 * USB Specification 2.0 Section 5.9.2 states that: "If
			 * there is only a single transaction in the microframe,
			 * only a DATA0 data packet PID is used.  If there are
			 * two transactions per microframe, DATA1 is used for
			 * the first transaction data packet and DATA0 is used
			 * for the second transaction data packet.  If there are
			 * three transactions per microframe, DATA2 is used for
			 * the first transaction data packet, DATA1 is used for
			 * the second, and DATA0 is used for the third."
			 *
			 * IOW, we should satisfy the following cases:
			 *
			 * 1) length <= maxpacket
			 *	- DATA0
			 *
			 * 2) maxpacket < length <= (2 * maxpacket)
			 *	- DATA1, DATA0
			 *
			 * 3) (2 * maxpacket) < length <= (3 * maxpacket)
			 *	- DATA2, DATA1, DATA0
			 */
			if (speed == USB_SPEED_HIGH) {
				struct usb_ep *ep = &dep->endpoint;
				unsigned int mult = 2;
				unsigned int maxp = usb_endpoint_maxp(ep->desc);

				if (length <= (2 * maxp))
					mult--;

				if (length <= maxp)
					mult--;

				trb->size |= DWC3_TRB_SIZE_PCM1(mult);
			}
		} else {
			trb->ctrl = DWC3_TRBCTL_ISOCHRONOUS;
		}

		/* always enable Interrupt on Missed ISOC */
		trb->ctrl |= DWC3_TRB_CTRL_ISP_IMI;
		break;

	case USB_ENDPOINT_XFER_BULK:
	case USB_ENDPOINT_XFER_INT:
		trb->ctrl = DWC3_TRBCTL_NORMAL;
		break;
	default:
		/*
		 * This is only possible with faulty memory because we
		 * checked it already :)
		 */
#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
		dev_WARN(dwc->dev, "Unknown endpoint type %d\n",
				usb_endpoint_type(dep->endpoint.desc));
#endif
		break;
	}

	/*
	 * Enable Continue on Short Packet
	 * when endpoint is not a stream capable
	 */
	if (usb_endpoint_dir_out(dep->endpoint.desc)) {
		if (!dep->stream_capable)
			trb->ctrl |= DWC3_TRB_CTRL_CSP;

		if (short_not_ok)
			trb->ctrl |= DWC3_TRB_CTRL_ISP_IMI;
	}

	if ((!no_interrupt && !chain) ||
			(dwc3_calc_trbs_left(dep) == 1))
		trb->ctrl |= DWC3_TRB_CTRL_IOC;

	if (chain)
		trb->ctrl |= DWC3_TRB_CTRL_CHN;

	if (usb_endpoint_xfer_bulk(dep->endpoint.desc) && dep->stream_capable)
		trb->ctrl |= DWC3_TRB_CTRL_SID_SOFN(stream_id);

	trb->ctrl |= DWC3_TRB_CTRL_HWO;

	dwc3_ep_inc_enq(dep);

	trace_dwc3_prepare_trb(dep, trb);
}

/**
 * dwc3_prepare_one_trb - setup one TRB from one request
 * @dep: endpoint for which this request is prepared
 * @req: dwc3_request pointer
 * @chain: should this TRB be chained to the next?
 * @node: only for isochronous endpoints. First TRB needs different type.
 */
static void dwc3_prepare_one_trb(struct dwc3_ep *dep,
		struct dwc3_request *req, unsigned chain, unsigned node)
{
	struct dwc3_trb		*trb;
	unsigned int		length;
	dma_addr_t		dma;
	unsigned		stream_id = req->request.stream_id;
	unsigned		short_not_ok = req->request.short_not_ok;
	unsigned		no_interrupt = req->request.no_interrupt;

	if (req->request.num_sgs > 0) {
		length = sg_dma_len(req->start_sg);
		dma = sg_dma_address(req->start_sg);
	} else {
		length = req->request.length;
		dma = req->request.dma;
	}

	trb = &dep->trb_pool[dep->trb_enqueue];

	if (!req->trb) {
		dwc3_gadget_move_started_request(req);
		req->trb = trb;
		req->trb_dma = dwc3_trb_dma_offset(dep, trb);
	}

	req->num_trbs++;

	__dwc3_prepare_one_trb(dep, trb, dma, length, chain, node,
			stream_id, short_not_ok, no_interrupt);
}

static void dwc3_prepare_one_trb_sg(struct dwc3_ep *dep,
		struct dwc3_request *req)
{
	struct scatterlist *sg = req->start_sg;
	struct scatterlist *s;
	int		i;

	unsigned int remaining = req->request.num_mapped_sgs
		- req->num_queued_sgs;

	for_each_sg(sg, s, remaining, i) {
		unsigned int length = req->request.length;
		unsigned int maxp = usb_endpoint_maxp(dep->endpoint.desc);
		unsigned int rem = length % maxp;
		unsigned chain = true;

		if (sg_is_last(s))
			chain = false;

		if (rem && usb_endpoint_dir_out(dep->endpoint.desc) && !chain) {
			struct dwc3	*dwc = dep->dwc;
			struct dwc3_trb	*trb;

			req->needs_extra_trb = true;

			/* prepare normal TRB */
			dwc3_prepare_one_trb(dep, req, true, i);

			/* Now prepare one extra TRB to align transfer size */
			trb = &dep->trb_pool[dep->trb_enqueue];
			req->num_trbs++;
			__dwc3_prepare_one_trb(dep, trb, dwc->bounce_addr,
					maxp - rem, false, 1,
					req->request.stream_id,
					req->request.short_not_ok,
					req->request.no_interrupt);
		} else {
			dwc3_prepare_one_trb(dep, req, chain, i);
		}

		/*
		 * There can be a situation where all sgs in sglist are not
		 * queued because of insufficient trb number. To handle this
		 * case, update start_sg to next sg to be queued, so that
		 * we have free trbs we can continue queuing from where we
		 * previously stopped
		 */
		if (chain)
			req->start_sg = sg_next(s);

		req->num_queued_sgs++;

		if (!dwc3_calc_trbs_left(dep))
			break;
	}
}

static void dwc3_prepare_one_trb_linear(struct dwc3_ep *dep,
		struct dwc3_request *req)
{
	unsigned int length = req->request.length;
	unsigned int maxp = usb_endpoint_maxp(dep->endpoint.desc);
	unsigned int rem = length % maxp;

	if ((!length || rem) && usb_endpoint_dir_out(dep->endpoint.desc)) {
		struct dwc3	*dwc = dep->dwc;
		struct dwc3_trb	*trb;

		req->needs_extra_trb = true;

		/* prepare normal TRB */
		dwc3_prepare_one_trb(dep, req, true, 0);

		/* Now prepare one extra TRB to align transfer size */
		trb = &dep->trb_pool[dep->trb_enqueue];
		req->num_trbs++;
		__dwc3_prepare_one_trb(dep, trb, dwc->bounce_addr, maxp - rem,
				false, 1, req->request.stream_id,
				req->request.short_not_ok,
				req->request.no_interrupt);
	} else if (req->request.zero && req->request.length &&
		   (IS_ALIGNED(req->request.length, maxp))) {
		struct dwc3	*dwc = dep->dwc;
		struct dwc3_trb	*trb;

		req->needs_extra_trb = true;

		/* prepare normal TRB */
		dwc3_prepare_one_trb(dep, req, true, 0);

		/* Now prepare one extra TRB to handle ZLP */
		trb = &dep->trb_pool[dep->trb_enqueue];
		req->num_trbs++;
		__dwc3_prepare_one_trb(dep, trb, dwc->bounce_addr, 0,
				false, 1, req->request.stream_id,
				req->request.short_not_ok,
				req->request.no_interrupt);
	} else {
		dwc3_prepare_one_trb(dep, req, false, 0);
	}
}

static int dwc3_dump_request(struct dwc3_request *req)
{
	pr_info("%s: start to dump dwc3_request!\n", __func__);
	pr_info("list->next: 0x%016llx, list->prev: 0x%016llx\n",
		(unsigned long long)req->list.next, (unsigned long long)req->list.prev);
	if (req->sg)
		pr_info("sg: 0x%016llx\n", (unsigned long long)req->sg);
	if (req->start_sg)
		pr_info("start_sg: 0x%016llx\n", (unsigned long long)req->start_sg);
	pr_info("num_pending_sgs: 0x%08x, num_queued_sgs: 0x%08x remaining: 0x%08x\n",
		req->num_pending_sgs, req->num_queued_sgs, req->remaining);
	pr_info("epnum: 0x%08x\n", req->epnum);
	if (req->trb)
		pr_info("req->trb: 0x%016llx\n", (unsigned long long)req->trb);
	pr_info("trb_dma: 0x%016llx\n", (unsigned long long)req->trb_dma);
	pr_info("num_trbs: 0x%08x\n", req->num_trbs);
	pr_info("needs_extra_trb: 0x%08x\n", req->needs_extra_trb);

	pr_info("%s: start to dump usb_request!\n", __func__);
	pr_info("buf: 0x%016llx\n", (unsigned long long)req->request.buf);
	pr_info("length: 0x%08x\n", req->request.length);
	pr_info("dma: 0x%016llx\n", (unsigned long long)req->request.dma);
	if (req->request.sg)
		pr_info("sg: 0x%016llx\n", (unsigned long long)req->request.sg);
	pr_info("num_sgs: 0x%08x\n", req->request.num_sgs);
	pr_info("num_mapped_sgs: 0x%08x\n", req->request.num_mapped_sgs);
	pr_info("stream_id: 0x%08x\n", req->request.stream_id);
	pr_info("list->next: 0x%016llx, list->prev:0x%016llx\n",
		(unsigned long long)req->request.list.next,
		(unsigned long long)req->request.list.prev);
	pr_info("status: %d\n", req->request.status);
	pr_info("actual: 0x%08x\n", req->request.actual);

	dump_stack();
	return 0;
}
/*
 * dwc3_prepare_trbs - setup TRBs from requests
 * @dep: endpoint for which requests are being prepared
 *
 * The function goes through the requests list and sets up TRBs for the
 * transfers. The function returns once there are no more TRBs available or
 * it runs out of requests.
 */
static void dwc3_prepare_trbs(struct dwc3_ep *dep)
{
	struct dwc3_request	*req, *n;

	BUILD_BUG_ON_NOT_POWER_OF_2(DWC3_TRB_NUM);

	/*
	 * We can get in a situation where there's a request in the started list
	 * but there weren't enough TRBs to fully kick it in the first time
	 * around, so it has been waiting for more TRBs to be freed up.
	 *
	 * In that case, we should check if we have a request with pending_sgs
	 * in the started list and prepare TRBs for that request first,
	 * otherwise we will prepare TRBs completely out of order and that will
	 * break things.
	 */
	list_for_each_entry(req, &dep->started_list, list) {
		if (req->num_pending_sgs > 0) {
			dwc3_dump_request(req);
			dwc3_prepare_one_trb_sg(dep, req);
		}

		if (!dwc3_calc_trbs_left(dep))
			return;
	}

	list_for_each_entry_safe(req, n, &dep->pending_list, list) {
		struct dwc3	*dwc = dep->dwc;
		int		ret;

		ret = usb_gadget_map_request_by_dev(dwc->sysdev, &req->request,
						    dep->direction);
		if (ret)
			return;

		req->sg			= req->request.sg;
		req->start_sg		= req->sg;
		req->num_queued_sgs	= 0;
		req->num_pending_sgs	= req->request.num_mapped_sgs;

		if (req->num_pending_sgs > 0) {
			dwc3_dump_request(req);
			dwc3_prepare_one_trb_sg(dep, req);
		} else
			dwc3_prepare_one_trb_linear(dep, req);

		if (!dwc3_calc_trbs_left(dep))
			return;
	}
}

static int __dwc3_gadget_kick_transfer(struct dwc3_ep *dep)
{
	struct dwc3_gadget_ep_cmd_params params;
	struct dwc3_request		*req;
	int				starting;
	int				ret;
	u32				cmd;

	if (!dwc3_calc_trbs_left(dep))
		return 0;

	starting = !(dep->flags & DWC3_EP_TRANSFER_STARTED);

	dwc3_prepare_trbs(dep);
	req = next_request(&dep->started_list);
	if (!req) {
		dep->flags |= DWC3_EP_PENDING_REQUEST;
		return 0;
	}

	memset(&params, 0, sizeof(params));

	if (starting) {
		params.param0 = upper_32_bits(req->trb_dma);
		params.param1 = lower_32_bits(req->trb_dma);
		cmd = DWC3_DEPCMD_STARTTRANSFER;

		if (usb_endpoint_xfer_isoc(dep->endpoint.desc))
			cmd |= DWC3_DEPCMD_PARAM(dep->frame_number);
	} else {
		cmd = DWC3_DEPCMD_UPDATETRANSFER |
			DWC3_DEPCMD_PARAM(dep->resource_index);
	}

	ret = dwc3_send_gadget_ep_cmd(dep, cmd, &params);
	if (ret < 0) {
		/*
		 * FIXME we need to iterate over the list of requests
		 * here and stop, unmap, free and del each of the linked
		 * requests instead of what we do now.
		 */
		if (req->trb)
			memset(req->trb, 0, sizeof(struct dwc3_trb));
		dwc3_gadget_del_and_unmap_request(dep, req, ret);
		return ret;
	}

	return 0;
}

static int __dwc3_gadget_get_frame(struct dwc3 *dwc)
{
	u32			reg;

	reg = dwc3_readl(dwc->regs, DWC3_DSTS);
	return DWC3_DSTS_SOFFN(reg);
}

static void __dwc3_gadget_start_isoc(struct dwc3_ep *dep)
{
	if (list_empty(&dep->pending_list)) {
		dev_info(dep->dwc->dev, "%s: ran out of requests\n",
				dep->name);
		dep->flags |= DWC3_EP_PENDING_REQUEST;
		return;
	}

	dep->frame_number = DWC3_ALIGN_FRAME(dep);
	__dwc3_gadget_kick_transfer(dep);
}

static int __dwc3_gadget_ep_queue(struct dwc3_ep *dep, struct dwc3_request *req)
{
	struct dwc3		*dwc = dep->dwc;

	if (!dep->endpoint.desc) {
		dev_err(dwc->dev, "%s: can't queue to disabled endpoint\n",
				dep->name);
		return -ESHUTDOWN;
	}

#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
	if (WARN(req->dep != dep, "request %pK belongs to '%s'\n",
				&req->request, req->dep->name))
#else
	if (req->dep != dep)
#endif
		return -EINVAL;

	pm_runtime_get(dwc->dev);

	req->request.actual	= 0;
	req->request.status	= -EINPROGRESS;

	trace_dwc3_ep_queue(req);

	list_add_tail(&req->list, &dep->pending_list);

	/* prevent starting transfer if controller is stopped */
	if (!dwc->pullups_connected) {
		dev_dbg(dwc->dev, "queue request while udc is stopped");
		return 0;
	}

	/*
	 * NOTICE: Isochronous endpoints should NEVER be prestarted. We must
	 * wait for a XferNotReady event so we will know what's the current
	 * (micro-)frame number.
	 *
	 * Without this trick, we are very, very likely gonna get Bus Expiry
	 * errors which will force us issue EndTransfer command.
	 */
	if (usb_endpoint_xfer_isoc(dep->endpoint.desc)) {
		if (!(dep->flags & DWC3_EP_PENDING_REQUEST) &&
				!(dep->flags & DWC3_EP_TRANSFER_STARTED))
			return 0;

		if ((dep->flags & DWC3_EP_PENDING_REQUEST)) {
			if (!(dep->flags & DWC3_EP_TRANSFER_STARTED)) {
				__dwc3_gadget_start_isoc(dep);
				return 0;
			}
		}
	}

	return __dwc3_gadget_kick_transfer(dep);
}

static int dwc3_gadget_ep_queue(struct usb_ep *ep, struct usb_request *request,
	gfp_t gfp_flags)
{
	struct dwc3_request		*req = to_dwc3_request(request);
	struct dwc3_ep			*dep = to_dwc3_ep(ep);
	struct dwc3			*dwc = dep->dwc;

	unsigned long			flags;

	int				ret;

	spin_lock_irqsave(&dwc->lock, flags);
	ret = __dwc3_gadget_ep_queue(dep, req);
	spin_unlock_irqrestore(&dwc->lock, flags);

	return ret;
}

static void dwc3_gadget_ep_skip_trbs(struct dwc3_ep *dep, struct dwc3_request *req)
{
	int i;

	/*
	 * If request was already started, this means we had to
	 * stop the transfer. With that we also need to ignore
	 * all TRBs used by the request, however TRBs can only
	 * be modified after completion of END_TRANSFER
	 * command. So what we do here is that we wait for
	 * END_TRANSFER completion and only after that, we jump
	 * over TRBs by clearing HWO and incrementing dequeue
	 * pointer.
	 */
	for (i = 0; i < req->num_trbs; i++) {
		struct dwc3_trb *trb;

		trb = req->trb + i;
		trb->ctrl &= ~DWC3_TRB_CTRL_HWO;
		dwc3_ep_inc_deq(dep);
	}

	req->num_trbs = 0;
}

static void dwc3_gadget_ep_cleanup_cancelled_requests(struct dwc3_ep *dep)
{
	struct dwc3_request		*req;
	struct dwc3_request		*tmp;

	list_for_each_entry_safe(req, tmp, &dep->cancelled_list, list) {
		dwc3_gadget_ep_skip_trbs(dep, req);
		dwc3_gadget_giveback(dep, req, -ECONNRESET);
	}
}

static int dwc3_gadget_ep_dequeue(struct usb_ep *ep,
		struct usb_request *request)
{
	struct dwc3_request		*req = to_dwc3_request(request);
	struct dwc3_request		*r = NULL;

	struct dwc3_ep			*dep = to_dwc3_ep(ep);
	struct dwc3			*dwc = dep->dwc;

	unsigned long			flags;
	int				ret = 0;

	trace_dwc3_ep_dequeue(req);

	spin_lock_irqsave(&dwc->lock, flags);

	list_for_each_entry(r, &dep->pending_list, list) {
		if (r == req)
			break;
	}

	if (r != req) {
		list_for_each_entry(r, &dep->started_list, list) {
			if (r == req)
				break;
		}
		if (r == req) {
			/* wait until it is processed */
			dwc3_stop_active_transfer(dep, true, true);

			if (!r->trb)
				goto out0;

			dwc3_gadget_move_cancelled_request(req);
			if (dep->flags & DWC3_EP_TRANSFER_STARTED)
				goto out0;
			else
				goto out1;
		}
		dev_err(dwc->dev, "request %pK was not queued to %s\n",
				request, ep->name);
		ret = -EINVAL;
		goto out0;
	}

out1:
	dwc3_gadget_ep_skip_trbs(dep, req);
	dwc3_gadget_giveback(dep, req, -ECONNRESET);

out0:
	spin_unlock_irqrestore(&dwc->lock, flags);

	return ret;
}

int __dwc3_gadget_ep_set_halt(struct dwc3_ep *dep, int value, int protocol)
{
	struct dwc3_gadget_ep_cmd_params	params;
	struct dwc3				*dwc = dep->dwc;
	int					ret;

	if (dep->endpoint.desc == NULL)
		return -EINVAL;

	if (usb_endpoint_xfer_isoc(dep->endpoint.desc)) {
		dev_err(dwc->dev, "%s is of Isochronous type\n", dep->name);
		return -EINVAL;
	}

	memset(&params, 0x00, sizeof(params));

	if (value) {
		struct dwc3_trb *trb;

		unsigned transfer_in_flight;
		unsigned started;

		if (dep->number > 1)
			trb = dwc3_ep_prev_trb(dep, dep->trb_enqueue);
		else
			trb = &dwc->ep0_trb[dep->trb_enqueue];

		transfer_in_flight = trb->ctrl & DWC3_TRB_CTRL_HWO;
		started = !list_empty(&dep->started_list);

		if (!protocol && ((dep->direction && transfer_in_flight) ||
				(!dep->direction && started))) {
			return -EAGAIN;
		}

		ret = dwc3_send_gadget_ep_cmd(dep, DWC3_DEPCMD_SETSTALL,
				&params);
		if (ret)
			dev_err(dwc->dev, "failed to set STALL on %s\n",
					dep->name);
		else
			dep->flags |= DWC3_EP_STALL;
	} else {

		ret = dwc3_send_clear_stall_ep_cmd(dep);
		if (ret)
			dev_err(dwc->dev, "failed to clear STALL on %s\n",
					dep->name);
		else
			dep->flags &= ~(DWC3_EP_STALL | DWC3_EP_WEDGE);
	}

	return ret;
}

static int dwc3_gadget_ep_set_halt(struct usb_ep *ep, int value)
{
	struct dwc3_ep			*dep = to_dwc3_ep(ep);
	struct dwc3			*dwc = dep->dwc;

	unsigned long			flags;

	int				ret;

	spin_lock_irqsave(&dwc->lock, flags);
	ret = __dwc3_gadget_ep_set_halt(dep, value, false);
	spin_unlock_irqrestore(&dwc->lock, flags);

	return ret;
}

static int dwc3_gadget_ep_set_wedge(struct usb_ep *ep)
{
	struct dwc3_ep			*dep = to_dwc3_ep(ep);
	struct dwc3			*dwc = dep->dwc;
	unsigned long			flags;
	int				ret;

	spin_lock_irqsave(&dwc->lock, flags);
	dep->flags |= DWC3_EP_WEDGE;

	if (dep->number == 0 || dep->number == 1)
		ret = __dwc3_gadget_ep0_set_halt(ep, 1);
	else
		ret = __dwc3_gadget_ep_set_halt(dep, 1, false);
	spin_unlock_irqrestore(&dwc->lock, flags);

	return ret;
}

/* -------------------------------------------------------------------------- */

static struct usb_endpoint_descriptor dwc3_gadget_ep0_desc = {
	.bLength	= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bmAttributes	= USB_ENDPOINT_XFER_CONTROL,
};

static const struct usb_ep_ops dwc3_gadget_ep0_ops = {
	.enable		= dwc3_gadget_ep0_enable,
	.disable	= dwc3_gadget_ep0_disable,
	.alloc_request	= dwc3_gadget_ep_alloc_request,
	.free_request	= dwc3_gadget_ep_free_request,
	.queue		= dwc3_gadget_ep0_queue,
	.dequeue	= dwc3_gadget_ep_dequeue,
	.set_halt	= dwc3_gadget_ep0_set_halt,
	.set_wedge	= dwc3_gadget_ep_set_wedge,
};

static const struct usb_ep_ops dwc3_gadget_ep_ops = {
	.enable		= dwc3_gadget_ep_enable,
	.disable	= dwc3_gadget_ep_disable,
	.alloc_request	= dwc3_gadget_ep_alloc_request,
	.free_request	= dwc3_gadget_ep_free_request,
	.queue		= dwc3_gadget_ep_queue,
	.dequeue	= dwc3_gadget_ep_dequeue,
	.set_halt	= dwc3_gadget_ep_set_halt,
	.set_wedge	= dwc3_gadget_ep_set_wedge,
};

/* -------------------------------------------------------------------------- */

static int dwc3_gadget_get_frame(struct usb_gadget *g)
{
	struct dwc3		*dwc = gadget_to_dwc(g);

	return __dwc3_gadget_get_frame(dwc);
}

static int __dwc3_gadget_wakeup(struct dwc3 *dwc)
{
	int			retries;

	int			ret;
	u32			reg;

	u8			link_state;
	u8			speed;

	/*
	 * According to the Databook Remote wakeup request should
	 * be issued only when the device is in early suspend state.
	 *
	 * We can check that via USB Link State bits in DSTS register.
	 */
	reg = dwc3_readl(dwc->regs, DWC3_DSTS);

	speed = reg & DWC3_DSTS_CONNECTSPD;
	if ((speed == DWC3_DSTS_SUPERSPEED) ||
	    (speed == DWC3_DSTS_SUPERSPEED_PLUS))
		return 0;

	link_state = DWC3_DSTS_USBLNKST(reg);

	switch (link_state) {
	case DWC3_LINK_STATE_RX_DET:	/* in HS, means Early Suspend */
#if defined(CONFIG_SOC_EXYNOS9830) /* To support L1*/
	case DWC3_LINK_STATE_U2:	/* in HS, means SLEEP */
#endif
	case DWC3_LINK_STATE_U3:	/* in HS, means SUSPEND */
		break;
	default:
		return -EINVAL;
	}

	ret = dwc3_gadget_set_link_state(dwc, DWC3_LINK_STATE_RECOV);
	if (ret < 0) {
		dev_err(dwc->dev, "failed to put link in Recovery\n");
		return ret;
	}

	/* Recent versions do this automatically */
	if (dwc->revision < DWC3_REVISION_194A) {
		/* write zeroes to Link Change Request */
		reg = dwc3_readl(dwc->regs, DWC3_DCTL);
		reg &= ~DWC3_DCTL_ULSTCHNGREQ_MASK;
		dwc3_writel(dwc->regs, DWC3_DCTL, reg);
	}

	/* poll until Link State changes to ON */
	retries = 20000;

	while (retries--) {
		reg = dwc3_readl(dwc->regs, DWC3_DSTS);

		/* in HS, means ON */
		if (DWC3_DSTS_USBLNKST(reg) == DWC3_LINK_STATE_U0)
			break;
	}

	if (DWC3_DSTS_USBLNKST(reg) != DWC3_LINK_STATE_U0) {
		dev_err(dwc->dev, "failed to send remote wakeup\n");
		return -EINVAL;
	}

	return 0;
}

static int dwc3_gadget_wakeup(struct usb_gadget *g)
{
	struct dwc3		*dwc = gadget_to_dwc(g);
	unsigned long		flags;
	int			ret;

	spin_lock_irqsave(&dwc->lock, flags);
	ret = __dwc3_gadget_wakeup(dwc);
	spin_unlock_irqrestore(&dwc->lock, flags);

	return ret;
}

static int dwc3_gadget_set_selfpowered(struct usb_gadget *g,
		int is_selfpowered)
{
	struct dwc3		*dwc = gadget_to_dwc(g);
	unsigned long		flags;

	spin_lock_irqsave(&dwc->lock, flags);
	g->is_selfpowered = !!is_selfpowered;
	spin_unlock_irqrestore(&dwc->lock, flags);

	return 0;
}

static int dwc3_udc_init(struct dwc3 *dwc)
{
	struct dwc3_ep		*dep;
	u32			reg;
	int			ret = 0;

	reg = dwc3_readl(dwc->regs, DWC3_DCFG);
	reg &= ~(DWC3_DCFG_SPEED_MASK);

	/**
	 * WORKAROUND: DWC3 revision < 2.20a have an issue
	 * which would cause metastability state on Run/Stop
	 * bit if we try to force the IP to USB2-only mode.
	 *
	 * Because of that, we cannot configure the IP to any
	 * speed other than the SuperSpeed
	 *
	 * Refers to:
	 *
	 * STAR#9000525659: Clock Domain Crossing on DCTL in
	 * USB 2.0 Mode
	 */
	if (dwc->revision < DWC3_REVISION_220A) {
		reg |= DWC3_DCFG_SUPERSPEED;
	} else {
		switch (dwc->maximum_speed) {
		case USB_SPEED_LOW:
			reg |= DWC3_DSTS_LOWSPEED;
			break;
		case USB_SPEED_FULL:
			reg |= DWC3_DSTS_FULLSPEED1;
			break;
		case USB_SPEED_HIGH:
			reg |= DWC3_DSTS_HIGHSPEED;
			break;
		case USB_SPEED_SUPER:
			reg |= DWC3_DSTS_SUPERSPEED;
			break;
		case USB_SPEED_SUPER_PLUS:
			reg |= DWC3_DSTS_SUPERSPEED_PLUS;
			break;
		case USB_SPEED_UNKNOWN: /* FALTHROUGH */
		default:
			reg |= DWC3_DSTS_HIGHSPEED;
		}
	}
	dwc3_writel(dwc->regs, DWC3_DCFG, reg);

	/* Start with SuperSpeed Default */
	dwc3_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(512);

	dep = dwc->eps[0];
	ret = __dwc3_gadget_ep_enable(dep, DWC3_DEPCFG_ACTION_INIT);
	if (ret) {
		dev_err(dwc->dev, "failed to enable %s\n", dep->name);
		goto err0;
	}

	dep = dwc->eps[1];
	ret = __dwc3_gadget_ep_enable(dep, DWC3_DEPCFG_ACTION_INIT);
	if (ret) {
		dev_err(dwc->dev, "failed to enable %s\n", dep->name);
		goto err1;
	}

	/* begin to receive SETUP packets */
	dwc->ep0state = EP0_SETUP_PHASE;
	dwc3_ep0_out_start(dwc);

	return 0;

err1:
	__dwc3_gadget_ep_disable(dwc->eps[0]);

err0:
	return ret;
}

static void dwc3_gadget_enable_irq(struct dwc3 *dwc)
{
	u32			reg;

	/* Enable all but Start and End of Frame IRQs */
	reg = (DWC3_DEVTEN_VNDRDEVTSTRCVEDEN |
			DWC3_DEVTEN_WKUPEVTEN |
			DWC3_DEVTEN_ULSTCNGEN |
			DWC3_DEVTEN_CONNECTDONEEN |
			DWC3_DEVTEN_USBRSTEN |
			DWC3_DEVTEN_DISCONNEVTEN);

	dwc3_writel(dwc->regs, DWC3_DEVTEN, reg);
}

static void dwc3_gadget_disable_irq(struct dwc3 *dwc)
{
	/* mask all interrupts */
	dwc3_writel(dwc->regs, DWC3_DEVTEN, 0x00);
}

static int dwc3_gadget_run_stop(struct dwc3 *dwc, int is_on, int suspend)
{
	u32			reg;
	u32			timeout = 1000;
	int			retries = 1000;
	int			ret = 0;

	if (pm_runtime_suspended(dwc->dev))
		return 0;

	reg = dwc3_readl(dwc->regs, DWC3_DCTL);
	if (is_on) {
		ret = dwc3_udc_init(dwc);
		if (ret) {
			dev_err(dwc->dev, "failed to reinitialize udc\n");
			return ret;
		}

		dwc3_gadget_enable_irq(dwc);

		if (dwc->revision <= DWC3_REVISION_187A) {
			reg &= ~DWC3_DCTL_TRGTULST_MASK;
			reg |= DWC3_DCTL_TRGTULST_RX_DET;
		}

		if (dwc->revision >= DWC3_REVISION_194A)
			reg &= ~DWC3_DCTL_KEEP_CONNECT;

		reg |= DWC3_DCTL_RUN_STOP;

		if (dwc->has_hibernation)
			reg |= DWC3_DCTL_KEEP_CONNECT;

		dwc->pullups_connected = true;
	} else {
		dwc3_gadget_link_state_print(dwc, "STOP");
		dwc3_gadget_disable_irq(dwc);
		reg = dwc3_readl(dwc->regs, DWC3_DCTL);
		reg &= ~DWC3_DCTL_ULSTCHNGREQ_MASK;
		reg |= DWC3_DCTL_ULSTCHNGREQ(0x5);
		dwc3_writel(dwc->regs, DWC3_DCTL, reg);

		reg &= ~DWC3_DCTL_RUN_STOP;

		if (dwc->has_hibernation && !suspend)
			reg &= ~DWC3_DCTL_KEEP_CONNECT;

		dwc->pullups_connected = false;
#if defined(CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE) && defined(CONFIG_SEC_FACTORY)
		cancel_delayed_work(&dwc->usb_link_state_check_work);
#endif
	}

	dwc3_writel(dwc->regs, DWC3_DCTL, reg);

	do {
		reg = dwc3_readl(dwc->regs, DWC3_DSTS);
		if (is_on) {
			if (!(reg & DWC3_DSTS_DEVCTRLHLT))
				break;
		} else {
			if (reg & DWC3_DSTS_DEVCTRLHLT)
				break;
		}
		timeout--;
		if (!timeout) {
			if (is_on) {
				reg = dwc3_readl(dwc->regs, DWC3_DCTL);
				reg |= DWC3_DCTL_CSFTRST;
				dwc3_writel(dwc->regs, DWC3_DCTL, reg);
				dev_err(dwc->dev,
					"gadget run/stop timeout, DCTL : 0x%x\n",
					reg);
				reg = dwc3_readl(dwc->regs, DWC3_DSTS);
				dev_err(dwc->dev,
					"gadget run/stop timeout, DSTS : 0x%x\n",
					reg);
				do {
					reg = dwc3_readl(dwc->regs, DWC3_DCTL);
					if (!(reg & DWC3_DCTL_CSFTRST)) {
						dev_info(dwc->dev,
							"gadget run/stop DCTL softreset, DCTL : 0x%x\n",
							reg);
						goto good;
					}
					udelay(1);

				} while (--retries);

				return -ETIMEDOUT;
			}

			/* Do nothing in DCTL stop timeout */
			dev_err(dwc->dev,
				"gadget DCTL stop timeout, DSTS: 0x%x\n",
				reg);
			goto good;
		}
		udelay(1);
	} while (1);
good:
	return ret;
}

static int dwc3_gadget_run_stop_vbus(struct dwc3 *dwc, int is_on, int suspend)
{
	u32			reg;
	u32			timeout = 1000;
	int			retries = 1000;
	int			ret = 0;

	if (pm_runtime_suspended(dwc->dev))
		return 0;

	reg = dwc3_readl(dwc->regs, DWC3_DCTL);
	if (is_on) {
		dwc3_event_buffers_setup(dwc);
		ret = dwc3_udc_init(dwc);
		if (ret) {
			dev_err(dwc->dev, "failed to reinitialize udc\n");
			return ret;
		}

		dwc3_gadget_enable_irq(dwc);

		reg = dwc3_readl(dwc->regs, DWC3_DCTL);

		if (dwc->revision <= DWC3_REVISION_187A) {
			reg &= ~DWC3_DCTL_TRGTULST_MASK;
			reg |= DWC3_DCTL_TRGTULST_RX_DET;
		}

		if (dwc->revision >= DWC3_REVISION_194A)
			reg &= ~DWC3_DCTL_KEEP_CONNECT;

		reg |= DWC3_DCTL_RUN_STOP;

		if (dwc->has_hibernation)
			reg |= DWC3_DCTL_KEEP_CONNECT;

		dwc->pullups_connected = true;
	} else {
		dwc3_gadget_link_state_print(dwc, "STOP_VBUS");
		dwc3_gadget_disable_irq(dwc);
		dwc3_event_buffers_cleanup(dwc);
		__dwc3_gadget_ep_disable(dwc->eps[1]);
		__dwc3_gadget_ep_disable(dwc->eps[0]);

		reg = dwc3_readl(dwc->regs, DWC3_DCTL);
		reg &= ~DWC3_DCTL_RUN_STOP;

		if (dwc->has_hibernation && !suspend)
			reg &= ~DWC3_DCTL_KEEP_CONNECT;

		dwc->pullups_connected = false;
#if defined(CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE) && defined(CONFIG_SEC_FACTORY)
		cancel_delayed_work(&dwc->usb_link_state_check_work);
#endif
	}

	dwc3_writel(dwc->regs, DWC3_DCTL, reg);

	do {
		reg = dwc3_readl(dwc->regs, DWC3_DSTS);
		if (is_on) {
			if (!(reg & DWC3_DSTS_DEVCTRLHLT))
				break;
		} else {
			if (reg & DWC3_DSTS_DEVCTRLHLT)
				break;
		}
		timeout--;
		if (!timeout) {
			if (is_on) {
				reg = dwc3_readl(dwc->regs, DWC3_DCTL);
				reg |= DWC3_DCTL_CSFTRST;
				dwc3_writel(dwc->regs, DWC3_DCTL, reg);
				dev_err(dwc->dev,
					"gadget run/stop timeout, DCTL : 0x%x\n",
							reg);
				reg = dwc3_readl(dwc->regs, DWC3_DSTS);
				dev_err(dwc->dev,
					"gadget run/stop timeout, DSTS : 0x%x\n",
							reg);
				do {
					reg = dwc3_readl(dwc->regs, DWC3_DCTL);
					if (!(reg & DWC3_DCTL_CSFTRST)) {
						dev_info(dwc->dev,
							"gadget run/stop DCTL softreset, DCTL : 0x%x\n",
									reg);
						goto good;
					}
					udelay(1);

				} while (--retries);

				return -ETIMEDOUT;
			}

			/* Do nothing in DCTL stop timeout */
			dev_err(dwc->dev,
			"gadget DCTL stop timeout - vbus, DSTS: 0x%x\n",
					reg);
			if (!dwc->vbus_state)
				dwc3_soft_reset(dwc);
			goto good;
		}
		udelay(1);
	} while (1);
good:
	return ret;
}

static int dwc3_gadget_vbus_session(struct usb_gadget *g, int is_active)
{
	struct dwc3 *dwc = gadget_to_dwc(g);
	unsigned long flags;
	int ret = 0;

	if (!dwc->dotg)
		return -EPERM;

	is_active = !!is_active;

	pr_info("usb: %s: is_active = %d, softconnect = %d, vbus_session = %d\n",
			__func__, is_active, dwc->softconnect, dwc->vbus_session);

	spin_lock_irqsave(&dwc->lock, flags);

	if (dwc->vbus_session == is_active) {
		dev_info(dwc->dev, "%s: already processed\n", __func__);
		spin_unlock_irqrestore(&dwc->lock, flags);
		return 0;
	}

	/* Mark that the vbus was powered */
	dwc->vbus_session = is_active;

	/*
	 * Check if upper level usb_gadget_driver was already registerd with
	 * this udc controller driver (if dwc3_gadget_start was called).
	 * removed checking dwc->softconnect in vbus to resolve that
	 * run_stop isn't called permanantely when pm_runtime check
	 * is implemented in gadget_pullup and gadget_set_speed
	 */
	if (dwc->gadget_driver) {
		if (dwc->vbus_session) {
			/*
			 * Both vbus was activated by otg and pullup was
			 * signaled by the gadget driver.
			 * In this point, dwc->softconnect should be one
			 * thus set dwc->softconnect even if setting it here
			 * is conceptually wrong.
			 */
			dwc->softconnect = dwc->vbus_session;
			dwc->max_cnt_link_info = DWC3_LINK_STATE_INFO_LIMIT;
			ret = dwc3_gadget_run_stop_vbus(dwc, 1, false);
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
			if (ret == 0)
				store_usblog_notify(NOTIFY_USBSTATE,
							(void *)"USB_STATE=VBUS:EN:SUCCESS", NULL);
			else
				store_usblog_notify(NOTIFY_USBSTATE,
							(void *)"USB_STATE=VBUS:EN:FAIL", NULL);
#endif
		} else {
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
			dwc3_gadget_cable_connect(dwc, false);
			dwc->start_config_issued = false;
			dwc->gadget.speed = USB_SPEED_UNKNOWN;
			dwc->gadget.state = USB_STATE_NOTATTACHED;
			dwc->vbus_current = 0;
			dwc->setup_packet_pending = false;
#endif
			ret = dwc3_gadget_run_stop_vbus(dwc, 0, false);
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
			if (ret == 0)
				store_usblog_notify(NOTIFY_USBSTATE,
							(void *)"USB_STATE=VBUS:DIS:SUCCESS", NULL);
			else
				store_usblog_notify(NOTIFY_USBSTATE,
							(void *)"USB_STATE=VBUS:DIS:FAIL", NULL);
#endif
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
			dwc3_disconnect_gadget(dwc);
			printk("usb: %s : link state = %d\n", __func__, dwc3_gadget_get_link_state(dwc));
#endif
		}
	}

	spin_unlock_irqrestore(&dwc->lock, flags);

	if (ret)
		dev_err(dwc->dev, "dwc3 gadget run/stop error:%d\n", ret);

	if (dwc->rst_err_noti) {
		dwc->event_state = RELEASE;
		dwc->rst_err_noti = false;
		schedule_delayed_work(&dwc->usb_event_work, msecs_to_jiffies(0));
	}
	dwc->rst_err_cnt = 0;
	acc_dev_status = 0;

	return 0;
}

static int dwc3_gadget_pullup(struct usb_gadget *g, int is_on)
{
	struct dwc3		*dwc = gadget_to_dwc(g);
	unsigned long		flags;
	int			ret;

#ifdef CONFIG_USB_TYPEC_MANAGER_NOTIFIER
	if(is_on)
		set_usb_enable_state();
#endif
	is_on = !!is_on;

	spin_lock_irqsave(&dwc->lock, flags);

	pr_info("usb: %s: pullup = %d, vbus = %d\n",
			__func__, is_on, dwc->vbus_session);
	if (is_on == dwc->softconnect) {
		dev_info(dwc->dev, "pullup is already %s\n",
				is_on ? "on" : "off");
		spin_unlock_irqrestore(&dwc->lock, flags);
		return 0;
	}

	dwc->softconnect = is_on;

	if (dwc->dotg && !dwc->vbus_session) {
		spin_unlock_irqrestore(&dwc->lock, flags);
		/* Need to wait for vbus_session(on) from otg driver */
		return 0;
	}

	if (dwc->is_not_vbus_pad) {
		if (is_on) {
			phy_set(dwc->usb2_generic_phy, SET_DPPULLUP_ENABLE, NULL);
			phy_set(dwc->usb3_generic_phy, SET_DPPULLUP_ENABLE, NULL);
			dwc->max_cnt_link_info = DWC3_LINK_STATE_INFO_LIMIT;
		} else {
			phy_set(dwc->usb2_generic_phy, SET_DPPULLUP_DISABLE, NULL);
			phy_set(dwc->usb3_generic_phy, SET_DPPULLUP_DISABLE, NULL);
		}
	}

	ret = dwc3_gadget_run_stop(dwc, is_on, false);
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
	if (ret == 0) {
		if (is_on)
			store_usblog_notify(NOTIFY_USBSTATE,
						(void *)"USB_STATE=PULLUP:EN:SUCCESS", NULL);
		else
			store_usblog_notify(NOTIFY_USBSTATE,
						(void *)"USB_STATE=PULLUP:DIS:SUCCESS", NULL);
	} else {
		if (is_on)
			store_usblog_notify(NOTIFY_USBSTATE,
						(void *)"USB_STATE=PULLUP:EN:FAIL", NULL);
		else
			store_usblog_notify(NOTIFY_USBSTATE,
						(void *)"USB_STATE=PULLUP:DIS:FAIL", NULL);
	}
#endif
	spin_unlock_irqrestore(&dwc->lock, flags);

	if (!is_on)
		msleep(50);

	return ret;
}

static irqreturn_t dwc3_interrupt(int irq, void *_dwc);
#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
static irqreturn_t dwc3_thread_interrupt(int irq, void *_dwc);
#endif

/**
 * dwc3_gadget_setup_nump - calculate and initialize NUMP field of %DWC3_DCFG
 * @dwc: pointer to our context structure
 *
 * The following looks like complex but it's actually very simple. In order to
 * calculate the number of packets we can burst at once on OUT transfers, we're
 * gonna use RxFIFO size.
 *
 * To calculate RxFIFO size we need two numbers:
 * MDWIDTH = size, in bits, of the internal memory bus
 * RAM2_DEPTH = depth, in MDWIDTH, of internal RAM2 (where RxFIFO sits)
 *
 * Given these two numbers, the formula is simple:
 *
 * RxFIFO Size = (RAM2_DEPTH * MDWIDTH / 8) - 24 - 16;
 *
 * 24 bytes is for 3x SETUP packets
 * 16 bytes is a clock domain crossing tolerance
 *
 * Given RxFIFO Size, NUMP = RxFIFOSize / 1024;
 */
static void dwc3_gadget_setup_nump(struct dwc3 *dwc)
{
	u32 ram2_depth;
	u32 mdwidth;
	u32 nump;
	u32 reg;

	ram2_depth = DWC3_GHWPARAMS7_RAM2_DEPTH(dwc->hwparams.hwparams7);
	mdwidth = DWC3_GHWPARAMS0_MDWIDTH(dwc->hwparams.hwparams0);

	nump = ((ram2_depth * mdwidth / 8) - 24 - 16) / 1024;
	nump = min_t(u32, nump, 16);

	/* update NumP */
	reg = dwc3_readl(dwc->regs, DWC3_DCFG);
	reg &= ~DWC3_DCFG_NUMP_MASK;
	reg |= nump << DWC3_DCFG_NUMP_SHIFT;
	dwc3_writel(dwc->regs, DWC3_DCFG, reg);
}

static int __dwc3_gadget_start(struct dwc3 *dwc)
{
	struct dwc3_ep		*dep;
	int			ret = 0;
	u32			reg;

	dwc3_event_buffers_setup(dwc);

	/*
	 * Use IMOD if enabled via dwc->imod_interval. Otherwise, if
	 * the core supports IMOD, disable it.
	 */
	if (dwc->imod_interval) {
		dwc3_writel(dwc->regs, DWC3_DEV_IMOD(0), dwc->imod_interval);
		dwc3_writel(dwc->regs, DWC3_GEVNTCOUNT(0), DWC3_GEVNTCOUNT_EHB);
	} else if (dwc3_has_imod(dwc)) {
		dwc3_writel(dwc->regs, DWC3_DEV_IMOD(0), 0);
	}

	/*
	 * We are telling dwc3 that we want to use DCFG.NUMP as ACK TP's NUMP
	 * field instead of letting dwc3 itself calculate that automatically.
	 *
	 * This way, we maximize the chances that we'll be able to get several
	 * bursts of data without going through any sort of endpoint throttling.
	 */
	reg = dwc3_readl(dwc->regs, DWC3_GRXTHRCFG);
	if (dwc3_is_usb31(dwc))
		reg &= ~DWC31_GRXTHRCFG_PKTCNTSEL;
	else
		reg &= ~DWC3_GRXTHRCFG_PKTCNTSEL;

	dwc3_writel(dwc->regs, DWC3_GRXTHRCFG, reg);

	dwc3_gadget_setup_nump(dwc);

	/* Start with SuperSpeed Default */
	dwc3_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(512);

	dep = dwc->eps[0];
	ret = __dwc3_gadget_ep_enable(dep, DWC3_DEPCFG_ACTION_INIT);
	if (ret) {
		dev_err(dwc->dev, "failed to enable %s\n", dep->name);
		goto err0;
	}

	dep = dwc->eps[1];
	ret = __dwc3_gadget_ep_enable(dep, DWC3_DEPCFG_ACTION_INIT);
	if (ret) {
		dev_err(dwc->dev, "failed to enable %s\n", dep->name);
		goto err1;
	}

	/* begin to receive SETUP packets */
	dwc->ep0state = EP0_SETUP_PHASE;
	dwc->link_state = DWC3_LINK_STATE_SS_DIS;
	dwc3_ep0_out_start(dwc);

	dwc3_gadget_enable_irq(dwc);

	return 0;

err1:
	__dwc3_gadget_ep_disable(dwc->eps[0]);

err0:
	return ret;
}

static int dwc3_gadget_start(struct usb_gadget *g,
		struct usb_gadget_driver *driver)
{
	struct dwc3		*dwc = gadget_to_dwc(g);
	unsigned long		flags;
	int			ret = 0;
	int			irq;
	struct dwc3_otg *dotg = dwc->dotg;
	struct otg_fsm	*fsm = &dotg->fsm;

	mutex_lock(&fsm->lock);
	irq = dwc->irq_gadget;
#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
	ret = request_threaded_irq(irq, dwc3_interrupt, dwc3_thread_interrupt,
			IRQF_SHARED, "dwc3", dwc->ev_buf);
#else
	ret = devm_request_irq(dwc->dev, irq, dwc3_interrupt,
			IRQF_SHARED, "dwc3", dwc->ev_buf);
#endif
	if (ret) {
		dev_err(dwc->dev, "failed to request irq #%d --> %d\n",
				irq, ret);
		goto err0;
	}

	spin_lock_irqsave(&dwc->lock, flags);
	if (dwc->gadget_driver) {
		dev_err(dwc->dev, "%s is already bound to %s\n",
				dwc->gadget.name,
				dwc->gadget_driver->driver.name);
		ret = -EBUSY;
		goto err1;
	}

	dwc->gadget_driver	= driver;

	/* Modifying Kernel 4.14 change -
	 * executing ep_enable here makes conflict
	 * with dwc3_udc_init in dwc3_gadget_run_stop
	 * if (pm_runtime_active(dwc->dev))
	 *      __dwc3_gadget_start(dwc);
	 */

	spin_unlock_irqrestore(&dwc->lock, flags);
	mutex_unlock(&fsm->lock);

#ifdef CONFIG_ARGOS
	if (!zalloc_cpumask_var(&affinity_cpu_mask, GFP_KERNEL))
		return -ENOMEM;
	if (!zalloc_cpumask_var(&default_cpu_mask, GFP_KERNEL))
		return -ENOMEM;

	cpumask_copy(default_cpu_mask, get_default_cpu_mask());
	cpumask_or(affinity_cpu_mask, affinity_cpu_mask, cpumask_of(3));
	argos_irq_affinity_setup_label(irq, "USB", affinity_cpu_mask, default_cpu_mask);
#endif
	return 0;

err1:
	spin_unlock_irqrestore(&dwc->lock, flags);
	free_irq(irq, dwc);

err0:
	mutex_unlock(&fsm->lock);
	return ret;
}

static void __dwc3_gadget_stop(struct dwc3 *dwc)
{
	dwc3_gadget_disable_irq(dwc);
	__dwc3_gadget_ep_disable(dwc->eps[1]);
	__dwc3_gadget_ep_disable(dwc->eps[0]);
}

static int dwc3_gadget_stop(struct usb_gadget *g)
{
	struct dwc3		*dwc = gadget_to_dwc(g);
	unsigned long		flags;
	struct dwc3_otg *dotg = dwc->dotg;
	struct otg_fsm  *fsm = &dotg->fsm;

	mutex_lock(&fsm->lock);
	spin_lock_irqsave(&dwc->lock, flags);

	if (pm_runtime_suspended(dwc->dev))
		goto out;

	__dwc3_gadget_stop(dwc);

out:
	dwc->gadget_driver	= NULL;
	spin_unlock_irqrestore(&dwc->lock, flags);
	mutex_unlock(&fsm->lock);

	free_irq(dwc->irq_gadget, dwc->ev_buf);

	return 0;
}

static void dwc3_gadget_set_speed(struct usb_gadget *g,
				  enum usb_device_speed speed)
{
	struct dwc3		*dwc = gadget_to_dwc(g);
	unsigned long		flags;
	u32			reg;

	if (pm_runtime_suspended(dwc->dev))
		return;
	spin_lock_irqsave(&dwc->lock, flags);
	reg = dwc3_readl(dwc->regs, DWC3_DCFG);
	reg &= ~(DWC3_DCFG_SPEED_MASK);

	/*
	 * WORKAROUND: DWC3 revision < 2.20a have an issue
	 * which would cause metastability state on Run/Stop
	 * bit if we try to force the IP to USB2-only mode.
	 *
	 * Because of that, we cannot configure the IP to any
	 * speed other than the SuperSpeed
	 *
	 * Refers to:
	 *
	 * STAR#9000525659: Clock Domain Crossing on DCTL in
	 * USB 2.0 Mode
	 */
	if (dwc->revision < DWC3_REVISION_220A &&
	    !dwc->dis_metastability_quirk) {
		reg |= DWC3_DCFG_SUPERSPEED;
	} else {
		switch (speed) {
		case USB_SPEED_LOW:
			reg |= DWC3_DCFG_LOWSPEED;
			break;
		case USB_SPEED_FULL:
			reg |= DWC3_DCFG_FULLSPEED;
			break;
		case USB_SPEED_HIGH:
			reg |= DWC3_DCFG_HIGHSPEED;
			break;
		case USB_SPEED_SUPER:
			reg |= DWC3_DCFG_SUPERSPEED;
			break;
		case USB_SPEED_SUPER_PLUS:
			if (dwc3_is_usb31(dwc))
				reg |= DWC3_DCFG_SUPERSPEED_PLUS;
			else
				reg |= DWC3_DCFG_SUPERSPEED;
			break;
		default:
			dev_err(dwc->dev, "invalid speed (%d)\n", speed);

			if (dwc->revision & DWC3_REVISION_IS_DWC31)
				reg |= DWC3_DCFG_SUPERSPEED_PLUS;
			else
				reg |= DWC3_DCFG_SUPERSPEED;
		}
	}
	dwc3_writel(dwc->regs, DWC3_DCFG, reg);

	spin_unlock_irqrestore(&dwc->lock, flags);
}

static const struct usb_gadget_ops dwc3_gadget_ops = {
	.get_frame		= dwc3_gadget_get_frame,
	.wakeup			= dwc3_gadget_wakeup,
	.set_selfpowered	= dwc3_gadget_set_selfpowered,
	.vbus_session		= dwc3_gadget_vbus_session,
	.pullup			= dwc3_gadget_pullup,
	.udc_start		= dwc3_gadget_start,
	.udc_stop		= dwc3_gadget_stop,
	.udc_set_speed		= dwc3_gadget_set_speed,
};

/* -------------------------------------------------------------------------- */

static int dwc3_gadget_init_control_endpoint(struct dwc3_ep *dep)
{
	struct dwc3 *dwc = dep->dwc;

	usb_ep_set_maxpacket_limit(&dep->endpoint, 512);
	dep->endpoint.maxburst = 1;
	dep->endpoint.ops = &dwc3_gadget_ep0_ops;
	if (!dep->direction)
		dwc->gadget.ep0 = &dep->endpoint;

	dep->endpoint.caps.type_control = true;

	return 0;
}

static int dwc3_gadget_init_in_endpoint(struct dwc3_ep *dep)
{
	struct dwc3 *dwc = dep->dwc;
	int mdwidth;
	int kbytes;
	int size;

	mdwidth = DWC3_MDWIDTH(dwc->hwparams.hwparams0);
	/* MDWIDTH is represented in bits, we need it in bytes */
	mdwidth /= 8;

	size = dwc3_readl(dwc->regs, DWC3_GTXFIFOSIZ(dep->number >> 1));
	if (dwc3_is_usb31(dwc))
		size = DWC31_GTXFIFOSIZ_TXFDEF(size);
	else
		size = DWC3_GTXFIFOSIZ_TXFDEF(size);

	/* FIFO Depth is in MDWDITH bytes. Multiply */
	size *= mdwidth;

	kbytes = size / 1024;
	if (kbytes == 0)
		kbytes = 1;

	/*
	 * FIFO sizes account an extra MDWIDTH * (kbytes + 1) bytes for
	 * internal overhead. We don't really know how these are used,
	 * but documentation say it exists.
	 */
	size -= mdwidth * (kbytes + 1);
	size /= kbytes;

	usb_ep_set_maxpacket_limit(&dep->endpoint, size);

	dep->endpoint.max_streams = 15;
	dep->endpoint.ops = &dwc3_gadget_ep_ops;
	list_add_tail(&dep->endpoint.ep_list,
			&dwc->gadget.ep_list);
	dep->endpoint.caps.type_iso = true;
	dep->endpoint.caps.type_bulk = true;
	dep->endpoint.caps.type_int = true;

	return dwc3_alloc_trb_pool(dep);
}

static int dwc3_gadget_init_out_endpoint(struct dwc3_ep *dep)
{
	struct dwc3 *dwc = dep->dwc;

	usb_ep_set_maxpacket_limit(&dep->endpoint, 1024);
	dep->endpoint.max_streams = 15;
	dep->endpoint.ops = &dwc3_gadget_ep_ops;
	list_add_tail(&dep->endpoint.ep_list,
			&dwc->gadget.ep_list);
	dep->endpoint.caps.type_iso = true;
	dep->endpoint.caps.type_bulk = true;
	dep->endpoint.caps.type_int = true;

	return dwc3_alloc_trb_pool(dep);
}

static int dwc3_gadget_init_endpoint(struct dwc3 *dwc, u8 epnum)
{
	struct dwc3_ep			*dep;
	bool				direction = epnum & 1;
	int				ret;
	u8				num = epnum >> 1;

	dep = kzalloc(sizeof(*dep), GFP_KERNEL);
	if (!dep)
		return -ENOMEM;

	dep->dwc = dwc;
	dep->number = epnum;
	dep->direction = direction;
	dep->regs = dwc->regs + DWC3_DEP_BASE(epnum);
	dwc->eps[epnum] = dep;

	snprintf(dep->name, sizeof(dep->name), "ep%u%s", num,
			direction ? "in" : "out");

	dep->endpoint.name = dep->name;

	if (!(dep->number > 1)) {
		dep->endpoint.desc = &dwc3_gadget_ep0_desc;
		dep->endpoint.comp_desc = NULL;
	}

	spin_lock_init(&dep->lock);

	if (num == 0)
		ret = dwc3_gadget_init_control_endpoint(dep);
	else if (direction)
		ret = dwc3_gadget_init_in_endpoint(dep);
	else
		ret = dwc3_gadget_init_out_endpoint(dep);

	if (ret)
		return ret;

	dep->endpoint.caps.dir_in = direction;
	dep->endpoint.caps.dir_out = !direction;

	INIT_LIST_HEAD(&dep->pending_list);
	INIT_LIST_HEAD(&dep->started_list);
	INIT_LIST_HEAD(&dep->cancelled_list);

	return 0;
}

static int dwc3_gadget_init_endpoints(struct dwc3 *dwc, u8 total)
{
	u8				epnum;

	INIT_LIST_HEAD(&dwc->gadget.ep_list);

	for (epnum = 0; epnum < total; epnum++) {
		int			ret;

		ret = dwc3_gadget_init_endpoint(dwc, epnum);
		if (ret)
			return ret;
	}

	return 0;
}

static void dwc3_gadget_free_endpoints(struct dwc3 *dwc)
{
	struct dwc3_ep			*dep;
	u8				epnum;

	for (epnum = 0; epnum < DWC3_ENDPOINTS_NUM; epnum++) {
		dep = dwc->eps[epnum];
		if (!dep)
			continue;
		/*
		 * Physical endpoints 0 and 1 are special; they form the
		 * bi-directional USB endpoint 0.
		 *
		 * For those two physical endpoints, we don't allocate a TRB
		 * pool nor do we add them the endpoints list. Due to that, we
		 * shouldn't do these two operations otherwise we would end up
		 * with all sorts of bugs when removing dwc3.ko.
		 */
		if (epnum != 0 && epnum != 1) {
			dwc3_free_trb_pool(dep);
			list_del(&dep->endpoint.ep_list);
		}

		kfree(dep);
	}
}

/* -------------------------------------------------------------------------- */

static int dwc3_gadget_ep_reclaim_completed_trb(struct dwc3_ep *dep,
		struct dwc3_request *req, struct dwc3_trb *trb,
		const struct dwc3_event_depevt *event, int status, int chain)
{
	unsigned int		count;

	dwc3_ep_inc_deq(dep);

	trace_dwc3_complete_trb(dep, trb);
	req->num_trbs--;

	/*
	 * If we're in the middle of series of chained TRBs and we
	 * receive a short transfer along the way, DWC3 will skip
	 * through all TRBs including the last TRB in the chain (the
	 * where CHN bit is zero. DWC3 will also avoid clearing HWO
	 * bit and SW has to do it manually.
	 *
	 * We're going to do that here to avoid problems of HW trying
	 * to use bogus TRBs for transfers.
	 */
	if (chain && (trb->ctrl & DWC3_TRB_CTRL_HWO))
		trb->ctrl &= ~DWC3_TRB_CTRL_HWO;

	/*
	 * If we're dealing with unaligned size OUT transfer, we will be left
	 * with one TRB pending in the ring. We need to manually clear HWO bit
	 * from that TRB.
	 */

	if (req->needs_extra_trb && !(trb->ctrl & DWC3_TRB_CTRL_CHN)) {
		trb->ctrl &= ~DWC3_TRB_CTRL_HWO;
		return 1;
	}

	count = trb->size & DWC3_TRB_SIZE_MASK;
	req->remaining += count;

	if ((trb->ctrl & DWC3_TRB_CTRL_HWO) && status != -ESHUTDOWN)
		return 1;

	if (event->status & DEPEVT_STATUS_SHORT && !chain)
		return 1;

	if (event->status & DEPEVT_STATUS_IOC)
		return 1;

	return 0;
}

static int dwc3_gadget_ep_reclaim_trb_sg(struct dwc3_ep *dep,
		struct dwc3_request *req, const struct dwc3_event_depevt *event,
		int status)
{
	struct dwc3_trb *trb = &dep->trb_pool[dep->trb_dequeue];
	struct scatterlist *sg = req->sg;
	struct scatterlist *s;
	unsigned int pending = req->num_pending_sgs;
	unsigned int i;
	int ret = 0;

	for_each_sg(sg, s, pending, i) {
		trb = &dep->trb_pool[dep->trb_dequeue];

		if (trb->ctrl & DWC3_TRB_CTRL_HWO)
			break;

		req->sg = sg_next(s);
		req->num_pending_sgs--;

		ret = dwc3_gadget_ep_reclaim_completed_trb(dep, req,
				trb, event, status, true);
		if (ret)
			break;
	}

	return ret;
}

static int dwc3_gadget_ep_reclaim_trb_linear(struct dwc3_ep *dep,
		struct dwc3_request *req, const struct dwc3_event_depevt *event,
		int status)
{
	struct dwc3_trb *trb = &dep->trb_pool[dep->trb_dequeue];

	return dwc3_gadget_ep_reclaim_completed_trb(dep, req, trb,
			event, status, false);
}

static bool dwc3_gadget_ep_request_completed(struct dwc3_request *req)
{
	return req->request.actual == req->request.length;
}

static int dwc3_gadget_ep_cleanup_completed_request(struct dwc3_ep *dep,
		const struct dwc3_event_depevt *event,
		struct dwc3_request *req, int status)
{
	int ret;

	if (req->num_pending_sgs)
		ret = dwc3_gadget_ep_reclaim_trb_sg(dep, req, event,
				status);
	else
		ret = dwc3_gadget_ep_reclaim_trb_linear(dep, req, event,
				status);

	if (req->needs_extra_trb) {
		ret = dwc3_gadget_ep_reclaim_trb_linear(dep, req, event,
				status);
		req->needs_extra_trb = false;
	}

	req->request.actual = req->request.length - req->remaining;

	if (!dwc3_gadget_ep_request_completed(req) &&
			req->num_pending_sgs) {
		__dwc3_gadget_kick_transfer(dep);
		goto out;
	}

	dwc3_gadget_giveback(dep, req, status);

out:
	return ret;
}

static void dwc3_gadget_ep_cleanup_completed_requests(struct dwc3_ep *dep,
		const struct dwc3_event_depevt *event, int status)
{
	struct dwc3_request	*req;
	struct dwc3_request	*tmp;

	list_for_each_entry_safe(req, tmp, &dep->started_list, list) {
		int ret;

		ret = dwc3_gadget_ep_cleanup_completed_request(dep, event,
				req, status);
		if (ret)
			break;
	}
}

static void dwc3_gadget_endpoint_frame_from_event(struct dwc3_ep *dep,
		const struct dwc3_event_depevt *event)
{
	dep->frame_number = event->parameters;
}

static void dwc3_gadget_endpoint_transfer_in_progress(struct dwc3_ep *dep,
		const struct dwc3_event_depevt *event)
{
	struct dwc3		*dwc = dep->dwc;
	unsigned		status = 0;
	bool			stop = false;

	dwc3_gadget_endpoint_frame_from_event(dep, event);

	if (event->status & DEPEVT_STATUS_BUSERR)
		status = -ECONNRESET;

	if (event->status & DEPEVT_STATUS_MISSED_ISOC) {
		status = -EXDEV;

		if (list_empty(&dep->started_list))
			stop = true;
	}

	dwc3_gadget_ep_cleanup_completed_requests(dep, event, status);

	if (stop) {
		dwc3_stop_active_transfer(dep, true, true);
		dep->flags = DWC3_EP_ENABLED;
	}

	/*
	 * WORKAROUND: This is the 2nd half of U1/U2 -> U0 workaround.
	 * See dwc3_gadget_linksts_change_interrupt() for 1st half.
	 */
	if (dwc->revision < DWC3_REVISION_183A) {
		u32		reg;
		int		i;

		for (i = 0; i < DWC3_ENDPOINTS_NUM; i++) {
			dep = dwc->eps[i];

			if (!(dep->flags & DWC3_EP_ENABLED))
				continue;

			if (!list_empty(&dep->started_list))
				return;
		}

		reg = dwc3_readl(dwc->regs, DWC3_DCTL);
		reg |= dwc->u1u2;
		dwc3_writel(dwc->regs, DWC3_DCTL, reg);

		dwc->u1u2 = 0;
	}
}

static void dwc3_gadget_endpoint_transfer_not_ready(struct dwc3_ep *dep,
		const struct dwc3_event_depevt *event)
{
	dwc3_gadget_endpoint_frame_from_event(dep, event);
	__dwc3_gadget_start_isoc(dep);
}

static void dwc3_endpoint_interrupt(struct dwc3 *dwc,
		const struct dwc3_event_depevt *event)
{
	struct dwc3_ep		*dep;
	u8			epnum = event->endpoint_number;
	u8			cmd;

	dep = dwc->eps[epnum];

	if (!(dep->flags & DWC3_EP_ENABLED)) {
		if (!(dep->flags & DWC3_EP_END_TRANSFER_PENDING))
			return;

		/* Handle only EPCMDCMPLT when EP disabled */
		if (event->endpoint_event != DWC3_DEPEVT_EPCMDCMPLT)
			return;
	}

	if (epnum == 0 || epnum == 1) {
		dwc3_ep0_interrupt(dwc, event);
		return;
	}

	switch (event->endpoint_event) {
	case DWC3_DEPEVT_XFERINPROGRESS:
		dwc3_gadget_endpoint_transfer_in_progress(dep, event);
		break;
	case DWC3_DEPEVT_XFERNOTREADY:
		dwc3_gadget_endpoint_transfer_not_ready(dep, event);
		break;
	case DWC3_DEPEVT_EPCMDCMPLT:
		cmd = DEPEVT_PARAMETER_CMD(event->parameters);

		if (cmd == DWC3_DEPCMD_ENDTRANSFER) {
			dep->flags &= ~(DWC3_EP_END_TRANSFER_PENDING |
					DWC3_EP_TRANSFER_STARTED);
			dwc3_gadget_ep_cleanup_cancelled_requests(dep);
		}
		break;
	case DWC3_DEPEVT_STREAMEVT:
	case DWC3_DEPEVT_XFERCOMPLETE:
	case DWC3_DEPEVT_RXTXFIFOEVT:
		break;
	}
}

static void dwc3_disconnect_gadget(struct dwc3 *dwc)
{
	if (dwc->gadget_driver && dwc->gadget_driver->disconnect) {
		spin_unlock(&dwc->lock);
		dwc->gadget_driver->disconnect(&dwc->gadget);
		spin_lock(&dwc->lock);
	}
}

static void dwc3_suspend_gadget(struct dwc3 *dwc)
{
	if (dwc->gadget_driver && dwc->gadget_driver->suspend) {
		spin_unlock(&dwc->lock);
		dwc->gadget_driver->suspend(&dwc->gadget);
		spin_lock(&dwc->lock);
	}
}

static void dwc3_resume_gadget(struct dwc3 *dwc)
{
	if (dwc->gadget_driver && dwc->gadget_driver->resume) {
		spin_unlock(&dwc->lock);
		dwc->gadget_driver->resume(&dwc->gadget);
		spin_lock(&dwc->lock);
	}
}

static void dwc3_reset_gadget(struct dwc3 *dwc)
{
	if (!dwc->gadget_driver)
		return;

	if (dwc->gadget.speed != USB_SPEED_UNKNOWN) {
		spin_unlock(&dwc->lock);
		usb_gadget_udc_reset(&dwc->gadget, dwc->gadget_driver);
		spin_lock(&dwc->lock);
	}
}

static void dwc3_stop_active_transfer(struct dwc3_ep *dep, bool force,
	bool interrupt)
{
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_gadget_ep_cmd_params params;
	u32 cmd;
	int ret;

	if ((dep->flags & DWC3_EP_END_TRANSFER_PENDING) ||
	    !dep->resource_index)
		return;

	/*
	 * NOTICE: We are violating what the Databook says about the
	 * EndTransfer command. Ideally we would _always_ wait for the
	 * EndTransfer Command Completion IRQ, but that's causing too
	 * much trouble synchronizing between us and gadget driver.
	 *
	 * We have discussed this with the IP Provider and it was
	 * suggested to giveback all requests here, but give HW some
	 * extra time to synchronize with the interconnect. We're using
	 * an arbitrary 100us delay for that.
	 *
	 * Note also that a similar handling was tested by Synopsys
	 * (thanks a lot Paul) and nothing bad has come out of it.
	 * In short, what we're doing is:
	 *
	 * - Issue EndTransfer WITH CMDIOC bit set
	 * - Wait 100us
	 *
	 * As of IP version 3.10a of the DWC_usb3 IP, the controller
	 * supports a mode to work around the above limitation. The
	 * software can poll the CMDACT bit in the DEPCMD register
	 * after issuing a EndTransfer command. This mode is enabled
	 * by writing GUCTL2[14]. This polling is already done in the
	 * dwc3_send_gadget_ep_cmd() function so if the mode is
	 * enabled, the EndTransfer command will have completed upon
	 * returning from this function and we don't need to delay for
	 * 100us.
	 *
	 * This mode is NOT available on the DWC_usb31 IP.
	 */

	cmd = DWC3_DEPCMD_ENDTRANSFER;
	cmd |= force ? DWC3_DEPCMD_HIPRI_FORCERM : 0;
	cmd |= interrupt ? DWC3_DEPCMD_CMDIOC : 0;
	cmd |= DWC3_DEPCMD_PARAM(dep->resource_index);
	memset(&params, 0, sizeof(params));
	ret = dwc3_send_gadget_ep_cmd(dep, cmd, &params);
	/* WA for MTP failure, Ignore CMDACT timeout such as Kernel 4.4 */
	/*WARN_ON_ONCE(ret);*/
	dep->resource_index = 0;

	if (dwc3_is_usb31(dwc) || dwc->revision < DWC3_REVISION_310A) {
		dep->flags |= DWC3_EP_END_TRANSFER_PENDING;
		udelay(100);
	}
}

static void dwc3_clear_stall_all_ep(struct dwc3 *dwc)
{
	u32 epnum;

	for (epnum = 1; epnum < DWC3_ENDPOINTS_NUM; epnum++) {
		struct dwc3_ep *dep;
		int ret;

		dep = dwc->eps[epnum];
		if (!dep)
			continue;

		if (!(dep->flags & DWC3_EP_STALL))
			continue;

		dep->flags &= ~DWC3_EP_STALL;

		ret = dwc3_send_clear_stall_ep_cmd(dep);
#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
		WARN_ON_ONCE(ret);
#endif
	}
}

static void dwc3_gadget_disconnect_interrupt(struct dwc3 *dwc)
{
	int			reg;

	reg = dwc3_readl(dwc->regs, DWC3_DCTL);
	reg &= ~DWC3_DCTL_INITU1ENA;
	dwc3_writel(dwc->regs, DWC3_DCTL, reg);

	reg &= ~DWC3_DCTL_INITU2ENA;
	dwc3_writel(dwc->regs, DWC3_DCTL, reg);

	dwc3_disconnect_gadget(dwc);

	dwc->gadget.speed = USB_SPEED_UNKNOWN;
	dwc->setup_packet_pending = false;
	usb_gadget_set_state(&dwc->gadget, USB_STATE_NOTATTACHED);

	complete(&dwc->disconnect);
	dwc->connected = false;
}

static void dwc3_gadget_usb_event_work(struct work_struct *work)
{
	struct dwc3 *dwc = container_of(work, struct dwc3, usb_event_work.work);

	pr_info("%s, event_state: %d\n", __func__, dwc->event_state);

	if (dwc->event_state)
		send_usb_err_uevent(USB_ERR_ABNORMAL_RESET, NOTIFY);
	else
		send_usb_err_uevent(USB_ERR_ABNORMAL_RESET, RELEASE);
}

static void dwc3_gadget_reset_interrupt(struct dwc3 *dwc)
{
	u32			reg;
	ktime_t current_time;

	dwc->connected = true;

	/*
	 * WORKAROUND: DWC3 revisions <1.88a have an issue which
	 * would cause a missing Disconnect Event if there's a
	 * pending Setup Packet in the FIFO.
	 *
	 * There's no suggested workaround on the official Bug
	 * report, which states that "unless the driver/application
	 * is doing any special handling of a disconnect event,
	 * there is no functional issue".
	 *
	 * Unfortunately, it turns out that we _do_ some special
	 * handling of a disconnect event, namely complete all
	 * pending transfers, notify gadget driver of the
	 * disconnection, and so on.
	 *
	 * Our suggested workaround is to follow the Disconnect
	 * Event steps here, instead, based on a setup_packet_pending
	 * flag. Such flag gets set whenever we have a SETUP_PENDING
	 * status for EP0 TRBs and gets cleared on XferComplete for the
	 * same endpoint.
	 *
	 * Refers to:
	 *
	 * STAR#9000466709: RTL: Device : Disconnect event not
	 * generated if setup packet pending in FIFO
	 */
	if (dwc->revision < DWC3_REVISION_188A) {
		if (dwc->setup_packet_pending)
			dwc3_gadget_disconnect_interrupt(dwc);
	}

	/* after reset -> Default State */
	usb_gadget_set_state(&dwc->gadget, USB_STATE_DEFAULT);

	dwc->vbus_current = USB_CURRENT_UNCONFIGURED;
	schedule_work(&dwc->set_vbus_current_work);

	dwc3_reset_gadget(dwc);

	reg = dwc3_readl(dwc->regs, DWC3_DCTL);
	reg &= ~DWC3_DCTL_TSTCTRL_MASK;
	dwc3_writel(dwc->regs, DWC3_DCTL, reg);
	dwc->test_mode = false;
	dwc3_clear_stall_all_ep(dwc);

	/* Reset device address to zero */
	reg = dwc3_readl(dwc->regs, DWC3_DCFG);
	reg &= ~(DWC3_DCFG_DEVADDR_MASK);
	dwc3_writel(dwc->regs, DWC3_DCFG, reg);

	if (acc_dev_status && (dwc->rst_err_noti == false)) {
		current_time = ktime_to_ms(ktime_get_boottime());

		if ((dwc->rst_err_cnt == 0) && (dwc->gadget.state < USB_STATE_CONFIGURED)) {
			if ((current_time - dwc->rst_time_before) < 1000) {
				dwc->rst_err_cnt++;
				dwc->rst_time_first = dwc->rst_time_before;
			}
		} else {
			if ((current_time - dwc->rst_time_first) < 1000) {
				dwc->rst_err_cnt++;
			} else {
				dwc->rst_err_cnt = 0;
			}
		}

		if (dwc->rst_err_cnt > ERR_RESET_CNT) {
			dwc->event_state = NOTIFY;
			schedule_delayed_work(&dwc->usb_event_work, msecs_to_jiffies(0));
			dwc->rst_err_noti = true;
		}

		pr_info("%s rst_err_cnt: %d, time_current: %lld, time_before: %lld\n",
			__func__, dwc->rst_err_cnt, current_time, dwc->rst_time_before);

		dwc->rst_time_before = current_time;
	}

#ifdef CONFIG_USB_FIX_PHY_PULLUP_ISSUE
	/* This W/A patch made for Lhotse H/W bugs.(Need to remove) */
	if (dwc->is_not_vbus_pad) {
		phy_set(dwc->usb2_generic_phy, SET_DPPULLUP_ENABLE, NULL);
		phy_set(dwc->usb3_generic_phy, SET_DPPULLUP_ENABLE, NULL);
	}
#endif
}

/* Remove Warning - Check later!
static void dwc3_update_ram_clk_sel(struct dwc3 *dwc, u32 speed)
{
	u32 reg;

	reg = dwc3_readl(dwc->regs, DWC3_GCTL);
	reg |= DWC3_GCTL_RAMCLKSEL(DWC3_GCTL_CLK_MASK);
	dwc3_writel(dwc->regs, DWC3_GCTL, reg);
}
*/

static void dwc3_gadget_conndone_interrupt(struct dwc3 *dwc)
{
	struct dwc3_ep		*dep;
	int			ret;
	u32			reg;
	u8			speed;

	reg = dwc3_readl(dwc->regs, DWC3_DSTS);
	speed = reg & DWC3_DSTS_CONNECTSPD;
	dwc->speed = speed;

	/*
	 * RAMClkSel is reset to 0 after USB reset, so it must be reprogrammed
	 * each time on Connect Done.
	 *
	 * Currently we always use the reset value. If any platform
	 * wants to set this to a different value, we need to add a
	 * setting and update GCTL.RAMCLKSEL here.
	 */

	switch (speed) {
	case DWC3_DSTS_SUPERSPEED_PLUS:
		dwc3_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(512);
		dwc->gadget.ep0->maxpacket = 512;
		dwc->gadget.speed = USB_SPEED_SUPER_PLUS;
		break;
	case DWC3_DSTS_SUPERSPEED:
		/*
		 * WORKAROUND: DWC3 revisions <1.90a have an issue which
		 * would cause a missing USB3 Reset event.
		 *
		 * In such situations, we should force a USB3 Reset
		 * event by calling our dwc3_gadget_reset_interrupt()
		 * routine.
		 *
		 * Refers to:
		 *
		 * STAR#9000483510: RTL: SS : USB3 reset event may
		 * not be generated always when the link enters poll
		 */
		if (dwc->revision < DWC3_REVISION_190A)
			dwc3_gadget_reset_interrupt(dwc);

		dwc3_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(512);
		dwc->gadget.ep0->maxpacket = 512;
		dwc->gadget.speed = USB_SPEED_SUPER;
		break;
	case DWC3_DSTS_HIGHSPEED:
		dwc3_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(64);
		dwc->gadget.ep0->maxpacket = 64;
		dwc->gadget.speed = USB_SPEED_HIGH;
		break;
	case DWC3_DSTS_FULLSPEED:
		dwc3_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(64);
		dwc->gadget.ep0->maxpacket = 64;
		dwc->gadget.speed = USB_SPEED_FULL;
		break;
	case DWC3_DSTS_LOWSPEED:
		dwc3_gadget_ep0_desc.wMaxPacketSize = cpu_to_le16(8);
		dwc->gadget.ep0->maxpacket = 8;
		dwc->gadget.speed = USB_SPEED_LOW;
		break;
	}

#if defined(CONFIG_SOC_EXYNOS9830)
	/*
	 * There is a clock limitation by the USB speed in exynos9830.
	 * USB bus(HSI0_BUS) clock should be more than 100MHz in SS or SSP.
	 * */
	if (dwc->gadget.speed <= USB_SPEED_HIGH)
		dwc3_otg_qos_lock(dwc, -1);
	else
		dwc3_otg_qos_lock(dwc, 1);
#endif

#ifdef CONFIG_USB_TYPEC_MANAGER_NOTIFIER
	set_usb_enumeration_state(dwc->gadget.speed);
#endif

	dwc->eps[1]->endpoint.maxpacket = dwc->gadget.ep0->maxpacket;

	/* Enable USB2 LPM Capability */

	if ((dwc->revision > DWC3_REVISION_194A) &&
	    (speed != DWC3_DSTS_SUPERSPEED) &&
	    (speed != DWC3_DSTS_SUPERSPEED_PLUS)) {
		reg = dwc3_readl(dwc->regs, DWC3_DCFG);
		reg |= DWC3_DCFG_LPM_CAP;
		dwc3_writel(dwc->regs, DWC3_DCFG, reg);

		reg = dwc3_readl(dwc->regs, DWC3_DCTL);
		reg &= ~(DWC3_DCTL_HIRD_THRES_MASK | DWC3_DCTL_L1_HIBER_EN);

		reg |= DWC3_DCTL_HIRD_THRES(dwc->hird_threshold);

		/*
		 * When dwc3 revisions >= 2.40a, LPM Erratum is enabled and
		 * DCFG.LPMCap is set, core responses with an ACK and the
		 * BESL value in the LPM token is less than or equal to LPM
		 * NYET threshold.
		 */
#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
		WARN_ONCE(dwc->revision < DWC3_REVISION_240A
				&& dwc->has_lpm_erratum,
				"LPM Erratum not available on dwc3 revisions < 2.40a\n");
#endif

		if (dwc->has_lpm_erratum && dwc->revision >= DWC3_REVISION_240A)
			reg |= DWC3_DCTL_LPM_ERRATA(dwc->lpm_nyet_threshold);

		dwc3_writel(dwc->regs, DWC3_DCTL, reg);
	} else {
		reg = dwc3_readl(dwc->regs, DWC3_DCTL);
		reg &= ~DWC3_DCTL_HIRD_THRES_MASK;
		dwc3_writel(dwc->regs, DWC3_DCTL, reg);
	}

	dep = dwc->eps[0];
	ret = __dwc3_gadget_ep_enable(dep, DWC3_DEPCFG_ACTION_MODIFY);
	if (ret) {
		dev_err(dwc->dev, "failed to enable %s\n", dep->name);
		return;
	}

	dep = dwc->eps[1];
	ret = __dwc3_gadget_ep_enable(dep, DWC3_DEPCFG_ACTION_MODIFY);
	if (ret) {
		dev_err(dwc->dev, "failed to enable %s\n", dep->name);
		return;
	}

	/*
	 * Configure PHY via GUSB3PIPECTLn if required.
	 *
	 * Update GTXFIFOSIZn
	 *
	 * In both cases reset values should be sufficient.
	 */

#ifdef CONFIG_USB_FIX_PHY_PULLUP_ISSUE
	/* This W/A patch made for Lhotse H/W bugs.(Need to remove) */
	/**
	 * In case there is not a resistance to detect VBUS,
	 * DP/DM controls by S/W are needed at this point.
	 */
	if (dwc->is_not_vbus_pad && !(speed & DWC3_DCFG_FULLSPEED1)) {
		phy_set(dwc->usb2_generic_phy, SET_DPPULLUP_DISABLE, NULL);
		phy_set(dwc->usb3_generic_phy, SET_DPPULLUP_DISABLE, NULL);
	}
#endif
}

static void dwc3_gadget_wakeup_interrupt(struct dwc3 *dwc)
{
#ifdef CONFIG_USB_FIX_PHY_PULLUP_ISSUE
	/* This W/A patch made for Lhotse H/W bugs.(Need to remove) */
	u8 speed;
	u32 reg;

	reg = dwc3_readl(dwc->regs, DWC3_DSTS);
	speed = reg & DWC3_DSTS_CONNECTSPD;

	if (dwc->is_not_vbus_pad && !(speed & DWC3_DCFG_FULLSPEED1)) {
		phy_set(dwc->usb2_generic_phy, SET_DPPULLUP_DISABLE, NULL);
		phy_set(dwc->usb3_generic_phy, SET_DPPULLUP_DISABLE, NULL);
	}
#endif

	/*
	 * TODO take core out of low power mode when that's
	 * implemented.
	 */

	if (dwc->gadget_driver && dwc->gadget_driver->resume) {
		spin_unlock(&dwc->lock);
		dwc->gadget_driver->resume(&dwc->gadget);
		spin_lock(&dwc->lock);
	}
}

static void dwc3_gadget_linksts_change_interrupt(struct dwc3 *dwc,
		unsigned int evtinfo)
{
	enum dwc3_link_state	next = evtinfo & DWC3_LINK_STATE_MASK;
	unsigned int		pwropt;

	/*
	 * WORKAROUND: DWC3 < 2.50a have an issue when configured without
	 * Hibernation mode enabled which would show up when device detects
	 * host-initiated U3 exit.
	 *
	 * In that case, device will generate a Link State Change Interrupt
	 * from U3 to RESUME which is only necessary if Hibernation is
	 * configured in.
	 *
	 * There are no functional changes due to such spurious event and we
	 * just need to ignore it.
	 *
	 * Refers to:
	 *
	 * STAR#9000570034 RTL: SS Resume event generated in non-Hibernation
	 * operational mode
	 */
	pwropt = DWC3_GHWPARAMS1_EN_PWROPT(dwc->hwparams.hwparams1);
	if ((dwc->revision < DWC3_REVISION_250A) &&
			(pwropt != DWC3_GHWPARAMS1_EN_PWROPT_HIB)) {
		if ((dwc->link_state == DWC3_LINK_STATE_U3) &&
				(next == DWC3_LINK_STATE_RESUME)) {
			return;
		}
	}

	/*
	 * WORKAROUND: DWC3 Revisions <1.83a have an issue which, depending
	 * on the link partner, the USB session might do multiple entry/exit
	 * of low power states before a transfer takes place.
	 *
	 * Due to this problem, we might experience lower throughput. The
	 * suggested workaround is to disable DCTL[12:9] bits if we're
	 * transitioning from U1/U2 to U0 and enable those bits again
	 * after a transfer completes and there are no pending transfers
	 * on any of the enabled endpoints.
	 *
	 * This is the first half of that workaround.
	 *
	 * Refers to:
	 *
	 * STAR#9000446952: RTL: Device SS : if U1/U2 ->U0 takes >128us
	 * core send LGO_Ux entering U0
	 */
	if (dwc->revision < DWC3_REVISION_183A) {
		if (next == DWC3_LINK_STATE_U0) {
			u32	u1u2;
			u32	reg;

			switch (dwc->link_state) {
			case DWC3_LINK_STATE_U1:
			case DWC3_LINK_STATE_U2:
				reg = dwc3_readl(dwc->regs, DWC3_DCTL);
				u1u2 = reg & (DWC3_DCTL_INITU2ENA
						| DWC3_DCTL_ACCEPTU2ENA
						| DWC3_DCTL_INITU1ENA
						| DWC3_DCTL_ACCEPTU1ENA);

				if (!dwc->u1u2)
					dwc->u1u2 = reg & u1u2;

				reg &= ~u1u2;

				dwc3_writel(dwc->regs, DWC3_DCTL, reg);
				break;
			default:
				/* do nothing */
				break;
			}
		}
	}

	if(dwc->max_cnt_link_info) {
		printk("usb: %s : evtinfo = 0x%x link state = %d\n", __func__, evtinfo, next);
		dwc->max_cnt_link_info--;
	}

	if (dwc->linkstate_ai >= DWC3_LINK_STATE_LAST_INFO_MEM)
		dwc->linkstate_ai = 0;
	dwc->linkstate_record[dwc->linkstate_ai++] = next;

	switch (next) {
	case DWC3_LINK_STATE_U0:
#ifdef CONFIG_USB_FIX_PHY_PULLUP_ISSUE
		/* This W/A patch made for Lhotse H/W bugs.(Need to remove) */
		if (dwc->is_not_vbus_pad) {
			phy_set(dwc->usb2_generic_phy, SET_DPPULLUP_ENABLE, NULL);
			phy_set(dwc->usb3_generic_phy, SET_DPPULLUP_ENABLE, NULL);
		}
#endif
#if defined(CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE) && defined(CONFIG_SEC_FACTORY)
		cancel_delayed_work(&dwc->usb_link_state_check_work);
#endif

		if (dwc->vbus_current == USB_CURRENT_SUSPENDED) {
			if (dwc->gadget.state == USB_STATE_CONFIGURED) {
				if (dwc->gadget.speed >= USB_SPEED_SUPER)
					dwc->vbus_current = USB_CURRENT_SUPER_SPEED;
				else
					dwc->vbus_current = USB_CURRENT_HIGH_SPEED;
			} else
				dwc->vbus_current = USB_CURRENT_UNCONFIGURED;
			schedule_work(&dwc->set_vbus_current_work);
		}
		break;
	case DWC3_LINK_STATE_U1:
		if (dwc->speed == USB_SPEED_SUPER)
			dwc3_suspend_gadget(dwc);
		break;
	case DWC3_LINK_STATE_U3:
#ifdef CONFIG_ENABLE_USB_SUSPEND_STATE
		printk("usb: sending usb u3 suspend state\n");
		dwc->vbus_current = USB_CURRENT_SUSPENDED;
		schedule_work(&dwc->set_vbus_current_work);
#else
		if (dwc->gadget.state == USB_STATE_CONFIGURED) {
			dwc->vbus_current = USB_CURRENT_UNCONFIGURED;
			schedule_work(&dwc->set_vbus_current_work);
		}
#endif
		dwc3_suspend_gadget(dwc);
		break;
	case DWC3_LINK_STATE_U2:
		dwc3_suspend_gadget(dwc);
		break;
#ifdef CONFIG_USB_FIX_PHY_PULLUP_ISSUE
	/* This W/A patch made for Lhotse H/W bugs.(Need to remove) */
	case DWC3_LINK_STATE_RX_DET:	/* Early Suspend in HS */
		if (dwc->is_not_vbus_pad) {
			phy_set(dwc->usb2_generic_phy, SET_DPPULLUP_ENABLE, NULL);
			phy_set(dwc->usb3_generic_phy, SET_DPPULLUP_ENABLE, NULL);
		}
		break;
#endif
	case DWC3_LINK_STATE_RESUME:
#ifdef CONFIG_USB_FIX_PHY_PULLUP_ISSUE
		/* This W/A patch made for Lhotse H/W bugs.(Need to remove) */
		if (dwc->is_not_vbus_pad) {
			phy_set(dwc->usb2_generic_phy, SET_DPPULLUP_ENABLE, NULL);
			phy_set(dwc->usb3_generic_phy, SET_DPPULLUP_ENABLE, NULL);
		}
#endif
		dwc3_resume_gadget(dwc);
		if (dwc->vbus_current == USB_CURRENT_SUSPENDED) {
			if (dwc->gadget.state == USB_STATE_CONFIGURED) {
				if (dwc->gadget.speed >= USB_SPEED_SUPER)
					dwc->vbus_current = USB_CURRENT_SUPER_SPEED;
				else
					dwc->vbus_current = USB_CURRENT_HIGH_SPEED;
			} else
				dwc->vbus_current = USB_CURRENT_UNCONFIGURED;
			schedule_work(&dwc->set_vbus_current_work);
		}
		break;
#if defined(CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE) && defined(CONFIG_SEC_FACTORY)
	case DWC3_LINK_STATE_POLL:
		schedule_delayed_work(&dwc->usb_link_state_check_work,
				msecs_to_jiffies(2000));
		break;
#endif
	default:
		/* do nothing */
		break;
	}

	dwc->link_state = next;
}

static void dwc3_gadget_suspend_interrupt(struct dwc3 *dwc,
					  unsigned int evtinfo)
{
	enum dwc3_link_state next = evtinfo & DWC3_LINK_STATE_MASK;

	/* WA for Lhotse U3 */
	if (dwc->gadget.speed >= USB_SPEED_SUPER)
		phy_ilbk(dwc->usb3_generic_phy);

	if (dwc->link_state != next && next == DWC3_LINK_STATE_U3)
		dwc3_suspend_gadget(dwc);

	dwc->link_state = next;
}

static void dwc3_gadget_hibernation_interrupt(struct dwc3 *dwc,
		unsigned int evtinfo)
{
	unsigned int is_ss = evtinfo & BIT(4);

	/*
	 * WORKAROUND: DWC3 revison 2.20a with hibernation support
	 * have a known issue which can cause USB CV TD.9.23 to fail
	 * randomly.
	 *
	 * Because of this issue, core could generate bogus hibernation
	 * events which SW needs to ignore.
	 *
	 * Refers to:
	 *
	 * STAR#9000546576: Device Mode Hibernation: Issue in USB 2.0
	 * Device Fallback from SuperSpeed
	 */
	if (is_ss ^ (dwc->speed == USB_SPEED_SUPER))
		return;

	/* enter hibernation here */
}

static void dwc3_gadget_interrupt(struct dwc3 *dwc,
		const struct dwc3_event_devt *event)
{
	switch (event->type) {
	case DWC3_DEVICE_EVENT_DISCONNECT:
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
		dwc3_gadget_cable_connect(dwc, false);
#endif
		pr_info("usb: %s DISCONNECT\n", __func__);
		dwc3_gadget_disconnect_interrupt(dwc);
		break;
	case DWC3_DEVICE_EVENT_RESET:
		pr_info("usb: %s RESET\n", __func__);
		dwc3_gadget_link_state_print(dwc, "RESET");
		dwc3_gadget_reset_interrupt(dwc);
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
		if (dwc->gadget.speed == USB_SPEED_FULL)
			store_usblog_notify(NOTIFY_USBSTATE,
						(void *)"USB_STATE=RESET:FULL", NULL);
		else if (dwc->gadget.speed == USB_SPEED_HIGH)
			store_usblog_notify(NOTIFY_USBSTATE,
				(void *)"USB_STATE=RESET:HIGH", NULL);
		else if (dwc->gadget.speed == USB_SPEED_SUPER)
			store_usblog_notify(NOTIFY_USBSTATE,
				(void *)"USB_STATE=RESET:SUPER", NULL);
#endif
		break;
	case DWC3_DEVICE_EVENT_CONNECT_DONE:
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
		dwc3_gadget_cable_connect(dwc, true);
#endif
		dwc3_gadget_conndone_interrupt(dwc);
		break;
	case DWC3_DEVICE_EVENT_WAKEUP:
		dwc3_gadget_wakeup_interrupt(dwc);
		break;
	case DWC3_DEVICE_EVENT_HIBER_REQ:
#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
		if (dev_WARN_ONCE(dwc->dev, !dwc->has_hibernation,
					"unexpected hibernation event\n"))
#else
		if (!dwc->has_hibernation)
#endif
			break;

		dwc3_gadget_hibernation_interrupt(dwc, event->event_info);
		break;
	case DWC3_DEVICE_EVENT_LINK_STATUS_CHANGE:
		dwc3_gadget_linksts_change_interrupt(dwc, event->event_info);
		break;
	case DWC3_DEVICE_EVENT_EOPF:
		/* It changed to be suspend event for version 2.30a and above */
		if (dwc->revision >= DWC3_REVISION_230A) {
			/*
			 * Ignore suspend event until the gadget enters into
			 * USB_STATE_CONFIGURED state.
			 */
			if (dwc->gadget.state >= USB_STATE_CONFIGURED)
				dwc3_gadget_suspend_interrupt(dwc,
						event->event_info);
		}
		break;
	case DWC3_DEVICE_EVENT_SOF:
	case DWC3_DEVICE_EVENT_ERRATIC_ERROR:
	case DWC3_DEVICE_EVENT_CMD_CMPL:
	case DWC3_DEVICE_EVENT_OVERFLOW:
		break;
	default:
#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
		dev_WARN(dwc->dev, "UNKNOWN IRQ %d\n", event->type);
#endif
		break;
	}
}

static void dwc3_process_event_entry(struct dwc3 *dwc,
		const union dwc3_event *event)
{
	trace_dwc3_event(event->raw, dwc);

	if (!event->type.is_devspec)
		dwc3_endpoint_interrupt(dwc, &event->depevt);
	else if (event->type.type == DWC3_EVENT_TYPE_DEV)
		dwc3_gadget_interrupt(dwc, &event->devt);
	else
		dev_err(dwc->dev, "UNKNOWN IRQ type %d\n", event->raw);
}

static irqreturn_t dwc3_process_event_buf(struct dwc3_event_buffer *evt)
{
	struct dwc3 *dwc = evt->dwc;
	irqreturn_t ret = IRQ_NONE;
	int left;
	u32 reg;

#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
	left = evt->count;

	if (!(evt->flags & DWC3_EVENT_PENDING))
		return IRQ_NONE;
#else
	reg = dwc3_readl(dwc->regs, DWC3_GEVNTCOUNT(0));
	reg &= DWC3_GEVNTCOUNT_MASK;
	evt->count = reg;
	left = evt->count;
#endif

	while (left > 0) {
		union dwc3_event event;

		event.raw = *(u32 *) (evt->buf + evt->lpos);

		dwc3_process_event_entry(dwc, &event);

		/*
		 * FIXME we wrap around correctly to the next entry as
		 * almost all entries are 4 bytes in size. There is one
		 * entry which has 12 bytes which is a regular entry
		 * followed by 8 bytes data. ATM I don't know how
		 * things are organized if we get next to the a
		 * boundary so I worry about that once we try to handle
		 * that.
		 */
		evt->lpos = (evt->lpos + 4) % DWC3_EVENT_BUFFERS_SIZE;
		left -= 4;

		dwc3_writel(dwc->regs, DWC3_GEVNTCOUNT(0), 4);
	}

	evt->count = 0;
#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
	evt->flags &= ~DWC3_EVENT_PENDING;
#endif
	ret = IRQ_HANDLED;

#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
	/* Unmask interrupt */
	reg = dwc3_readl(dwc->regs, DWC3_GEVNTSIZ(0));
	reg &= ~DWC3_GEVNTSIZ_INTMASK;
	dwc3_writel(dwc->regs, DWC3_GEVNTSIZ(0), reg);
#endif

	if (dwc->imod_interval) {
		dwc3_writel(dwc->regs, DWC3_GEVNTCOUNT(0), DWC3_GEVNTCOUNT_EHB);
		dwc3_writel(dwc->regs, DWC3_DEV_IMOD(0), dwc->imod_interval);
	}

	return ret;
}

#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
static irqreturn_t dwc3_thread_interrupt(int irq, void *_evt)
{
	struct dwc3_event_buffer *evt = _evt;
	struct dwc3 *dwc = evt->dwc;
	unsigned long flags;
	irqreturn_t ret = IRQ_NONE;

	spin_lock_irqsave(&dwc->lock, flags);
	ret = dwc3_process_event_buf(evt);
	spin_unlock_irqrestore(&dwc->lock, flags);

	return ret;
}

static irqreturn_t dwc3_check_event_buf(struct dwc3_event_buffer *evt)
{
	struct dwc3 *dwc = evt->dwc;
	u32 amount;
	u32 count;
	u32 reg;

	if (pm_runtime_suspended(dwc->dev)) {
		pm_runtime_get(dwc->dev);
		disable_irq_nosync(dwc->irq_gadget);
		dwc->pending_events = true;
		return IRQ_HANDLED;
	}

	/*
	 * With PCIe legacy interrupt, test shows that top-half irq handler can
	 * be called again after HW interrupt deassertion. Check if bottom-half
	 * irq event handler completes before caching new event to prevent
	 * losing events.
	 */
	if (evt->flags & DWC3_EVENT_PENDING)
		return IRQ_HANDLED;

	count = dwc3_readl(dwc->regs, DWC3_GEVNTCOUNT(0));
	count &= DWC3_GEVNTCOUNT_MASK;
	if (!count)
		return IRQ_NONE;

	evt->count = count;
	evt->flags |= DWC3_EVENT_PENDING;

	/* Mask interrupt */
	reg = dwc3_readl(dwc->regs, DWC3_GEVNTSIZ(0));
	reg |= DWC3_GEVNTSIZ_INTMASK;
	dwc3_writel(dwc->regs, DWC3_GEVNTSIZ(0), reg);

	amount = min(count, evt->length - evt->lpos);
	memcpy(evt->cache + evt->lpos, evt->buf + evt->lpos, amount);

	if (amount < count)
		memcpy(evt->cache, evt->buf, count - amount);

	dwc3_writel(dwc->regs, DWC3_GEVNTCOUNT(0), count);

	return IRQ_WAKE_THREAD;
}
#endif

static irqreturn_t dwc3_interrupt(int irq, void *_evt)
{
	struct dwc3_event_buffer	*evt = _evt;
	irqreturn_t			ret = IRQ_NONE;
	struct dwc3 *dwc = evt->dwc;

	spin_lock(&dwc->lock);

#if IS_ENABLED(DWC3_GADGET_IRQ_ORG)
	irqreturn_t status;

	status = dwc3_check_event_buf(evt);
	if (status == IRQ_WAKE_THREAD)
		ret = status;
#else
	ret |= dwc3_process_event_buf(evt);
#endif

	spin_unlock(&dwc->lock);

	return ret;
}

static int dwc3_gadget_get_irq(struct dwc3 *dwc)
{
	struct platform_device *dwc3_pdev = to_platform_device(dwc->dev);
	int irq;

	irq = platform_get_irq_byname(dwc3_pdev, "peripheral");
	if (irq > 0)
		goto out;

	if (irq == -EPROBE_DEFER)
		goto out;

	irq = platform_get_irq_byname(dwc3_pdev, "dwc_usb3");
	if (irq > 0)
		goto out;

	if (irq == -EPROBE_DEFER)
		goto out;

	irq = platform_get_irq(dwc3_pdev, 0);
	if (irq > 0)
		goto out;

	if (irq != -EPROBE_DEFER)
		dev_err(dwc->dev, "missing peripheral IRQ\n");

	if (!irq)
		irq = -EINVAL;

out:
	return irq;
}

/**
 * dwc3_gadget_init - initializes gadget related registers
 * @dwc: pointer to our controller context structure
 *
 * Returns 0 on success otherwise negative errno.
 */
int dwc3_gadget_init(struct dwc3 *dwc)
{
	int ret;
	int irq;

	irq = dwc3_gadget_get_irq(dwc);
	if (irq < 0) {
		ret = irq;
		goto err0;
	}

	dwc->irq_gadget = irq;
	INIT_DELAYED_WORK(&dwc->usb_event_work, dwc3_gadget_usb_event_work);

	dwc->ep0_trb = dma_alloc_coherent(dwc->sysdev,
					  sizeof(*dwc->ep0_trb) * 2,
					  &dwc->ep0_trb_addr, GFP_KERNEL);
	if (!dwc->ep0_trb) {
		dev_err(dwc->dev, "failed to allocate ep0 trb\n");
		ret = -ENOMEM;
		goto err0;
	}

	dwc->setup_buf = kzalloc(DWC3_EP0_SETUP_SIZE, GFP_KERNEL);
	if (!dwc->setup_buf) {
		ret = -ENOMEM;
		goto err1;
	}

	dwc->bounce = dma_alloc_coherent(dwc->sysdev, DWC3_BOUNCE_SIZE,
			&dwc->bounce_addr, GFP_KERNEL);
	if (!dwc->bounce) {
		ret = -ENOMEM;
		goto err2;
	}

	init_completion(&dwc->ep0_in_setup);

	dwc->gadget.ops			= &dwc3_gadget_ops;
	dwc->gadget.speed		= USB_SPEED_UNKNOWN;
	dwc->gadget.sg_supported	= true;
	dwc->gadget.name		= "dwc3-gadget";
	dwc->gadget.is_otg		= dwc->dr_mode == USB_DR_MODE_OTG;

	/*
	 * FIXME We might be setting max_speed to <SUPER, however versions
	 * <2.20a of dwc3 have an issue with metastability (documented
	 * elsewhere in this driver) which tells us we can't set max speed to
	 * anything lower than SUPER.
	 *
	 * Because gadget.max_speed is only used by composite.c and function
	 * drivers (i.e. it won't go into dwc3's registers) we are allowing this
	 * to happen so we avoid sending SuperSpeed Capability descriptor
	 * together with our BOS descriptor as that could confuse host into
	 * thinking we can handle super speed.
	 *
	 * Note that, in fact, we won't even support GetBOS requests when speed
	 * is less than super speed because we don't have means, yet, to tell
	 * composite.c that we are USB 2.0 + LPM ECN.
	 */
	if (dwc->revision < DWC3_REVISION_220A &&
	    !dwc->dis_metastability_quirk)
		dev_info(dwc->dev, "changing max_speed on rev %08x\n",
				dwc->revision);

	dwc->gadget.max_speed		= dwc->maximum_speed;

	/*
	 * REVISIT: Here we should clear all pending IRQs to be
	 * sure we're starting from a well known location.
	 */

	ret = dwc3_gadget_init_endpoints(dwc, dwc->num_eps);
	if (ret)
		goto err3;

	ret = usb_add_gadget_udc(dwc->dev, &dwc->gadget);
	if (ret) {
		dev_err(dwc->dev, "failed to register udc\n");
		goto err4;
	}

	if (dwc->dotg) {
		ret = otg_set_peripheral(&dwc->dotg->otg, &dwc->gadget);
		if (ret) {
			dev_err(dwc->dev, "failed to set otg peripheral\n");
			goto err4;
		}
	}

#ifdef CONFIG_USB_TYPEC_MANAGER_NOTIFIER
	probe_typec_manager_gadget_ops(&manager_dwc3_gadget_ops);
#endif
	/* Kernel minor update - 4.19.36
	dwc3_gadget_set_speed(&dwc->gadget, dwc->maximum_speed);
	*/

	return 0;

err4:
	usb_del_gadget_udc(&dwc->gadget);
	dwc3_gadget_free_endpoints(dwc);

err3:
	dma_free_coherent(dwc->sysdev, DWC3_BOUNCE_SIZE, dwc->bounce,
			dwc->bounce_addr);

err2:
	kfree(dwc->setup_buf);

err1:
	dma_free_coherent(dwc->sysdev, sizeof(*dwc->ep0_trb) * 2,
			dwc->ep0_trb, dwc->ep0_trb_addr);

err0:
	return ret;
}

/* -------------------------------------------------------------------------- */

void dwc3_gadget_exit(struct dwc3 *dwc)
{
	if (dwc->dotg)
		otg_set_peripheral(&dwc->dotg->otg, NULL);

	usb_del_gadget_udc(&dwc->gadget);
	dwc3_gadget_free_endpoints(dwc);
	dma_free_coherent(dwc->sysdev, DWC3_BOUNCE_SIZE, dwc->bounce,
			  dwc->bounce_addr);
	kfree(dwc->setup_buf);
	dma_free_coherent(dwc->sysdev, sizeof(*dwc->ep0_trb) * 2,
			  dwc->ep0_trb, dwc->ep0_trb_addr);
}

int dwc3_gadget_suspend(struct dwc3 *dwc)
{
	if (!dwc->gadget_driver)
		return 0;

	dwc3_gadget_run_stop(dwc, false, false);
	dwc3_disconnect_gadget(dwc);
	__dwc3_gadget_stop(dwc);

	return 0;
}

int dwc3_gadget_resume(struct dwc3 *dwc)
{
	int			ret;

	if (!dwc->gadget_driver)
		return 0;

	ret = __dwc3_gadget_start(dwc);
	if (ret < 0)
		goto err0;

	ret = dwc3_gadget_run_stop(dwc, true, false);
	if (ret < 0)
		goto err1;

	return 0;

err1:
	__dwc3_gadget_stop(dwc);

err0:
	return ret;
}

void dwc3_gadget_disconnect_proc(struct dwc3 *dwc)
{
	int			reg;

	reg = dwc3_readl(dwc->regs, DWC3_DCTL);
	reg &= ~DWC3_DCTL_INITU1ENA;
	dwc3_writel(dwc->regs, DWC3_DCTL, reg);

	reg &= ~DWC3_DCTL_INITU2ENA;
	dwc3_writel(dwc->regs, DWC3_DCTL, reg);

	if (dwc->gadget_driver && dwc->gadget_driver->disconnect)
		dwc->gadget_driver->disconnect(&dwc->gadget);

	dwc->start_config_issued = false;

	dwc->gadget.speed = USB_SPEED_UNKNOWN;
	dwc->setup_packet_pending = false;

	complete(&dwc->disconnect);
}

void dwc3_gadget_process_pending_events(struct dwc3 *dwc)
{
	if (dwc->pending_events) {
		dwc3_interrupt(dwc->irq_gadget, dwc->ev_buf);
		dwc->pending_events = false;
		enable_irq(dwc->irq_gadget);
	}
}
