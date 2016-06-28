/*
 * Copyright (c) 2015, Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2015, Intel Corp., Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <rdma/fi_errno.h>
#include <rdma/fi_log.h>

#include "fi.h"

static const char * const log_subsys[] = {
	[FI_LOG_CORE] = "core",
	[FI_LOG_FABRIC] = "fabric",
	[FI_LOG_DOMAIN] = "domain",
	[FI_LOG_EP_CTRL] = "ep_ctrl",
	[FI_LOG_EP_DATA] = "ep_data",
	[FI_LOG_AV] = "av",
	[FI_LOG_CQ] = "cq",
	[FI_LOG_EQ] = "eq",
	[FI_LOG_MR] = "mr",
	[FI_LOG_SUBSYS_MAX] = NULL
};

static const char * const log_levels[] = {
	[FI_LOG_WARN] = "warn",
	[FI_LOG_TRACE] = "trace",
	[FI_LOG_INFO] = "info",
	[FI_LOG_DEBUG] = "debug",
	[FI_LOG_MAX] = NULL
};

extern struct fi_provider core_prov;

enum {
	FI_LOG_SUBSYS_OFFSET	= FI_LOG_MAX,
	FI_LOG_PROV_OFFSET	= FI_LOG_SUBSYS_OFFSET + FI_LOG_SUBSYS_MAX,
	FI_LOG_LEVEL_MASK	= ((1 << FI_LOG_MAX) - 1),
	FI_LOG_SUBSYS_MASK	= (((1 << FI_LOG_SUBSYS_MAX) - 1) <<
				   FI_LOG_SUBSYS_OFFSET),
//	FI_LOG_PROV_MASK	= (((1 << (64 - FI_LOG_PROV_OFFSET)) - 1) <<
//				   FI_LOG_PROV_OFFSET)
};

#define FI_LOG_TAG(prov, level, subsys) \
	(((uint64_t) prov << FI_LOG_PROV_OFFSET) | \
	 ((uint64_t) (1 << (subsys + FI_LOG_SUBSYS_OFFSET))) | \
	 ((uint64_t) (1 << level)))

uint64_t log_mask;
struct fi_filter prov_log_filter;

static int fi_convert_log_str(const char *value)
{
	int i;

	if (!value)
		return -1;

	for (i = 0; log_levels[i]; i++) {
		if (!strcasecmp(value, log_levels[i]))
			return i;
	}
	return 0;
}

void fi_log_init(void)
{
	struct fi_filter subsys_filter;
	int level, i;
	char *levelstr = NULL, *provstr = NULL, *subsysstr = NULL;

	fi_param_define(NULL, "log_level", FI_PARAM_STRING,
			"Specify logging level: warn, trace, info, debug (default: warn)");
	fi_param_get_str(NULL, "log_level", &levelstr);
	level = fi_convert_log_str(levelstr);
	if (level >= 0)
		log_mask = ((1 << (level + 1)) - 1);

	fi_param_define(NULL, "log_prov", FI_PARAM_STRING,
			"Specify specific provider to log (default: all)");
	fi_param_get_str(NULL, "log_prov", &provstr);
	fi_create_filter(&prov_log_filter, provstr);

	fi_param_define(NULL, "log_subsys", FI_PARAM_STRING,
			"Specify specific subsystem to log (default: all)");
	fi_param_get_str(NULL, "log_subsys", &subsysstr);
	fi_create_filter(&subsys_filter, subsysstr);
	for (i = 0; i < FI_LOG_SUBSYS_MAX; i++) {
		if (!fi_apply_filter(&subsys_filter, log_subsys[i]))
			log_mask |= (1 << (i + FI_LOG_SUBSYS_OFFSET));
	}
	fi_free_filter(&subsys_filter);
}

void fi_log_fini(void)
{
	fi_free_filter(&prov_log_filter);
}

__attribute__((visibility ("default")))
int DEFAULT_SYMVER_PRE(fi_log_enabled)(const struct fi_provider *prov, enum fi_log_level level,
		   enum fi_log_subsys subsys)
{
	struct fi_prov_context *ctx;

	ctx = (struct fi_prov_context *) &prov->context;
	return ((FI_LOG_TAG(ctx->disable_logging, level, subsys) & log_mask) ==
		FI_LOG_TAG(ctx->disable_logging, level, subsys));
}
DEFAULT_SYMVER(fi_log_enabled_, fi_log_enabled);

struct log_handle {
	char *filename;
	FILE *fp;
	pid_t pid;
};

struct log_handle *handle;

void fill_handle(struct log_handle *out)
{
	const char *directory = "usdfv-log";
	size_t size;
	int ret;

	handle->pid = getpid();

	ret = mkdir(directory, 0700);
	if (ret && errno != EEXIST) {
		fprintf(stderr, "libfabric: Failed to create logging directory\n");
		return;
	}

	size = snprintf(NULL, 0, "%s/usdfv-%d.log", directory, handle->pid) + 1;
	handle->filename = calloc(1, size);
	if (!handle->filename) {
		fprintf(stderr, "Logging failed to initialize\n");
		return;
	}

	sprintf(handle->filename, "%s/usdfv-%d.log", directory, handle->pid);
	handle->fp = fopen(handle->filename, "a+");
	if (!out->fp)
		fprintf(stderr, "Log file creation failed: %s\n",
				strerror(errno));
}

void child_fork_handler(void)
{
	fill_handle(handle);
}

static void init_logging(void)
{
	handle = calloc(1, sizeof(*handle));
	if (!handle) {
		fprintf(stderr, "failed to allocate log handle\n");
		return;
	}

	fill_handle(handle);

	pthread_atfork(NULL, NULL, child_fork_handler);
}

static __attribute__((destructor)) void destruct_logging(void)
{
	if (handle) {
		free(handle->filename);
		fclose(handle->fp);
		free(handle);
		handle = NULL;
	}
}

__attribute__((visibility ("default")))
void DEFAULT_SYMVER_PRE(fi_log)(const struct fi_provider *prov, enum fi_log_level level,
	    enum fi_log_subsys subsys, const char *func, int line,
	    const char *fmt, ...)
{
	pid_t current_pid;
	char buf[2048];
	int size;

	va_list vargs;

	if (!handle) {
		init_logging();
		if (!handle)
			return;
	}

	current_pid = getpid();
	size = snprintf(buf, sizeof(buf), "[%d]:%s:%s:%s:%s():%d<%s> ",
			current_pid, PACKAGE,
			prov->name, log_subsys[subsys], func, line,
			log_levels[level]);

	va_start(vargs, fmt);
	vsnprintf(buf + size, sizeof(buf) - size, fmt, vargs);
	va_end(vargs);

	fprintf(handle->fp, "%s", buf);
	fflush(handle->fp);
}
DEFAULT_SYMVER(fi_log_, fi_log);
