/*
 * Greybus connections
 *
 * Copyright 2014 Google Inc.
 *
 * Released under the GPLv2 only.
 */

#ifndef __CONNECTION_H
#define __CONNECTION_H

#include <linux/list.h>

#include "greybus.h"

struct gb_connection {
	struct greybus_host_device	*hd;
	struct gb_interface		*interface;
	u16				hd_cport_id;
	u16				interface_cport_id;

	struct rb_node			hd_node;
	struct list_head		interface_links;
	enum greybus_protocol		protocol;

	struct list_head		operations;
	struct rb_root			pending;	/* awaiting reponse */
	atomic_t			op_cycle;

	void				*private;
};

struct gb_connection *gb_connection_create(struct gb_interface *interface,
				u16 cport_id, enum greybus_protocol protocol);
void gb_connection_destroy(struct gb_connection *connection);

struct gb_connection *gb_hd_connection_find(struct greybus_host_device *hd,
				u16 cport_id);

u16 gb_connection_operation_id(struct gb_connection *connection);

__printf(2, 3)
void gb_connection_err(struct gb_connection *connection, const char *fmt, ...);

#endif /* __CONNECTION_H */
