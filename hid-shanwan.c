// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Force feedback support for Shanwan USB WirelessGamepad

 * mapping according to Gamepad Protocol
 *
 * Button 01: BTN_SOUTH (CROSS)
 * Button 02: BTN_EAST(CIRCLE)
 * Button 03: BTN_NORTH (TRIANGLE)
 * Button 04: BTN_WEST (SQUARE)
 * Button 05: BTL_TL (L1)
 * Button 06: BTM_TR (R1)
 * Button 07: BTN_TL2 (L2)
 * Button 08: BTN_TR2 (R2)
 * Button 09: BTN_SELECT
 * Button 10: BTN_START
 * Button 11: BTN_MODE
 * Button 12: BTN_THUMBL (LS1)
 * Button 13: BTN_THUMBR (LS1)
 * LS1: X/Y AXIS
 * LS2: Rx/Ry AXIS
 * R2/L2 Touch Sensors: R/Rz

*/

#include <linux/input.h>
#include <linux/slab.h>
#include <linux/hid.h>
#include <linux/module.h>

#define PID0575_RDESC_ORIG_SIZE 137
#define USB_VENDOR_ID_SHANWAN 0x2563
#define USB_DEVICE_ID_SHANWAN_USB_WIRELESSGAMEPAD 0x0575

static __u8 pid0575_rdesc_fixed[] = {
	0x05, 0x01,        /* Usage Page (Generic Desktop Ctrls) */
	0x09, 0x05,        /* Usage (Game Pad) */
	0xA1, 0x01,        /* Collection (Application) */
	0x15, 0x00,        /*   Logical Minimum (0) */
	0x25, 0x01,        /*   Logical Maximum (1) */
	0x35, 0x00,        /*   Physical Minimum (0) */
	0x45, 0x01,        /*   Physical Maximum (1) */
	0x75, 0x01,        /*   Report Size (1) */
	0x95, 0x0D,        /*   Report Count (13) */
	0x05, 0x09,        /*   Usage Page (Button) */
	0x09, 0x03,        /*   Usage (BTN_NORTH) */
	0x09, 0x02,        /*   Usage (BTN_EAST) */
	0x09, 0x01,        /*   Usage (BTN_SOUTH) */
	0x09, 0x04,        /*   Usage (BTN_WEST) */
	0x09, 0x05,        /*   Usage (BTN_TL) */
	0x09, 0x06,        /*   Usage (BTN_TR) */
	0x09, 0x07,        /*   Usage (BTN_TL2) */
	0x09, 0x08,        /*   Usage (BTN_TR2) */
	0x09, 0x09,        /*   Usage (BTN_SELECT) */
	0x09, 0x10,        /*   Usage (BTN_START) */
	0x09, 0x12,        /*   Usage (BTN_THUMBL) */
	0x09, 0x13,        /*   Usage (BTN_THUMBR) */
	0x09, 0x11,        /*   Usage (BTN_MODE) */
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
	0xB1, 0x02,        /*   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile) */
	0x0A, 0x21, 0x26,  /*   Usage (0x2621) */
	0x95, 0x08,        /*   Report Count (8) */
	0x91, 0x02,        /*   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile) */
	0x0A, 0x21, 0x26,  /*   Usage (0x2621) */
	0x95, 0x08,        /*   Report Count (8) */
	0x81, 0x02,        /*   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position) */
	0xC0,              /* End Collection */
};

struct shanwan_device {
	struct hid_report *report;
};

static int shanwan_play(struct input_dev *dev, void *data, struct ff_effect *effect)
{
	struct hid_device *hid = input_get_drvdata(dev);
	struct shanwan_device *shanwan = data;
	struct hid_report *report = shanwan->report;

	if (effect->type != FF_RUMBLE)
		return 0;

	report->field[0]->value[0] = 0x02; /* 2 = rumble effect message */
	report->field[0]->value[1] = 0x08; /* reserved value, always 8 */
	report->field[0]->value[2] = effect->u.rumble.weak_magnitude / 256; /*right motor*/
	report->field[0]->value[3] = effect->u.rumble.strong_magnitude / 256; /* left motor*/
	report->field[0]->value[4] = 0xff; /* duration 0-254 (255 = nonstop) */
	report->field[0]->value[5] = 0x00; /* padding */
	report->field[0]->value[6] = 0x00; /* padding */
	report->field[0]->value[7] = 0x00; /* padding */
	hid_hw_request(hid, report, HID_REQ_SET_REPORT);

	return 0;
}


static int shanwan_init(struct hid_device *hid)
{
	struct shanwan_device *shanwan;
	struct hid_report *report;
	struct hid_input *hidinput;
	struct list_head *report_list =&hid->report_enum[HID_OUTPUT_REPORT].report_list;
	struct input_dev *dev;
	int error;

	if (list_empty(&hid->inputs)) {
		hid_err(hid, "no inputs found\n");
		return -ENODEV;
	}
	hidinput = list_first_entry(&hid->inputs, struct hid_input, list);
	dev = hidinput->input;

	if (list_empty(report_list)) {
		hid_err(hid, "no output reports found\n");
		return -ENODEV;
	}

	report = list_first_entry(report_list, struct hid_report, list);

	shanwan = kzalloc(sizeof(struct shanwan_device), GFP_KERNEL);
	if (!shanwan)
		return -ENOMEM;

	set_bit(FF_RUMBLE, dev->ffbit);

	error = input_ff_create_memless(dev, shanwan, shanwan_play);
	if (error)
		goto err_free_mem;

	shanwan->report = report;

	return 0;

err_free_mem:
	kfree(shanwan);
	return error;
}


static int shanwan_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int error;

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

	error = shanwan_init(hdev);
	if (error) {
		hid_warn(hdev,
			 "Failed to enable force feedback support, error: %d\n",
			 error);
	}

	error = hid_hw_open(hdev);
	if (error) {
		dev_err(&hdev->dev, "hw open failed\n");
		hid_hw_stop(hdev);
		return error;
	}

	return 0;
}

static void shanwan_remove(struct hid_device *hdev)
{
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id shanwan_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_SHANWAN, USB_DEVICE_ID_SHANWAN_USB_WIRELESSGAMEPAD), },
	{ }
};

static __u8 *shanwan_report_fixup(struct hid_device *hid, __u8 *rdesc,
	unsigned int *rsize)
{
	if (*rsize == PID0575_RDESC_ORIG_SIZE) {
		rdesc = pid0575_rdesc_fixed;
		*rsize = sizeof(pid0575_rdesc_fixed);
	} else
		hid_warn(hid, "unexpected rdesc, please submit for review\n");
	return rdesc;
}


MODULE_DEVICE_TABLE(hid, shanwan_devices);

static struct hid_driver shanwan_driver = {
	.name			= "shanwan",
	.id_table		= shanwan_devices,
	.probe			= shanwan_probe,
	.report_fixup 	= shanwan_report_fixup,
	.remove			= shanwan_remove,
};

module_hid_driver(shanwan_driver);

MODULE_AUTHOR("Huseyin BIYIK");
MODULE_DESCRIPTION("Force feedback support for Shanwan USB WirelessGamepad");
MODULE_LICENSE("GPL");
