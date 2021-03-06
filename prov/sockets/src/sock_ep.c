/*
 * Copyright (c) 2013-2014 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenFabrics.org BSD license below:
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

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "sock.h"
#include "sock_util.h"

#define SOCK_LOG_DBG(...) _SOCK_LOG_DBG(FI_LOG_EP_CTRL, __VA_ARGS__)
#define SOCK_LOG_ERROR(...) _SOCK_LOG_ERROR(FI_LOG_EP_CTRL, __VA_ARGS__)

extern struct fi_ops_rma sock_ep_rma;
extern struct fi_ops_msg sock_ep_msg_ops;
extern struct fi_ops_tagged sock_ep_tagged;
extern struct fi_ops_atomic sock_ep_atomic;

extern struct fi_ops_cm sock_ep_cm_ops;
extern struct fi_ops_ep sock_ep_ops;
extern struct fi_ops sock_ep_fi_ops;
extern struct fi_ops_ep sock_ctx_ep_ops;

extern const struct fi_domain_attr sock_domain_attr;
extern const struct fi_fabric_attr sock_fabric_attr;

const struct fi_tx_attr sock_stx_attr = {
	.caps = SOCK_EP_RDM_CAP,
	.mode = SOCK_MODE,
	.op_flags = FI_TRANSMIT_COMPLETE,
	.msg_order = SOCK_EP_MSG_ORDER,
	.inject_size = SOCK_EP_MAX_INJECT_SZ,
	.size = SOCK_EP_TX_SZ,
	.iov_limit = SOCK_EP_MAX_IOV_LIMIT,
	.rma_iov_limit = SOCK_EP_MAX_IOV_LIMIT,
};

const struct fi_rx_attr sock_srx_attr = {
	.caps = SOCK_EP_RDM_CAP,
	.mode = SOCK_MODE,
	.op_flags = 0,
	.msg_order = SOCK_EP_MSG_ORDER,
	.comp_order = SOCK_EP_COMP_ORDER,
	.total_buffered_recv = 0,
	.size = SOCK_EP_MAX_MSG_SZ,
	.iov_limit = SOCK_EP_MAX_IOV_LIMIT,
};

static int sock_ctx_close(struct fid *fid)
{
	struct sock_tx_ctx *tx_ctx;
	struct sock_rx_ctx *rx_ctx;

	switch (fid->fclass) {
	case FI_CLASS_TX_CTX:
		tx_ctx = container_of(fid, struct sock_tx_ctx, fid.ctx.fid);
		sock_pe_remove_tx_ctx(tx_ctx);
		atomic_dec(&tx_ctx->ep->num_tx_ctx);
		atomic_dec(&tx_ctx->domain->ref);
		sock_tx_ctx_free(tx_ctx);
		break;

	case FI_CLASS_RX_CTX:
		rx_ctx = container_of(fid, struct sock_rx_ctx, ctx.fid);
		sock_pe_remove_rx_ctx(rx_ctx);
		atomic_dec(&rx_ctx->ep->num_rx_ctx);
		atomic_dec(&rx_ctx->domain->ref);
		sock_rx_ctx_free(rx_ctx);
		break;

	case FI_CLASS_STX_CTX:
		tx_ctx = container_of(fid, struct sock_tx_ctx, fid.stx.fid);
		atomic_dec(&tx_ctx->domain->ref);
		sock_pe_remove_tx_ctx(tx_ctx);
		sock_tx_ctx_free(tx_ctx);
		break;

	case FI_CLASS_SRX_CTX:
		rx_ctx = container_of(fid, struct sock_rx_ctx, ctx.fid);
		atomic_dec(&rx_ctx->domain->ref);
		sock_pe_remove_rx_ctx(rx_ctx);
		sock_rx_ctx_free(rx_ctx);
		break;

	default:
		SOCK_LOG_ERROR("Invalid fid\n");
		return -FI_EINVAL;
	}
	return 0;
}

static int sock_ctx_bind_cq(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct sock_cq *sock_cq;
	struct sock_tx_ctx *tx_ctx;
	struct sock_rx_ctx *rx_ctx;

	if ((flags | SOCK_EP_CQ_FLAGS) != SOCK_EP_CQ_FLAGS) {
		SOCK_LOG_ERROR("Invalid cq flag\n");
		return -FI_EINVAL;
	}
	sock_cq = container_of(bfid, struct sock_cq, cq_fid.fid);
	switch (fid->fclass) {
	case FI_CLASS_TX_CTX:
		tx_ctx = container_of(fid, struct sock_tx_ctx, fid.ctx);
		if (flags & FI_SEND) {
			tx_ctx->comp.send_cq = sock_cq;
			if (flags & FI_SELECTIVE_COMPLETION)
				tx_ctx->comp.send_cq_event = 1;
		}

		fastlock_acquire(&sock_cq->list_lock);
		dlist_insert_tail(&tx_ctx->cq_entry, &sock_cq->tx_list);
		fastlock_release(&sock_cq->list_lock);
		break;

	case FI_CLASS_RX_CTX:
		rx_ctx = container_of(fid, struct sock_rx_ctx, ctx.fid);
		if (flags & FI_RECV) {
			rx_ctx->comp.recv_cq = sock_cq;
			if (flags & FI_SELECTIVE_COMPLETION)
				rx_ctx->comp.recv_cq_event = 1;
		}

		fastlock_acquire(&sock_cq->list_lock);
		dlist_insert_tail(&rx_ctx->cq_entry, &sock_cq->rx_list);
		fastlock_release(&sock_cq->list_lock);
		break;

	case FI_CLASS_STX_CTX:
		tx_ctx = container_of(fid, struct sock_tx_ctx, fid.stx.fid);
		if (flags & FI_SEND) {
			tx_ctx->comp.send_cq = sock_cq;
			if (flags & FI_SELECTIVE_COMPLETION)
				tx_ctx->comp.send_cq_event = 1;
		}

		fastlock_acquire(&sock_cq->list_lock);
		dlist_insert_tail(&tx_ctx->cq_entry, &sock_cq->tx_list);
		fastlock_release(&sock_cq->list_lock);
		break;

	default:
		SOCK_LOG_ERROR("Invalid fid\n");
		return -FI_EINVAL;
	}
	return 0;
}

static int sock_ctx_bind_cntr(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct sock_cntr *cntr;
	struct sock_tx_ctx *tx_ctx;
	struct sock_rx_ctx *rx_ctx;

	if ((flags | SOCK_EP_CNTR_FLAGS) != SOCK_EP_CNTR_FLAGS) {
		SOCK_LOG_ERROR("Invalid cntr flag\n");
		return -FI_EINVAL;
	}

	cntr = container_of(bfid, struct sock_cntr, cntr_fid.fid);
	switch (fid->fclass) {
	case FI_CLASS_TX_CTX:
		tx_ctx = container_of(fid, struct sock_tx_ctx, fid.ctx.fid);
		if (flags & FI_SEND)
			tx_ctx->comp.send_cntr = cntr;

		if (flags & FI_READ)
			tx_ctx->comp.read_cntr = cntr;

		if (flags & FI_WRITE)
			tx_ctx->comp.write_cntr = cntr;

		fastlock_acquire(&cntr->list_lock);
		dlist_insert_tail(&tx_ctx->cntr_entry, &cntr->tx_list);
		fastlock_release(&cntr->list_lock);
		break;

	case FI_CLASS_RX_CTX:
		rx_ctx = container_of(fid, struct sock_rx_ctx, ctx.fid);
		if (flags & FI_RECV)
			rx_ctx->comp.recv_cntr = cntr;

		if (flags & FI_REMOTE_READ)
			rx_ctx->comp.rem_read_cntr = cntr;

		if (flags & FI_REMOTE_WRITE)
			rx_ctx->comp.rem_write_cntr = cntr;

		fastlock_acquire(&cntr->list_lock);
		dlist_insert_tail(&rx_ctx->cntr_entry, &cntr->rx_list);
		fastlock_release(&cntr->list_lock);
		break;

	case FI_CLASS_STX_CTX:
		tx_ctx = container_of(fid, struct sock_tx_ctx, fid.ctx.fid);
		if (flags & FI_SEND)
			tx_ctx->comp.send_cntr = cntr;

		if (flags & FI_READ)
			tx_ctx->comp.read_cntr = cntr;

		if (flags & FI_WRITE)
			tx_ctx->comp.write_cntr = cntr;

		fastlock_acquire(&cntr->list_lock);
		dlist_insert_tail(&tx_ctx->cntr_entry, &cntr->tx_list);
		fastlock_release(&cntr->list_lock);

		break;

	default:
		SOCK_LOG_ERROR("Invalid fid\n");
		return -FI_EINVAL;
	}
	return 0;
}

static int sock_ctx_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	switch (bfid->fclass) {
	case FI_CLASS_CQ:
		return sock_ctx_bind_cq(fid, bfid, flags);

	case FI_CLASS_CNTR:
		return sock_ctx_bind_cntr(fid, bfid, flags);

	case FI_CLASS_MR:
		return 0;

	default:
		SOCK_LOG_ERROR("Invalid bind()\n");
		return -FI_EINVAL;
	}

}

static int sock_ctx_enable(struct fid_ep *ep)
{
	struct sock_tx_ctx *tx_ctx;
	struct sock_rx_ctx *rx_ctx;

	switch (ep->fid.fclass) {
	case FI_CLASS_RX_CTX:
		rx_ctx = container_of(ep, struct sock_rx_ctx, ctx.fid);
		rx_ctx->enabled = 1;
		if (!rx_ctx->progress) {
			sock_pe_add_rx_ctx(rx_ctx->domain->pe, rx_ctx);
			rx_ctx->progress = 1;
		}
		if (!rx_ctx->ep->listener.listener_thread &&
		    sock_conn_listen(rx_ctx->ep)) {
			SOCK_LOG_ERROR("failed to create listener\n");
		}
		return 0;

	case FI_CLASS_TX_CTX:
		tx_ctx = container_of(ep, struct sock_tx_ctx, fid.ctx.fid);
		tx_ctx->enabled = 1;
		if (!tx_ctx->progress) {
			sock_pe_add_tx_ctx(tx_ctx->domain->pe, tx_ctx);
			tx_ctx->progress = 1;
		}
		if (!tx_ctx->ep->listener.listener_thread &&
		    sock_conn_listen(tx_ctx->ep)) {
			SOCK_LOG_ERROR("failed to create listener\n");
		}
		return 0;

	default:
		SOCK_LOG_ERROR("Invalid CTX\n");
		break;
	}
	return -FI_EINVAL;
}

static int sock_ctx_control(struct fid *fid, int command, void *arg)
{
	struct fid_ep *ep;
	struct sock_tx_ctx *tx_ctx;
	struct sock_rx_ctx *rx_ctx;

	switch (fid->fclass) {
	case FI_CLASS_TX_CTX:
		tx_ctx = container_of(fid, struct sock_tx_ctx, fid.ctx.fid);
		switch (command) {
		case FI_GETOPSFLAG:
			*(uint64_t *) arg = tx_ctx->attr.op_flags;
			break;
		case FI_SETOPSFLAG:
			tx_ctx->attr.op_flags = *(uint64_t *) arg;
			tx_ctx->attr.op_flags |= FI_TRANSMIT_COMPLETE;
			break;
		case FI_ENABLE:
			ep = container_of(fid, struct fid_ep, fid);
			return sock_ctx_enable(ep);
			break;
		default:
			return -FI_ENOSYS;
		}
		break;

	case FI_CLASS_RX_CTX:
		rx_ctx = container_of(fid, struct sock_rx_ctx, ctx.fid);
		switch (command) {
		case FI_GETOPSFLAG:
			*(uint64_t *) arg = rx_ctx->attr.op_flags;
			break;
		case FI_SETOPSFLAG:
			rx_ctx->attr.op_flags = *(uint64_t *) arg;
			break;
		case FI_ENABLE:
			ep = container_of(fid, struct fid_ep, fid);
			return sock_ctx_enable(ep);
			break;
		default:
			return -FI_ENOSYS;
		}
		break;

	case FI_CLASS_STX_CTX:
		tx_ctx = container_of(fid, struct sock_tx_ctx, fid.stx.fid);
		switch (command) {
		case FI_GETOPSFLAG:
			*(uint64_t *) arg = tx_ctx->attr.op_flags;
			break;
		case FI_SETOPSFLAG:
			tx_ctx->attr.op_flags = *(uint64_t *) arg;
			tx_ctx->attr.op_flags |= FI_TRANSMIT_COMPLETE;
			break;
		default:
			return -FI_ENOSYS;
		}
		break;

	default:
		return -FI_ENOSYS;
	}

	return 0;
}

static struct fi_ops sock_ctx_ops = {
	.size = sizeof(struct fi_ops),
	.close = sock_ctx_close,
	.bind = sock_ctx_bind,
	.control = sock_ctx_control,
	.ops_open = fi_no_ops_open,
};

static int sock_ctx_getopt(fid_t fid, int level, int optname,
		       void *optval, size_t *optlen)
{
	struct sock_rx_ctx *rx_ctx;
	rx_ctx = container_of(fid, struct sock_rx_ctx, ctx.fid);

	if (level != FI_OPT_ENDPOINT)
		return -ENOPROTOOPT;

	switch (optname) {
	case FI_OPT_MIN_MULTI_RECV:
		if (*optlen < sizeof(size_t))
			return -FI_ETOOSMALL;
		*(size_t *)optval = rx_ctx->min_multi_recv;
		*optlen = sizeof(size_t);
		break;
	case FI_OPT_CM_DATA_SIZE:
		if (*optlen < sizeof(size_t))
			return -FI_ETOOSMALL;
		*((size_t *) optval) = SOCK_EP_MAX_CM_DATA_SZ;
		*optlen = sizeof(size_t);
		break;
	default:
		return -FI_ENOPROTOOPT;
	}
	return 0;
}

static int sock_ctx_setopt(fid_t fid, int level, int optname,
		       const void *optval, size_t optlen)
{
	struct sock_rx_ctx *rx_ctx;
	rx_ctx = container_of(fid, struct sock_rx_ctx, ctx.fid);

	if (level != FI_OPT_ENDPOINT)
		return -ENOPROTOOPT;

	switch (optname) {
	case FI_OPT_MIN_MULTI_RECV:
		rx_ctx->min_multi_recv = *(size_t *)optval;
		break;

	default:
		return -ENOPROTOOPT;
	}
	return 0;
}

static ssize_t sock_rx_ctx_cancel(struct sock_rx_ctx *rx_ctx, void *context)
{
	struct dlist_entry *entry;
	ssize_t ret = -FI_ENOENT;
	struct sock_rx_entry *rx_entry;
	struct sock_pe_entry pe_entry;

	fastlock_acquire(&rx_ctx->lock);
	for (entry = rx_ctx->rx_entry_list.next;
	     entry != &rx_ctx->rx_entry_list; entry = entry->next) {

		rx_entry = container_of(entry, struct sock_rx_entry, entry);
		if (rx_entry->is_busy)
			continue;

		if ((uintptr_t) context == rx_entry->context) {
			if (rx_ctx->comp.recv_cq) {
				memset(&pe_entry, 0, sizeof(pe_entry));
				pe_entry.comp = &rx_ctx->comp;
				pe_entry.tag = rx_entry->tag;
				pe_entry.context = rx_entry->context;
				pe_entry.flags = (FI_MSG | FI_RECV);
				if (rx_entry->is_tagged)
					pe_entry.flags |= FI_TAGGED;

				if (sock_cq_report_error(pe_entry.comp->recv_cq,
							  &pe_entry, 0, FI_ECANCELED,
							  -FI_ECANCELED, NULL)) {
					SOCK_LOG_ERROR("failed to report error\n");
				}
			}

			if (rx_ctx->comp.recv_cntr)
				sock_cntr_err_inc(rx_ctx->comp.recv_cntr);

			dlist_remove(&rx_entry->entry);
			sock_rx_release_entry(rx_entry);
			ret = 0;
			break;
		}
	}
	fastlock_release(&rx_ctx->lock);
	return ret;
}

static ssize_t sock_ep_cancel(fid_t fid, void *context)
{
	struct sock_rx_ctx *rx_ctx = NULL;
	struct sock_ep *sock_ep;

	switch (fid->fclass) {
	case FI_CLASS_EP:
		sock_ep = container_of(fid, struct sock_ep, ep.fid);
		rx_ctx = sock_ep->rx_ctx;
		break;

	case FI_CLASS_RX_CTX:
	case FI_CLASS_SRX_CTX:
		rx_ctx = container_of(fid, struct sock_rx_ctx, ctx.fid);
		sock_ep = rx_ctx->ep;
		break;

	case FI_CLASS_TX_CTX:
	case FI_CLASS_STX_CTX:
		return -FI_ENOENT;

	default:
		SOCK_LOG_ERROR("Invalid ep type\n");
		return -FI_EINVAL;
	}

	return sock_rx_ctx_cancel(rx_ctx, context);
}

static ssize_t sock_rx_size_left(struct fid_ep *ep)
{
	struct sock_rx_ctx *rx_ctx;
	struct sock_ep *sock_ep;

	switch (ep->fid.fclass) {
	case FI_CLASS_EP:
		sock_ep = container_of(ep, struct sock_ep, ep);
		rx_ctx = sock_ep->rx_ctx;
		break;

	case FI_CLASS_RX_CTX:
	case FI_CLASS_SRX_CTX:
		rx_ctx = container_of(ep, struct sock_rx_ctx, ctx);
		break;

	default:
		SOCK_LOG_ERROR("Invalid ep type\n");
		return -FI_EINVAL;
	}

	return rx_ctx->num_left;
}

static ssize_t sock_tx_size_left(struct fid_ep *ep)
{
	struct sock_ep *sock_ep;
	struct sock_tx_ctx *tx_ctx;
	ssize_t num_left = 0;

	switch (ep->fid.fclass) {
	case FI_CLASS_EP:
		sock_ep = container_of(ep, struct sock_ep, ep);
		tx_ctx = sock_ep->tx_ctx;
		break;

	case FI_CLASS_TX_CTX:
		tx_ctx = container_of(ep, struct sock_tx_ctx, fid.ctx);
		break;

	default:
		SOCK_LOG_ERROR("Invalid EP type\n");
		return -FI_EINVAL;
	}

	fastlock_acquire(&tx_ctx->wlock);
	num_left = rbavail(&tx_ctx->rb)/SOCK_EP_TX_ENTRY_SZ;
	fastlock_release(&tx_ctx->wlock);
	return num_left;
}

struct fi_ops_ep sock_ctx_ep_ops = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = sock_ep_cancel,
	.getopt = sock_ctx_getopt,
	.setopt = sock_ctx_setopt,
	.tx_ctx = fi_no_tx_ctx,
	.rx_ctx = fi_no_rx_ctx,
	.rx_size_left = sock_rx_size_left,
	.tx_size_left = sock_tx_size_left,
};

static int sock_ep_close(struct fid *fid)
{
	struct sock_ep *sock_ep;
	char c = 0;

	switch (fid->fclass) {
	case FI_CLASS_EP:
		sock_ep = container_of(fid, struct sock_ep, ep.fid);
		break;

	case FI_CLASS_SEP:
		sock_ep = container_of(fid, struct sock_ep, ep.fid);
		break;

	default:
		return -FI_EINVAL;
	}

	if (atomic_get(&sock_ep->ref) || atomic_get(&sock_ep->num_rx_ctx) ||
	    atomic_get(&sock_ep->num_tx_ctx))
		return -FI_EBUSY;

	if (sock_ep->ep_type == FI_EP_MSG) {
		sock_ep->cm.do_listen = 0;
		if (write(sock_ep->cm.signal_fds[0], &c, 1) != 1)
			SOCK_LOG_DBG("Failed to signal\n");

		if (sock_ep->cm.listener_thread &&
		    pthread_join(sock_ep->cm.listener_thread, NULL)) {
			SOCK_LOG_ERROR("pthread join failed (%d)\n", errno);
		}
		close(sock_ep->cm.signal_fds[0]);
		close(sock_ep->cm.signal_fds[1]);
	} else {
		if (sock_ep->av)
			atomic_dec(&sock_ep->av->ref);
	}

	pthread_mutex_lock(&sock_ep->domain->pe->list_lock);
	if (sock_ep->tx_shared) {
		fastlock_acquire(&sock_ep->tx_ctx->lock);
		dlist_remove(&sock_ep->tx_ctx_entry);
		fastlock_release(&sock_ep->tx_ctx->lock);
	}

	if (sock_ep->rx_shared) {
		fastlock_acquire(&sock_ep->rx_ctx->lock);
		dlist_remove(&sock_ep->rx_ctx_entry);
		fastlock_release(&sock_ep->rx_ctx->lock);
	}
	pthread_mutex_unlock(&sock_ep->domain->pe->list_lock);

	sock_ep->listener.do_listen = 0;
	if (write(sock_ep->listener.signal_fds[0], &c, 1) != 1)
		SOCK_LOG_DBG("Failed to signal\n");

	if (sock_ep->listener.listener_thread &&
	    pthread_join(sock_ep->listener.listener_thread, NULL)) {
		SOCK_LOG_ERROR("pthread join failed (%d)\n", errno);
	}

	close(sock_ep->listener.signal_fds[0]);
	close(sock_ep->listener.signal_fds[1]);
	fastlock_destroy(&sock_ep->cm.lock);

	if (sock_ep->fclass != FI_CLASS_SEP && !sock_ep->tx_shared) {
		sock_pe_remove_tx_ctx(sock_ep->tx_array[0]);
		sock_tx_ctx_free(sock_ep->tx_array[0]);
	}

	if (sock_ep->fclass != FI_CLASS_SEP && !sock_ep->rx_shared) {
		sock_pe_remove_rx_ctx(sock_ep->rx_array[0]);
		sock_rx_ctx_free(sock_ep->rx_array[0]);
	}

	free(sock_ep->tx_array);
	free(sock_ep->rx_array);

	if (sock_ep->src_addr)
		free(sock_ep->src_addr);
	if (sock_ep->dest_addr)
		free(sock_ep->dest_addr);

	sock_fabric_remove_service(sock_ep->domain->fab,
				   atoi(sock_ep->listener.service));

	sock_conn_map_destroy(&sock_ep->cmap);
	atomic_dec(&sock_ep->domain->ref);
	fastlock_destroy(&sock_ep->lock);
	free(sock_ep);
	return 0;
}

static int sock_ep_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	int ret, i;
	struct sock_ep *ep;
	struct sock_eq *eq;
	struct sock_cq *cq;
	struct sock_av *av;
	struct sock_cntr *cntr;
	struct sock_tx_ctx *tx_ctx;
	struct sock_rx_ctx *rx_ctx;

	switch (fid->fclass) {
	case FI_CLASS_EP:
		ep = container_of(fid, struct sock_ep, ep.fid);
		break;

	case FI_CLASS_SEP:
		ep = container_of(fid, struct sock_ep, ep.fid);
		break;

	default:
		return -FI_EINVAL;
	}

	switch (bfid->fclass) {
	case FI_CLASS_EQ:
		eq = container_of(bfid, struct sock_eq, eq.fid);
		ep->eq = eq;
		break;

	case FI_CLASS_MR:
		return 0;

	case FI_CLASS_CQ:
		cq = container_of(bfid, struct sock_cq, cq_fid.fid);
		if (ep->domain != cq->domain)
			return -FI_EINVAL;

		if (flags & FI_SEND) {
			ep->comp.send_cq = cq;
			if (flags & FI_SELECTIVE_COMPLETION)
				ep->comp.send_cq_event = 1;
		}

		if (flags & FI_RECV) {
			ep->comp.recv_cq = cq;
			if (flags & FI_SELECTIVE_COMPLETION)
				ep->comp.recv_cq_event = 1;
		}

		if (flags & FI_SEND) {
			for (i = 0; i < ep->ep_attr.tx_ctx_cnt; i++) {
				tx_ctx = ep->tx_array[i];

				if (!tx_ctx)
					continue;

				ret = sock_ctx_bind_cq(&tx_ctx->fid.ctx.fid,
							bfid, flags);
				if (ret)
					return ret;
			}
		}

		if (flags & FI_RECV) {
			for (i = 0; i < ep->ep_attr.rx_ctx_cnt; i++) {
				rx_ctx = ep->rx_array[i];

				if (!rx_ctx)
					continue;

				if (rx_ctx->ctx.fid.fclass == FI_CLASS_SRX_CTX) {
					if (flags & FI_RECV) {
						ep->comp.recv_cq = cq;
						if (flags & FI_SELECTIVE_COMPLETION)
							ep->comp.recv_cq_event = 1;
					}

					fastlock_acquire(&cq->list_lock);
					dlist_insert_tail(&rx_ctx->cq_entry,
								&cq->rx_list);
					fastlock_release(&cq->list_lock);
					continue;
				}

				ret = sock_ctx_bind_cq(&rx_ctx->ctx.fid,
							bfid, flags);
				if (ret)
					return ret;
			}
		}
		break;

	case FI_CLASS_CNTR:
		cntr = container_of(bfid, struct sock_cntr, cntr_fid.fid);
		if (ep->domain != cntr->domain)
			return -FI_EINVAL;

		if (flags & FI_SEND)
			ep->comp.send_cntr = cntr;

		if (flags & FI_RECV)
			ep->comp.recv_cntr = cntr;

		if (flags & FI_READ)
			ep->comp.read_cntr = cntr;

		if (flags & FI_WRITE)
			ep->comp.write_cntr = cntr;

		if (flags & FI_REMOTE_READ)
			ep->comp.rem_read_cntr = cntr;

		if (flags & FI_REMOTE_WRITE)
			ep->comp.rem_write_cntr = cntr;

		if (flags & FI_SEND || flags & FI_WRITE || flags & FI_READ) {
			for (i = 0; i < ep->ep_attr.tx_ctx_cnt; i++) {
				tx_ctx = ep->tx_array[i];

				if (!tx_ctx)
					continue;

				ret = sock_ctx_bind_cntr(&tx_ctx->fid.ctx.fid,
								bfid, flags);
				if (ret)
					return ret;
			}
		}

		if (flags & FI_RECV || flags & FI_REMOTE_READ ||
		    flags & FI_REMOTE_WRITE) {
			for (i = 0; i < ep->ep_attr.rx_ctx_cnt; i++) {
				rx_ctx = ep->rx_array[i];

				if (!rx_ctx)
					continue;

				if (rx_ctx->ctx.fid.fclass == FI_CLASS_SRX_CTX) {
					if (flags & FI_RECV)
						rx_ctx->comp.recv_cntr = cntr;

					if (flags & FI_REMOTE_READ)
						rx_ctx->comp.rem_read_cntr = cntr;

					if (flags & FI_REMOTE_WRITE)
						rx_ctx->comp.rem_write_cntr = cntr;

					fastlock_acquire(&cntr->list_lock);
					dlist_insert_tail(&rx_ctx->cntr_entry,
								&cntr->rx_list);
					fastlock_release(&cntr->list_lock);
					continue;
				}

				ret = sock_ctx_bind_cntr(&rx_ctx->ctx.fid, bfid,
								flags);
				if (ret)
					return ret;
			}
		}
		break;

	case FI_CLASS_AV:
		av = container_of(bfid, struct sock_av, av_fid.fid);
		if (ep->domain != av->domain)
			return -FI_EINVAL;

		ep->av = av;
		atomic_inc(&av->ref);

		if (ep->tx_ctx &&
		    ep->tx_ctx->fid.ctx.fid.fclass == FI_CLASS_TX_CTX) {
			ep->tx_ctx->av = av;
		}

		if (ep->rx_ctx &&
		    ep->rx_ctx->ctx.fid.fclass == FI_CLASS_RX_CTX)
			ep->rx_ctx->av = av;

		for (i = 0; i < ep->ep_attr.tx_ctx_cnt; i++) {
			if (ep->tx_array[i])
				ep->tx_array[i]->av = av;
		}

		for (i = 0; i < ep->ep_attr.rx_ctx_cnt; i++) {
			if (ep->rx_array[i])
				ep->rx_array[i]->av = av;
		}

		break;

	case FI_CLASS_STX_CTX:
		tx_ctx = container_of(bfid, struct sock_tx_ctx, fid.stx.fid);
		fastlock_acquire(&tx_ctx->lock);
		dlist_insert_tail(&ep->tx_ctx_entry, &tx_ctx->ep_list);
		fastlock_release(&tx_ctx->lock);
		ep->tx_ctx = tx_ctx;
		ep->tx_array[0] = tx_ctx;
		break;

	case FI_CLASS_SRX_CTX:
		rx_ctx = container_of(bfid, struct sock_rx_ctx, ctx);
		fastlock_acquire(&rx_ctx->lock);
		dlist_insert_tail(&ep->rx_ctx_entry, &rx_ctx->ep_list);
		fastlock_release(&rx_ctx->lock);
		ep->rx_ctx = rx_ctx;
		ep->rx_array[0] = rx_ctx;
		break;

	default:
		return -ENOSYS;
	}

	return 0;
}

static int sock_ep_control(struct fid *fid, int command, void *arg)
{
	struct fid_ep *ep_fid;
	struct fi_alias *alias;
	struct sock_ep *ep, *new_ep;

	switch (fid->fclass) {
	case FI_CLASS_EP:
		ep = container_of(fid, struct sock_ep, ep.fid);
		break;

	case FI_CLASS_SEP:
		ep = container_of(fid, struct sock_ep, ep.fid);
		break;

	default:
		return -FI_EINVAL;
	}

	switch (command) {
	case FI_ALIAS:
		alias = (struct fi_alias *)arg;
		new_ep = calloc(1, sizeof(*new_ep));
		if (!new_ep)
			return -FI_ENOMEM;
		*new_ep = *ep;
		new_ep->op_flags = alias->flags;
		*alias->fid = &new_ep->ep.fid;
		break;

	case FI_GETOPSFLAG:
		*(uint64_t *) arg = ep->op_flags;
		break;
	case FI_SETOPSFLAG:
		ep->op_flags = *(uint64_t *) arg;
		ep->op_flags |= FI_TRANSMIT_COMPLETE;
		break;
	case FI_ENABLE:
		ep_fid = container_of(fid, struct fid_ep, fid);
		return sock_ep_enable(ep_fid);

	default:
		return -FI_EINVAL;
	}
	return 0;
}


struct fi_ops sock_ep_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = sock_ep_close,
	.bind = sock_ep_bind,
	.control = sock_ep_control,
	.ops_open = fi_no_ops_open,
};

int sock_ep_enable(struct fid_ep *ep)
{
	int i;
	struct sock_ep *sock_ep;

	sock_ep = container_of(ep, struct sock_ep, ep);

	if (sock_ep->tx_ctx &&
	    sock_ep->tx_ctx->fid.ctx.fid.fclass == FI_CLASS_TX_CTX) {
		sock_ep->tx_ctx->enabled = 1;
		if (!sock_ep->tx_ctx->progress) {
			sock_pe_add_tx_ctx(sock_ep->domain->pe, sock_ep->tx_ctx);
			sock_ep->tx_ctx->progress = 1;
		}
	}

	if (sock_ep->rx_ctx &&
	    sock_ep->rx_ctx->ctx.fid.fclass == FI_CLASS_RX_CTX) {
		sock_ep->rx_ctx->enabled = 1;
		if (!sock_ep->rx_ctx->progress) {
				sock_pe_add_rx_ctx(sock_ep->domain->pe,
							sock_ep->rx_ctx);
				sock_ep->rx_ctx->progress = 1;
		}
	}

	for (i = 0; i < sock_ep->ep_attr.tx_ctx_cnt; i++) {
		if (sock_ep->tx_array[i]) {
			sock_ep->tx_array[i]->enabled = 1;
			if (!sock_ep->tx_array[i]->progress) {
				sock_pe_add_tx_ctx(sock_ep->domain->pe,
							sock_ep->tx_array[i]);
				sock_ep->tx_array[i]->progress = 1;
			}
		}
	}

	for (i = 0; i < sock_ep->ep_attr.rx_ctx_cnt; i++) {
		if (sock_ep->rx_array[i]) {
			sock_ep->rx_array[i]->enabled = 1;
			if (!sock_ep->rx_array[i]->progress) {
				sock_pe_add_rx_ctx(sock_ep->domain->pe,
							sock_ep->rx_array[i]);
				sock_ep->rx_array[i]->progress = 1;
			}
		}
	}

	if (sock_ep->ep_type != FI_EP_MSG &&
	    !sock_ep->listener.listener_thread && sock_conn_listen(sock_ep))
		SOCK_LOG_ERROR("cannot start connection thread\n");
	return 0;
}

int sock_ep_disable(struct fid_ep *ep)
{
	int i;
	struct sock_ep *sock_ep;

	sock_ep = container_of(ep, struct sock_ep, ep);

	if (sock_ep->tx_ctx &&
	    sock_ep->tx_ctx->fid.ctx.fid.fclass == FI_CLASS_TX_CTX) {
		sock_ep->tx_ctx->enabled = 0;
	}

	if (sock_ep->rx_ctx &&
	    sock_ep->rx_ctx->ctx.fid.fclass == FI_CLASS_RX_CTX) {
		sock_ep->rx_ctx->enabled = 0;
	}

	for (i = 0; i < sock_ep->ep_attr.tx_ctx_cnt; i++) {
		if (sock_ep->tx_array[i])
			sock_ep->tx_array[i]->enabled = 0;
	}

	for (i = 0; i < sock_ep->ep_attr.rx_ctx_cnt; i++) {
		if (sock_ep->rx_array[i])
			sock_ep->rx_array[i]->enabled = 0;
	}
	sock_ep->is_disabled = 1;
	return 0;
}

static int sock_ep_getopt(fid_t fid, int level, int optname,
		       void *optval, size_t *optlen)
{
	struct sock_ep *sock_ep;
	sock_ep = container_of(fid, struct sock_ep, ep.fid);

	if (level != FI_OPT_ENDPOINT)
		return -ENOPROTOOPT;

	switch (optname) {
	case FI_OPT_MIN_MULTI_RECV:
		*(size_t *)optval = sock_ep->min_multi_recv;
		*optlen = sizeof(size_t);
		break;

	default:
		return -FI_ENOPROTOOPT;
	}
	return 0;
}

static int sock_ep_setopt(fid_t fid, int level, int optname,
		       const void *optval, size_t optlen)
{
	int i;
	struct sock_ep *sock_ep;
	sock_ep = container_of(fid, struct sock_ep, ep.fid);

	if (level != FI_OPT_ENDPOINT)
		return -ENOPROTOOPT;

	switch (optname) {
	case FI_OPT_MIN_MULTI_RECV:

		sock_ep->min_multi_recv = *(size_t *)optval;
		for (i = 0; i < sock_ep->ep_attr.rx_ctx_cnt; i++) {
			if (sock_ep->rx_array[i] != NULL) {
				sock_ep->rx_array[i]->min_multi_recv =
					sock_ep->min_multi_recv;
			}
		}
		break;

	default:
		return -ENOPROTOOPT;
	}
	return 0;
}

static int sock_ep_tx_ctx(struct fid_ep *ep, int index, struct fi_tx_attr *attr,
			  struct fid_ep **tx_ep, void *context)
{
	struct sock_ep *sock_ep;
	struct sock_tx_ctx *tx_ctx;

	sock_ep = container_of(ep, struct sock_ep, ep);
	if (sock_ep->fclass != FI_CLASS_SEP ||
		index >= sock_ep->ep_attr.tx_ctx_cnt)
		return -FI_EINVAL;

	tx_ctx = sock_tx_ctx_alloc(&sock_ep->tx_attr, context);
	if (!tx_ctx)
		return -FI_ENOMEM;

	tx_ctx->tx_id = index;
	tx_ctx->ep = sock_ep;
	tx_ctx->domain = sock_ep->domain;
	tx_ctx->av = sock_ep->av;
	dlist_insert_tail(&sock_ep->tx_ctx_entry, &tx_ctx->ep_list);

	tx_ctx->fid.ctx.fid.ops = &sock_ctx_ops;
	tx_ctx->fid.ctx.ops = &sock_ctx_ep_ops;
	tx_ctx->fid.ctx.msg = &sock_ep_msg_ops;
	tx_ctx->fid.ctx.tagged = &sock_ep_tagged;
	tx_ctx->fid.ctx.rma = &sock_ep_rma;
	tx_ctx->fid.ctx.atomic = &sock_ep_atomic;

	*tx_ep = &tx_ctx->fid.ctx;
	sock_ep->tx_array[index] = tx_ctx;
	atomic_inc(&sock_ep->num_tx_ctx);
	atomic_inc(&sock_ep->domain->ref);
	return 0;
}

static int sock_ep_rx_ctx(struct fid_ep *ep, int index, struct fi_rx_attr *attr,
		    struct fid_ep **rx_ep, void *context)
{
	struct sock_ep *sock_ep;
	struct sock_rx_ctx *rx_ctx;

	sock_ep = container_of(ep, struct sock_ep, ep);
	if (sock_ep->fclass != FI_CLASS_SEP ||
		index >= sock_ep->ep_attr.rx_ctx_cnt)
		return -FI_EINVAL;

	rx_ctx = sock_rx_ctx_alloc(attr ? attr : &sock_ep->rx_attr, context);
	if (!rx_ctx)
		return -FI_ENOMEM;

	rx_ctx->rx_id = index;
	rx_ctx->ep = sock_ep;
	rx_ctx->domain = sock_ep->domain;
	rx_ctx->av = sock_ep->av;
	dlist_insert_tail(&sock_ep->rx_ctx_entry, &rx_ctx->ep_list);

	rx_ctx->ctx.fid.ops = &sock_ctx_ops;
	rx_ctx->ctx.ops = &sock_ctx_ep_ops;
	rx_ctx->ctx.msg = &sock_ep_msg_ops;
	rx_ctx->ctx.tagged = &sock_ep_tagged;

	rx_ctx->min_multi_recv = sock_ep->min_multi_recv;
	*rx_ep = &rx_ctx->ctx;
	sock_ep->rx_array[index] = rx_ctx;
	atomic_inc(&sock_ep->num_rx_ctx);
	atomic_inc(&sock_ep->domain->ref);
	return 0;
}

struct fi_ops_ep sock_ep_ops = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = sock_ep_cancel,
	.getopt = sock_ep_getopt,
	.setopt = sock_ep_setopt,
	.tx_ctx = sock_ep_tx_ctx,
	.rx_ctx = sock_ep_rx_ctx,
	.rx_size_left = sock_rx_size_left,
	.tx_size_left = sock_tx_size_left,
};

static int sock_verify_tx_attr(const struct fi_tx_attr *attr)
{
	if (!attr)
		return 0;

	if (attr->inject_size > SOCK_EP_MAX_INJECT_SZ)
		return -FI_ENODATA;

	if (attr->size > SOCK_EP_TX_SZ)
		return -FI_ENODATA;

	if (attr->iov_limit > SOCK_EP_MAX_IOV_LIMIT)
		return -FI_ENODATA;

	if (attr->rma_iov_limit > SOCK_EP_MAX_IOV_LIMIT)
		return -FI_ENODATA;

	return 0;
}

int sock_stx_ctx(struct fid_domain *domain,
		 struct fi_tx_attr *attr, struct fid_stx **stx, void *context)
{
	struct sock_domain *dom;
	struct sock_tx_ctx *tx_ctx;

	if (attr && sock_verify_tx_attr(attr))
		return -FI_EINVAL;

	dom = container_of(domain, struct sock_domain, dom_fid);

	tx_ctx = sock_stx_ctx_alloc(attr ? attr : &sock_stx_attr, context);
	if (!tx_ctx)
		return -FI_ENOMEM;

	tx_ctx->domain = dom;
	tx_ctx->fid.stx.fid.ops = &sock_ctx_ops;
	tx_ctx->fid.stx.ops = &sock_ep_ops;
	atomic_inc(&dom->ref);

	*stx = &tx_ctx->fid.stx;
	return 0;
}

static int sock_verify_rx_attr(const struct fi_rx_attr *attr)
{
	if (!attr)
		return 0;

	if ((attr->msg_order | SOCK_EP_MSG_ORDER) != SOCK_EP_MSG_ORDER)
		return -FI_ENODATA;

	if ((attr->comp_order | SOCK_EP_COMP_ORDER) != SOCK_EP_COMP_ORDER)
		return -FI_ENODATA;

	if (attr->total_buffered_recv > SOCK_EP_MAX_BUFF_RECV)
		return -FI_ENODATA;

	if (attr->size > SOCK_EP_TX_SZ)
		return -FI_ENODATA;

	if (attr->iov_limit > SOCK_EP_MAX_IOV_LIMIT)
		return -FI_ENODATA;

	return 0;
}

int sock_srx_ctx(struct fid_domain *domain,
		 struct fi_rx_attr *attr, struct fid_ep **srx, void *context)
{
	struct sock_domain *dom;
	struct sock_rx_ctx *rx_ctx;

	if (attr && sock_verify_rx_attr(attr))
		return -FI_EINVAL;

	dom = container_of(domain, struct sock_domain, dom_fid);
	rx_ctx = sock_rx_ctx_alloc(attr ? attr : &sock_srx_attr, context);
	if (!rx_ctx)
		return -FI_ENOMEM;

	rx_ctx->domain = dom;
	rx_ctx->ctx.fid.fclass = FI_CLASS_SRX_CTX;

	rx_ctx->ctx.fid.ops = &sock_ctx_ops;
	rx_ctx->ctx.ops = &sock_ctx_ep_ops;
	rx_ctx->ctx.msg = &sock_ep_msg_ops;
	rx_ctx->ctx.tagged = &sock_ep_tagged;

	/* default config */
	rx_ctx->min_multi_recv = SOCK_EP_MIN_MULTI_RECV;
	*srx = &rx_ctx->ctx;
	atomic_inc(&dom->ref);
	return 0;
}

static void sock_set_fabric_attr(const struct fi_fabric_attr *hint_attr,
				 struct fi_fabric_attr *attr)
{
	struct sock_fabric *fabric;

	*attr = sock_fabric_attr;
	if (hint_attr && hint_attr->fabric) {
		attr->fabric = hint_attr->fabric;
	} else {
		fabric = sock_fab_list_head();
		attr->fabric = fabric ? &fabric->fab_fid : NULL;
	}
	attr->name = strdup(sock_fab_name);
	attr->prov_name = NULL;
}

static void sock_set_domain_attr(const struct fi_domain_attr *hint_attr,
				 struct fi_domain_attr *attr)
{
	struct sock_domain *domain;

	domain = sock_dom_list_head();
	attr->domain = domain ? &domain->dom_fid : NULL;
	if (!hint_attr) {
		*attr = sock_domain_attr;
		goto out;
	}

	if (hint_attr->domain) {
		domain = container_of(hint_attr->domain,
				      struct sock_domain, dom_fid);
		*attr = domain->attr;
		attr->domain = hint_attr->domain;
		goto out;
	}

	*attr = *hint_attr;
	if (attr->threading == FI_THREAD_UNSPEC)
		attr->threading = sock_domain_attr.threading;
	if (attr->control_progress == FI_PROGRESS_UNSPEC)
		attr->control_progress = sock_domain_attr.control_progress;
	if (attr->data_progress == FI_PROGRESS_UNSPEC)
		attr->data_progress = sock_domain_attr.data_progress;
	if (attr->mr_mode == FI_MR_UNSPEC)
		attr->mr_mode = sock_domain_attr.mr_mode;

	if (attr->cq_cnt == 0)
		attr->cq_cnt = sock_domain_attr.cq_cnt;
	if (attr->ep_cnt == 0)
		attr->ep_cnt = sock_domain_attr.ep_cnt;
	if (attr->tx_ctx_cnt == 0)
		attr->tx_ctx_cnt = sock_domain_attr.tx_ctx_cnt;
	if (attr->rx_ctx_cnt == 0)
		attr->rx_ctx_cnt = sock_domain_attr.rx_ctx_cnt;
	if (attr->max_ep_tx_ctx == 0)
		attr->max_ep_tx_ctx = sock_domain_attr.max_ep_tx_ctx;
	if (attr->max_ep_rx_ctx == 0)
		attr->max_ep_rx_ctx = sock_domain_attr.max_ep_rx_ctx;

	attr->mr_key_size = sock_domain_attr.mr_key_size;
	attr->cq_data_size = sock_domain_attr.cq_data_size;
	attr->resource_mgmt = sock_domain_attr.resource_mgmt;
out:
	attr->name = strdup(sock_dom_name);
}


struct fi_info *sock_fi_info(enum fi_ep_type ep_type, struct fi_info *hints,
			     void *src_addr, void *dest_addr)
{
	struct fi_info *info;

	info = fi_allocinfo();
	if (!info)
		return NULL;

	info->src_addr = calloc(1, sizeof(struct sockaddr_in));
	if (!info->src_addr)
		goto err;

	info->mode = SOCK_MODE;
	info->addr_format = FI_SOCKADDR_IN;

	if (src_addr) {
		memcpy(info->src_addr, src_addr, sizeof(struct sockaddr_in));
		info->src_addrlen = sizeof(struct sockaddr_in);
	}

	if (dest_addr) {
		info->dest_addr = calloc(1, sizeof(struct sockaddr_in));
		if (!info->dest_addr)
			goto err;
		info->dest_addrlen = sizeof(struct sockaddr_in);
		memcpy(info->dest_addr, dest_addr, sizeof(struct sockaddr_in));
	}

	if (hints) {
		if (hints->caps)
			info->caps = hints->caps;

		if (hints->ep_attr)
			*(info->ep_attr) = *(hints->ep_attr);

		if (hints->tx_attr)
			*(info->tx_attr) = *(hints->tx_attr);

		if (hints->rx_attr)
			*(info->rx_attr) = *(hints->rx_attr);

		if (hints->handle)
			info->handle = hints->handle;

		sock_set_domain_attr(hints->domain_attr, info->domain_attr);
		sock_set_fabric_attr(hints->fabric_attr, info->fabric_attr);
	} else {
		sock_set_domain_attr(NULL, info->domain_attr);
		sock_set_fabric_attr(NULL, info->fabric_attr);
	}

	info->ep_attr->type = ep_type;
	return info;
err:
	free(info->src_addr);
	free(info->dest_addr);
	free(info);
	return NULL;
}

int sock_get_src_addr_from_hostname(struct sockaddr_in *src_addr,
					const char *service)
{
	int ret;
	struct addrinfo ai, *rai = NULL;
	char hostname[HOST_NAME_MAX];

	memset(&ai, 0, sizeof(ai));
	ai.ai_family = AF_INET;
	ai.ai_socktype = SOCK_STREAM;

	sock_getnodename(hostname, sizeof(hostname));
	ret = getaddrinfo(hostname, service, &ai, &rai);
	if (ret) {
		SOCK_LOG_DBG("getaddrinfo failed!\n");
		return -FI_EINVAL;
	}
	memcpy(src_addr, (struct sockaddr_in *)rai->ai_addr,
		sizeof(*src_addr));
	freeaddrinfo(rai);
	return 0;
}

static int sock_ep_assign_src_addr(struct sock_ep *sock_ep, struct fi_info *info)
{
	sock_ep->src_addr = calloc(1, sizeof(struct sockaddr_in));
	if (!sock_ep->src_addr)
		return -FI_ENOMEM;

	if (info && info->dest_addr)
		return sock_get_src_addr(info->dest_addr, sock_ep->src_addr);
	else
		return sock_get_src_addr_from_hostname(sock_ep->src_addr, NULL);
}

int sock_alloc_endpoint(struct fid_domain *domain, struct fi_info *info,
		  struct sock_ep **ep, void *context, size_t fclass)
{
	int ret, flags;
	struct sock_ep *sock_ep;
	struct sock_tx_ctx *tx_ctx;
	struct sock_rx_ctx *rx_ctx;
	struct sock_domain *sock_dom;

	if (info) {
		ret = sock_verify_info(info);
		if (ret) {
			SOCK_LOG_DBG("Cannot support requested options!\n");
			return -FI_EINVAL;
		}
	}

	sock_dom = container_of(domain, struct sock_domain, dom_fid);
	sock_ep = (struct sock_ep *) calloc(1, sizeof(*sock_ep));
	if (!sock_ep)
		return -FI_ENOMEM;

	switch (fclass) {
	case FI_CLASS_EP:
		sock_ep->ep.fid.fclass = FI_CLASS_EP;
		sock_ep->ep.fid.context = context;
		sock_ep->ep.fid.ops = &sock_ep_fi_ops;

		sock_ep->ep.ops = &sock_ep_ops;
		sock_ep->ep.cm = &sock_ep_cm_ops;
		sock_ep->ep.msg = &sock_ep_msg_ops;
		sock_ep->ep.rma = &sock_ep_rma;
		sock_ep->ep.tagged = &sock_ep_tagged;
		sock_ep->ep.atomic = &sock_ep_atomic;
		break;

	case FI_CLASS_SEP:
		sock_ep->ep.fid.fclass = FI_CLASS_SEP;
		sock_ep->ep.fid.context = context;
		sock_ep->ep.fid.ops = &sock_ep_fi_ops;

		sock_ep->ep.ops = &sock_ep_ops;
		sock_ep->ep.cm = &sock_ep_cm_ops;
		break;

	default:
		ret = -FI_EINVAL;
		goto err;
	}

	sock_ep->fclass = fclass;
	*ep = sock_ep;

	if (info) {
		sock_ep->info.caps = info->caps;
		sock_ep->info.addr_format = FI_SOCKADDR_IN;

		if (info->ep_attr) {
			sock_ep->ep_type = info->ep_attr->type;
			sock_ep->ep_attr.tx_ctx_cnt = info->ep_attr->tx_ctx_cnt;
			sock_ep->ep_attr.rx_ctx_cnt = info->ep_attr->rx_ctx_cnt;
		}

		if (info->src_addr) {
			sock_ep->src_addr = calloc(1, sizeof(struct sockaddr_in));
			if (!sock_ep->src_addr) {
				ret = -FI_ENOMEM;
				goto err;
			}
			memcpy(sock_ep->src_addr, info->src_addr,
			       sizeof(struct sockaddr_in));
		}

		if (info->dest_addr) {
			sock_ep->dest_addr = calloc(1, sizeof(struct sockaddr_in));
			if (!sock_ep->dest_addr) {
				ret = -FI_ENOMEM;
				goto err;
			}
			memcpy(sock_ep->dest_addr, info->dest_addr,
			       sizeof(struct sockaddr_in));
		}

		if (info->tx_attr) {
			sock_ep->tx_attr = *info->tx_attr;
			sock_ep->op_flags = info->tx_attr->op_flags;
			sock_ep->tx_attr.size = sock_ep->tx_attr.size ?
				sock_ep->tx_attr.size :
				(SOCK_EP_TX_SZ * SOCK_EP_TX_ENTRY_SZ);
			sock_ep->op_flags |= FI_TRANSMIT_COMPLETE;
		}

		if (info->rx_attr) {
			sock_ep->rx_attr = *info->rx_attr;
			sock_ep->op_flags |= info->rx_attr->op_flags;
		}
		sock_ep->info.handle = info->handle;
	}

	if (!sock_ep->src_addr && sock_ep_assign_src_addr(sock_ep, info)) {
		SOCK_LOG_ERROR("failed to get src_address\n");
		ret = -FI_EINVAL;
		goto err;
	}

	atomic_initialize(&sock_ep->ref, 0);
	atomic_initialize(&sock_ep->num_tx_ctx, 0);
	atomic_initialize(&sock_ep->num_rx_ctx, 0);
	fastlock_init(&sock_ep->lock);
	dlist_init(&sock_ep->conn_list);

	if (sock_ep->ep_attr.tx_ctx_cnt == FI_SHARED_CONTEXT)
		sock_ep->tx_shared = 1;
	if (sock_ep->ep_attr.rx_ctx_cnt == FI_SHARED_CONTEXT)
		sock_ep->rx_shared = 1;

	if (sock_ep->fclass != FI_CLASS_SEP) {
		sock_ep->ep_attr.tx_ctx_cnt = 1;
		sock_ep->ep_attr.rx_ctx_cnt = 1;
	}

	sock_ep->tx_array = calloc(sock_ep->ep_attr.tx_ctx_cnt,
				   sizeof(struct sock_tx_ctx *));
	if (!sock_ep->tx_array) {
		ret = -FI_ENOMEM;
		goto err;
	}

	sock_ep->rx_array = calloc(sock_ep->ep_attr.rx_ctx_cnt,
				   sizeof(struct sock_rx_ctx *));
	if (!sock_ep->rx_array) {
		ret = -FI_ENOMEM;
		goto err;
	}

	if (sock_ep->fclass != FI_CLASS_SEP &&
	    sock_ep->ep_attr.tx_ctx_cnt != FI_SHARED_CONTEXT) {
		/* default tx ctx */
		tx_ctx = sock_tx_ctx_alloc(&sock_ep->tx_attr, context);
		if (!tx_ctx) {
			ret = -FI_ENOMEM;
			goto err;
		}
		tx_ctx->ep = sock_ep;
		tx_ctx->domain = sock_dom;
		tx_ctx->tx_id = 0;
		dlist_insert_tail(&sock_ep->tx_ctx_entry, &tx_ctx->ep_list);
		sock_ep->tx_array[0] = tx_ctx;
		sock_ep->tx_ctx = tx_ctx;
	}

	if (sock_ep->fclass != FI_CLASS_SEP &&
	    sock_ep->ep_attr.rx_ctx_cnt != FI_SHARED_CONTEXT) {
		/* default rx_ctx */
		rx_ctx = sock_rx_ctx_alloc(&sock_ep->rx_attr, context);
		if (!rx_ctx) {
			ret = -FI_ENOMEM;
			goto err;
		}
		rx_ctx->ep = sock_ep;
		rx_ctx->domain = sock_dom;
		rx_ctx->rx_id = 0;
		dlist_insert_tail(&sock_ep->rx_ctx_entry, &rx_ctx->ep_list);
		sock_ep->rx_array[0] = rx_ctx;
		sock_ep->rx_ctx = rx_ctx;
	}

	/* default config */
	sock_ep->min_multi_recv = SOCK_EP_MIN_MULTI_RECV;

	if (info)
		memcpy(&sock_ep->info, info, sizeof(struct fi_info));

	sock_ep->domain = sock_dom;
	fastlock_init(&sock_ep->cm.lock);
	if (sock_ep->ep_type == FI_EP_MSG) {
		dlist_init(&sock_ep->cm.msg_list);
		if (socketpair(AF_UNIX, SOCK_STREAM, 0,
			       sock_ep->cm.signal_fds) < 0) {
			ret = -FI_EINVAL;
			goto err;
		}

		flags = fcntl(sock_ep->cm.signal_fds[1], F_GETFL, 0);
		if (fcntl(sock_ep->cm.signal_fds[1], F_SETFL, flags | O_NONBLOCK))
			SOCK_LOG_ERROR("fcntl failed");
	}

	if (sock_conn_map_init(sock_ep, sock_cm_def_map_sz)) {
		SOCK_LOG_ERROR("failed to init connection map: %s\n", strerror(errno));
		ret = -FI_EINVAL;
		goto err;
	}

	atomic_inc(&sock_dom->ref);
	return 0;

err:
	if (sock_ep->src_addr)
		free(sock_ep->src_addr);
	if (sock_ep->dest_addr)
		free(sock_ep->dest_addr);
	free(sock_ep);
	return ret;
}

struct sock_conn *sock_ep_lookup_conn(struct sock_ep *ep, fi_addr_t index,
					struct sockaddr_in *addr)
{
	int i;
	uint16_t idx;
	struct sock_conn *conn;

	idx = (ep->ep_type == FI_EP_MSG) ? index : index & ep->av->mask;
	conn = idm_lookup(&ep->av_idm, idx);
	if (conn && conn != SOCK_CM_CONN_IN_PROGRESS) {
		assert(sock_compare_addr(&conn->addr, addr));
		return conn;
	}

	for (i = 0; i < ep->cmap.used; i++) {
		if (sock_compare_addr(&ep->cmap.table[i].addr, addr))
			return &ep->cmap.table[i];
	}
	return conn;
}

int sock_ep_get_conn(struct sock_ep *ep, struct sock_tx_ctx *tx_ctx,
		     fi_addr_t index, struct sock_conn **pconn)
{
	struct sock_conn *conn;
	uint64_t av_index = (ep->ep_type == FI_EP_MSG) ? 0 : (index & ep->av->mask);
	struct sockaddr_in *addr;

	if (ep->ep_type == FI_EP_MSG)
		addr = ep->dest_addr;
	else
		addr = (struct sockaddr_in *)&ep->av->table[av_index].addr;

	fastlock_acquire(&ep->cmap.lock);
	conn = sock_ep_lookup_conn(ep, av_index, addr);
	if (!conn) {
		conn = SOCK_CM_CONN_IN_PROGRESS;
		idm_set(&ep->av_idm, av_index, conn);
	}
	fastlock_release(&ep->cmap.lock);

	if (conn == SOCK_CM_CONN_IN_PROGRESS)
		conn = sock_ep_connect(ep, av_index);

	if (!conn) {
		SOCK_LOG_ERROR("Error in connecting: %s\n", strerror(errno));
		if (errno == EINPROGRESS)
			return -FI_EAGAIN;
		else
			return -errno;
	}

	*pconn = conn;
	return conn->address_published ? 0 : sock_conn_send_src_addr(ep, tx_ctx, conn);
}
