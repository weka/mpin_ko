/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
#ifndef _MPIN_USER_H
#define _MPIN_USER_H

/* open("/dev/"mpin_user_dev, ...) */
#define mpin_user_dev "mpin_user_pages"

/**
 * struct mpin_user_address - Expected pin user space address and size
 * @addr: Address to pin
 * @size: Size of pin address
 */
struct mpin_user_address {
	__u64 addr;		/* virtual address of calling process */
	__u64 size;		/* size in bytes to map */
};

/* MPIN_CMD_PIN: Pin a range of memory */
#define MPIN_CMD_PIN		_IOW('W', 2, struct mpin_user_address)

/* MPIN_CMD_UNPIN: Unpin a range of memory */
#define MPIN_CMD_UNPIN		_IOW('W', 3, struct mpin_user_address)

#ifndef __KERNEL__

#include <sys/ioctl.h>

static inline mpin_user_add(int fd, void *virt, size_t size)
{
	struct mpin_user_address mua = {
		.addr = (__u64)virt,
		.size = size,
	};

	return ioctl(fd, MPIN_CMD_PIN, &mua);
}

static inline mpin_user_remove(int fd, void *virt, size_t size)
{
	struct mpin_user_address mua = {
		.addr = (__u64)virt,
		.size = size,
	};

	return ioctl(fd, MPIN_CMD_UNPIN, &mua);
}

#endif
#endif
