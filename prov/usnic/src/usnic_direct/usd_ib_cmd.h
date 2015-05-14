/*
 * Copyright (c) 2014, Cisco Systems, Inc. All rights reserved.
 *
 * LICENSE_BEGIN
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * LICENSE_END
 *
 *
 */

#ifndef _USD_IB_CMD_
#define _USD_IB_CMD_

#include "usd.h"
#include <infiniband/kern-abi.h>

int usd_ib_cmd_get_context(struct usd_context *uctx);
int usd_ib_cmd_alloc_pd(struct usd_device *dev, uint32_t * pd_handle_o);
int usd_ib_cmd_reg_mr(struct usd_device *dev, void *vaddr, size_t length,
                        struct usd_mr *mr);
int usd_ib_cmd_reg_mr_v1(struct usd_device *dev, void *vaddr, size_t length,
                        uint32_t reg_op, uint32_t vfid, uint32_t mr_type,
                        uint32_t queue_index, struct usd_mr *mr);
int usd_ib_cmd_dereg_mr(struct usd_device *dev, struct usd_mr *mr);
int usd_ib_cmd_create_cq(struct usd_device *dev, struct usd_cq_impl *cq,
                        void *ibv_cq, int comp_channel, int comp_vector);
int usd_ib_cmd_destroy_cq(struct usd_device *dev, struct usd_cq_impl *cq);
int usd_ib_cmd_create_qp(struct usd_device *dev, struct usd_qp_impl *qp,
                         struct usd_vf_info *vfip);
int usd_ib_cmd_modify_qp(struct usd_device *dev, struct usd_qp_impl *qp,
                         int state);
int usd_ib_cmd_destroy_qp(struct usd_device *dev, struct usd_qp_impl *qp);

int usd_ib_query_dev(struct usd_device *dev);
int usd_ib_cmd_devcmd(struct usd_device *dev, enum vnic_devcmd_cmd devcmd,
                        u64 *a0, u64 *a1, int wait);
int usd_ib_cmd_create_comp_channel(struct usd_device *dev, int *comp_fd_o);
int usd_ib_cmd_destroy_comp_channel(struct usd_device *dev, int comp_fd);
int usd_ib_cmd_query_port(struct usd_device *dev,
                         struct ibv_query_port_resp *irp);
int usd_ib_cmd_query_device(struct usd_device *dev,
                           struct ibv_query_device_resp *irp);
#if USNIC_HAVE_SHPD
int usd_ib_cmd_alloc_shpd(struct usd_device *dev, uint32_t pd_handle,
                            uint64_t share_key, void *iova_start,
                            size_t iova_len, uint32_t *handle_o);
int usd_ib_cmd_share_pd(struct usd_device *dev, uint32_t shpd_handle,
                        uint64_t share_key, uint32_t *handle_o);
#endif
#endif /* _USD_IB_CMD_ */
