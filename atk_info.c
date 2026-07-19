// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * atk_info - HID driver for ATK / Compx mice. Currently exposes the battery to
 * the kernel power_supply subsystem (and therefore to UPower / GNOME Settings).
 *
 * These mice/dongles present several HID interfaces on one USB device (e.g. a
 * plain mouse, a keyboard, and a composite vendor interface). The battery
 * command channel lives on the vendor interface as output/input report id 0x08
 * (usage page 0xFF02), alongside the real 8k mouse report on the same interface.
 *
 * Because our id_table matches every interface of the device, we must bind and
 * drive them all like hid-generic would (HID_CONNECT_DEFAULT keeps mouse and
 * keyboard input working); we only additionally register a power_supply on the
 * interface that actually carries the 0x08 battery channel. Rejecting the other
 * interfaces with -ENODEV would leave them unbound and kill input.
 *
 * Protocol (reverse-engineered by the projects referenced in the README):
 *
 *   Request  (output report, 17 bytes = 1 report-id + 16 payload):
 *     [0]      report id        0x08
 *     [1]      opcode           0x04  (query battery)
 *     [2..15]  body             0x00
 *     [16]     checksum         so that sum(payload[0..15]) == 0x4D
 *
 *   Reply    (input report, >= 10 bytes, report id 0x08):
 *     [6]      battery percent  0..100
 *     [7]      charging flag    0x01 = charging (cable), 0x00 = on battery
 *     [8..9]   voltage in mV    big-endian u16
 *
 */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define USB_VENDOR_ID_COMPX 0x373b
#define USB_DEVICE_ID_ATK_A9_MINI_WIRELESS 0x1278 /* 8k dongle-L */
#define USB_DEVICE_ID_ATK_A9_MINI_WIRED 0x128e /* ATK A9 Mini+ */
/* Same 0x08 command set, taken from noosxe/atk-tool (untested here). */
#define USB_DEVICE_ID_ATK_A9_PLUS 0x1115
#define USB_DEVICE_ID_ATK_NEARLINK_DONGLE 0x10c9

#define ATK_REPORT_ID 0x08
#define ATK_CMD_QUERY_BATTERY 0x04
#define ATK_CHECKSUM_TARGET 0x4d
#define ATK_PAYLOAD_SIZE 16
#define ATK_PACKET_SIZE (1 + ATK_PAYLOAD_SIZE) /* report id + payload */
#define ATK_RESP_SIZE 64
#define ATK_RESP_MIN 10

#define ATK_REPLY_TIMEOUT_MS 300
#define ATK_POLL_MIN_MS 1000
#define ATK_FAIL_LIMIT 3

static unsigned int poll_interval_ms = 120 * 1000;
module_param(poll_interval_ms, uint, 0644);
MODULE_PARM_DESC(poll_interval_ms,
		 "Battery poll interval in ms (default 120000, min 1000)");

/*
 * Read-only at runtime (0444): set it at load time (modprobe atk_info
 * model_name=...). A writable charp would let a concurrent sysfs write free the
 * string while atk_battery_get_property() hands the pointer to userspace.
 */
static char *model_name;
module_param(model_name, charp, 0444);
MODULE_PARM_DESC(model_name, "Override the battery model name shown to UPower");

/* Monotonic id for power_supply names; never reused, as in hid-logitech-hidpp,
 * so an unbinding device can't race a probing one for the same name.
 */
static atomic_t atk_battery_no = ATOMIC_INIT(0);

struct atk_battery {
	struct hid_device *hdev;
	struct power_supply *psy;
	struct power_supply_desc desc;
	struct delayed_work work;
	struct completion reply;
	const char *model;

	spinlock_t lock; /* protects everything below */
	u8 resp[ATK_RESP_SIZE];
	int resp_len;
	u8 capacity;
	u16 voltage_mv;
	bool charging;
	bool present; /* mouse currently responding to queries */
	unsigned int fails; /* consecutive failed polls, for debounce */
};

/* True only for the interface that carries the battery command channel, i.e. an
 * output report with id 0x08. The mouse/keyboard interfaces don't have it, so we
 * bind them as plain hid devices without a power_supply.
 */
static bool atk_has_battery_report(struct hid_device *hdev)
{
	struct hid_report_enum *out = &hdev->report_enum[HID_OUTPUT_REPORT];

	return out->report_id_hash[ATK_REPORT_ID];
}

static void atk_build_query(u8 *buf)
{
	u8 sum = 0;
	int i;

	memset(buf, 0, ATK_PACKET_SIZE);
	buf[0] = ATK_REPORT_ID;
	buf[1] = ATK_CMD_QUERY_BATTERY;

	/* checksum lives in the last payload byte (buf[16]); the 16 payload
	 * bytes buf[1..16] must sum (mod 256) to ATK_CHECKSUM_TARGET.
	 */
	for (i = 1; i < ATK_PACKET_SIZE - 1; i++)
		sum += buf[i];
	buf[ATK_PACKET_SIZE - 1] = ATK_CHECKSUM_TARGET - sum;
}

static int atk_send_query(struct hid_device *hdev, u8 *buf)
{
	int ret;

	/* Fall back to a SET_REPORT control transfer when the transport has no
	 * output-report path, same as hidinput_led_worker().
	 */
	ret = hid_hw_output_report(hdev, buf, ATK_PACKET_SIZE);
	if (ret == -ENOSYS)
		ret = hid_hw_raw_request(hdev, buf[0], buf, ATK_PACKET_SIZE,
					 HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	return ret;
}

static void atk_battery_poll(struct work_struct *work)
{
	struct atk_battery *atk =
		container_of(to_delayed_work(work), struct atk_battery, work);
	unsigned long flags;
	bool changed = false;
	u8 *buf;
	int ret;

	buf = kmalloc(ATK_PACKET_SIZE, GFP_KERNEL);
	if (!buf)
		goto reschedule;

	atk_build_query(buf);

	reinit_completion(&atk->reply);
	ret = atk_send_query(atk->hdev, buf);
	if (ret < 0) {
		hid_dbg(atk->hdev, "battery query send failed: %d\n", ret);
	} else {
		unsigned long t = msecs_to_jiffies(ATK_REPLY_TIMEOUT_MS);

		ret = wait_for_completion_timeout(&atk->reply, t);
		if (!ret)
			hid_dbg(atk->hdev, "battery query timed out\n");
	}

	spin_lock_irqsave(&atk->lock, flags);
	if (ret > 0 && atk->resp_len >= ATK_RESP_MIN) {
		u8 capacity = min_t(u8, atk->resp[6], 100);
		bool charging = atk->resp[7] == 0x01;
		u16 voltage_mv = (atk->resp[8] << 8) | atk->resp[9];

		changed = !atk->present || atk->capacity != capacity ||
			  atk->charging != charging ||
			  atk->voltage_mv != voltage_mv;
		atk->capacity = capacity;
		atk->charging = charging;
		atk->voltage_mv = voltage_mv;
		atk->present = true;
		atk->fails = 0;
	} else {
		/* Debounce transient wireless misses before reporting the mouse
		 * as gone (asleep / powered off / out of range).
		 */
		if (atk->fails < ATK_FAIL_LIMIT)
			atk->fails++;
		if (atk->fails >= ATK_FAIL_LIMIT && atk->present) {
			atk->present = false;
			changed = true;
		}
	}
	spin_unlock_irqrestore(&atk->lock, flags);

	if (changed)
		power_supply_changed(atk->psy);
	kfree(buf);
reschedule:
	/* Freezable wq so polls pause across system suspend instead of racing
	 * the suspending USB bus and logging spurious failures.
	 */
	queue_delayed_work(system_freezable_wq, &atk->work,
			   msecs_to_jiffies(max_t(unsigned int,
						  poll_interval_ms,
						  ATK_POLL_MIN_MS)));
}

static int atk_info_raw_event(struct hid_device *hdev,
			      struct hid_report *report, u8 *data, int size)
{
	struct atk_battery *atk = hid_get_drvdata(hdev);
	unsigned long flags;

	/* atk is NULL on the plain mouse/keyboard interfaces; let the HID core
	 * process their reports as usual.
	 */
	if (!atk || size < ATK_RESP_MIN || data[0] != ATK_REPORT_ID)
		return 0;

	spin_lock_irqsave(&atk->lock, flags);
	atk->resp_len = min(size, ATK_RESP_SIZE);
	memcpy(atk->resp, data, atk->resp_len);
	spin_unlock_irqrestore(&atk->lock, flags);

	complete(&atk->reply);
	return 0;
}

/* clang-format off */
static const enum power_supply_property atk_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

/* clang-format on */

static int atk_battery_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct atk_battery *atk = power_supply_get_drvdata(psy);
	unsigned long flags;
	int ret = 0;

	/* MODEL_NAME reads a stable pointer; no need for the data lock. */
	if (psp == POWER_SUPPLY_PROP_MODEL_NAME) {
		if (model_name && model_name[0])
			val->strval = model_name;
		else if (atk->model)
			val->strval = atk->model;
		else
			val->strval = atk->hdev->name;
		return 0;
	}

	spin_lock_irqsave(&atk->lock, flags);
	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = atk->present;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (!atk->present)
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		else if (atk->charging)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (atk->capacity >= 100)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (!atk->present)
			ret = -ENODATA;
		else
			val->intval = atk->capacity;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (!atk->present || !atk->voltage_mv)
			ret = -ENODATA;
		else
			val->intval = atk->voltage_mv * 1000; /* µV */
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	spin_unlock_irqrestore(&atk->lock, flags);

	return ret;
}

static int atk_battery_register(struct hid_device *hdev,
				const struct hid_device_id *id)
{
	struct power_supply_config psy_cfg = {};
	struct atk_battery *atk;
	int ret;

	atk = devm_kzalloc(&hdev->dev, sizeof(*atk), GFP_KERNEL);
	if (!atk)
		return -ENOMEM;

	atk->hdev = hdev;
	atk->model = (const char *)id->driver_data;
	spin_lock_init(&atk->lock);
	init_completion(&atk->reply);
	INIT_DELAYED_WORK(&atk->work, atk_battery_poll);

	/* Open the transport so the interrupt-IN URB runs and replies reach
	 * atk_info_raw_event().
	 */
	ret = hid_hw_open(hdev);
	if (ret)
		return ret;

	atk->desc.name = devm_kasprintf(&hdev->dev, GFP_KERNEL,
					"atk-battery-%d",
					atomic_inc_return(&atk_battery_no) - 1);
	if (!atk->desc.name) {
		ret = -ENOMEM;
		goto err_close;
	}
	atk->desc.type = POWER_SUPPLY_TYPE_BATTERY;
	atk->desc.properties = atk_battery_props;
	atk->desc.num_properties = ARRAY_SIZE(atk_battery_props);
	atk->desc.get_property = atk_battery_get_property;

	psy_cfg.drv_data = atk;

	atk->psy = devm_power_supply_register(&hdev->dev, &atk->desc, &psy_cfg);
	if (IS_ERR(atk->psy)) {
		ret = PTR_ERR(atk->psy);
		goto err_close;
	}

	/* Publish drvdata only once fully constructed, so a failed setup never
	 * leaves a stale pointer for atk_info_remove() to act on.
	 */
	hid_set_drvdata(hdev, atk);
	queue_delayed_work(system_freezable_wq, &atk->work, 0);
	hid_info(hdev, "ATK battery monitor registered as %s\n",
		 atk->desc.name);
	return 0;

err_close:
	hid_hw_close(hdev);
	return ret;
}

static int atk_info_probe(struct hid_device *hdev,
			  const struct hid_device_id *id)
{
	int ret;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	/* Drive every interface like hid-generic so mouse/keyboard input keeps
	 * working; only the vendor interface additionally gets a battery.
	 */
	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	if (!atk_has_battery_report(hdev))
		return 0;

	ret = atk_battery_register(hdev, id);
	if (ret) {
		/* Battery setup failed, but keep the interface bound so its
		 * input (this device carries the mouse here too) still works.
		 */
		hid_warn(hdev, "battery registration failed (%d), input only\n",
			 ret);
	}
	return 0;
}

static void atk_info_remove(struct hid_device *hdev)
{
	struct atk_battery *atk = hid_get_drvdata(hdev);

	if (atk) {
		cancel_delayed_work_sync(&atk->work);
		hid_hw_close(hdev);
	}
	hid_hw_stop(hdev);
}

/* clang-format off */
static const struct hid_device_id atk_info_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_COMPX,
			 USB_DEVICE_ID_ATK_A9_MINI_WIRELESS),
	  .driver_data = (kernel_ulong_t)"ATK A9 Mini+ (wireless)" },
	{ HID_USB_DEVICE(USB_VENDOR_ID_COMPX, USB_DEVICE_ID_ATK_A9_MINI_WIRED),
	  .driver_data = (kernel_ulong_t)"ATK A9 Mini+ (wired)" },
	{ HID_USB_DEVICE(USB_VENDOR_ID_COMPX, USB_DEVICE_ID_ATK_A9_PLUS),
	  .driver_data = (kernel_ulong_t)"ATK A9 Plus" },
	{ HID_USB_DEVICE(USB_VENDOR_ID_COMPX,
			 USB_DEVICE_ID_ATK_NEARLINK_DONGLE),
	  .driver_data = (kernel_ulong_t)"ATK Nearlink Mouse Dongle" },
	{}
};

/* clang-format on */
MODULE_DEVICE_TABLE(hid, atk_info_devices);

static struct hid_driver atk_info_driver = {
	.name = "atk-info",
	.id_table = atk_info_devices,
	.probe = atk_info_probe,
	.remove = atk_info_remove,
	.raw_event = atk_info_raw_event,
};
module_hid_driver(atk_info_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("nat");
MODULE_DESCRIPTION("ATK / Compx mouse driver; battery via power_supply/UPower");
