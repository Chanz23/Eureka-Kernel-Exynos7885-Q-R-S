/*
 * I2C bridge driver for the Greybus "generic" I2C module.
 *
 * Copyright 2014 Google Inc.
 *
 * Released under the GPLv2 only.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/serial.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/idr.h>
#include "greybus.h"

#define GB_TTY_MAJOR	180	/* FIXME use a real number!!! */
#define GB_NUM_MINORS	255	/* 255 is enough for anyone... */

struct gb_tty {
	struct tty_port port;
	struct greybus_device *gdev;
	int cport;
	unsigned int minor;
	unsigned char clocal;
	unsigned int throttled:1;
	unsigned int throttle_req:1;
	spinlock_t read_lock;
	spinlock_t write_lock;
	// FIXME locking!!!
};

static const struct greybus_device_id id_table[] = {
	{ GREYBUS_DEVICE(0x45, 0x45) },	/* make shit up */
	{ },	/* terminating NULL entry */
};

static struct tty_driver *gb_tty_driver;
static DEFINE_IDR(tty_minors);
static DEFINE_MUTEX(table_lock);

static struct gb_tty *get_gb_by_minor(unsigned minor)
{
	struct gb_tty *gb_tty;

	mutex_lock(&table_lock);
	gb_tty = idr_find(&tty_minors, minor);
	mutex_unlock(&table_lock);
	return gb_tty;
}

static int alloc_minor(struct gb_tty *gb_tty)
{
	int minor;

	mutex_lock(&table_lock);
	minor = idr_alloc(&tty_minors, gb_tty, 0, 0, GFP_KERNEL);
	if (minor < 0)
		goto error;
	gb_tty->minor = minor;
error:
	mutex_unlock(&table_lock);
	return minor;
}

static void release_minor(struct gb_tty *gb_tty)
{
	mutex_lock(&table_lock);
	idr_remove(&tty_minors, gb_tty->minor);
	mutex_unlock(&table_lock);
}

static int gb_tty_install(struct tty_driver *driver, struct tty_struct *tty)
{
	struct gb_tty *gb_tty;
	int retval;

	gb_tty = get_gb_by_minor(tty->index);
	if (!gb_tty)
		return -ENODEV;

	retval = tty_standard_install(driver, tty);
	if (retval)
		goto error;

	tty->driver_data = gb_tty;
	return 0;
error:
	tty_port_put(&gb_tty->port);
	return retval;
}

static int gb_tty_open(struct tty_struct *tty, struct file *file)
{
	struct gb_tty *gb_tty = tty->driver_data;

	return tty_port_open(&gb_tty->port, tty, file);
}

static void gb_tty_close(struct tty_struct *tty, struct file *file)
{
	struct gb_tty *gb_tty = tty->driver_data;

	tty_port_close(&gb_tty->port, tty, file);
}

static void gb_tty_cleanup(struct tty_struct *tty)
{
	struct gb_tty *gb_tty = tty->driver_data;

	tty_port_put(&gb_tty->port);
}

static void gb_tty_hangup(struct tty_struct *tty)
{
	struct gb_tty *gb_tty = tty->driver_data;

	tty_port_hangup(&gb_tty->port);
}

static int gb_tty_write(struct tty_struct *tty, const unsigned char *buf,
			int count)
{
	struct gb_tty *gb_tty = tty->driver_data;

	// FIXME - actually implement...

	return 0;
}

static int gb_tty_write_room(struct tty_struct *tty)
{
	struct gb_tty *gb_tty = tty->driver_data;

	// FIXME - how much do we want to say we have room for?
	return 0;
}

static int gb_tty_chars_in_buffer(struct tty_struct *tty)
{
	struct gb_tty *gb_tty = tty->driver_data;

	// FIXME - how many left to send?
	return 0;
}

static void gb_tty_throttle(struct tty_struct *tty)
{
	struct gb_tty *gb_tty = tty->driver_data;

	spin_lock_irq(&gb_tty->read_lock);
	gb_tty->throttle_req = 1;
	spin_unlock_irq(&gb_tty->read_lock);
}

static void gb_tty_unthrottle(struct tty_struct *tty)
{
	struct gb_tty *gb_tty = tty->driver_data;
	unsigned int was_throttled;

	spin_lock_irq(&gb_tty->read_lock);
	was_throttled = gb_tty->throttled;
	gb_tty->throttle_req = 0;
	gb_tty->throttled = 0;
	spin_unlock_irq(&gb_tty->read_lock);

	if (was_throttled) {
		// FIXME - send more data
	}
}


static const struct tty_operations gb_ops = {
	.install =		gb_tty_install,
	.open =			gb_tty_open,
	.close =		gb_tty_close,
	.cleanup =		gb_tty_cleanup,
	.hangup =		gb_tty_hangup,
	.write =		gb_tty_write,
	.write_room =		gb_tty_write_room,
	.ioctl =		gb_tty_ioctl,
	.throttle =		gb_tty_throttle,
	.unthrottle =		gb_tty_unthrottle,
	.chars_in_buffer =	gb_tty_chars_in_buffer,
	.break_ctl =		gb_tty_break_ctl,
	.set_termios =		gb_tty_set_termios,
	.tiocmget =		gb_tty_tiocmget,
	.tiocmset =		gb_tty_tiocmset,
};


static int tty_gb_probe(struct greybus_device *gdev, const struct greybus_device_id *id)
{
	struct gb_tty *gb_tty;
	struct device *tty_dev;
	int retval;
	int minor;

	gb_tty = devm_kzalloc(&gdev->dev, sizeof(*gb_tty), GFP_KERNEL);
	if (!gb_tty)
		return -ENOMEM;

	minor = alloc_minor(gb_tty);
	if (minor == GB_NUM_MINORS) {
		dev_err(&gdev->dev, "no more free minor numbers\n");
		return -ENODEV;
	}

	gb_tty->minor = minor;
	gb_tty->gdev = gdev;
	spin_lock_init(&gb_tty->write_lock);
	spin_lock_init(&gb_tty->read_lock);

	/* FIXME - allocate gb buffers */

	greybus_set_drvdata(gdev, gb_tty);

	tty_dev = tty_port_register_device(&gb_tty->port, gb_tty_driver, minor,
					   &gdev->dev);
	if (IS_ERR(tty_dev)) {
		retval = PTR_ERR(tty_dev);
		goto error;
	}

	return 0;
error:
	release_minor(gb_tty);
	return retval;
}

static void tty_gb_disconnect(struct greybus_device *gdev)
{
	struct gb_tty *gb_tty = greybus_get_drvdata(gdev);
	struct tty_struct *tty;

	tty = tty_port_tty_get(&gb_tty->port);
	if (tty) {
		tty_vhangup(tty);
		tty_kref_put(tty);
	}
	/* FIXME - stop all traffic */

	tty_unregister_device(gb_tty_driver, gb_tty->minor);

	tty_port_put(&gb_tty->port);
}

static struct greybus_driver tty_gb_driver = {
	.probe =	tty_gb_probe,
	.disconnect =	tty_gb_disconnect,
	.id_table =	id_table,
};


static int __init gb_tty_init(void)
{
	int retval;

	gb_tty_driver = alloc_tty_driver(GB_NUM_MINORS);
	if (!gb_tty_driver)
		return -ENOMEM;

	gb_tty_driver->driver_name = "gb";
	gb_tty_driver->name = "ttyGB";
	gb_tty_driver->major = GB_TTY_MAJOR;
	gb_tty_driver->minor_start = 0;
	gb_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	gb_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	gb_tty_driver->flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV;
	gb_tty_driver->init_termios = tty_std_termios;
	gb_tty_driver->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	tty_set_operations(gb_tty_driver, &gb_ops);

	retval = tty_register_driver(gb_tty_driver);
	if (retval) {
		put_tty_driver(gb_tty_driver);
		return retval;
	}

	retval = greybus_register(&tty_gb_driver);
	if (retval) {
		tty_unregister_driver(gb_tty_driver);
		put_tty_driver(gb_tty_driver);
	}
	return retval;
}

static void __exit gb_tty_exit(void)
{
	greybus_deregister(&tty_gb_driver);
	tty_unregister_driver(gb_tty_driver);
	put_tty_driver(gb_tty_driver);
}

module_init(gb_tty_init);
module_exit(gb_tty_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Greg Kroah-Hartman <gregkh@linuxfoundation.org>");
