/*
 * Platform data for the chipidea USB dual role controller
 */

#ifndef __LINUX_USB_CHIPIDEA_H
#define __LINUX_USB_CHIPIDEA_H

#include <linux/usb/otg.h>

struct ci_hdrc;
struct ci_hdrc_platform_data {
	const char	*name;
	/* offset of the capability registers */
	uintptr_t	 capoffset;
	unsigned	 power_budget;
	struct usb_phy	*phy;
	enum usb_phy_interface phy_mode;
	unsigned long	 flags;
//	unsigned int	nz_itc;							/* TODO: djStolen; whole batch could be old remnant or cherry picking from coming Linux Kernel versions */
//#define CI_HDRC_ZERO_ITC		BIT(4)
//#define CI_HDRC_IS_OTG			BIT(5)
//	bool		l1_supported;
//
//#define CI_HDRC_CONTROLLER_CONNECT_EVENT	1
//#define CI_HDRC_CONTROLLER_SUSPEND_EVENT		2
//#define CI_HDRC_CONTROLLER_REMOTE_WAKEUP_EVENT	3
//#define CI_HDRC_CONTROLLER_RESUME_EVENT		4
//#define CI_HDRC_CONTROLLER_DISCONNECT_EVENT	5
//#define CI_HDRC_CONTROLLER_UDC_STARTED_EVENT	6

#define CI_HDRC_REGS_SHARED		BIT(0)
#define CI_HDRC_REQUIRE_TRANSCEIVER	BIT(1)
#define CI_HDRC_PULLUP_ON_VBUS		BIT(2)
#define CI_HDRC_DISABLE_STREAMING	BIT(3)
	enum usb_dr_mode	dr_mode;
#define CI_HDRC_CONTROLLER_RESET_EVENT		0
#define CI_HDRC_CONTROLLER_STOPPED_EVENT	1
	void	(*notify_event) (struct ci_hdrc *ci, unsigned event);
};

/* Default offset of capability registers */
#define DEF_CAPOFFSET		0x100

/* Add ci hdrc device */
struct platform_device *ci_hdrc_add_device(struct device *dev,
			struct resource *res, int nres,
			struct ci_hdrc_platform_data *platdata);
/* Remove ci hdrc device */
void ci_hdrc_remove_device(struct platform_device *pdev);

#endif
