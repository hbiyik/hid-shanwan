// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Force feedback support for Shanwan USB WirelessGamepad
 *
 * 0575 mapping according to Gamepad Protocol: the code for each physical
 * button, not the order of the bits in the report
 *
 * Button 01: BTN_SOUTH (CROSS)
 * Button 02: BTN_EAST(CIRCLE)
 * Button 03: BTN_NORTH (TRIANGLE)
 * Button 04: BTN_WEST (SQUARE)
 * Button 05: BTN_TL (L1)
 * Button 06: BTN_TR (R1)
 * Button 07: BTN_TL2 (L2)
 * Button 08: BTN_TR2 (R2)
 * Button 09: BTN_SELECT
 * Button 10: BTN_START
 * Button 11: BTN_MODE
 * Button 12: BTN_THUMBL (LS1)
 * Button 13: BTN_THUMBR (LS2)
 * LS1: X/Y AXIS
 * LS2: Rx/Ry AXIS
 * L2/R2 pressure: Z/Rz (which trigger is which not verified)
 *
 * 2563:0526 ("Android Gamepad" mode) keeps the device's own descriptor and
 * only gets rumble. Its firmware runs the weak (right) motor only on or off,
 * so the driver sets that motor's strength by switching it in whole USB
 * windows of the OUT endpoint (software PWM, the weak_pwm parameters).
 */

#include <linux/bitmap.h>
#include <linux/input.h>
#include <linux/log2.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#define USB_VENDOR_ID_SHANWAN 0x2563
#define USB_DEVICE_ID_SHANWAN_USB_WIRELESSGAMEPAD 0x0575
/*
 * "Android Gamepad" mode, e.g. Genesis PV58: the HID descriptor has no output
 * report, but interface 0 has an interrupt OUT endpoint that takes the same
 * 02 08 <weak> <strong> <duration> rumble command.
 */
#define USB_DEVICE_ID_SHANWAN_ANDROID_GAMEPAD 0x0526
#define SHANWAN_RUMBLE_LEN 8
#define SHANWAN_SEND_RETRIES 3
#define SHANWAN_RETRY_MS 50

/* hid_device_id.driver_data */
#define SHANWAN_OPEN_ON_PROBE BIT(0)
#define SHANWAN_KNOWN_RDESC_ONLY BIT(1)	/* rumble only with the known descriptor */
#define SHANWAN_STRONG_MIN BIT(2)	/* scale the strong motor onto its start threshold */
#define SHANWAN_WEAK_PWM BIT(3)		/* weak motor strength by software PWM */

/* the PV58's strong (left) motor starts at about 0x40 */
#define SHANWAN_STRONG_MIN_BYTE 0x40

/*
 * Rumble of the 0526 mode, also changeable at runtime in
 * /sys/module/hid_shanwan/parameters/; the next effect or PWM phase uses it.
 * Numbers out of range are clamped where they are used, not refused at load.
 */
static unsigned int strong_min = SHANWAN_STRONG_MIN_BYTE;
module_param(strong_min, uint, 0644);
MODULE_PARM_DESC(strong_min, "Byte the strong (left) motor starts at, 0-255 (default 64)");

static bool weak_pwm = true;
module_param(weak_pwm, bool, 0644);
MODULE_PARM_DESC(weak_pwm, "Set the strength of the on/off weak (right) motor by software PWM (default Y)");

static unsigned int weak_pwm_period = 64;
module_param(weak_pwm_period, uint, 0644);
MODULE_PARM_DESC(weak_pwm_period,
		 "PWM period in ms, in whole USB windows of the OUT endpoint, 8 ms on the PV58 (default 64)");

static unsigned int weak_pwm_min = 1;
module_param(weak_pwm_min, uint, 0644);
MODULE_PARM_DESC(weak_pwm_min, "USB windows the weak motor is on per period for the gentlest effect (default 1)");

/* rich mode descriptor of 0575 as the device sends it, replaced by pid0575_rdesc_fixed */
static const __u8 pid0575_rdesc_orig[] = {
	0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x15, 0x00, 0x25, 0x01, 0x35, 0x00,
	0x45, 0x01, 0x75, 0x01, 0x95, 0x0D, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0D,
	0x81, 0x02, 0x95, 0x03, 0x81, 0x01, 0x05, 0x01, 0x25, 0x07, 0x46, 0x3B,
	0x01, 0x75, 0x04, 0x95, 0x01, 0x65, 0x14, 0x09, 0x39, 0x81, 0x42, 0x65,
	0x00, 0x95, 0x01, 0x81, 0x01, 0x26, 0xFF, 0x00, 0x46, 0xFF, 0x00, 0x09,
	0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x75, 0x08, 0x95, 0x04, 0x81,
	0x02, 0x06, 0x00, 0xFF, 0x09, 0x20, 0x09, 0x21, 0x09, 0x22, 0x09, 0x23,
	0x09, 0x24, 0x09, 0x25, 0x09, 0x26, 0x09, 0x27, 0x09, 0x28, 0x09, 0x29,
	0x09, 0x2A, 0x09, 0x2B, 0x95, 0x0C, 0x81, 0x02, 0x0A, 0x21, 0x26, 0x95,
	0x08, 0xB1, 0x02, 0x0A, 0x21, 0x26, 0x91, 0x02, 0x26, 0xFF, 0x03, 0x46,
	0xFF, 0x03, 0x09, 0x2C, 0x09, 0x2D, 0x09, 0x2E, 0x09, 0x2F, 0x75, 0x10,
	0x95, 0x04, 0x81, 0x02, 0xC0,
};


static const __u8 pid0575_rdesc_fixed[] = {
	0x05, 0x01,        /* Usage Page (Generic Desktop Ctrls) */
	0x09, 0x05,        /* Usage (Game Pad) */
	0xA1, 0x01,        /* Collection (Application) */
	0x15, 0x00,        /*   Logical Minimum (0) */
	0x25, 0x01,        /*   Logical Maximum (1) */
	0x35, 0x00,        /*   Physical Minimum (0) */
	0x45, 0x01,        /*   Physical Maximum (1) */
	0x75, 0x01,        /*   Report Size (1) */
	0x95, 0x0D,        /*   Report Count (13) */
	0x05, 0x09,        /*   Usage Page (Button), hid-input: Button N -> BTN_SOUTH + N - 1 */
	0x09, 0x04,        /*   Usage (Button 4)  BTN_NORTH */
	0x09, 0x02,        /*   Usage (Button 2)  BTN_EAST */
	0x09, 0x01,        /*   Usage (Button 1)  BTN_SOUTH */
	0x09, 0x05,        /*   Usage (Button 5)  BTN_WEST */
	0x09, 0x07,        /*   Usage (Button 7)  BTN_TL */
	0x09, 0x08,        /*   Usage (Button 8)  BTN_TR */
	0x09, 0x09,        /*   Usage (Button 9)  BTN_TL2 */
	0x09, 0x0A,        /*   Usage (Button 10) BTN_TR2 */
	0x09, 0x0B,        /*   Usage (Button 11) BTN_SELECT */
	0x09, 0x0C,        /*   Usage (Button 12) BTN_START */
	0x09, 0x0E,        /*   Usage (Button 14) BTN_THUMBL */
	0x09, 0x0F,        /*   Usage (Button 15) BTN_THUMBR */
	0x09, 0x0D,        /*   Usage (Button 13) BTN_MODE */
	0x81, 0x02,        /*   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position) */
	0x75, 0x01,        /*   Report Size (1) */
	0x95, 0x03,        /*   Report Count (3) */
	0x81, 0x01,        /*   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position) */
	0x05, 0x01,        /*   Usage Page (Generic Desktop Ctrls) */
	0x25, 0x07,        /*   Logical Maximum (7) */
	0x46, 0x3B, 0x01,  /*   Physical Maximum (315) */
	0x75, 0x04,        /*   Report Size (4) */
	0x95, 0x01,        /*   Report Count (1) */
	0x65, 0x14,        /*   Unit (System: English Rotation, Length: Centimeter) */
	0x09, 0x39,        /*   Usage (Hat switch) */
	0x81, 0x42,        /*   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,Null State) */
	0x65, 0x00,        /*   Unit (None) */
	0x95, 0x01,        /*   Report Count (1) */
	0x81, 0x01,        /*   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position) */
	0x26, 0xFF, 0x00,  /*   Logical Maximum (255) */
	0x46, 0xFF, 0x00,  /*   Physical Maximum (255) */
	0x09, 0x30,        /*   Usage (X) */
	0x09, 0x31,        /*   Usage (Y) */
	0x09, 0x33,        /*   Usage (Rx) */
	0x09, 0x34,        /*   Usage (Ry) */
	0x75, 0x08,        /*   Report Size (8) */
	0x95, 0x04,        /*   Report Count (4) */
	0x81, 0x02,        /*   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position) */
	0x95, 0x0A,        /*   Report Count (10) */
	0x81, 0x01,        /*   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position) */
	0x05, 0x01,        /*   Usage Page (Generic Desktop Ctrls) */
	0x26, 0xFF, 0x00,  /*   Logical Maximum (255) */
	0x46, 0xFF, 0x00,  /*   Physical Maximum (255) */
	0x09, 0x32,        /*   Usage (Z) */
	0x09, 0x35,        /*   Usage (Rz) */
	0x95, 0x02,        /*   Report Count (2) */
	0x81, 0x02,        /*   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position) */
	0x95, 0x08,        /*   Report Count (8) */
	0x81, 0x01,        /*   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position) */
	0x06, 0x00, 0xFF,  /*   Usage Page (Vendor Defined 0xFF00) */
	0x0A, 0x21, 0x26,  /*   Usage (0x2621) */
	0xB1, 0x02,        /*   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile) */
	0x0A, 0x21, 0x26,  /*   Usage (0x2621) */
	0x95, 0x08,        /*   Report Count (8) */
	0x91, 0x02,        /*   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile) */
	0xC0,              /* End Collection */
};


struct shanwan_device {
	struct hid_device *hdev;
	unsigned long quirks;	/* hid_device_id.driver_data */
	struct delayed_work rumble_work;
	/* protects weak_mag, strong, retries, removed, suspended and pwm_active */
	spinlock_t lock;
	u16 weak_mag;		/* weak magnitude of the last effect */
	u8 strong;
	u8 retries;
	bool removed;
	bool suspended;
	bool pwm_active;	/* the work is queued for the end of a PWM phase */
	/*
	 * The last command that reached the pad was non-zero. Written only by
	 * the worker, or after cancel_delayed_work_sync() with removed or
	 * suspended set, so it needs no lock. The same holds for pwm_on.
	 */
	bool running;
	bool pwm_on;		/* the PWM phase sent last has the weak motor on */
	bool opened;
	bool known_rdesc;	/* 0575 descriptor matched pid0575_rdesc_orig */
	unsigned int pwm_slot_ms;	/* interval of the OUT endpoint as the host runs it */
	u8 *buf;
};


/*
 * Both modes take the rumble command on the interrupt OUT endpoint of
 * interface 0, the 0575 output report from the fixed descriptor maps to
 * the same 8 bytes.
 */
static int shanwan_send_rumble(struct shanwan_device *shanwan, u8 weak, u8 strong)
{
	u8 *buf = shanwan->buf;
	int ret;

	memset(buf, 0, SHANWAN_RUMBLE_LEN);
	buf[0] = 0x02; /* 2 = rumble effect message */
	buf[1] = 0x08; /* reserved value, always 8 */
	buf[2] = weak; /* right motor */
	buf[3] = strong; /* left motor */
	buf[4] = (weak || strong) ? 0xff : 0x00; /* duration (255 = nonstop) */

	/* a failed start may still have reached the pad, so assume it did */
	if (weak || strong)
		shanwan->running = true;

	ret = hid_hw_output_report(shanwan->hdev, buf, SHANWAN_RUMBLE_LEN);
	if (ret != SHANWAN_RUMBLE_LEN)
		return ret < 0 ? ret : -EIO;

	/* only a stop that was fully sent clears it */
	shanwan->running = weak || strong;
	return 0;
}


static bool shanwan_pwm(struct shanwan_device *shanwan)
{
	return (shanwan->quirks & SHANWAN_WEAK_PWM) && READ_ONCE(weak_pwm);
}


/* USB windows per PWM period, at least one on and one off */
static unsigned int shanwan_pwm_windows(struct shanwan_device *shanwan)
{
	return clamp(READ_ONCE(weak_pwm_period) / max(shanwan->pwm_slot_ms, 1U), 2U, 125U);
}


/*
 * Windows the weak motor is on per period: 0 is off, n is on all the time.
 * The magnitudes split into equal shares and the gentlest gets weak_pwm_min
 * windows: with 8 windows and weak_pwm_min 1, 1..8192 -> 1, 57345..65535 -> 8.
 */
static unsigned int shanwan_weak_windows(u16 magnitude, unsigned int n)
{
	unsigned int first = clamp(READ_ONCE(weak_pwm_min), 1U, n);

	if (!magnitude)
		return 0;
	return first + (u32)(magnitude - 1) * (n - first + 1) / 0xffff;
}


/* the magnitude needs PWM phases: the weak motor is neither off nor on all the time */
static bool shanwan_weak_toggles(struct shanwan_device *shanwan, u16 magnitude)
{
	unsigned int n, windows;

	if (!shanwan_pwm(shanwan))
		return false;
	n = shanwan_pwm_windows(shanwan);
	windows = shanwan_weak_windows(magnitude, n);
	return windows && windows < n;
}


static void shanwan_rumble_work(struct work_struct *work)
{
	struct shanwan_device *shanwan =
		container_of(to_delayed_work(work), struct shanwan_device, rumble_work);
	unsigned int slot = shanwan->pwm_slot_ms, n = 0, windows = 0, phase_ms = 0;
	unsigned long flags;
	bool active, pwm_on = false;
	u16 weak_mag;
	u8 weak, strong;
	int ret;

	spin_lock_irqsave(&shanwan->lock, flags);
	weak_mag = shanwan->weak_mag;
	strong = shanwan->strong;
	active = shanwan->pwm_active;
	spin_unlock_irqrestore(&shanwan->lock, flags);

	if (shanwan_pwm(shanwan)) {
		n = shanwan_pwm_windows(shanwan);
		windows = shanwan_weak_windows(weak_mag, n);
	}

	if (windows && windows < n) {
		/* a new PWM run starts with the on phase, then the phases alternate */
		pwm_on = !(active && shanwan->pwm_on);
		weak = pwm_on ? 0xff : 0;
		phase_ms = (pwm_on ? windows : n - windows) * slot;
	} else if (n) {
		/* the firmware takes any non-zero byte as on */
		weak = windows ? 0xff : 0;
	} else {
		/* round up, a tiny magnitude still starts the motor: 1..257 -> 1, 65535 -> 255 */
		weak = DIV_ROUND_UP(weak_mag, 257);
	}

	ret = shanwan_send_rumble(shanwan, weak, strong);
	shanwan->pwm_on = pwm_on;

	/*
	 * A lost stop would leave the motors running (duration 255), so retry a
	 * few times; the retry sends the latest weak/strong and starts a new PWM
	 * run. No retry once the device is gone, removed or suspended.
	 */
	spin_lock_irqsave(&shanwan->lock, flags);
	shanwan->pwm_active = false;
	if (!ret) {
		shanwan->retries = 0;
		/*
		 * The send returned once a USB window took the packet, so this
		 * phase ends phase_ms from now. Run half a window early: the
		 * next packet then goes out in the window that ends the phase.
		 */
		if (phase_ms && !shanwan->removed && !shanwan->suspended) {
			shanwan->pwm_active = true;
			schedule_delayed_work(&shanwan->rumble_work,
					      msecs_to_jiffies(phase_ms - slot / 2));
		} else if (!shanwan->removed && !shanwan->suspended &&
			   (shanwan->weak_mag != weak_mag || shanwan->strong != strong)) {
			/*
			 * shanwan_play() saw pwm_active still set during the send
			 * and left a PWM effect to the end of the phase, but this
			 * packet ended PWM and no phase follows: send it now.
			 */
			schedule_delayed_work(&shanwan->rumble_work, 0);
		}
	} else if (ret != -ENODEV && ret != -ESHUTDOWN &&
		   shanwan->retries < SHANWAN_SEND_RETRIES &&
		   !shanwan->removed && !shanwan->suspended) {
		shanwan->retries++;
		schedule_delayed_work(&shanwan->rumble_work,
				      msecs_to_jiffies(SHANWAN_RETRY_MS));
	}
	spin_unlock_irqrestore(&shanwan->lock, flags);

	if (ret < 0)
		dev_warn_ratelimited(&shanwan->hdev->dev,
				     "failed to send rumble command: %d\n", ret);
}


/*
 * Map the 16 bit strong magnitude onto the motor's byte. With
 * SHANWAN_STRONG_MIN every non-zero magnitude lands on strong_min..0xff
 * (0x40..0xff by default), so weak effects are still felt instead of
 * falling below the start threshold.
 */
static u8 shanwan_strong_byte(struct shanwan_device *shanwan, u16 magnitude)
{
	unsigned int start;

	if (!(shanwan->quirks & SHANWAN_STRONG_MIN))
		return magnitude / 256;
	if (!magnitude)
		return 0;
	start = min(READ_ONCE(strong_min), 0xffU);
	return start + (u32)magnitude * (0xff - start) / 0xffff;
}


/* Called by ff-memless in atomic context, hid_hw_output_report() sleeps. */
static int shanwan_play(struct input_dev *dev, void *data, struct ff_effect *effect)
{
	struct hid_device *hid = input_get_drvdata(dev);
	struct shanwan_device *shanwan = hid_get_drvdata(hid);
	unsigned long flags;

	if (effect->type != FF_RUMBLE)
		return 0;

	spin_lock_irqsave(&shanwan->lock, flags);
	shanwan->weak_mag = effect->u.rumble.weak_magnitude;
	shanwan->strong = shanwan_strong_byte(shanwan, effect->u.rumble.strong_magnitude);
	shanwan->retries = 0;
	/*
	 * While the weak motor runs PWM phases and still needs them, the work
	 * takes the new values at the end of the current phase, so the phases
	 * keep their length. Anything else goes out at once, a stop too: a
	 * pending retry must not delay it.
	 */
	if (!shanwan->removed && !shanwan->suspended &&
	    !(shanwan->pwm_active && shanwan_weak_toggles(shanwan, shanwan->weak_mag))) {
		cancel_delayed_work(&shanwan->rumble_work);
		schedule_delayed_work(&shanwan->rumble_work, 0);
	}
	spin_unlock_irqrestore(&shanwan->lock, flags);

	return 0;
}


/*
 * usbhid sends output reports on the first interrupt OUT endpoint of the
 * current altsetting. Only interface 0 has one; the second interface of the
 * Android mode carries just system and consumer keys.
 */
static const struct usb_endpoint_descriptor *shanwan_rumble_ep(struct hid_device *hid)
{
	struct usb_host_interface *alt;
	int i;

	if (!hid_is_usb(hid))
		return NULL;

	alt = to_usb_interface(hid->dev.parent)->cur_altsetting;
	if (alt->desc.bInterfaceNumber != 0)
		return NULL;

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		const struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;

		if (usb_endpoint_is_int_out(ep))
			return usb_endpoint_maxp(ep) >= SHANWAN_RUMBLE_LEN ? ep : NULL;
	}

	return NULL;
}


/*
 * One packet goes out per interval of the OUT endpoint, and an output report
 * returns once its packet went out. At full speed the host controller rounds
 * bInterval (ms) down to a power of two, so 10 ms runs as 8 ms. At high speed
 * bInterval is an exponent of 125 us.
 */
static unsigned int shanwan_out_interval_ms(struct hid_device *hid,
					    const struct usb_endpoint_descriptor *ep)
{
	struct usb_device *udev = interface_to_usbdev(to_usb_interface(hid->dev.parent));

	if (udev->speed >= USB_SPEED_HIGH)
		return max(1U, (125U << (clamp_val(ep->bInterval, 1, 16) - 1)) / 1000);
	return rounddown_pow_of_two(clamp_val(ep->bInterval, 1, 255));
}


/*
 * Called by hidinput_connect() before the input device is registered, so
 * force feedback is fully set up by the time userspace can open it.
 */
static int shanwan_input_configured(struct hid_device *hdev,
				    struct hid_input *hidinput)
{
	struct shanwan_device *shanwan = hid_get_drvdata(hdev);
	struct input_dev *dev = hidinput->input;
	const struct usb_endpoint_descriptor *ep;
	int error;

	/* one rumble input: the gamepad on the interface with the OUT endpoint */
	if (shanwan->buf)
		return 0;
	ep = shanwan_rumble_ep(hdev);
	if (!ep)
		return 0;

	if (hidinput->application != HID_GD_GAMEPAD &&
	    hidinput->application != HID_GD_JOYSTICK)
		return 0;

	/* a 0575 descriptor we have not seen may come with another protocol */
	if ((shanwan->quirks & SHANWAN_KNOWN_RDESC_ONLY) && !shanwan->known_rdesc) {
		hid_info(hdev, "unknown descriptor, rumble disabled\n");
		return 0;
	}

	shanwan->pwm_slot_ms = shanwan_out_interval_ms(hdev, ep);
	shanwan->buf = devm_kzalloc(&hdev->dev, SHANWAN_RUMBLE_LEN, GFP_KERNEL);
	if (shanwan->buf) {
		set_bit(FF_RUMBLE, dev->ffbit);
		/* NULL: ff-memless kfree()s its private data, shanwan is devm-managed */
		error = input_ff_create_memless(dev, NULL, shanwan_play);
	} else {
		error = -ENOMEM;
	}

	/* the gamepad works without rumble, so never fail the input device */
	if (error) {
		/* no FF bits without an ff device, ff-memless also set FF_GAIN */
		bitmap_zero(dev->ffbit, FF_CNT);
		devm_kfree(&hdev->dev, shanwan->buf);
		shanwan->buf = NULL;
		hid_warn(hdev, "Failed to enable force feedback support, error: %d\n",
			 error);
	}
	return 0;
}


/*
 * Must run before hid_hw_stop() and before the devm data is freed: the input
 * flush in hid_hw_stop() calls shanwan_play() again.
 */
static void shanwan_stop_rumble(struct shanwan_device *shanwan)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&shanwan->lock, flags);
	shanwan->removed = true;
	spin_unlock_irqrestore(&shanwan->lock, flags);
	/* also waits for and drops a retry the work queued for itself */
	cancel_delayed_work_sync(&shanwan->rumble_work);

	/* duration 255 keeps the motors running after unbind */
	if (!shanwan->running)
		return;

	/*
	 * Unplugged, the send fails with -ENODEV (-ESHUTDOWN without a host
	 * controller), nothing to warn about. No retry: each try can block for
	 * USB_CTRL_SET_TIMEOUT (5 s), and unlike suspend nothing comes later
	 * to send the stop again.
	 */
	ret = shanwan_send_rumble(shanwan, 0, 0);
	if (ret && ret != -ENODEV && ret != -ESHUTDOWN)
		hid_warn(shanwan->hdev,
			 "rumble stop failed, motors may keep running: %d\n", ret);
}


static int shanwan_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct shanwan_device *shanwan;
	int error;

	shanwan = devm_kzalloc(&hdev->dev, sizeof(*shanwan), GFP_KERNEL);
	if (!shanwan)
		return -ENOMEM;

	shanwan->hdev = hdev;
	shanwan->quirks = id->driver_data;
	spin_lock_init(&shanwan->lock);
	INIT_DELAYED_WORK(&shanwan->rumble_work, shanwan_rumble_work);
	/* before hid_parse(): report_fixup and input_configured use it */
	hid_set_drvdata(hdev, shanwan);

	/* one input device per application, as hid-generic does */
	hdev->quirks |= HID_QUIRK_INPUT_PER_APP;

	error = hid_parse(hdev);
	if (error) {
		hid_err(hdev, "parse failed\n");
		return error;
	}

	error = hid_hw_start(hdev, HID_CONNECT_DEFAULT & ~HID_CONNECT_FF);
	if (error) {
		hid_err(hdev, "hw start failed\n");
		return error;
	}

	/*
	 * Keeping 0575 open from probe comes from the original driver, which
	 * does not say why; 0526 does not need it.
	 */
	if (!(shanwan->quirks & SHANWAN_OPEN_ON_PROBE))
		return 0;

	error = hid_hw_open(hdev);
	if (error) {
		dev_err(&hdev->dev, "hw open failed\n");
		shanwan_stop_rumble(shanwan);
		hid_hw_stop(hdev);
		return error;
	}
	shanwan->opened = true;

	return 0;
}


static void shanwan_remove(struct hid_device *hdev)
{
	struct shanwan_device *shanwan = hid_get_drvdata(hdev);

	shanwan_stop_rumble(shanwan);

	if (shanwan->opened)
		hid_hw_close(hdev);
	hid_hw_stop(hdev);
}


#ifdef CONFIG_PM
/* Duration 255 would keep the motors running while the system sleeps. */
static int shanwan_suspend(struct hid_device *hdev, pm_message_t message)
{
	struct shanwan_device *shanwan = hid_get_drvdata(hdev);
	unsigned long flags;

	spin_lock_irqsave(&shanwan->lock, flags);
	shanwan->suspended = true;
	shanwan->pwm_active = false;
	spin_unlock_irqrestore(&shanwan->lock, flags);
	cancel_delayed_work_sync(&shanwan->rumble_work);

	/* best effort: running stays set, so resume sends the stop again */
	if (shanwan->running && shanwan_send_rumble(shanwan, 0, 0))
		hid_warn(hdev, "rumble stop failed, retrying on resume\n");
	return 0;
}


static int shanwan_resume(struct hid_device *hdev)
{
	struct shanwan_device *shanwan = hid_get_drvdata(hdev);
	unsigned long flags;

	spin_lock_irqsave(&shanwan->lock, flags);
	shanwan->suspended = false;
	/*
	 * Restore what was playing before suspend, or retry a stop that did
	 * not get through; I/O is restarted by now.
	 */
	if (!shanwan->removed &&
	    (shanwan->weak_mag || shanwan->strong || shanwan->running)) {
		shanwan->retries = 0;
		/* run now even if a retry is still pending */
		cancel_delayed_work(&shanwan->rumble_work);
		schedule_delayed_work(&shanwan->rumble_work, 0);
	}
	spin_unlock_irqrestore(&shanwan->lock, flags);
	return 0;
}
#endif


static const struct hid_device_id shanwan_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_SHANWAN, USB_DEVICE_ID_SHANWAN_USB_WIRELESSGAMEPAD),
	  .driver_data = SHANWAN_OPEN_ON_PROBE | SHANWAN_KNOWN_RDESC_ONLY },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SHANWAN, USB_DEVICE_ID_SHANWAN_ANDROID_GAMEPAD),
	  .driver_data = SHANWAN_STRONG_MIN | SHANWAN_WEAK_PWM },
	{ }
};


static const __u8 *shanwan_report_fixup(struct hid_device *hid, __u8 *rdesc,
	unsigned int *rsize)
{
	struct shanwan_device *shanwan = hid_get_drvdata(hid);

	if (hid->product != USB_DEVICE_ID_SHANWAN_USB_WIRELESSGAMEPAD)
		return rdesc;

	if (*rsize == sizeof(pid0575_rdesc_orig) &&
	    !memcmp(rdesc, pid0575_rdesc_orig, sizeof(pid0575_rdesc_orig))) {
		if (shanwan)
			shanwan->known_rdesc = true;
		*rsize = sizeof(pid0575_rdesc_fixed);
		return pid0575_rdesc_fixed;
	}

	hid_warn(hid, "unexpected rdesc, please submit for review\n");
	return rdesc;
}


static struct hid_driver shanwan_driver = {
	.name			= "shanwan",
	.id_table		= shanwan_devices,
	.probe			= shanwan_probe,
	.input_configured	= shanwan_input_configured,
	.report_fixup		= shanwan_report_fixup,
	.remove			= shanwan_remove,
#ifdef CONFIG_PM
	.suspend		= shanwan_suspend,
	.resume			= shanwan_resume,
	.reset_resume		= shanwan_resume,
#endif
};


module_hid_driver(shanwan_driver);
MODULE_DEVICE_TABLE(hid, shanwan_devices);
MODULE_AUTHOR("Huseyin BIYIK");
MODULE_DESCRIPTION("Force feedback support for Shanwan USB WirelessGamepad");
MODULE_LICENSE("GPL");
