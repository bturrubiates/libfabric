/*
 * Copyright (c) 2013-2015 Intel Corporation, Inc.  All rights reserved.
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
 */

#include <arpa/inet.h>

#include "verbs_rdm.h"


extern struct fi_provider fi_ibv_prov;
extern struct fi_ibv_rdm_tagged_conn *fi_ibv_rdm_tagged_conn_hash;


int fi_ibv_rdm_start_connection(struct fi_ibv_rdm_ep *ep,
                                struct fi_ibv_rdm_tagged_conn *conn)
{
	struct rdma_cm_id *id = NULL;
	assert(ep->cm.listener);

	if (conn->state != FI_VERBS_CONN_ALLOCATED)
		return 0;

	if (ep->is_closing) {
		VERBS_INFO(FI_LOG_AV, "Attempt to start connection with addr %s:%u when ep is closing\n",
			inet_ntoa(conn->addr.sin_addr),
			ntohs(conn->addr.sin_port));
		return -1;
	}

	conn->state = FI_VERBS_CONN_STARTED;

	if (rdma_create_id(ep->cm.ec, &id, conn, RDMA_PS_TCP))
		return -1;

	if (conn->cm_role == FI_VERBS_CM_ACTIVE || 
	    conn->cm_role == FI_VERBS_CM_SELF)
	{
		conn->id[0] = id;
	}

	return rdma_resolve_addr(id, NULL, (struct sockaddr *)&conn->addr, 30000);
}

int fi_ibv_rdm_start_disconnection(struct fi_ibv_rdm_tagged_conn *conn)
{
	int ret = 0;

	FI_INFO(&fi_ibv_prov, FI_LOG_AV,
		"Closing connection %p, state %d\n", conn, conn->state);

	if (conn->id[0]) {
		ret = rdma_disconnect(conn->id[0]);
		assert(ret == 0);
	}

	switch (conn->state) {
	case FI_VERBS_CONN_ALLOCATED:
	case FI_VERBS_CONN_REMOTE_DISCONNECT:
		fi_ibv_rdm_tagged_conn_cleanup(conn);
		break;
	case FI_VERBS_CONN_ESTABLISHED:
		conn->state = FI_VERBS_CONN_LOCAL_DISCONNECT;
		break;
	case FI_VERBS_CONN_REJECTED:
		conn->state = FI_VERBS_CONN_CLOSED;
		break;
	default:
		ret = -1;
	}

	assert(ret == 0);
	return ret;
}

static int fi_ibv_rdm_av_insert(struct fid_av *av, const void *addr,
                                size_t count, fi_addr_t * fi_addr,
                                uint64_t flags, void *context)
{
	struct fi_ibv_av *fid_av = container_of(av, struct fi_ibv_av, av);
	struct fi_ibv_rdm_ep *ep = fid_av->ep;
	int i,  ret = 0;

	pthread_mutex_lock(&ep->cm_lock);
	for (i = 0; i < count; i++) {
		struct fi_ibv_rdm_tagged_conn *conn = NULL;
		void *addr_i = (char *) addr + i * ep->addrlen;

		HASH_FIND(hh, fi_ibv_rdm_tagged_conn_hash, addr_i,
			FI_IBV_RDM_DFLT_ADDRLEN, conn);

		if (!conn) {
			/* If addr_i is not found in HASH then we malloc it.
			 * It could be found if the connection was initiated by the remote
			 * side.
			 */
			conn = memalign(64, sizeof *conn);
			if (!conn) {
				ret = -FI_ENOMEM;
				goto out;
			}

			memset(conn, 0, sizeof *conn);
			dlist_init(&conn->postponed_requests_head);
			conn->state = FI_VERBS_CONN_ALLOCATED;
			memcpy(&conn->addr, addr_i, FI_IBV_RDM_DFLT_ADDRLEN);
			HASH_ADD(hh, fi_ibv_rdm_tagged_conn_hash, addr,
			FI_IBV_RDM_DFLT_ADDRLEN, conn);
		}

		fi_ibv_rdm_conn_init_cm_role(conn, ep);

		fi_addr[i] = (uintptr_t) (void *) conn;
		FI_INFO(&fi_ibv_prov, FI_LOG_AV, "fi_av_insert: addr %s:%u conn %p %d\n",
			inet_ntoa(conn->addr.sin_addr),
			ntohs(conn->addr.sin_port), conn, conn->cm_role);

		ret++;
	}

out:
	pthread_mutex_unlock(&ep->cm_lock);
	return ret;
}

static int fi_ibv_rdm_av_remove(struct fid_av *av, fi_addr_t * fi_addr,
                                size_t count, uint64_t flags)
{
	struct fi_ibv_rdm_tagged_conn *conn;
	int i;

	for (i = 0; i < count; i++) {
		conn = (struct fi_ibv_rdm_tagged_conn *) fi_addr[i];
		FI_INFO(&fi_ibv_prov, FI_LOG_AV, "av_remove conn %p, addr %s:%u\n",
			conn, inet_ntoa(conn->addr.sin_addr),
			ntohs(conn->addr.sin_port));
		HASH_DEL(fi_ibv_rdm_tagged_conn_hash, conn);
		fi_ibv_rdm_start_disconnection(conn);
	}

	return 0;
}

static struct fi_ops_av fi_ibv_rdm_av_ops = {
	.size = sizeof(struct fi_ops_av),
	.insert = fi_ibv_rdm_av_insert,
	.remove = fi_ibv_rdm_av_remove,
};

struct fi_ops_av *fi_ibv_rdm_set_av_ops(void)
{
	return &fi_ibv_rdm_av_ops;
}
