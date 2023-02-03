/* a96t3x6.c -- Linux driver for A96T3X6 chip as grip sensor
 *
 * Copyright (C) 2017 Samsung Electronics Co.Ltd
 * Author: YunJae Hwang <yjz.hwang@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/wakelock.h>
#include <asm/unaligned.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>

#if defined(CONFIG_CCIC_NOTIFIER) && defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
#include <linux/usb/typec/manager/usb_typec_manager_notifier.h>
#include <linux/usb/typec/common/pdic_notifier.h>
#elif defined(CONFIG_VBUS_NOTIFIER)
#include <linux/vbus_notifier.h>
#endif

#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif

#include "a96t3x6.h"

struct a96t3x6_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct input_dev *noti_input_dev;
	struct device *dev;
	struct mutex lock;
	struct delayed_work debug_work;
	struct delayed_work firmware_work;

	struct delayed_work reset_work;
#if defined(CONFIG_CCIC_NOTIFIER) || defined(CONFIG_PDIC_NOTIFIER) \
		|| defined(CONFIG_MUIC_NOTIFIER) || defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	struct work_struct cmdon_work;
	struct work_struct cmdoff_work;
#endif

	atomic_t enable;

	const struct firmware *firm_data_bin;
	const u8 *firm_data_ums;
	char phys[32];
	long firm_size;
	int irq;
	int noti_enable;
	struct wake_lock grip_wake_lock;
	u16 grip_p_thd;
	u16 grip_r_thd;
	u16 grip_n_thd;
	u16 grip_baseline;
	u16 grip_raw;
	u16 grip_raw_d;
	u16 grip_event;
	u16 diff;
	u16 diff_d;
	bool sar_mode;
	bool sar_enable_off;
	bool earjack;
	u8 earjack_noise;
#ifdef CONFIG_SEC_FACTORY
	int irq_count;
	int abnormal_mode;
	s16 max_diff;
	s16 max_normal_diff;
#endif
	int irq_en_cnt;
	u8 fw_update_state;
	u8 fw_ver;
	u8 md_ver;
	u8 fw_ver_bin;
	u8 md_ver_bin;
	u8 checksum_h;
	u8 checksum_h_bin;
	u8 checksum_l;
	u8 checksum_l_bin;

	bool skip_event;
	bool resume_called;

	bool first_working;

	s16 is_unknown_mode;
	s16 motion;

	int ldo_en;			/* ldo_en pin gpio */
	int grip_int;			/* irq pin gpio */
	const char *dvdd_vreg_name;		/* regulator name */
	struct regulator *dvdd_vreg;	/* regulator */
	int (*power)(void *, bool on);	/* power onoff function ptr */
	const char *chipid;
	const char *fw_path;
	bool bringup;
	bool probe_done;

	int firmup_cmd;
	int debug_count;
	int firmware_count;

#if defined(CONFIG_CCIC_NOTIFIER)
	struct notifier_block ccic_nb;
#elif defined(CONFIG_VBUS_NOTIFIER)
	struct notifier_block vbus_nb;
#elif defined(CONFIG_MUIC_NOTIFIER)
	struct notifier_block muic_nb;
#endif
};

static void a96t3x6_reset(struct a96t3x6_data *data);
static void a96t3x6_check_first_status(struct a96t3x6_data *data, int enable);
#ifdef CONFIG_SENSORS_FW_VENDOR
static int a96t3x6_fw_check(struct a96t3x6_data *data);
static void a96t3x6_set_firmware_work(struct a96t3x6_data *data, u8 enable,
		unsigned int time_ms);
#endif

static int a96t3x6_i2c_read(struct i2c_client *client,
	u8 reg, u8 *val, unsigned int len)
{
	struct a96t3x6_data *data = i2c_get_clientdata(client);
	struct i2c_msg msg;
	int ret;
	int retry = 3;

	mutex_lock(&data->lock);
	msg.addr = client->addr;
	msg.flags = I2C_M_WR;
	msg.len = 1;
	msg.buf = &reg;
	while (retry--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0)
			break;

		GRIP_INFO("fail(address set)(%d)(%d)\n", retry, ret);
		usleep_range(10000, 11000);
	}
	if (ret < 0) {
		mutex_unlock(&data->lock);
		return ret;
	}
	retry = 3;
	msg.flags = 1;/*I2C_M_RD*/
	msg.len = len;
	msg.buf = val;
	while (retry--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0) {
			mutex_unlock(&data->lock);
			return 0;
		}
		GRIP_INFO("fail(data read)(%d)(%d)\n", retry, ret);
		usleep_range(10000, 11000);
	}
	mutex_unlock(&data->lock);
	return ret;
}

static int a96t3x6_i2c_read_data(struct i2c_client *client, u8 *val,
	unsigned int len)
{
	struct a96t3x6_data *data = i2c_get_clientdata(client);
	struct i2c_msg msg;
	int ret;
	int retry = 3;

	mutex_lock(&data->lock);
	msg.addr = client->addr;
	msg.flags = 1;/*I2C_M_RD*/
	msg.len = len;
	msg.buf = val;
	while (retry--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0) {
			mutex_unlock(&data->lock);
			return 0;
		}
		GRIP_INFO("fail(data read)(%d)\n", retry);
		usleep_range(10000, 11000);
	}
	mutex_unlock(&data->lock);
	return ret;
}

static int a96t3x6_i2c_write(struct i2c_client *client, u8 reg, u8 *val)
{
	struct a96t3x6_data *data = i2c_get_clientdata(client);
	struct i2c_msg msg[1];
	unsigned char buf[2];
	int ret;
	int retry = 3;

	mutex_lock(&data->lock);
	buf[0] = reg;
	buf[1] = *val;
	msg->addr = client->addr;
	msg->flags = I2C_M_WR;
	msg->len = 2;
	msg->buf = buf;

	while (retry--) {
		ret = i2c_transfer(client->adapter, msg, 1);
		if (ret >= 0) {
			mutex_unlock(&data->lock);
			return 0;
		}
		GRIP_INFO("fail(%d)\n", retry);
		usleep_range(10000, 11000);
	}
	mutex_unlock(&data->lock);
	return ret;
}

/*
 * @enable: turn it on or off.
 * @force: if caller is grip_sensing_change(), it's true. others, it's false.
 * 
 * This function was designed to prevent noise issue from ic for specific models.
 * If earjack_noise is true, it handled enable control for it.
 */
static void a96t3x6_set_enable(struct a96t3x6_data *data, int enable)
{
	u8 cmd;
	int ret;
	int pre_enable = atomic_read(&data->enable);

	GRIP_INFO("enable = %d pre_enable = %d\n", enable, pre_enable);

	if (pre_enable == enable) {
		GRIP_INFO("skip\n");
		return;
	}
	
	if (enable) {
		cmd = CMD_ON;

		ret = a96t3x6_i2c_write(data->client, REG_SAR_ENABLE, &cmd);
		if (ret < 0) {
			GRIP_ERR("failed to enable grip irq\n");
			atomic_set(&data->enable, 0);
			return;
		}
		a96t3x6_check_first_status(data, enable);
		
		enable_irq(data->irq);
		enable_irq_wake(data->irq);

		data->irq_en_cnt++;
		atomic_set(&data->enable, 1);

	} else {
		cmd = CMD_OFF;

		disable_irq_wake(data->irq);
		disable_irq(data->irq);

		ret = a96t3x6_i2c_write(data->client, REG_SAR_ENABLE, &cmd);
		if (ret < 0)
			GRIP_ERR("failed to disable grip irq\n");

		atomic_set(&data->enable, 0);
	}
}

static void a96t3x6_sar_only_mode(struct a96t3x6_data *data, int on)
{
#ifdef CONFIG_SENSORS_A96T3X6_BLOCK_SAR_ONLY
	GRIP_INFO("No action with sar only mode");
#else
	int ret;
	u8 cmd;
	u8 r_buf;

	if (data->sar_mode == on) {
		GRIP_INFO("skip already %s\n", (on == 1) ? "sar only mode" : "normal mode");
		return;
	}

	if (on == 1)
		cmd = CMD_ON;
	else
		cmd = CMD_OFF;

	GRIP_INFO("%s, cmd=%x\n", (on == 1) ? "sar only mode" : "normal mode", cmd);

	ret = a96t3x6_i2c_write(data->client, REG_SAR_MODE, &cmd);
	if (ret < 0)
		GRIP_INFO("i2c read fail(%d)", ret);

	usleep_range(40000, 40000);

	ret = a96t3x6_i2c_read(data->client, REG_SAR_MODE, &r_buf, 1);
	if (ret < 0)
		GRIP_INFO("i2c read fail(%d)\n", ret);
	else {
		GRIP_INFO("read reg = %x\n", r_buf);

		if (r_buf == CMD_ON)
			data->sar_mode = 1;
		else
			data->sar_mode = 0;
	}
#endif
}

static void grip_always_active(struct a96t3x6_data *data, int on)
{
	int ret, retry = 3;
	u8 cmd, r_buf;

	GRIP_INFO("Grip always active mode %d\n", on);

	if (on == 1)
		cmd = CMD_ON;
	else
		cmd = CMD_OFF;

	while (retry--) {
		ret = a96t3x6_i2c_write(data->client, REG_GRIP_ALWAYS_ACTIVE, &cmd);
		if (ret < 0) {
			GRIP_ERR("failed to change grip always active mode\n");
			continue;
		}

		msleep(20);

		ret = a96t3x6_i2c_read(data->client, REG_GRIP_ALWAYS_ACTIVE, &r_buf, 1);
		if (ret < 0) {
			GRIP_ERR("i2c read fail(%d)\n", ret);
			continue;
		}

		if ((cmd == CMD_ON && r_buf == GRIP_ALWAYS_ACTIVE_READY) ||
			(cmd == CMD_OFF && r_buf == CMD_OFF))
			break;
		else
			GRIP_ERR("Wrong value 0x%x, retry again %d\n", r_buf, retry);
	}

	if (retry < 0)
		GRIP_INFO("failed to change grip always active mode\n");
	else
		GRIP_INFO("Grip check mode: cmd 0x%x, return value 0x%x\n", cmd, r_buf);
}

static void a96t3x6_sar_sensing(struct a96t3x6_data *data, int on)
{
	u8 cmd;
	int ret;

	GRIP_INFO("%s", (on) ? "on" : "off");

	if (on)
		cmd = CMD_ON;
	else
		cmd = CMD_OFF;

	ret = a96t3x6_i2c_write(data->client, REG_SAR_SENSING, &cmd);
	if (ret < 0)
		GRIP_INFO("failed to %s grip sensing\n", (on) ? "enable" : "disable");
}

static void a96t3x6_reset_for_bootmode(struct a96t3x6_data *data)
{
	GRIP_INFO("\n");

	data->power(data, false);
	usleep_range(50000, 50000);
	data->power(data, true);
}

static void a96t3x6_reset(struct a96t3x6_data *data)
{
	int enable = atomic_read(&data->enable);

	if (!enable)
		return;

	GRIP_INFO("start\n");
	disable_irq_nosync(data->irq);

	a96t3x6_reset_for_bootmode(data);
	usleep_range(RESET_DELAY, RESET_DELAY);

	if (enable)
		a96t3x6_set_enable(data, 1);

	GRIP_INFO("done\n");
}

static void a96t3x6_diff_getdata(struct a96t3x6_data *data, int log)
{
	int ret;
	int retry = 3;
	u8 r_buf[4] = {0,};

	while (retry--) {
		ret = a96t3x6_i2c_read(data->client, REG_SAR_DIFFDATA, r_buf, 4);
		if (ret == 0)
			break;
		GRIP_ERR("read failed(%d)\n", retry);
		usleep_range(10000, 10000);
	}

	data->diff = (r_buf[0] << 8) | r_buf[1];
	data->diff_d = (r_buf[2] << 8) | r_buf[3];
	if (log)
		GRIP_INFO("%u\n", data->diff);
}

static void a96t3x6_enter_unknown_mode(struct a96t3x6_data *data, int type)
{
	if (data->noti_enable && !data->skip_event) {
		data->motion = 0;
		data->first_working = false;
		if (data->is_unknown_mode == UNKNOWN_OFF) {
			data->is_unknown_mode = UNKNOWN_ON;
			if (!data->skip_event) {
				input_report_rel(data->input_dev, REL_X, data->is_unknown_mode);
				input_sync(data->input_dev);
			}
			GRIP_INFO("UNKNOWN Re-enter\n");
		} else {
			GRIP_INFO("already UNKNOWN\n");
		}

		input_report_rel(data->noti_input_dev, REL_X, type);
		input_sync(data->noti_input_dev);
	}
}


static void a96t3x6_check_first_status(struct a96t3x6_data *data, int enable)
{
	u8 r_buf[2];
	u16 grip_thd;

	if (data->skip_event == true) {
		GRIP_INFO("skip event..\n");
		return;
	}

	a96t3x6_i2c_read(data->client, REG_SAR_THRESHOLD, r_buf, 4);
	grip_thd = (r_buf[0] << 8) | r_buf[1];

	a96t3x6_diff_getdata(data, 1);

	if (grip_thd < data->diff) {
		input_report_rel(data->input_dev, REL_MISC, 1);
	} else {
		input_report_rel(data->input_dev, REL_MISC, 2);
	}
	input_report_rel(data->input_dev, REL_X, data->is_unknown_mode);

	input_sync(data->input_dev);
}

static void a96t3x6_check_diff_and_cap(struct a96t3x6_data *data)
{
	u8 r_buf[2] = {0,0};
	u8 cmd = 0x20;
	int ret;
	int value = 0;

	ret = a96t3x6_i2c_write(data->client, REG_SAR_TOTALCAP, &cmd);
	if (ret < 0)
		GRIP_ERR("write fail(%d)\n", ret);

	usleep_range(20, 20);

	ret = a96t3x6_i2c_read(data->client, REG_SAR_TOTALCAP_READ, r_buf, 2);
	if (ret < 0)
		GRIP_ERR("fail(%d)\n", ret);

	value = (r_buf[0] << 8) | r_buf[1];
	GRIP_INFO("Cap Read %d\n", value);

	a96t3x6_diff_getdata(data, 1);
}

static void a96t3x6_grip_sw_reset(struct a96t3x6_data *data)
{
	int ret, retry = 3;
	u8 cmd = CMD_SW_RESET;

	GRIP_INFO("\n");

	while (retry--) {
		a96t3x6_check_diff_and_cap(data);
		usleep_range(10000, 10000);
	}

	ret = a96t3x6_i2c_write(data->client, REG_SW_RESET, &cmd);
	if (ret < 0)
		GRIP_ERR("fail(%d)\n", ret);
	else
		usleep_range(35000, 35000);
}

static int a96t3x6_get_hallic_state(char *file_path)
{
	char hall_buf[6];
	int ret = -ENODEV;
	int hall_state = -1;
	mm_segment_t old_fs;
	struct file *filep;

	memset(hall_buf, 0, sizeof(hall_buf));
	old_fs = get_fs();
	set_fs(KERNEL_DS);

	filep = filp_open(file_path, O_RDONLY, 0666);
	if (IS_ERR(filep)) {
		set_fs(old_fs);
		return hall_state;
	}

	ret = filep->f_op->read(filep, hall_buf,
		sizeof(hall_buf) - 1, &filep->f_pos);
	if (ret != sizeof(hall_buf) - 1)
		goto exit;

	if (strcmp(hall_buf, "CLOSE") == 0)
		hall_state = HALL_CLOSE_STATE;

exit:
	filp_close(filep, current->files);
	set_fs(old_fs);

	return hall_state;
}

static void a96t3x6_reset_work_func(struct work_struct *work)
{
	struct a96t3x6_data *data = container_of((struct delayed_work *)work,
		struct a96t3x6_data, reset_work);

	a96t3x6_grip_sw_reset(data);
}

#ifdef CONFIG_SENSORS_FW_VENDOR
static void a96t3x6_firmware_work_func(struct work_struct *work)
{
	struct a96t3x6_data *data = container_of((struct delayed_work *)work,
		struct a96t3x6_data, firmware_work);

	int ret;

	GRIP_INFO("called\n");

	ret = a96t3x6_fw_check(data);
	if (ret) {
		if (data->firmware_count++ < FIRMWARE_VENDOR_CALL_CNT) {
			GRIP_ERR("failed to load firmware (%d)\n",
				data->firmware_count);
			schedule_delayed_work(&data->firmware_work,
					msecs_to_jiffies(1000));
			return;
		}
		GRIP_ERR("final retry failed\n");
	} else {
		GRIP_INFO("fw check success\n");
	}
}
#endif

static void a96t3x6_check_first_working(struct a96t3x6_data *data, int channel_num)
{
	if (data->noti_enable && data->motion) {
		if (channel_num == 1) {
			if (data->grip_p_thd < data->diff) {
				if (!data->first_working) {
					data->first_working = true;
					GRIP_INFO("first working detected %d\n", data->diff);
				}
			} else {
				if (data->first_working &&
					(data->is_unknown_mode == UNKNOWN_ON)) {
					data->is_unknown_mode = UNKNOWN_OFF;
					GRIP_INFO("Release detected %d, unknown mode off\n", data->diff);
				}
			}
		}
	}
}

static void a96t3x6_debug_work_func(struct work_struct *work)
{
	struct a96t3x6_data *data = container_of((struct delayed_work *)work,
		struct a96t3x6_data, debug_work);

	static int hall_prev_state, cert_hall_prev_state;
	int hall_state, cert_hall_state;

	int enable = atomic_read(&data->enable);

	if (data->resume_called == true) {
		data->resume_called = false;
		a96t3x6_sar_only_mode(data, 0);
		schedule_delayed_work(&data->debug_work, msecs_to_jiffies(1000));
		return;
	}
	hall_state = a96t3x6_get_hallic_state(HALL_PATH);
	cert_hall_state = a96t3x6_get_hallic_state(HALLIC_CERT_PATH);

	if ((hall_state == HALL_CLOSE_STATE && hall_prev_state != hall_state)
		||(cert_hall_state == HALL_CLOSE_STATE && cert_hall_prev_state != cert_hall_state)) {
		GRIP_INFO("%s - hall is closed %d %d\n", __func__, hall_state, cert_hall_state);
		schedule_delayed_work(&data->reset_work, msecs_to_jiffies(1));
		a96t3x6_enter_unknown_mode(data, TYPE_HALL);
	} else if ((hall_state == HALL_OPEN_STATE && hall_prev_state != hall_state)
		|| (cert_hall_state == HALL_OPEN_STATE && cert_hall_prev_state != cert_hall_state)) {
		GRIP_INFO("%s - hall is open %d %d\n", __func__, hall_state, cert_hall_state);
		a96t3x6_enter_unknown_mode(data, TYPE_HALL);
	}

	hall_prev_state = hall_state;
	cert_hall_prev_state = cert_hall_state;

	if (enable) {
#ifdef CONFIG_SEC_FACTORY
		if (data->abnormal_mode) {
			a96t3x6_diff_getdata(data);
			if (data->max_normal_diff < data->diff)
				data->max_normal_diff = data->diff;
		} else {
#endif
		if (data->debug_count >= GRIP_LOG_TIME) {
			a96t3x6_diff_getdata(data, 1);
			if (data->is_unknown_mode == UNKNOWN_ON && data->motion)
				a96t3x6_check_first_working(data, 1);
			data->debug_count = 0;
		} else {
			if (data->is_unknown_mode == UNKNOWN_ON && data->motion) {
				a96t3x6_diff_getdata(data, 0);
				a96t3x6_check_first_working(data, 1);
			}
			data->debug_count++;
		}
#ifdef CONFIG_SEC_FACTORY
		}
#endif
	}
	schedule_delayed_work(&data->debug_work, msecs_to_jiffies(2000));
}

static void a96t3x6_set_debug_work(struct a96t3x6_data *data, u8 enable,
	unsigned int time_ms)
{
	GRIP_INFO("enable = %d\n", enable);
	
	if (enable == 1) {
		data->debug_count = 0;
		schedule_delayed_work(&data->debug_work,
			msecs_to_jiffies(time_ms));
	} else {
		cancel_delayed_work_sync(&data->debug_work);
	}
}
#ifdef CONFIG_SENSORS_FW_VENDOR
static void a96t3x6_set_firmware_work(struct a96t3x6_data *data, u8 enable,
	unsigned int time_ms)
{
	GRIP_INFO("%s\n", __func__, enable ? "enabled": "disabled");
	
	if (enable == 1) {
		data->firmware_count = 0;
		schedule_delayed_work(&data->firmware_work,
			msecs_to_jiffies(time_ms));
	} else {
		cancel_delayed_work_sync(&data->firmware_work);
	}
}
#endif
static irqreturn_t a96t3x6_interrupt(int irq, void *dev_id)
{
	struct a96t3x6_data *data = dev_id;
	struct i2c_client *client = data->client;
	int ret, retry;
	u8 buf;
	int grip_data;
	u8 grip_press = 0;

	wake_lock(&data->grip_wake_lock);

	ret = a96t3x6_i2c_read(client, REG_BTNSTATUS, &buf, 1);
	if (ret < 0) {
		retry = 3;
		while (retry--) {
			GRIP_INFO("read fail(%d)\n", retry);
			ret = a96t3x6_i2c_read(client, REG_BTNSTATUS, &buf, 1);
			if (ret == 0)
				break;
			usleep_range(10000, 11000);
		}
		if (retry < 0) {
			a96t3x6_reset(data);
			wake_unlock(&data->grip_wake_lock);
			return IRQ_HANDLED;
		}
	}

	GRIP_INFO("buf = 0x%02x\n", buf);

	grip_data = (buf >> 4) & 0x03;
	grip_press = !(grip_data % 2);

	if (grip_data) {
		if (data->skip_event) {
			GRIP_INFO("int was generated, but event skipped\n");
		} else {
			if (grip_press) {
				input_report_rel(data->input_dev, REL_MISC, 1);
				if (data->is_unknown_mode == UNKNOWN_ON && data->motion)
					data->first_working = true;
			} else {
				input_report_rel(data->input_dev, REL_MISC, 2);
				if (data->is_unknown_mode == UNKNOWN_ON && data->motion) {
					if (data->first_working) {
						GRIP_INFO("unknown mode off\n");
						data->is_unknown_mode = UNKNOWN_OFF;
					}
				}
			}

			input_report_rel(data->input_dev, REL_X, data->is_unknown_mode);

			input_sync(data->input_dev);
			data->grip_event = grip_press;
		}
	}
	a96t3x6_diff_getdata(data, 1);
#ifdef CONFIG_SEC_FACTORY
	if (data->abnormal_mode) {
		if (data->grip_event) {
			if (data->max_diff < data->diff)
				data->max_diff = data->diff;
			data->irq_count++;
		}
	}
#endif
	if (grip_data)
		GRIP_INFO("%s %x\n", grip_press ? "grip P" : "grip R", buf);

	wake_unlock(&data->grip_wake_lock);
	return IRQ_HANDLED;
}

static int a96t3x6_get_raw_data(struct a96t3x6_data *data)
{
	int ret;
	u8 r_buf[4] = {0,};

	ret = a96t3x6_i2c_read(data->client, REG_SAR_RAWDATA, r_buf, 4);
	if (ret < 0) {
		GRIP_INFO("fail(%d)\n", ret);
		data->grip_raw = 0;
		data->grip_raw_d = 0;
		return ret;
	}

	data->grip_raw = (r_buf[0] << 8) | r_buf[1];
	data->grip_raw_d = (r_buf[2] << 8) | r_buf[3];

	GRIP_INFO("grip_raw = %d\n", data->grip_raw);

	return ret;
}

static ssize_t grip_sar_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u\n", !data->skip_event);
}

static ssize_t grip_sar_enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int ret, enable;

	ret = sscanf(buf, "%2d", &enable);
	if (ret != 1) {
		GRIP_ERR("cmd read err\n");
		return count;
	}

	if (!(enable >= 0 && enable <= 3)) {
		GRIP_ERR("wrong command(%d)\n", enable);
		return count;
	}

	GRIP_INFO("enable = %d\n", enable);

	/* enable 0:off, 1:on, 2:skip event , 3:cancel skip event */
	if (enable == 2) {
		data->skip_event = true;
		data->motion = 1;
		data->is_unknown_mode = UNKNOWN_OFF;
		data->first_working = false;

		input_report_rel(data->input_dev, REL_MISC, 2);
		input_report_rel(data->input_dev, REL_X, UNKNOWN_OFF);
		input_sync(data->input_dev);
	} else if (enable == 3) {
		data->skip_event = false;
	} else {
		a96t3x6_set_enable(data, enable);
	}

	return count;
}

static ssize_t grip_threshold_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	u8 r_buf[4];
	int ret;

	ret = a96t3x6_i2c_read(data->client, REG_SAR_THRESHOLD, r_buf, 4);
	if (ret < 0) {
		GRIP_ERR("fail(%d)\n", ret);
		data->grip_p_thd = 0;
		data->grip_r_thd = 0;
		return snprintf(buf, PAGE_SIZE, "%u\n", 0);
	}
	data->grip_p_thd = (r_buf[0] << 8) | r_buf[1];
	data->grip_r_thd = (r_buf[2] << 8) | r_buf[3];

	ret = a96t3x6_i2c_read(data->client, REG_SAR_NOISE_THRESHOLD, r_buf, 2);
	if (ret < 0) {
		GRIP_ERR("fail(%d)\n", ret);
		data->grip_n_thd = 0;
		return snprintf(buf, PAGE_SIZE, "%u\n", 0);
	}
	data->grip_n_thd = (r_buf[0] << 8) | r_buf[1];

	return sprintf(buf, "%u,%u,%u\n", data->grip_p_thd, data->grip_r_thd, data->grip_n_thd);
}
static ssize_t grip_total_cap_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	u8 r_buf[2];
	u8 cmd;
	int ret;
	int value;

	cmd = 0x20;
	ret = a96t3x6_i2c_write(data->client, REG_SAR_TOTALCAP, &cmd);
	if (ret < 0)
		GRIP_ERR("write fail(%d)\n", ret);

	usleep_range(10, 20);

	ret = a96t3x6_i2c_read(data->client, REG_SAR_TOTALCAP_READ, r_buf, 2);
	if (ret < 0) {
		GRIP_ERR("fail(%d)\n", ret);
		return snprintf(buf, PAGE_SIZE, "%u\n", 0);
	}
	value = (r_buf[0] << 8) | r_buf[1];

	return snprintf(buf, PAGE_SIZE, "%d\n", value / 100);
}

static ssize_t grip_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int ret;
	int retry = 3;
	u8 r_buf[4] = {0,};

	while (retry--) {
		ret = a96t3x6_i2c_read(data->client, REG_SAR_DIFFDATA, r_buf, 4);
		if (ret == 0)
			break;
		GRIP_ERR("read failed(%d)\n", retry);
		usleep_range(10000, 10000);
	}

	data->diff = (r_buf[0] << 8) | r_buf[1];
	data->diff_d = (r_buf[2] << 8) | r_buf[3];

	return sprintf(buf, "%u,%u\n", data->diff, data->diff_d);
}

static ssize_t grip_baseline_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	u8 r_buf[2];
	int ret;

	ret = a96t3x6_i2c_read(data->client, REG_SAR_BASELINE, r_buf, 2);
	if (ret < 0) {
		GRIP_ERR("fail(%d)\n", ret);
		data->grip_baseline = 0;
		return snprintf(buf, PAGE_SIZE, "%d\n", 0);
	}
	data->grip_baseline = (r_buf[0] << 8) | r_buf[1];

	return snprintf(buf, PAGE_SIZE, "%u\n", data->grip_baseline);
}

static ssize_t grip_raw_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int ret;

	ret = a96t3x6_get_raw_data(data);
	if (ret < 0)
		return sprintf(buf, "%d\n", 0);
	else
		return sprintf(buf, "%u,%u\n", data->grip_raw,
				data->grip_raw_d);
}

static ssize_t grip_ref_cap_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	u8 r_buf[2];
	int ref_cap;
	int ret;

	ret = a96t3x6_i2c_read(data->client, REG_REF_CAP, r_buf, 2);
	if (ret < 0) {
		GRIP_ERR("fail(%d)\n", ret);
		return snprintf(buf, PAGE_SIZE, "%d\n", 0);
	}

	ref_cap = (r_buf[0] << 8) | r_buf[1];
	do_div(ref_cap, 100);

	GRIP_INFO("Ref Cap : %x,%x\n", r_buf[0], r_buf[1]);
	GRIP_INFO("Ref Cap / 100 : %d\n", ref_cap);

	return sprintf(buf, "%d\n", ref_cap);
}

static ssize_t grip_gain_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	u8 r_buf1[3], r_buf2[3];
	int ret;

	ret = a96t3x6_i2c_read(data->client, REG_GAINDATA, r_buf1, 3);
	if (ret < 0) {
		GRIP_ERR("fail(%d)\n", ret);
		return snprintf(buf, PAGE_SIZE, "%d\n", 0);
	}

	GRIP_INFO("Gain : %d,%d,%d\n", (int)r_buf1[0], (int)r_buf1[1], (int)r_buf1[2]);

	ret = a96t3x6_i2c_read(data->client, REG_REF_GAINDATA, r_buf2, 3);
	if (ret < 0) {
		GRIP_ERR("fail(%d)\n", ret);
		return snprintf(buf, PAGE_SIZE, "%d\n", 0);
	}

	GRIP_INFO("Ref Gain : %d,%d,%d\n", (int)r_buf2[0], (int)r_buf2[1], (int)r_buf2[2]);

	return sprintf(buf, "%d,%d,%d,%d,%d,%d\n", (int)r_buf1[0], (int)r_buf1[1], (int)r_buf1[2],
	                                           (int)r_buf2[0], (int)r_buf2[1], (int)r_buf2[2]);
}

static ssize_t grip_check_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	a96t3x6_diff_getdata(data, 1);

	return snprintf(buf, PAGE_SIZE, "%d\n", data->grip_event);
}

static ssize_t grip_sw_reset_ready_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int ret;
	int retry = 10;
	u8 r_buf[1] = {0};

	GRIP_INFO("Wait start\n");

	/* To garuantee grip sensor sw reset delay*/
	msleep(500);

	while (retry--) {
		ret = a96t3x6_i2c_read(data->client, REG_SW_RESET, r_buf, 1);
		if (r_buf[0] == 0x20)
			break;
		if (ret < 0)
			GRIP_ERR("failed(%d)\n", retry);
		msleep(100);
	}

	GRIP_INFO("expect 0x20 read 0x%x\n", r_buf[0]);
	a96t3x6_check_diff_and_cap(data);

	return snprintf(buf, PAGE_SIZE, "1\n");
}

static ssize_t grip_sw_reset(struct device *dev,
	struct device_attribute *attr, const char *buf,
	size_t count)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	u8 cmd;
	int ret;

	ret = kstrtou8(buf, 2, &cmd);
	if (ret) {
		GRIP_ERR("cmd read err\n");
		return count;
	}

	if (!(cmd == 1)) {
		GRIP_ERR("wrong command(%d)\n", cmd);
		return count;
	}

	data->grip_event = 0;

	GRIP_INFO("cmd(%d)\n", cmd);

	a96t3x6_grip_sw_reset(data);

	return count;
}

static ssize_t grip_sensing_change(struct device *dev,
	struct device_attribute *attr, const char *buf,
	size_t count)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int ret, earjack;

	ret = sscanf(buf, "%2d", &earjack);
	if (ret != 1) {
		GRIP_ERR("cmd read err\n");
		return count;
	}

	if (!(earjack == 0 || earjack == 1)) {
		GRIP_ERR("wrong command(%d)\n", earjack);
		return count;
	}

	if (!data->earjack_noise) {
		if (earjack == 1)
			a96t3x6_sar_only_mode(data, 1);
		else
			a96t3x6_sar_only_mode(data, 0);
	} else {
		if (earjack == 1) {
			a96t3x6_set_enable(data, 0);
			a96t3x6_sar_sensing(data, 0);
			data->grip_event = 0;
			input_report_rel(data->input_dev, REL_MISC, 2);

			if (data->skip_event)
				input_report_rel(data->input_dev, REL_X, UNKNOWN_OFF);
			else
				input_report_rel(data->input_dev, REL_X, data->is_unknown_mode);
			input_sync(data->input_dev);
		} else {
			a96t3x6_grip_sw_reset(data);
			a96t3x6_sar_sensing(data, 1);
			a96t3x6_set_enable(data, 1);
		}
	}

	data->earjack = earjack;

	GRIP_INFO("earjack was %s\n", (earjack) ? "inserted" : "removed");

	return count;
}

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
static ssize_t grip_sar_press_threshold_store(struct device *dev,
	struct device_attribute *attr, const char *buf,
	size_t count)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	int ret;
	int threshold;
	u8 cmd[2];

	ret = sscanf(buf, "%11d", &threshold);
	if (ret != 1) {
		GRIP_ERR("failed to read thresold, buf is %s\n", buf);
		return count;
	}

	if (threshold > 0xff) {
		cmd[0] = (threshold >> 8) & 0xff;
		cmd[1] = 0xff & threshold;
	} else if (threshold < 0) {
		cmd[0] = 0x0;
		cmd[1] = 0x0;
	} else {
		cmd[0] = 0x0;
		cmd[1] = (u8)threshold;
	}

	GRIP_INFO("buf : %d, threshold : %d\n", threshold,
			(cmd[0] << 8) | cmd[1]);

	ret = a96t3x6_i2c_write(data->client, REG_SAR_THRESHOLD, &cmd[0]);
	if (ret != 0) {
		GRIP_INFO("failed to write press_threhold data1");
		goto press_threshold_out;
	}
	ret = a96t3x6_i2c_write(data->client, REG_SAR_THRESHOLD + 0x01, &cmd[1]);
	if (ret != 0) {
		GRIP_INFO("failed to write press_threhold data2");
		goto press_threshold_out;
	}
press_threshold_out:
	return count;
}

static ssize_t grip_sar_release_threshold_store(struct device *dev,
	struct device_attribute *attr, const char *buf,
	size_t count)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	int ret;
	int threshold;
	u8 cmd[2];

	ret = sscanf(buf, "%11d", &threshold);
	if (ret != 1) {
		GRIP_ERR("failed to read thresold, buf is %s\n", buf);
		return count;
	}

	if (threshold > 0xff) {
		cmd[0] = (threshold >> 8) & 0xff;
		cmd[1] = 0xff & threshold;
	} else if (threshold < 0) {
		cmd[0] = 0x0;
		cmd[1] = 0x0;
	} else {
		cmd[0] = 0x0;
		cmd[1] = (u8)threshold;
	}

	GRIP_INFO("buf : %d, threshold : %d\n", threshold,
				(cmd[0] << 8) | cmd[1]);

	ret = a96t3x6_i2c_write(data->client, REG_SAR_THRESHOLD + 0x02,
				&cmd[0]);
	GRIP_INFO("ret : %d\n", ret);

	if (ret != 0) {
		GRIP_INFO("failed to write release_threshold_data1");
		goto release_threshold_out;
	}
	ret = a96t3x6_i2c_write(data->client, REG_SAR_THRESHOLD + 0x03,
				&cmd[1]);
	GRIP_INFO("ret : %d\n", ret);
	if (ret != 0) {
		GRIP_INFO("failed to write release_threshold_data2");
		goto release_threshold_out;
	}
release_threshold_out:
	return count;
}

static ssize_t grip_mode_change(struct device *dev,
	struct device_attribute *attr, const char *buf,
	size_t count)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int ret, mode;

	ret = sscanf(buf, "%2d", &mode);
	if (ret != 1) {
		GRIP_ERR("cmd read err\n");
		return count;
	}

	if (!(mode == 0 || mode == 1)) {
		GRIP_ERR("wrong command(%d)\n", mode);
		return count;
	}

	GRIP_INFO("mode(%d)\n", mode);

	a96t3x6_sar_only_mode(data, mode);

	return count;
}
#endif

#ifdef CONFIG_SEC_FACTORY
static ssize_t a96t3x6_irq_count_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int result = 0;
	s16 max_diff_val = 0;

	if (data->irq_count) {
		result = -1;
		max_diff_val = data->max_diff;
	} else {
		max_diff_val = data->max_normal_diff;
	}

	return snprintf(buf, PAGE_SIZE, "%d,%d,%d\n", result,
			data->irq_count, max_diff_val);
}

static ssize_t a96t3x6_irq_count_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	u8 onoff;
	int ret;

	ret = kstrtou8(buf, 10, &onoff);
	if (ret < 0) {
		GRIP_ERR("kstrtou8 failed.(%d)\n", ret);
		return count;
	}

	mutex_lock(&data->lock);
	if (onoff == 0) {
		data->abnormal_mode = 0;
	} else if (onoff == 1) {
		data->abnormal_mode = 1;
		data->irq_count = 0;
		data->max_diff = 0;
		data->max_normal_diff = 0;
	} else {
		GRIP_ERR("Invalid value.(%d)\n", onoff);
	}
	mutex_unlock(&data->lock);

	GRIP_INFO("result : %d\n", onoff);
	return count;
}
#endif

static ssize_t grip_vendor_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", VENDOR_NAME);
}
static ssize_t grip_name_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", MODEL_NAME);
}

static ssize_t bin_fw_ver(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "0x%02x%02x\n", data->md_ver_bin, data->fw_ver_bin);
}

static int a96t3x6_get_fw_version(struct a96t3x6_data *data, bool bootmode)
{
	struct i2c_client *client = data->client;
	u8 buf;
	int ret;
	int retry = 3;

	grip_always_active(data, 1);

	ret = a96t3x6_i2c_read(client, REG_FW_VER, &buf, 1);
	if (ret < 0) {
		while (retry--) {
			GRIP_ERR("read fail(%d)\n", retry);
			if (!bootmode)
				a96t3x6_reset(data);
			else
				goto err_grip_revert_mode;
			ret = a96t3x6_i2c_read(client, REG_FW_VER, &buf, 1);
			if (ret == 0)
				break;
		}
		if (retry <= 0)
			goto err_grip_revert_mode;
	}
	data->fw_ver = buf;

	retry = 3;
	ret = a96t3x6_i2c_read(client, REG_MODEL_NO, &buf, 1);
	if (ret < 0) {
		while (retry--) {
			GRIP_ERR("read fail(%d)\n", retry);
			if (!bootmode)
				a96t3x6_reset(data);
			else
				goto err_grip_revert_mode;
			ret = a96t3x6_i2c_read(client, REG_MODEL_NO, &buf, 1);
			if (ret == 0)
				break;
		}
		if (retry <= 0)
			goto err_grip_revert_mode;
	}
	data->md_ver = buf;

	GRIP_INFO(" fw = 0x%x, md = 0x%x\n", data->fw_ver, data->md_ver);

	grip_always_active(data, 0);

	return 0;

err_grip_revert_mode:
	grip_always_active(data, 0);

	return -1;
}

static ssize_t read_fw_ver(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int ret;

	ret = a96t3x6_get_fw_version(data, false);
	if (ret < 0) {
		GRIP_ERR("read fail\n");
		data->fw_ver = 0;
	}

	return snprintf(buf, PAGE_SIZE, "0x%02x%02x\n", data->md_ver, data->fw_ver);
}

static int a96t3x6_load_fw_kernel(struct a96t3x6_data *data)
{
	int ret = 0;

	ret = request_firmware(&data->firm_data_bin,
		data->fw_path, &data->client->dev);
	if (ret) {
		GRIP_ERR("request_firmware fail.\n");
		return ret;
	}
	data->firm_size = data->firm_data_bin->size;
	data->fw_ver_bin = data->firm_data_bin->data[5];
	data->md_ver_bin = data->firm_data_bin->data[1];
	GRIP_INFO("fw = 0x%x, md = 0x%x\n", data->fw_ver_bin, data->md_ver_bin);

	data->checksum_h_bin = data->firm_data_bin->data[8];
	data->checksum_l_bin = data->firm_data_bin->data[9];

	GRIP_INFO("crc 0x%x 0x%x\n", data->checksum_h_bin, data->checksum_l_bin);

	return ret;
}

static int a96t3x6_load_fw(struct a96t3x6_data *data, u8 cmd)
{
	struct file *fp;
	mm_segment_t old_fs;
	long fsize, nread;
	int ret = 0;

	switch (cmd) {
	case BUILT_IN:
		break;

	case SDCARD:
		old_fs = get_fs();
		set_fs(get_ds());
		fp = filp_open(TK_FW_PATH_SDCARD, O_RDONLY, 0400);
		if (IS_ERR(fp)) {
			GRIP_ERR("%s open error (%d)\n", TK_FW_PATH_SDCARD, (int)PTR_ERR(fp));
			ret = -ENOENT;
			goto fail_sdcard_open;
		}

		fsize = fp->f_path.dentry->d_inode->i_size;
		data->firm_data_ums = kzalloc((size_t)fsize, GFP_KERNEL);
		if (!data->firm_data_ums) {
			GRIP_ERR("fail to kzalloc for fw\n");
			ret = -ENOMEM;
			goto fail_sdcard_kzalloc;
		}

		nread = vfs_read(fp,
			(char __user *)data->firm_data_ums, fsize, &fp->f_pos);
		if (nread != fsize) {
			GRIP_ERR("fail to vfs_read file\n");
			ret = -EINVAL;
			goto fail_sdcard_size;
		}
		filp_close(fp, current->files);
		set_fs(old_fs);
		data->firm_size = nread;
		break;

	default:
		ret = -1;
		break;
	}
	GRIP_INFO("fw_size : %lu, success (%u)\n", data->firm_size, cmd);
	return ret;

fail_sdcard_size:
	kfree(&data->firm_data_ums);
fail_sdcard_kzalloc:
	filp_close(fp, current->files);
fail_sdcard_open:
	set_fs(old_fs);
	return ret;
}

static int a96t3x6_check_busy(struct a96t3x6_data *data)
{
	int ret, count = 0;
	unsigned char val = 0x00;

	do {
		ret = i2c_master_recv(data->client, &val, sizeof(val));

		if (val)
			count++;
		else
			break;

		if (count > 1000)
			break;
	} while (1);

	if (count > 1000)
		GRIP_ERR("busy %d\n", count);
	return ret;
}

static int a96t3x6_i2c_read_checksum(struct a96t3x6_data *data)
{
	unsigned char buf[6] = {0xAC, 0x9E, 0x10, 0x00, 0x3F, 0xFF};
	unsigned char buf2[1] = {0x00};
	unsigned char checksum[6] = {0, };
	int ret;

	i2c_master_send(data->client, buf, 6);
	usleep_range(5000, 6000);

	i2c_master_send(data->client, buf2, 1);
	usleep_range(5000, 6000);

	ret = a96t3x6_i2c_read_data(data->client, checksum, 6);

	GRIP_INFO("ret:%d [%X][%X][%X][%X][%X]\n", ret,
			checksum[0], checksum[1], checksum[2],
			checksum[4], checksum[5]);
	data->checksum_h = checksum[4];
	data->checksum_l = checksum[5];
	return 0;
}

static int a96t3x6_fw_write(struct a96t3x6_data *data, unsigned char *addrH,
	unsigned char *addrL, unsigned char *val)
{
	int length = 36, ret = 0;
	unsigned char buf[36];

	buf[0] = 0xAC;
	buf[1] = 0x7A;
	memcpy(&buf[2], addrH, 1);
	memcpy(&buf[3], addrL, 1);
	memcpy(&buf[4], val, 32);

	ret = i2c_master_send(data->client, buf, length);
	if (ret != length) {
		GRIP_ERR("write fail[%x%x], %d\n", *addrH, *addrL, ret);
		return ret;
	}

	usleep_range(3000, 3000);

	a96t3x6_check_busy(data);

	return 0;
}

static int a96t3x6_fw_mode_enter(struct a96t3x6_data *data)
{
	unsigned char buf[2] = {0xAC, 0x5B};
	u8 cmd = 0;
	int ret = 0;

	GRIP_INFO("cmd send\n");
	ret = i2c_master_send(data->client, buf, 2);
	if (ret != 2) {
		GRIP_ERR("write fail\n");
		return -1;
	}

	ret = i2c_master_recv(data->client, &cmd, 1);
	GRIP_INFO("cmd receive %2x, %2x\n", data->firmup_cmd, cmd);
	if (data->firmup_cmd != cmd) {
		GRIP_ERR("cmd not matched, firmup fail (ret = %d)\n", ret);
		return -2;
	}

	return 0;
}

static int a96t3x6_flash_erase(struct a96t3x6_data *data)
{
	unsigned char buf[2] = {0xAC, 0x2D};
	int ret = 0;

	ret = i2c_master_send(data->client, buf, 2);
	if (ret != 2) {
		GRIP_ERR("write fail\n");
		return -1;
	}

	return 0;

}

static int a96t3x6_fw_mode_exit(struct a96t3x6_data *data)
{
	unsigned char buf[2] = {0xAC, 0xE1};
	int ret = 0;

	ret = i2c_master_send(data->client, buf, 2);
	if (ret != 2) {
		GRIP_ERR("write fail\n");
		return -1;
	}

	return 0;
}

static int a96t3x6_fw_update(struct a96t3x6_data *data, u8 cmd)
{
	int ret, i = 0;
	int count;
	int retry = 5;
	unsigned short address;
	unsigned char addrH, addrL;
	unsigned char buf[32] = {0, };

	GRIP_INFO("start\n");

	count = data->firm_size / 32;
	address = USER_CODE_ADDRESS;

	while(retry > 0)
	{
		a96t3x6_reset_for_bootmode(data);
		usleep_range(BOOT_DELAY, BOOT_DELAY);

		ret = a96t3x6_fw_mode_enter(data);
		if (ret < 0)
			GRIP_ERR("a96t3x6_fw_mode_enter fail, retry : %d\n",
				((5-retry)+1));
		else
			break;
		
		retry--;
	}
	
	if(ret < 0 && retry == 0) {
		GRIP_ERR("a96t3x6_fw_mode_enter fail\n");
		return ret;
	}
	
	usleep_range(5000, 5000);
	GRIP_INFO("fw_mode_cmd sent\n");

	ret = a96t3x6_flash_erase(data);
	usleep_range(FLASH_DELAY, FLASH_DELAY);

	GRIP_INFO("fw_write start\n");
	for (i = 1; i < count; i++) {
		/* first 32byte is header */
		addrH = (unsigned char)((address >> 8) & 0xFF);
		addrL = (unsigned char)(address & 0xFF);
		if (cmd == BUILT_IN)
			memcpy(buf, &data->firm_data_bin->data[i * 32], 32);
		else if (cmd == SDCARD)
			memcpy(buf, &data->firm_data_ums[i * 32], 32);

		ret = a96t3x6_fw_write(data, &addrH, &addrL, buf);
		if (ret < 0) {
			GRIP_ERR("err, no device : %d\n", ret);
			return ret;
		}

		address += 0x20;

		memset(buf, 0, 32);
	}

	ret = a96t3x6_i2c_read_checksum(data);
	GRIP_INFO("checksum read%d\n", ret);

	ret = a96t3x6_fw_mode_exit(data);
	GRIP_INFO("fw_write end\n");

	return ret;
}

static void a96t3x6_release_fw(struct a96t3x6_data *data, u8 cmd)
{
	switch (cmd) {
	case BUILT_IN:
		release_firmware(data->firm_data_bin);
		break;

	case SDCARD:
		kfree(data->firm_data_ums);
		break;

	default:
		break;
	}
}

static int a96t3x6_flash_fw(struct a96t3x6_data *data, bool probe, u8 cmd)
{
	int retry = 2;
	int ret;
	int block_count;
	const u8 *fw_data;

	ret = a96t3x6_get_fw_version(data, probe);
	if (ret)
		data->fw_ver = 0;

	ret = a96t3x6_load_fw(data, cmd);
	if (ret) {
		GRIP_ERR("fw load fail\n");
		return ret;
	}

	switch (cmd) {
	case BUILT_IN:
		fw_data = data->firm_data_bin->data;
		break;

	case SDCARD:
		fw_data = data->firm_data_ums;
		break;

	default:
		return -1;
	}

	block_count = (int)(data->firm_size / 32);

	while (retry--) {
		ret = a96t3x6_fw_update(data, cmd);
		if (ret < 0)
			break;

		if (cmd == BUILT_IN) {
			if ((data->checksum_h != data->checksum_h_bin) ||
				(data->checksum_l != data->checksum_l_bin)) {
				GRIP_ERR("checksum fail.(0x%x,0x%x),(0x%x,0x%x) retry:%d\n",
						data->checksum_h, data->checksum_l,
						data->checksum_h_bin, data->checksum_l_bin, retry);
				ret = -1;
				continue;
			}
		}


		ret = a96t3x6_get_fw_version(data, true);
		if (ret) {
			GRIP_ERR("fw version read fail\n");
			ret = -1;
			continue;
		}

		if (data->fw_ver == 0) {
			GRIP_ERR("fw version fail (0x%x)\n", data->fw_ver);
			ret = -1;
			continue;
		}

		if ((cmd == BUILT_IN) && (data->fw_ver != data->fw_ver_bin)) {
			GRIP_ERR("fw version fail 0x%x, 0x%x\n",
						data->fw_ver, data->fw_ver_bin);
			ret = -1;
			continue;
		}
		ret = 0;
		break;
	}

	a96t3x6_release_fw(data, cmd);

	return ret;
}

static ssize_t grip_fw_update(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int ret;
	u8 cmd;

	int enable = atomic_read(&data->enable);

	switch (*buf) {
	case 's':
	case 'S':
		cmd = BUILT_IN;
		break;
	case 'i':
	case 'I':
		cmd = SDCARD;
		break;
	default:
		data->fw_update_state = 2;
		goto fw_update_out;
	}

	data->fw_update_state = 1;
	disable_irq(data->irq);

	if (cmd == BUILT_IN) {
		ret = a96t3x6_load_fw_kernel(data);
		if (ret) {
			GRIP_ERR("failed to load firmware(%d)\n", ret);
			goto fw_update_out;
		} else {
			GRIP_INFO("fw version read success (%d)\n", ret);
		}
	}
	ret = a96t3x6_flash_fw(data, false, cmd);

	if (enable) {
		cmd = CMD_ON;
		ret = a96t3x6_i2c_write(data->client, REG_SAR_ENABLE, &cmd);
		if (ret < 0)
			GRIP_INFO("failed to enable grip irq\n");

		a96t3x6_check_first_status(data, 1);
	}

	enable_irq(data->irq);
	if (ret) {
		GRIP_ERR("failed to flash firmware(%d)\n", ret);
		data->fw_update_state = 2;
	} else {
		GRIP_INFO("success\n");
		data->fw_update_state = 0;
	}

fw_update_out:
	GRIP_INFO("fw_update_state = %d\n", data->fw_update_state);

	return count;
}

static ssize_t grip_fw_update_status(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int count = 0;

	GRIP_INFO("%d\n", data->fw_update_state);

	if (data->fw_update_state == 0)
		count = snprintf(buf, PAGE_SIZE, "PASS\n");
	else if (data->fw_update_state == 1)
		count = snprintf(buf, PAGE_SIZE, "Downloading\n");
	else if (data->fw_update_state == 2)
		count = snprintf(buf, PAGE_SIZE, "Fail\n");

	return count;
}

static ssize_t grip_irq_state_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int status = 0;

	status = gpio_get_value(data->grip_int);
	GRIP_INFO("status=%d\n", status);

	return snprintf(buf, PAGE_SIZE, "%d\n", status);
}

static ssize_t grip_irq_en_cnt_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	GRIP_INFO("irq_en_cnt=%d\n", data->irq_en_cnt);

	return snprintf(buf, PAGE_SIZE, "%d\n", data->irq_en_cnt);
}

static ssize_t grip_reg_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	u8 val = 0;
	int offset = 0, i = 0;
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	for (i = 0; i < 128; i++) {
		a96t3x6_i2c_read(data->client, i, &val, 1);
		GRIP_INFO("%s: reg=%02X val=%02X\n", __func__, i, val);
		
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"reg=0x%x val=0x%x\n", i, val);
	}

	return offset;
}

static ssize_t grip_reg_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int regist = 0, val = 0;
	u8 cmd = 0;
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	if (sscanf(buf, "%4x,%4x", &regist, &val) != 2) {
		GRIP_ERR("The number of data are wrong\n");
		return -EINVAL;
	}

	GRIP_INFO("reg=0x%2x value=0x%2x\n", regist, val);

	cmd = (u8) val;
	a96t3x6_i2c_write(data->client, (u8)regist, &cmd);

	return size;
}

static ssize_t grip_crc_check_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int ret;
#ifndef CONFIG_SENSORS_A96T3X6_CRC_CHECK
	unsigned char cmd[3] = {0x1B, 0x00, 0x10};
	unsigned char checksum[2] = {0, };

	i2c_master_send(data->client, cmd, 3);
	usleep_range(50 * 1000, 50 * 1000);

	ret = a96t3x6_i2c_read(data->client, 0x1B, checksum, 2);

	if (ret < 0) {
		GRIP_ERR("i2c read fail\n");
		return snprintf(buf, PAGE_SIZE, "NG,0000\n");
	}

	GRIP_INFO("CRC:%02x%02x, BIN:%02x%02x\n",
		checksum[0], checksum[1],
		data->checksum_h_bin, data->checksum_l_bin);

	if ((checksum[0] != data->checksum_h_bin) ||
		(checksum[1] != data->checksum_l_bin))
		return snprintf(buf, PAGE_SIZE, "NG,%02x%02x\n",
			checksum[0], checksum[1]);
	else
		return snprintf(buf, PAGE_SIZE, "OK,%02x%02x\n",
			checksum[0], checksum[1]);
#else
	unsigned char cmd = 0xAA;
	unsigned char val = 0xFF;
	unsigned char crc_check = CRC_FAIL;
	unsigned char retry = 10;

	/* 
	 * abov grip fw uses active/deactive mode in each period
	 * To check crc check, make the mode as always active mode.
	 */

	grip_always_active(data, 1);

	/* crc check */
	ret = a96t3x6_i2c_write(data->client, REG_FW_VER, &cmd);
	if (ret < 0) {
		GRIP_ERR("crc checking enter failed\n");
	}

	while (retry--) {
		msleep(400);

		ret = a96t3x6_i2c_read(data->client, REG_FW_VER, &val, 1);
		if (ret < 0) {
			GRIP_ERR("crc read failed\n");
		}

		GRIP_INFO("crc check value = 0x%2x\n", val);

		if (val == 0x00) {
			GRIP_ERR("crc check fail\n");
		} else {
			GRIP_INFO("crc check normal\n");
			/* only success route */
			crc_check = CRC_PASS;
			break;
		}
	}
	grip_always_active(data, 0);

	if (crc_check == CRC_PASS)
		return snprintf(buf, PAGE_SIZE, "OK,%02x\n", val);
	else
		return snprintf(buf, PAGE_SIZE, "NG,%02x\n", val);
#endif
}

static ssize_t a96t3x6_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int enable = atomic_read(&data->enable);

	return snprintf(buf, PAGE_SIZE, "%d\n", enable);
}

static ssize_t grip_motion_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	if (data->motion)
		return snprintf(buf, PAGE_SIZE, "motion_detect\n");
	else
		return snprintf(buf, PAGE_SIZE, "motion_non_detect\n");
}

static ssize_t grip_motion_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int val;
	int ret;
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	ret = kstrtoint(buf, 10, &val);
	if (ret) {
		GRIP_INFO("Invalid Argument\n");
		return ret;
	}

	if (val == 0) {
		GRIP_INFO("motion event off\n");
		data->motion = val;
	} else if (val == 1) {
		GRIP_INFO("motion event\n");
		data->motion = val;
	} else {
		GRIP_INFO("Invalid Argument : %u\n", val);
	}

	GRIP_INFO("%u\n", val);
	return count;
}

static ssize_t grip_unknown_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%s\n",
		(data->is_unknown_mode == 1) ? "UNKNOWN" : "NORMAL");
}

static ssize_t grip_unknown_state_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int val;
	int ret;
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	ret = kstrtoint(buf, 10, &val);
	if (ret) {
		GRIP_INFO("Invalid Argument\n");
		return ret;
	}

	if (val == 1)
		a96t3x6_enter_unknown_mode(data, TYPE_FORCE);
	else if (val == 0)
		data->is_unknown_mode = UNKNOWN_OFF;
	else
		GRIP_INFO("Invalid Argument(%d)\n", val);

	GRIP_INFO("%u\n", val);
	return count;
}

static ssize_t a96t3x6_noti_enable_store(struct device *dev,
				     struct device_attribute *attr, const char *buf, size_t size)
{
	int ret;
	u8 enable;
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	ret = kstrtou8(buf, 2, &enable);
	if (ret) {
		GRIP_ERR("invalid argument\n");
		return size;
	}

	data->noti_enable = enable;
	GRIP_INFO("new_value=%d\n", (int)enable);

	if (data->noti_enable)
		a96t3x6_enter_unknown_mode(data, TYPE_BOOT);

	return size;
}

static ssize_t a96t3x6_noti_enable_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	GRIP_INFO("noti_enable = %d\n", data->noti_enable);

	return sprintf(buf, "%d\n", data->noti_enable);
}


static DEVICE_ATTR(grip_threshold, 0444, grip_threshold_show, NULL);
static DEVICE_ATTR(grip_total_cap, 0444, grip_total_cap_show, NULL);
static DEVICE_ATTR(grip_sar_enable, 0664, grip_sar_enable_show,
			grip_sar_enable_store);
static DEVICE_ATTR(grip_sw_reset_ready, 0444, grip_sw_reset_ready_show, NULL);
static DEVICE_ATTR(grip_sw_reset, 0220, NULL, grip_sw_reset);
static DEVICE_ATTR(grip_earjack, 0220, NULL, grip_sensing_change);
static DEVICE_ATTR(grip, 0444, grip_show, NULL);
static DEVICE_ATTR(grip_baseline, 0444, grip_baseline_show, NULL);
static DEVICE_ATTR(grip_raw, 0444, grip_raw_show, NULL);
static DEVICE_ATTR(grip_ref_cap, 0444, grip_ref_cap_show, NULL);
static DEVICE_ATTR(grip_gain, 0444, grip_gain_show, NULL);
static DEVICE_ATTR(grip_check, 0444, grip_check_show, NULL);
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
static DEVICE_ATTR(grip_sar_only_mode, 0220, NULL, grip_mode_change);
static DEVICE_ATTR(grip_sar_press_threshold, 0220,
		NULL, grip_sar_press_threshold_store);
static DEVICE_ATTR(grip_sar_release_threshold, 0220,
		NULL, grip_sar_release_threshold_store);
#endif
#ifdef CONFIG_SEC_FACTORY
static DEVICE_ATTR(grip_irq_count, 0664, a96t3x6_irq_count_show,
			a96t3x6_irq_count_store);
#endif
static DEVICE_ATTR(name, 0444, grip_name_show, NULL);
static DEVICE_ATTR(vendor, 0444, grip_vendor_show, NULL);
static DEVICE_ATTR(grip_firm_version_phone, 0444, bin_fw_ver, NULL);
static DEVICE_ATTR(grip_firm_version_panel, 0444, read_fw_ver, NULL);
static DEVICE_ATTR(grip_firm_update, 0220, NULL, grip_fw_update);
static DEVICE_ATTR(grip_firm_update_status, 0444, grip_fw_update_status, NULL);
static DEVICE_ATTR(grip_irq_state, 0444, grip_irq_state_show, NULL);
static DEVICE_ATTR(grip_irq_en_cnt, 0444, grip_irq_en_cnt_show, NULL);
static DEVICE_ATTR(grip_reg_rw, 0664, grip_reg_show, grip_reg_store);
static DEVICE_ATTR(grip_crc_check, 0444, grip_crc_check_show, NULL);
static DEVICE_ATTR(motion, 0664, grip_motion_show, grip_motion_store);
static DEVICE_ATTR(unknown_state, 0664,
	grip_unknown_state_show, grip_unknown_state_store);
static DEVICE_ATTR(noti_enable, 0664, a96t3x6_noti_enable_show, a96t3x6_noti_enable_store);

static struct device_attribute *grip_sensor_attributes[] = {
	&dev_attr_grip_threshold,
	&dev_attr_grip_total_cap,
	&dev_attr_grip_sar_enable,
	&dev_attr_grip_sw_reset,
	&dev_attr_grip_sw_reset_ready,
	&dev_attr_grip_earjack,
	&dev_attr_grip,
	&dev_attr_grip_baseline,
	&dev_attr_grip_raw,
	&dev_attr_grip_ref_cap,
	&dev_attr_grip_gain,
	&dev_attr_grip_check,
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	&dev_attr_grip_sar_only_mode,
	&dev_attr_grip_sar_press_threshold,
	&dev_attr_grip_sar_release_threshold,
#endif
#ifdef CONFIG_SEC_FACTORY
	&dev_attr_grip_irq_count,
#endif
	&dev_attr_name,
	&dev_attr_vendor,
	&dev_attr_grip_firm_version_phone,
	&dev_attr_grip_firm_version_panel,
	&dev_attr_grip_firm_update,
	&dev_attr_grip_firm_update_status,
	&dev_attr_grip_irq_state,
	&dev_attr_grip_irq_en_cnt,
	&dev_attr_grip_reg_rw,
	&dev_attr_grip_crc_check,
	&dev_attr_motion,
	&dev_attr_unknown_state,
	&dev_attr_noti_enable,
	NULL,
};

static DEVICE_ATTR(enable, 0664, a96t3x6_enable_show, grip_sar_enable_store);

static struct attribute *a96t3x6_attributes[] = {
	&dev_attr_enable.attr,
	NULL
};

static struct attribute_group a96t3x6_attribute_group = {
	.attrs = a96t3x6_attributes
};

static int a96t3x6_fw_check(struct a96t3x6_data *data)
{
	int ret;
	int fw_up = 0;

	if (data->bringup) {
		GRIP_INFO("bring up mode. skip firmware check\n");
		return 0;
	}

	ret = a96t3x6_load_fw_kernel(data);
#ifdef CONFIG_SENSORS_FW_VENDOR
	if (ret) {
		GRIP_ERR("fw was not loaded yet from ueventd\n");
		return ret;
	}
#endif
	ret = a96t3x6_get_fw_version(data, true);
	if (!ret) {

		if (data->fw_ver < data->fw_ver_bin)
			fw_up |= 0x01;

		if (data->fw_ver > TEST_FIRMWARE_DETECT_VER)
			fw_up |= 0x02;
	}

	if (data->md_ver != data->md_ver_bin) {
		GRIP_ERR("MD version is different.(IC %x, BN %x). Do force FW update\n",
			data->md_ver, data->md_ver_bin);
		fw_up |= 0x04;
	}

	if (fw_up) {
		GRIP_INFO("fw update (0x%x -> 0x%x) request = %d\n",
			data->fw_ver, data->fw_ver_bin, fw_up);
		ret = a96t3x6_flash_fw(data, true, BUILT_IN);
		if (ret)
			GRIP_ERR("failed to a96t3x6_flash_fw (%d)\n", ret);
		else
			GRIP_INFO("fw update success\n");
	}
	return ret;
}

static int a96t3x6_power_onoff(void *pdata, bool on)
{
	struct a96t3x6_data *data = (struct a96t3x6_data *)pdata;

	int ret = 0;
	int voltage = 0;
	int reg_enabled = 0;

	if (data->ldo_en) {
		gpio_set_value(data->ldo_en, on);
		GRIP_INFO("ldo_en power %d\n", on);
	}

	if (data->dvdd_vreg_name) {
		if (data->dvdd_vreg == NULL) {
			data->dvdd_vreg = regulator_get(NULL, data->dvdd_vreg_name);
			if (IS_ERR(data->dvdd_vreg)) {
				data->dvdd_vreg = NULL;
				GRIP_ERR("failed to get dvdd_vreg %s\n", data->dvdd_vreg_name);
			}
		}
	}		

	if (data->dvdd_vreg) {
		voltage = regulator_get_voltage(data->dvdd_vreg);
		reg_enabled = regulator_is_enabled(data->dvdd_vreg);
		GRIP_INFO("dvdd_vreg reg_enabled=%d voltage=%d\n", reg_enabled, voltage);
	}

	// To enter into firmware download mode, power must be turned OFF and ON,
	// so regulator must be enabled and disabled not more than one time.
	if (on) {
		if (data->dvdd_vreg) {
			if (reg_enabled == 0) {
				ret = regulator_enable(data->dvdd_vreg);
				if (ret) {
					GRIP_ERR("dvdd reg enable fail\n");
					return ret;
				}
				GRIP_INFO("dvdd_vreg turned on\n");
			}
		}
	} else {
		if (data->dvdd_vreg) {
			if (reg_enabled == 1) {
				ret = regulator_disable(data->dvdd_vreg);
				if (ret) {
					GRIP_ERR("dvdd reg disable fail\n");
					return ret;
				}
				GRIP_INFO("dvdd_vreg turned off\n");
			}
		}
	}

	GRIP_INFO("%s\n", on ? "on" : "off");

	return ret;
}

static int a96t3x6_irq_init(struct device *dev,
	struct a96t3x6_data *data)
{
	int ret = 0;

	ret = gpio_request(data->grip_int, "a96t3x6_IRQ");
	if (ret < 0) {
		GRIP_ERR("gpio %d request failed (%d)\n", data->grip_int, ret);
		return ret;
	}

	ret = gpio_direction_input(data->grip_int);
	if (ret < 0) {
		GRIP_ERR("failed to set direction input gpio %d(%d)\n",
				data->grip_int, ret);
		gpio_free(data->grip_int);
		return ret;
	}
	// assigned power function to function ptr
	data->power = a96t3x6_power_onoff;

	return ret;
}

static int a96t3x6_parse_dt(struct a96t3x6_data *data, struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct pinctrl *p;
	int ret;
	enum of_gpio_flags flags;

	data->grip_int = of_get_named_gpio(np, "a96t3x6,irq_gpio", 0);
	if (data->grip_int < 0) {
		GRIP_ERR("Cannot get grip_int\n");
		return data->grip_int;
	}

	data->ldo_en = of_get_named_gpio_flags(np, "a96t3x6,ldo_en", 0, &flags);
	if (data->ldo_en < 0) {
		GRIP_ERR("fail to get ldo_en\n");
		data->ldo_en = 0;
	} else {
		ret = gpio_request(data->ldo_en, "a96t3x6_ldo_en");
		if (ret < 0) {
			GRIP_ERR("gpio %d request failed %d\n", data->ldo_en, ret);
			return ret;
		}
		gpio_direction_output(data->ldo_en, 0);
	}

	if (of_property_read_string_index(np, "a96t3x6,dvdd_vreg_name", 0,
			(const char **)&data->dvdd_vreg_name)) {
		data->dvdd_vreg_name = NULL;
	}
	GRIP_INFO("dvdd_vreg_name: %s\n", data->dvdd_vreg_name);

	ret = of_property_read_string(np, "a96t3x6,fw_path", (const char **)&data->fw_path);
	if (ret < 0) {
		GRIP_ERR("failed to read fw_path %d\n", ret);
		data->fw_path = TK_FW_PATH_BIN;
	}
	GRIP_INFO("fw path %s\n", data->fw_path);

	data->bringup = of_property_read_bool(np, "a96t3x6,bringup");

	ret = of_property_read_u32(np, "a96t3x6,firmup_cmd", &data->firmup_cmd);
	if (ret < 0)
		data->firmup_cmd = 0;

	ret = of_property_read_u8(np, "a96t3x6,earjack_noise",
		&data->earjack_noise);
	if (ret < 0) {
		GRIP_INFO("failed to get the earjack noise dt\n");
		data->earjack_noise = 0;
	} else {
		GRIP_INFO("grip earjack noise block is adjusting %d \n", (int)data->earjack_noise);
		data->earjack_noise = 1;
	}

	p = pinctrl_get_select_default(dev);
	if (IS_ERR(p)) {
		GRIP_INFO("failed pinctrl_get\n");
	}

	GRIP_INFO("grip_int:%d, ldo_en:%d\n", data->grip_int, data->ldo_en);

	return 0;
}

#if defined(CONFIG_CCIC_NOTIFIER) || defined(CONFIG_PDIC_NOTIFIER) \
	|| defined(CONFIG_MUIC_NOTIFIER) || defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
static void cmdon_work_func(struct work_struct *work)
{
	struct a96t3x6_data *grip_data = container_of((struct work_struct *)work,
						struct a96t3x6_data, cmdon_work);
	u8 cmd = CMD_ON;

	if (grip_data != NULL)
		a96t3x6_i2c_write(grip_data->client, REG_TSPTA, &cmd);
}
static void cmdoff_work_func(struct work_struct *work)
{
	struct a96t3x6_data *grip_data = container_of((struct work_struct *)work,
						struct a96t3x6_data, cmdoff_work);
	u8 cmd = CMD_OFF;

	if (grip_data != NULL)
		a96t3x6_i2c_write(grip_data->client, REG_TSPTA, &cmd);
}
#endif

#if defined(CONFIG_CCIC_NOTIFIER) && defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
static int a96t3x6_ccic_handle_notification(struct notifier_block *nb,
	unsigned long action, void *data) {

	CC_NOTI_ATTACH_TYPEDEF usb_typec_info =
		*(CC_NOTI_ATTACH_TYPEDEF *)data;
	struct a96t3x6_data *grip_data =
		container_of(nb, struct a96t3x6_data, ccic_nb);
	static int pre_attach = -1;
	u8 cmd = CMD_ON;

	GRIP_INFO("src = %d, id = %d, attach = %d",
			usb_typec_info.src, usb_typec_info.id, usb_typec_info.attach);

	if (usb_typec_info.id != CCIC_NOTIFY_ID_ATTACH)
		return 0;

	if (pre_attach == usb_typec_info.attach)
		return 0;

	if (usb_typec_info.attach == MUIC_NOTIFY_CMD_DETACH) {
		cmd = CMD_ON;
		schedule_work(&grip_data->cmdon_work);
		a96t3x6_enter_unknown_mode(grip_data, TYPE_USB);
	} else if (usb_typec_info.attach == MUIC_NOTIFY_CMD_ATTACH) {
		cmd = CMD_OFF;
		schedule_work(&grip_data->cmdoff_work);
		a96t3x6_enter_unknown_mode(grip_data, TYPE_USB);
	} else {
		return 0;
	}

	pre_attach = usb_typec_info.attach;
	return 0;
}
#elif defined(CONFIG_VBUS_NOTIFIER)
static int a96t3x6_cpuidle_vbus_notifier(struct notifier_block *nb,
				unsigned long action, void *data)
{
	vbus_status_t vbus_type = *(vbus_status_t *) data;
	struct a96t3x6_data *grip_data =
		container_of(nb, struct a96t3x6_data, vbus_nb);
	static int vbus_pre_attach;
	u8 cmd = CMD_ON;

	if (vbus_pre_attach == vbus_type)
		return 0;

	switch (vbus_type) {
	case STATUS_VBUS_HIGH:
		cmd = CMD_OFF;
		a96t3x6_i2c_write(grip_data->client, REG_TSPTA, &cmd);
		GRIP_INFO("TA/USB is inserted\n");
		break;
	case STATUS_VBUS_LOW:
		cmd = CMD_ON;
		a96t3x6_i2c_write(grip_data->client, REG_TSPTA, &cmd);
		GRIP_INFO("TA/USB is removed\n");
		break;
	default:
		GRIP_INFO("vbus skip attach = %d\n",
			 vbus_type);
		break;
	}

	vbus_pre_attach = vbus_type;
	return 0;
}
#elif defined(CONFIG_MUIC_NOTIFIER)
static int a96t3x6_cpuidle_muic_notifier(struct notifier_block *nb,
	unsigned long action, void *data)
{
	struct a96t3x6_data *grip_data;
	u8 cmd = CMD_ON;

	muic_attached_dev_t attached_dev = *(muic_attached_dev_t *)data;

	grip_data = container_of(nb, struct a96t3x6_data, muic_nb);
	switch (attached_dev) {
	case ATTACHED_DEV_OTG_MUIC:
	case ATTACHED_DEV_USB_MUIC:
	case ATTACHED_DEV_TA_MUIC:
	case ATTACHED_DEV_AFC_CHARGER_PREPARE_MUIC:
	case ATTACHED_DEV_AFC_CHARGER_9V_MUIC:
		if (action == MUIC_NOTIFY_CMD_ATTACH) {
			cmd = CMD_OFF;
			GRIP_INFO("TA/USB is inserted\n");
		}
		else if (action == MUIC_NOTIFY_CMD_DETACH) {
			cmd = CMD_ON;
			GRIP_INFO("TA/USB is removed\n");
		}
		a96t3x6_i2c_write(grip_data->client, REG_TSPTA, &cmd);
		break;
	default:
		break;
	}

	GRIP_INFO("dev=%d, action=%lu\n", attached_dev, action);
	return NOTIFY_DONE;
}
#endif

static int a96t3x6_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	struct a96t3x6_data *data;
	struct input_dev *input_dev;
	struct input_dev *noti_input_dev;
	int ret;
#ifdef CONFIG_SENSORS_FW_VENDOR
	u8 buf;
#endif

	GRIP_INFO("start (0x%x)\n", client->addr);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		GRIP_ERR("i2c_check_functionality fail\n");
		return -EIO;
	}

	data = kzalloc(sizeof(struct a96t3x6_data), GFP_KERNEL);
	if (!data) {
		GRIP_ERR("Failed to allocate memory\n");
		ret = -ENOMEM;
		goto err_alloc;
	}

	input_dev = input_allocate_device();
	if (!input_dev) {
		GRIP_ERR("Failed to allocate memory for input device\n");
		ret = -ENOMEM;
		goto err_input_alloc;
	}

	noti_input_dev = input_allocate_device();
	if (!noti_input_dev) {
		GRIP_ERR("Failed to allocate memory for input device\n");
		ret = -ENOMEM;
		goto err_noti_input_alloc;
	}

	data->client = client;
	data->input_dev = input_dev;
	data->noti_input_dev = noti_input_dev;
	data->probe_done = false;
	data->earjack = 0;
	data->skip_event = false;
	data->sar_mode = false;
	data->is_unknown_mode = UNKNOWN_ON;
	data->first_working = false;

	data->motion = 0;

	wake_lock_init(&data->grip_wake_lock, WAKE_LOCK_SUSPEND, "grip wake lock");

	ret = a96t3x6_parse_dt(data, &client->dev);
	if (ret) {
		GRIP_ERR("failed to a96t3x6_parse_dt\n");
		goto err_config;
	}

	ret = a96t3x6_irq_init(&client->dev, data);
	if (ret) {
		GRIP_ERR("failed to init reg\n");
		goto pwr_config;
	}

	if (data->power) {
		data->power(data, true);
		usleep_range(RESET_DELAY, RESET_DELAY);
	}

	data->irq = -1;
	client->irq = gpio_to_irq(data->grip_int);
	mutex_init(&data->lock);

	i2c_set_clientdata(client, data);
#ifndef CONFIG_SENSORS_FW_VENDOR
	ret = a96t3x6_fw_check(data);
	if (ret) {
		GRIP_ERR("failed to firmware check (%d)\n", ret);
		goto err_reg_input_dev;
	}
#else
	/*
	 * Add probe fail routine if i2c is failed
	 * non fw IC returns 0 from ALL register but i2c is success.
	 */
	ret = a96t3x6_i2c_read(client, REG_MODEL_NO, &buf, 1);
	if (ret) {
		GRIP_ERR("i2c is failed %d\n", ret);
		goto err_reg_input_dev;
	} else {
		GRIP_INFO("i2c is normal, model_no = 0x%2x\n", buf);
	}
#endif
	input_dev->name = MODULE_NAME;
	input_dev->id.bustype = BUS_I2C;

	input_set_capability(input_dev, EV_REL, REL_X);
	input_set_capability(input_dev, EV_REL, REL_MISC);
	input_set_drvdata(input_dev, data);

	INIT_DELAYED_WORK(&data->debug_work, a96t3x6_debug_work_func);
#ifdef CONFIG_SENSORS_FW_VENDOR	
	INIT_DELAYED_WORK(&data->firmware_work, a96t3x6_firmware_work_func);
#endif
	INIT_DELAYED_WORK(&data->reset_work, a96t3x6_reset_work_func);
#if defined(CONFIG_CCIC_NOTIFIER) || defined(CONFIG_PDIC_NOTIFIER) \
		|| defined(CONFIG_MUIC_NOTIFIER) || defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	INIT_WORK(&data->cmdon_work, cmdon_work_func);
	INIT_WORK(&data->cmdoff_work, cmdoff_work_func);
#endif

	ret = input_register_device(input_dev);
	if (ret) {
		GRIP_ERR("failed to register input dev (%d)\n",
			ret);
		goto err_reg_input_dev;
	}

	ret = sensors_create_symlink(data->input_dev);
	if (ret < 0) {
		GRIP_ERR("Failed to create sysfs symlink\n");
		goto err_sysfs_symlink;
	}

	ret = sysfs_create_group(&data->input_dev->dev.kobj,
				&a96t3x6_attribute_group);
	if (ret < 0) {
		GRIP_ERR("Failed to create sysfs group\n");
		goto err_sysfs_group;
	}

	noti_input_dev->name = MODULE_NOTIFIER_NAME;
	noti_input_dev->id.bustype = BUS_I2C;

	input_set_capability(noti_input_dev, EV_REL, REL_X);
	input_set_drvdata(noti_input_dev, data);

	ret = input_register_device(noti_input_dev);
	if (ret) {
		GRIP_ERR("failed to register input dev (%d)\n",
			ret);
		goto err_reg_noti_input_dev;
	}

	ret = sensors_register(data->dev, data, grip_sensor_attributes,
				MODULE_NAME);
	if (ret) {
		GRIP_ERR("could not register grip_sensor(%d)\n", ret);
		goto err_sensor_register;
	}

	ret = request_threaded_irq(client->irq, NULL, a96t3x6_interrupt,
			IRQF_TRIGGER_LOW | IRQF_ONESHOT, MODEL_NAME, data);

	disable_irq(client->irq);

	if (ret < 0) {
		GRIP_ERR("Failed to register interrupt\n");
		goto err_req_irq;
	}
	data->irq = client->irq;
	data->dev = &client->dev;

	device_init_wakeup(&client->dev, true);

	a96t3x6_set_debug_work(data, 1, 20000);
#ifdef CONFIG_SENSORS_FW_VENDOR
	a96t3x6_set_firmware_work(data, 1, 1);
#endif

#if defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER) && defined(CONFIG_CCIC_NOTIFIER)
	manager_notifier_register(&data->ccic_nb,
		a96t3x6_ccic_handle_notification, MANAGER_NOTIFY_CCIC_SENSORHUB);
#elif defined(CONFIG_VBUS_NOTIFIER)
	vbus_notifier_register(&data->vbus_nb,
		a96t3x6_cpuidle_vbus_notifier,  VBUS_NOTIFY_DEV_CHARGER);
#elif defined(CONFIG_MUIC_NOTIFIER)
	muic_notifier_register(&data->muic_nb,
		a96t3x6_cpuidle_muic_notifier, MUIC_NOTIFY_DEV_CPUIDLE);
#endif
	data->motion = 1;
	data->first_working = false;
	data->is_unknown_mode = UNKNOWN_OFF;

	GRIP_INFO("done\n");
	data->probe_done = true;
	data->resume_called = false;
	return 0;

err_req_irq:
	sensors_unregister(data->dev, grip_sensor_attributes);
err_reg_noti_input_dev:
	input_unregister_device(noti_input_dev);
err_sensor_register:
	sysfs_remove_group(&data->input_dev->dev.kobj,
			&a96t3x6_attribute_group);
err_sysfs_group:
	sensors_remove_symlink(data->input_dev);
err_sysfs_symlink:
	input_unregister_device(input_dev);
err_reg_input_dev:
	mutex_destroy(&data->lock);
	gpio_free(data->grip_int);
	if (data->power)
		data->power(data, false);
pwr_config:
err_config:
	wake_lock_destroy(&data->grip_wake_lock);
	input_free_device(noti_input_dev);
err_noti_input_alloc:
	input_free_device(input_dev);
err_input_alloc:
	kfree(data);
err_alloc:
	GRIP_ERR("failed\n");
	return ret;
}

static int a96t3x6_remove(struct i2c_client *client)
{
	struct a96t3x6_data *data = i2c_get_clientdata(client);

	data->power(data, false);

	device_init_wakeup(&client->dev, false);
	wake_lock_destroy(&data->grip_wake_lock);
	cancel_delayed_work_sync(&data->debug_work);
#ifdef CONFIG_SENSORS_FW_VENDOR
	cancel_delayed_work_sync(&data->firmware_work);
#endif
	cancel_delayed_work_sync(&data->reset_work);
#if defined(CONFIG_CCIC_NOTIFIER) || defined(CONFIG_PDIC_NOTIFIER) \
		|| defined(CONFIG_MUIC_NOTIFIER) || defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	cancel_work_sync(&data->cmdoff_work);
	cancel_work_sync(&data->cmdon_work);
#endif

	if (data->irq >= 0)
		free_irq(data->irq, data);
	sensors_unregister(data->dev, grip_sensor_attributes);
	sysfs_remove_group(&data->input_dev->dev.kobj,
				&a96t3x6_attribute_group);
	sensors_remove_symlink(data->input_dev);
	input_unregister_device(data->input_dev);
	input_free_device(data->input_dev);
	kfree(data);

	return 0;
}

static int a96t3x6_suspend(struct device *dev)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	data->resume_called = false;
	GRIP_INFO("%s\n", __func__);
	a96t3x6_sar_only_mode(data, 1);
	a96t3x6_set_debug_work(data, 0, 1000);

	return 0;
}

static int a96t3x6_resume(struct device *dev)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	GRIP_INFO("%s\n", __func__);
	data->resume_called = true;
	a96t3x6_set_debug_work(data, 1, 0);

	return 0;
}

static void a96t3x6_shutdown(struct i2c_client *client)
{
	struct a96t3x6_data *data = i2c_get_clientdata(client);

	a96t3x6_set_debug_work(data, 0, 1000);

	disable_irq(data->irq);
	data->power(data, false);
}

static const struct i2c_device_id a96t3x6_device_id[] = {
	{MODULE_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, a96t3x6_device_id);

#ifdef CONFIG_OF
static const struct of_device_id a96t3x6_match_table[] = {
	{ .compatible = "a96t3x6",},
	{ },
};
#else
#define a96t3x6_match_table NULL
#endif

static const struct dev_pm_ops a96t3x6_pm_ops = {
	.suspend = a96t3x6_suspend,
	.resume = a96t3x6_resume,
};

static struct i2c_driver a96t3x6_driver = {
	.probe = a96t3x6_probe,
	.remove = a96t3x6_remove,
	.shutdown = a96t3x6_shutdown,
	.id_table = a96t3x6_device_id,
	.driver = {
		   .name = MODEL_NAME,
		   .owner = THIS_MODULE,		   	
		   .of_match_table = a96t3x6_match_table,
		   .pm = &a96t3x6_pm_ops
	},
};

static int __init a96t3x6_init(void)
{
	return i2c_add_driver(&a96t3x6_driver);
}

static void __exit a96t3x6_exit(void)
{
	i2c_del_driver(&a96t3x6_driver);
}

module_init(a96t3x6_init);
module_exit(a96t3x6_exit);

MODULE_AUTHOR("Samsung Electronics");
MODULE_DESCRIPTION("Grip sensor driver for A96T3X6 chip");
MODULE_LICENSE("GPL");
