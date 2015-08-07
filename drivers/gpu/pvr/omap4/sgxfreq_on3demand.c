/*
 * Copyright (C) 2012 Texas Instruments, Inc
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/sysfs.h>
#include <linux/vmalloc.h>
#include <asm/io.h>
#include "sgxfreq.h"

static int on3demand_start(struct sgxfreq_sgx_data *data);
static void on3demand_stop(void);
static void on3demand_predict(void);
static void on3demand_frame_done(void);
static void on3demand_active(void);
static void on3demand_timeout(struct work_struct *work);


static struct sgxfreq_governor on3demand_gov = {
	.name =	"on3demand",
	.gov_start = on3demand_start,
	.gov_stop = on3demand_stop,
	.sgx_frame_done = on3demand_frame_done,
	.sgx_active = on3demand_active,
};

static struct on3demand_data {
	unsigned int up_threshold;
	unsigned int down_threshold;
	unsigned int history_size;
	unsigned int low_load_cnt;
	unsigned int poll_interval;
	unsigned int frame_done_deadline;
	bool polling_enabled;
	struct delayed_work work;
	struct mutex mutex;
} odd;

#define ON3DEMAND_DEFAULT_UP_THRESHOLD			95
#define ON3DEMAND_DEFAULT_DOWN_THRESHOLD		75
#define ON3DEMAND_DEFAULT_HISTORY_SIZE_THRESHOLD	10
/* For Live wallpaper frame done at interval of ~64ms */
#define ON3DEMAND_DEFAULT_POLL_INTERVAL			75

/*FIXME: This should be dynamic and queried from platform */
#define ON3DEMAND_DEFAULT_FRAME_DONE_DEADLINE_MS 16


/*********************** begin sysfs interface ***********************/

extern struct kobject *sgxfreq_kobj;

static ssize_t show_down_threshold(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", odd.down_threshold);
}

static ssize_t store_down_threshold(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned int thres;

	ret = sscanf(buf, "%u", &thres);
	if (ret != 1)
		return -EINVAL;

	if (thres > 100) thres = 100;

	mutex_lock(&odd.mutex);

	odd.down_threshold = thres;
	odd.low_load_cnt = 0;

	mutex_unlock(&odd.mutex);

	return count;
}

static ssize_t show_up_threshold(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", odd.up_threshold);
}

static ssize_t store_up_threshold(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned int thres;

	ret = sscanf(buf, "%u", &thres);
	if (ret != 1)
		return -EINVAL;

	if (thres > 100) thres = 100;

	mutex_lock(&odd.mutex);

	odd.up_threshold = thres;
	odd.low_load_cnt = 0;

	mutex_unlock(&odd.mutex);

	return count;
}

static ssize_t show_history_size(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", odd.history_size);
}

static ssize_t store_history_size(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned int size;

	ret = sscanf(buf, "%u", &size);
	if (ret != 1)
		return -EINVAL;

	if (size < 1) size = 1;

	mutex_lock(&odd.mutex);

	odd.history_size = size;
	odd.low_load_cnt = 0;

	mutex_unlock(&odd.mutex);

	return count;
}

static ssize_t show_poll_interval(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", odd.poll_interval);
}

static ssize_t store_poll_interval(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned int interval;

	ret = sscanf(buf, "%u", &interval);
	if (ret != 1)
		return -EINVAL;

	if (interval < 1) interval = 1;

	mutex_lock(&odd.mutex);

	odd.poll_interval = interval;
	odd.low_load_cnt = 0;

	mutex_unlock(&odd.mutex);

	return count;
}

static ssize_t show_frame_done_deadline(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", odd.frame_done_deadline);
}

static ssize_t store_frame_done_deadline(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned int deadline;

	ret = sscanf(buf, "%u", &deadline);
	if (ret != 1)
		return -EINVAL;

	if (deadline < 1) deadline = 1;

	mutex_lock(&odd.mutex);

	odd.frame_done_deadline = deadline;
	odd.low_load_cnt = 0;

	mutex_unlock(&odd.mutex);

	return count;
}

static DEVICE_ATTR(down_threshold, 0644,
	show_down_threshold, store_down_threshold);
static DEVICE_ATTR(up_threshold, 0644,
	show_up_threshold, store_up_threshold);
static DEVICE_ATTR(history_size, 0644,
	show_history_size, store_history_size);
static DEVICE_ATTR(poll_interval, 0644,
	show_poll_interval, store_poll_interval);
static DEVICE_ATTR(frame_done_deadline, 0644,
	show_frame_done_deadline, store_frame_done_deadline);

static struct attribute *on3demand_attributes[] = {
	&dev_attr_down_threshold.attr,
	&dev_attr_up_threshold.attr,
	&dev_attr_history_size.attr,
	&dev_attr_poll_interval.attr,
	&dev_attr_frame_done_deadline.attr,
	NULL
};

static struct attribute_group on3demand_attr_group = {
	.attrs = on3demand_attributes,
	.name = "on3demand",
};
/************************ end sysfs interface ************************/

int on3demand_init(void)
{
	int ret;

	mutex_init(&odd.mutex);

	ret = sgxfreq_register_governor(&on3demand_gov);
	if (ret)
		return ret;

	return 0;
}

int on3demand_deinit(void)
{
	return 0;
}

static int on3demand_start(struct sgxfreq_sgx_data *data)
{
	int ret;

	odd.up_threshold = ON3DEMAND_DEFAULT_UP_THRESHOLD;
	odd.down_threshold = ON3DEMAND_DEFAULT_DOWN_THRESHOLD;
	odd.history_size = ON3DEMAND_DEFAULT_HISTORY_SIZE_THRESHOLD;
	odd.low_load_cnt = 0;
	odd.poll_interval = ON3DEMAND_DEFAULT_POLL_INTERVAL;
	odd.polling_enabled = false;
	odd.frame_done_deadline = ON3DEMAND_DEFAULT_FRAME_DONE_DEADLINE_MS;

	INIT_DELAYED_WORK(&odd.work, on3demand_timeout);

	ret = sysfs_create_group(sgxfreq_kobj, &on3demand_attr_group);
	if (ret)
		return ret;

	return 0;
}

static void on3demand_stop(void)
{
	cancel_delayed_work_sync(&odd.work);
	sysfs_remove_group(sgxfreq_kobj, &on3demand_attr_group);
}

static void on3demand_predict(void)
{
	unsigned long freq;
	int load = sgxfreq_get_load();

	/*
	 * If SGX was active for longer than frame display time (1/fps),
	 * scale to highest possible frequency.
	 */
	if (sgxfreq_get_delta_active() > odd.frame_done_deadline) {
		odd.low_load_cnt = 0;
		sgxfreq_set_freq_request(sgxfreq_get_freq_max());
	}

	/* Scale GPU frequency on purpose */
	if (load >= odd.up_threshold) {
		odd.low_load_cnt = 0;
		sgxfreq_set_freq_request(sgxfreq_get_freq_max());
	} else if (load <= odd.down_threshold) {
		if (odd.low_load_cnt == odd.history_size) {
			/* Convert load to frequency */
			freq = (sgxfreq_get_freq() * load) / 10;
			sgxfreq_set_freq_request(freq);
			odd.low_load_cnt = 0;
		} else {
			odd.low_load_cnt++;
		}
	} else {
		odd.low_load_cnt = 0;
	}
}


static void on3demand_active(void)
{
	if (!odd.polling_enabled) {
		sgxfreq_set_freq_request(sgxfreq_get_freq_max());
		odd.low_load_cnt = 0;
		odd.polling_enabled = true;
		schedule_delayed_work(&odd.work, odd.poll_interval * HZ/1000);
	}

}

static void on3demand_frame_done(void)
{
	if (odd.polling_enabled) {
		cancel_delayed_work_sync(&odd.work);
		schedule_delayed_work(&odd.work, odd.poll_interval * HZ/1000);
	}
	on3demand_predict();
}

static void on3demand_timeout(struct work_struct *work)
{
	/*
	 * If sgx was idle all throughout timer disable polling and
	 * enable it on next sgx active event
	 */
	if (!sgxfreq_get_delta_active()) {
		sgxfreq_set_freq_request(sgxfreq_get_freq_min());
		odd.low_load_cnt = 0;
		odd.polling_enabled = false;
	} else {
		on3demand_predict();
		odd.polling_enabled = true;
		schedule_delayed_work(&odd.work, odd.poll_interval * HZ/1000);
	}
}
