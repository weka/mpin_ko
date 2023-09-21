// SPDX-License-Identifier: GPL-2.0
/*
 * mpin_user.c - Memory pinning kernel module
 *
 * Copyright (C) 2023 Weka.io ltd.
 *
 * Author: Zhou Wang <wangzhou1-AT-hisilicon.com>
 * Author: Boaz Harrosh <boaz@weka.io>
 * ~~~~~
 * Original code is from a posted kernel patch:
 * Subject: uacce: Add uacce_ctrl misc device
 * From: Zhou Wang <wangzhou1-AT-hisilicon.com>
 * https://lwn.net/Articles/843432/
 *
 */
/*
 * This module provides an ioctl to pin user memory pages.
 * This is necessary to prevent physical addresses from changing,
 * as user, like dpdk/spdk use the physical addresses in user-mode apps,
 * And also to overcome limitations when using DPDK in systems without the
 * IOMMU enabled.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/fs.h> /* lets us pick up on xarray.h */

#include "mpin_user.h"

/*
 * We assume that there is no need to do pinning at all on kernel versions
 * older than where xarray.h is included, based on our observations. At
 * any rate, our implementation depends on xarray.
 *
 * In order to provide the same API to user space we
 * "pretend" to pin pages anyway on old kernels, but actually do nothing.
 */
#ifndef XA_FLAGS_ALLOC
#undef ENABLE_MPIN
#else
#define ENABLE_MPIN
#endif

#ifdef ENABLE_MPIN

#ifndef FOLL_PIN

static inline
long pin_user_pages(unsigned long start, unsigned long nr_pages,
		unsigned int gup_flags, struct page **pages,
		struct vm_area_struct **vmas)
{
	return get_user_pages(start, nr_pages, gup_flags, pages, vmas);
}

void unpin_user_pages(struct page **pages, unsigned long npages)
{
	uint i;

	for (i = 0; i < npages; ++i)
		put_page(pages[i]);
}

#ifndef	FOLL_LONGTERM
#define	FOLL_LONGTERM 0
#endif

#endif /* missing FOLL_PIN, introduced in Linux 5.6 */

struct mpin_user_container {
	struct xarray array;
};

struct pin_pages {
	unsigned long first;
	unsigned long nr_pages;
	struct page **pages;
};

static int mpin_user_pin_page(struct mpin_user_container *priv, struct mpin_user_address *addr)
{
	unsigned int flags = FOLL_FORCE | FOLL_WRITE | FOLL_LONGTERM;
	unsigned long first, last, nr_pages;
	struct page **pages;
	struct pin_pages *p;
	int ret;

	if (!(addr->addr && addr->size)) {
		pr_err("mpin_user: %s: called-by(%s:%d) addr=0x%llx size=0x%llx\n",
			__func__, current->comm, current->pid, addr->addr, addr->size);
		return 0; /* nothing to pin */
	}

	first = (addr->addr & PAGE_MASK) >> PAGE_SHIFT;
	last = ((addr->addr + addr->size - 1) & PAGE_MASK) >> PAGE_SHIFT;
	nr_pages = last - first + 1;

	pr_debug("mpin_user: %s: called-by(%s:%d) addr=0x%llx size=0x%llx first=0x%lx last=0x%lx nr_pages=0x%lx",
		__func__, current->comm, current->pid, addr->addr, addr->size, first, last,
		nr_pages);

	pages = vmalloc(nr_pages * sizeof(struct page *));
	if (pages == NULL) {
		pr_err("mpin_user: %s called-by(%s:%d) addr=0x%llx size=0x%llx first=0x%lx last=0x%lx nr_pages=0x%lx",
			__func__, current->comm, current->pid, addr->addr, addr->size, first, last,
			nr_pages);
		return -ENOMEM;
	}

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (p == NULL) {
		ret = -ENOMEM;
		goto free;
	}

	ret = pin_user_pages(addr->addr & PAGE_MASK, nr_pages, flags, pages, NULL);
	if (ret != nr_pages) {
		pr_err("uacce: Failed to pin page\n");
		goto free_p;
	}

	p->first = first;
	p->nr_pages = nr_pages;
	p->pages = pages;

	ret = xa_err(xa_store(&priv->array, p->first, p, GFP_KERNEL));
	if (ret != 0)
		goto unpin_pages;
	return 0;

unpin_pages:
	unpin_user_pages(pages, nr_pages);
free_p:
	kfree(p);
free:
	vfree(pages);
	return ret;
}

static int mpin_user_unpin_page(struct mpin_user_container *priv,
				struct mpin_user_address *addr)
{
	unsigned long first, last, nr_pages;
	struct pin_pages *p;

	first = (addr->addr & PAGE_MASK) >> PAGE_SHIFT;
	last = ((addr->addr + addr->size - 1) & PAGE_MASK) >> PAGE_SHIFT;
	nr_pages = last - first + 1;

	/* find pin_pages */
	p = xa_load(&priv->array, first);
	if (p == NULL)
		return -ENODEV;
	if (p->nr_pages != nr_pages)
		return -EINVAL;

	/* unpin */
	unpin_user_pages(p->pages, p->nr_pages);

	/* release resource */
	xa_erase(&priv->array, first);
	vfree(p->pages);
	kfree(p);

	return 0;
}

#endif /* ENABLE_MPIN */

static int mpin_open(struct inode *inode, struct file *file)
{
#ifdef ENABLE_MPIN
	struct mpin_user_container *p;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (p == NULL)
		return -ENOMEM;

	file->private_data = p;
	xa_init(&p->array);
#endif /* ENABLE_MPIN */

	return 0;
}

static int mpin_release(struct inode *inode, struct file *file)
{
#ifdef ENABLE_MPIN
	struct mpin_user_container *priv = file->private_data;
	struct pin_pages *p;
	unsigned long idx;

	xa_for_each(&priv->array, idx, p) {
		unpin_user_pages(p->pages, p->nr_pages);
		xa_erase(&priv->array, p->first);
		vfree(p->pages);
		kfree(p);
	}

	xa_destroy(&priv->array);
	kfree(priv);
#endif /* ENABLE_MPIN */

	return 0;
}
static long mpin_unl_ioctl(struct file *filep, unsigned int cmd,
				unsigned long arg)
{
#ifdef ENABLE_MPIN
	struct mpin_user_container *p = filep->private_data;
	struct mpin_user_address addr;

	switch (cmd) {
	case MPIN_CMD_PIN:
		if (copy_from_user(&addr, (void __user *)arg, sizeof(struct mpin_user_address)))
			return -EFAULT;
		return mpin_user_pin_page(p, &addr);

	case MPIN_CMD_UNPIN:
		if (copy_from_user(&addr, (void __user *)arg, sizeof(struct mpin_user_address)))
			return -EFAULT;
		return mpin_user_unpin_page(p, &addr);

	default:
		return -EINVAL;
	}
#else /* ! ENABLE_MPIN */
	switch (cmd) {
	case MPIN_CMD_PIN:
		return 0;
	case MPIN_CMD_UNPIN:
		return 0;
	default:
		return -EINVAL;
	}
#endif /* ! ENABLE_MPIN */
}

static const struct file_operations mpin_fops = {
	.owner = THIS_MODULE,
	.open = mpin_open,
	.release = mpin_release,
	.unlocked_ioctl = mpin_unl_ioctl,
};

static struct miscdevice mpin_misc_device = {
	.name = MPIN_USER_N,
	.minor = MISC_DYNAMIC_MINOR,
	.fops = &mpin_fops,
};

static int __init mpin_misc_init(void)
{
	int error;

	error = misc_register(&mpin_misc_device);
	if (error) {
		pr_err("mpin: misc_register failed!!!\n");
		return error;
	}
	return 0;
}

static void __exit mpin_misc_exit(void)
{
	misc_deregister(&mpin_misc_device);
	pr_info("misc_register exit done!!!\n");
}

module_init(mpin_misc_init)
module_exit(mpin_misc_exit)

MODULE_LICENSE("GPL");
MODULE_VERSION(__stringify(MPIN_USER_VERSION));

#pragma message("MPIN_USER_VERSION " MPIN_USER_VERSION)
