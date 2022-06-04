/*
 * Greybus module manifest parsing
 *
 * Copyright 2014 Google Inc.
 *
 * Released under the GPLv2 only.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>

#include "greybus.h"

/*
 * We scan the manifest once to identify where all the descriptors
 * are.  The result is a list of these manifest_desc structures.  We
 * then pick through them for what we're looking for (starting with
 * the module descriptor).  As each is processed we remove it from
 * the list.  When we're done the list should (probably) be empty.
 */
struct manifest_desc {
	struct list_head		links;

	size_t				size;
	void				*data;
	enum greybus_descriptor_type	type;
};

static LIST_HEAD(manifest_descs);

static void release_manifest_descriptor(struct manifest_desc *descriptor)
{
	list_del(&descriptor->links);
	kfree(descriptor);
}

static void release_manifest_descriptors(void)
{
	struct manifest_desc *descriptor;
	struct manifest_desc *next;

	list_for_each_entry_safe(descriptor, next, &manifest_descs, links)
		release_manifest_descriptor(descriptor);
}

/*
 * Validate the given descriptor.  Its reported size must fit within
 * the number of bytes reamining, and it must have a recognized
 * type.  Check that the reported size is at least as big as what
 * we expect to see.  (It could be bigger, perhaps for a new version
 * of the format.)
 *
 * Returns the number of bytes consumed by the descriptor, or a
 * negative errno.
 */
static int identify_descriptor(struct greybus_descriptor *desc, size_t size)
{
	struct greybus_descriptor_header *desc_header = &desc->header;
	struct manifest_desc *descriptor;
	int desc_size;
	size_t expected_size;

	if (size < sizeof(*desc_header)) {
		pr_err("manifest too small\n");
		return -EINVAL;		/* Must at least have header */
	}

	desc_size = (int)le16_to_cpu(desc_header->size);
	if ((size_t)desc_size > size) {
		pr_err("descriptor too big\n");
		return -EINVAL;
	}

	switch (desc_header->type) {
	case GREYBUS_TYPE_MODULE:
		if (desc_size < sizeof(struct greybus_descriptor_module)) {
			pr_err("module descriptor too small (%u)\n",
				desc_size);
			return -EINVAL;
		}
		break;
	case GREYBUS_TYPE_DEVICE:
		break;
	case GREYBUS_TYPE_CLASS:
		pr_err("class descriptor found (ignoring)\n");
		break;
	case GREYBUS_TYPE_STRING:
		expected_size = sizeof(struct greybus_descriptor_header);
		expected_size += sizeof(struct greybus_descriptor_string);
		expected_size += (size_t)desc->string.length;
		if (desc_size < expected_size) {
			pr_err("string descriptor too small (%u)\n",
				desc_size);
			return -EINVAL;
		}
		break;
	case GREYBUS_TYPE_CPORT:
		if (desc_size < sizeof(struct greybus_descriptor_cport)) {
			pr_err("cport descriptor too small (%u)\n",
				desc_size);
			return -EINVAL;
		}
		break;
	case GREYBUS_TYPE_INVALID:
	default:
		pr_err("invalid descriptor type (%hhu)\n", desc_header->type);
		return -EINVAL;
	}

	descriptor = kzalloc(sizeof(*descriptor), GFP_KERNEL);
	if (!descriptor)
		return -ENOMEM;

	descriptor->size = desc_size;
	descriptor->data = desc;
	descriptor->type = desc_header->type;
	list_add_tail(&descriptor->links, &manifest_descs);

	return desc_size;
}

/*
 * Find the string descriptor having the given id, validate it, and
 * allocate a duplicate copy of it.  The duplicate has an extra byte
 * which guarantees the returned string is NUL-terminated.
 *
 * String index 0 is valid (it represents "no string"), and for
 * that a null pointer is returned.
 *
 * Otherwise returns a pointer to a newly-allocated copy of the
 * descriptor string, or an error-coded pointer on failure.
 */
static char *gb_string_get(u8 string_id)
{
	struct greybus_descriptor_string *desc_string;
	struct manifest_desc *descriptor;
	bool found = false;
	char *string;

	/* A zero string id means no string (but no error) */
	if (!string_id)
		return NULL;

	list_for_each_entry(descriptor, &manifest_descs, links) {
		struct greybus_descriptor *desc;

		if (descriptor->type != GREYBUS_TYPE_STRING)
			continue;

		desc = descriptor->data;
		desc_string = &desc->string;
		if (desc_string->id == string_id) {
			found = true;
			break;
		}
	}
	if (!found)
		return ERR_PTR(-ENOENT);

	/* Allocate an extra byte so we can guarantee it's NUL-terminated */
	string = kmemdup(&desc_string->string, (size_t)desc_string->length + 1,
				GFP_KERNEL);
	if (!string)
		return ERR_PTR(-ENOMEM);
	string[desc_string->length] = '\0';

	/* Ok we've used this string, so we're done with it */
	release_manifest_descriptor(descriptor);

	return string;
}

struct gb_module *gb_manifest_parse_module(struct manifest_desc *module_desc)
{
	struct greybus_descriptor *desc = module_desc->data;
	struct greybus_descriptor_module *desc_module = &desc->module;
	struct gb_module *gmod;

	gmod = kzalloc(sizeof(*gmod), GFP_KERNEL);
	if (!gmod)
		return NULL;

	/* Handle the strings first--they can fail */
	gmod->vendor_string = gb_string_get(desc_module->vendor_stringid);
	if (IS_ERR(gmod->vendor_string)) {
		kfree(gmod);
		return NULL;
	}
	gmod->product_string = gb_string_get(desc_module->product_stringid);
	if (IS_ERR(gmod->product_string)) {
		kfree(gmod->vendor_string);
		kfree(gmod);
		return NULL;
	}

	gmod->vendor = le16_to_cpu(desc_module->vendor);
	gmod->product = le16_to_cpu(desc_module->product);
	gmod->version = le16_to_cpu(desc_module->version);
	gmod->serial_number = le64_to_cpu(desc_module->serial_number);

	/* Release the module descriptor, now that we're done with it */
	release_manifest_descriptor(module_desc);

	return gmod;
}

/*
 * Parse a buffer containing a module manifest.
 *
 * If we find anything wrong with the content/format of the buffer
 * we reject it.
 *
 * The first requirement is that the manifest's version is
 * one we can parse.
 *
 * We make an initial pass through the buffer and identify all of
 * the descriptors it contains, keeping track for each its type
 * and the location size of its data in the buffer.
 *
 * Next we scan the descriptors, looking for a module descriptor;
 * there must be exactly one of those.  When found, we record the
 * information it contains, and then remove that descriptor (and any
 * string descriptors it refers to) from further consideration.
 *
 * After that we look for the module's interfaces--there must be at
 * least one of those.
 *
 * Return a pointer to an initialized gb_module structure
 * representing the content of the module manifest, or a null
 * pointer if an error occurs.
 */
struct gb_module *gb_manifest_parse(void *data, size_t size)
{
	struct greybus_manifest *manifest;
	struct greybus_manifest_header *header;
	struct greybus_descriptor *desc;
	struct manifest_desc *descriptor;
	struct manifest_desc *module_desc = NULL;
	struct gb_module *gmod;
	u16 manifest_size;
	u32 found = 0;

	/* we have to have at _least_ the manifest header */
	if (size <= sizeof(manifest->header)) {
		pr_err("short manifest (%zu)\n", size);
		return NULL;
	}

	/* Make sure the size is right */
	manifest = data;
	header = &manifest->header;
	manifest_size = le16_to_cpu(header->size);
	if (manifest_size != size) {
		pr_err("manifest size mismatch %zu != %hu\n",
			size, manifest_size);
		return NULL;
	}

	/* Validate major/minor number */
	if (header->version_major > GREYBUS_VERSION_MAJOR) {
		pr_err("manifest version too new (%hhu.%hhu > %hhu.%hhu)\n",
			header->version_major, header->version_minor,
			GREYBUS_VERSION_MAJOR, GREYBUS_VERSION_MINOR);
		return NULL;
	}

	/* OK, find all the descriptors */
	desc = (struct greybus_descriptor *)(header + 1);
	size -= sizeof(*header);
	while (size) {
		int desc_size;

		desc_size = identify_descriptor(desc, size);
		if (desc_size <= 0) {
			if (!desc_size)
				pr_err("zero-sized manifest descriptor\n");
			goto out_err;
		}
		desc = (struct greybus_descriptor *)((char *)desc + desc_size);
		size -= desc_size;
	}

	/* There must be a single module descriptor */
	list_for_each_entry(descriptor, &manifest_descs, links) {
		if (descriptor->type == GREYBUS_TYPE_MODULE)
			if (!found++)
				module_desc = descriptor;
	}
	if (found != 1) {
		pr_err("manifest must have 1 module descriptor (%u found)\n",
			found);
		goto out_err;
	}

	/* Parse the module manifest, starting with the module descriptor */
	gmod = gb_manifest_parse_module(module_desc);

	/*
	 * We really should have no remaining descriptors, but we
	 * don't know what newer format manifests might leave.
	 */
	if (!list_empty(&manifest_descs)) {
		pr_info("excess descriptors in module manifest\n");
		release_manifest_descriptors();
	}

	return gmod;
out_err:
	release_manifest_descriptors();

	return NULL;
}
