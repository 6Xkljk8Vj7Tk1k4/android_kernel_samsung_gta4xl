/*
 * Copyright (C) 2012-2018 Samsung Electronics, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include "tz_common.h"
#include "tz_deploy_tzar.h"
#include "tz_iwsock.h"
#include "tz_mem.h"
#include "tzlog.h"
#include "teec/iw_messages.h"

#define MAX_LOADING_ATTEMPTS 10
#define MAX_CONNECTION_ATTEMPTS 20
#define CONNECTION_ATTEMPT_TIMEOUT 100

bool use_new_tzar;

__asm__ (
  ".section .init.data,\"aw\"\n"
  "tzdev_tzar_begin:\n"
  ".incbin \"" KBUILD_SRC "/drivers/misc/tzdev/startup.tzar\"\n"
  "tzdev_tzar_end:\n"
  ".previous\n"
);
extern char tzdev_tzar_begin[], tzdev_tzar_end[];

__asm__ (
  ".section .init.data,\"aw\"\n"
  "tzdev_old_tzar_begin:\n"
  ".incbin \"" KBUILD_SRC "/drivers/misc/tzdev/startup_old.tzar\"\n"
  "tzdev_old_tzar_end:\n"
  ".previous\n"
);
extern char tzdev_old_tzar_begin[], tzdev_old_tzar_end[];

/*
 * Both archives sit in .init.data, and the handler below runs in a kthread that may still be
 * retrying after the init memory is freed (4.19 unmaps it: the second attempt of a failed
 * deployment took a paging fault in memcpy). tzdev_deploy_tzar() copies them while it is
 * still init time; the handler frees the copies when it is done.
 */
static void *tzar_copy, *old_tzar_copy;
static size_t tzar_copy_size, old_tzar_copy_size;

enum iw_startup_loader_cmd_type {
	CMD_STARTUP_LOADER_INVALID_ID,
	CMD_STARTUP_LOADER_UPLOAD_CONTAINER,
	CMD_STARTUP_LOADER_REPLY,
	CMD_STARTUP_LOADER_REPLY_UPLOAD_CONTAINER,
	CMD_STARTUP_LOADER_MAX
};

struct cmd_startup_loader_upload_container {
	struct cmd_request base;
	struct shared_buffer_description buf_desc;
} IW_STRUCTURE;

struct cmd_startup_loader_reply_upload_container {
	struct cmd_reply base;
} IW_STRUCTURE;

static __ref int tz_deploy_handler(void *arg)
{
	int ret, retry_cnt;
	size_t size, old_size;
	struct sock_desc *sd;
	struct cmd_startup_loader_upload_container command;
	struct cmd_startup_loader_reply_upload_container reply;
	void *tzar;
	unsigned int attempt;
	bool loaded = false;
	(void)arg;

	/* initialize return value and reply structure */
	ret = -1;
	reply.base.result = -1;

	size = tzar_copy_size;
	old_size = old_tzar_copy_size;
	tzdev_print(0, "[debug] tzar size = %zu, old_tzar size = %zu\n", size, old_size);

	for (retry_cnt = 0; retry_cnt < MAX_LOADING_ATTEMPTS; retry_cnt++) {
		use_new_tzar = retry_cnt % 2 == 0;

		tzar = use_new_tzar ? vmalloc(size) : vmalloc(old_size);
		if (!tzar) {
			tzdev_print(0, "vmalloc fail\n");
			ret = -ENOMEM;
			continue;
		}

		memcpy(tzar, use_new_tzar ? tzar_copy : old_tzar_copy,
		       use_new_tzar ? tzar_copy_size : old_tzar_copy_size);

		size = DIV_ROUND_UP(size, PAGE_SIZE) * PAGE_SIZE;
		old_size = DIV_ROUND_UP(old_size, PAGE_SIZE) * PAGE_SIZE;
		ret = tzdev_mem_register(tzar, use_new_tzar ? size : old_size, 0, NULL, NULL);
		if (ret < 0) {
			tzdev_print(0, "tzdev_mem_register fail, ret=%d\n", ret);
			goto out_tzar;
		}

		command.base.cmd = CMD_STARTUP_LOADER_UPLOAD_CONTAINER;
		command.buf_desc.id = ret;
		command.buf_desc.size = use_new_tzar ? (unsigned int)size : (unsigned int)old_size;

		sd = tz_iwsock_socket(1);
		if (IS_ERR(sd)) {
			ret = PTR_ERR(sd);
			tzdev_print(0, "tz_iwsock_socket fail, ret=%d\n", ret);
			goto out_mem;
		}

		for (attempt = 0; attempt < MAX_CONNECTION_ATTEMPTS; attempt++) {
			ret = tz_iwsock_connect(sd, STARTUP_LOADER_SOCK, 0);
			if (!ret)
				break;

			tzdev_print(0, "Failed to connect to startup loader socket, "
				"error = %d, retrying...\n",
				ret);

			msleep(CONNECTION_ATTEMPT_TIMEOUT);
		}
		if (ret < 0) {
			tzdev_print(0, "tz_iwsock_connect fail, ret=%d\n", ret);
			goto out_sock;
		}

		ret = tz_iwsock_write(sd, &command, sizeof(command), 0);
		if (ret != sizeof(command)) {
			tzdev_print(0, "tz_iwsock_write fail, ret=%d\n", ret);
			goto out_sock;
		}

		ret = tz_iwsock_read(sd, &reply, sizeof(reply), 0);
		if (ret < 0 || reply.base.result != 0) {
			tzdev_print(0, "startup load result, ret=%d\n", reply.base.result);
			tzdev_print(0, "tz_iwsock_read fail, ret=%d\n", ret);
			goto out_sock;
		}

		ret = 0;
		loaded = true;
		tzdev_print(0, "[debug] Startuploader complete\n");
		break;

		out_sock:
			tz_iwsock_release(sd);
		out_mem:
			tzdev_mem_release(command.buf_desc.id);
		out_tzar:
			vfree(tzar);

		msleep(CONNECTION_ATTEMPT_TIMEOUT);
	}
	if (reply.base.result)
		tzdev_print(0, "tzdev: iwsock error. result=%d\n", reply.base.result);

	if (reply.base.base.cmd != CMD_STARTUP_LOADER_REPLY_UPLOAD_CONTAINER)
		tzdev_print(0, "tzdev: iwsock wrong cmd. cmd=%u\n", reply.base.base.cmd);

	if (!loaded) {
		tzdev_print(0, "tzdev: Startuploader failed\n");
		if (!ret)
			ret = -EIO;
	} else {
		tz_iwsock_release(sd);
		tzdev_mem_release(command.buf_desc.id);
		vfree(tzar);
	}

	vfree(tzar_copy);
	vfree(old_tzar_copy);
	tzar_copy = old_tzar_copy = NULL;

	return ret;
}

int __ref tzdev_deploy_tzar(void)
{
	struct task_struct *t;

	tzar_copy_size = tzdev_tzar_end - tzdev_tzar_begin;
	old_tzar_copy_size = tzdev_old_tzar_end - tzdev_old_tzar_begin;
	tzar_copy = vmalloc(tzar_copy_size);
	old_tzar_copy = vmalloc(old_tzar_copy_size);
	if (!tzar_copy || !old_tzar_copy) {
		vfree(tzar_copy);
		vfree(old_tzar_copy);
		tzar_copy = old_tzar_copy = NULL;
		return -ENOMEM;
	}
	memcpy(tzar_copy, tzdev_tzar_begin, tzar_copy_size);
	memcpy(old_tzar_copy, tzdev_old_tzar_begin, old_tzar_copy_size);

	t = kthread_run(tz_deploy_handler, NULL, "tzdev_deploy_tzar");
	if (IS_ERR(t)) {
		printk("Can't start startup.tzar deployment thread\n");
		vfree(tzar_copy);
		vfree(old_tzar_copy);
		tzar_copy = old_tzar_copy = NULL;
		return PTR_ERR(t);
	}
	return 0;
}
