/*
 * Greybus Vibrator protocol driver.
 *
 * Copyright 2014 Google Inc.
 * Copyright 2014 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/idr.h>
#include "greybus.h"

struct gb_vibrator_device {
	struct gb_connection	*connection;
	struct device		*dev;
	int			minor;		/* vibrator minor number */
	u8			version_major;
	u8			version_minor;
};

/* Version of the Greybus vibrator protocol we support */
#define	GB_VIBRATOR_VERSION_MAJOR		0x00
#define	GB_VIBRATOR_VERSION_MINOR		0x01

/* Greybus Vibrator request types */
#define	GB_VIBRATOR_TYPE_INVALID		0x00
#define	GB_VIBRATOR_TYPE_PROTOCOL_VERSION	0x01
#define	GB_VIBRATOR_TYPE_ON			0x02
#define	GB_VIBRATOR_TYPE_OFF			0x03
#define	GB_VIBRATOR_TYPE_RESPONSE		0x80	/* OR'd with rest */

struct gb_vibrator_proto_version_response {
	__u8	major;
	__u8	minor;
};

struct gb_vibrator_on_request {
	__le16	timeout_ms;
};

/*
 * This request only uses the connection field, and if successful,
 * fills in the major and minor protocol version of the target.
 */
static int get_version(struct gb_vibrator_device *vib)
{
	struct gb_connection *connection = vib->connection;
	struct gb_vibrator_proto_version_response version_response;
	int retval;

	retval = gb_operation_sync(connection,
				   GB_VIBRATOR_TYPE_PROTOCOL_VERSION,
				   NULL, 0,
				   &version_response, sizeof(version_response));
	if (retval)
		return retval;

	if (version_response.major > GB_VIBRATOR_VERSION_MAJOR) {
		dev_err(&connection->dev,
			"unsupported major version (%hhu > %hhu)\n",
			version_response.major, GB_VIBRATOR_VERSION_MAJOR);
		return -ENOTSUPP;
	}

	vib->version_major = version_response.major;
	vib->version_minor = version_response.minor;
	return 0;
}

static int turn_on(struct gb_vibrator_device *vib, u16 timeout_ms)
{
	struct gb_vibrator_on_request request;

	request.timeout_ms = cpu_to_le16(timeout_ms);
	return gb_operation_sync(vib->connection, GB_VIBRATOR_TYPE_ON,
				 &request, sizeof(request), NULL, 0);
}

static int turn_off(struct gb_vibrator_device *vib)
{
	return gb_operation_sync(vib->connection, GB_VIBRATOR_TYPE_OFF,
				 NULL, 0, NULL, 0);
}

static ssize_t timeout_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct gb_vibrator_device *vib = dev_get_drvdata(dev);
	unsigned long val;
	int retval;

	retval = kstrtoul(buf, 10, &val);
	if (retval < 0) {
		dev_err(dev, "could not parse timeout value %d\n", retval);
		return retval;
	}

	if (val < 0)
		return -EINVAL;
	if (val)
		retval = turn_on(vib, (u16)val);
	else
		retval = turn_off(vib);
	if (retval)
		return retval;

	return count;
}
static DEVICE_ATTR_WO(timeout);

static struct attribute *vibrator_attrs[] = {
	&dev_attr_timeout.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vibrator);

static struct class vibrator_class = {
	.name		= "vibrator",
	.owner		= THIS_MODULE,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,11,0)
	.dev_groups	= vibrator_groups,
#endif
};

static DEFINE_IDR(minors);

static int gb_vibrator_connection_init(struct gb_connection *connection)
{
	struct gb_vibrator_device *vib;
	struct device *dev;
	int retval;

	vib = kzalloc(sizeof(*vib), GFP_KERNEL);
	if (!vib)
		return -ENOMEM;

	vib->connection = connection;
	connection->private = vib;

	retval = get_version(vib);
	if (retval)
		goto error;

	/*
	 * For now we create a device in sysfs for the vibrator, but odds are
	 * there is a "real" device somewhere in the kernel for this, but I
	 * can't find it at the moment...
	 */
	vib->minor = idr_alloc(&minors, vib, 0, 0, GFP_KERNEL);
	if (vib->minor < 0) {
		retval = vib->minor;
		goto error;
	}
	dev = device_create(&vibrator_class, &connection->dev, MKDEV(0, 0), vib,
			    "vibrator%d", vib->minor);
	if (IS_ERR(dev)) {
		retval = -EINVAL;
		goto error;
	}
	vib->dev = dev;

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,11,0)
	/*
	 * Newer kernels handle this in a race-free manner, by the dev_groups
	 * field in the struct class up above.  But for older kernels, we need
	 * to "open code this :(
	 */
	retval = sysfs_create_group(&dev->kobj, vibrator_groups[0]);
	if (retval) {
		device_unregister(dev);
		goto error;
	}
#endif

	return 0;

error:
	kfree(vib);
	return retval;
}

static void gb_vibrator_connection_exit(struct gb_connection *connection)
{
	struct gb_vibrator_device *vib = connection->private;

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,11,0)
	sysfs_remove_group(&vib->dev->kobj, vibrator_groups[0]);
#endif
	idr_remove(&minors, vib->minor);
	device_unregister(vib->dev);
	kfree(vib);
}

static struct gb_protocol vibrator_protocol = {
	.id			= GREYBUS_PROTOCOL_VIBRATOR,
	.major			= 0,
	.minor			= 1,
	.connection_init	= gb_vibrator_connection_init,
	.connection_exit	= gb_vibrator_connection_exit,
	.request_recv		= NULL,	/* no incoming requests */
};

bool gb_vibrator_protocol_init(void)
{
	int retval;

	retval = class_register(&vibrator_class);
	if (retval)
		return retval;

	return gb_protocol_register(&vibrator_protocol);
}

void gb_vibrator_protocol_exit(void)
{
	gb_protocol_deregister(&vibrator_protocol);
	class_unregister(&vibrator_class);
}
