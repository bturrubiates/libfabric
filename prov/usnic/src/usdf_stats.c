#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include <fi.h>

#include "usdf.h"

void usdf_reset_stats(struct fid_ep *fep)
{
	const struct usdf_ep_stats cleared = {0};
	struct usdf_ep *ep;

	ep = ep_ftou(fep);

	if (ep->ep_tx)
		ep->ep_tx->stats = cleared;

	if (ep->ep_rx)
		ep->ep_rx->stats = cleared;
}

void usdf_add_stats(struct usdf_ep_stats *a, struct usdf_ep_stats *b)
{
	a->num_total_recvs += b->num_total_recvs;
	a->num_total_sends += b->num_total_sends;

	a->num_success_recvs += b->num_success_recvs;
	a->num_error_crc_recvs += b->num_error_crc_recvs;
	a->num_error_trunc_recvs += b->num_error_trunc_recvs;
	a->num_error_timeout_recvs += b->num_error_timeout_recvs;
	a->num_error_internal_recvs += b->num_error_internal_recvs;
	a->num_error_unknown_recvs += b->num_error_unknown_recvs;

	a->num_success_sends += b->num_success_sends;

	a->num_ack_sends += b->num_ack_sends;
	a->num_nak_sends += b->num_nak_sends;

	a->num_ack_recvs += b->num_ack_recvs;
	a->num_nak_recvs += b->num_nak_recvs;

	a->num_rx_overruns += b->num_rx_overruns;
}

void usdf_print_metric(const char *key, uint64_t value)
{
	fprintf(stderr, "%*s:" "%*" PRIu64 "\n", 30, key, 30, value);
}

void usdf_print_stats(struct fid_ep *fep)
{
	struct usdf_ep_stats stats = {0};
	struct usdf_ep *ep;

	ep = ep_ftou(fep);

	if (ep->ep_tx)
		usdf_add_stats(&stats, &ep->ep_tx->stats);

	if (ep->ep_rx)
		usdf_add_stats(&stats, &ep->ep_rx->stats);

	fprintf(stderr,
		"********************************************************************************\n");

	usdf_print_metric("Number of total recvs", stats.num_total_recvs);
	usdf_print_metric("Number of total sends", stats.num_total_sends);

	usdf_print_metric("Number of successful recvs",
			stats.num_success_recvs);
	usdf_print_metric("Number of CRC errors", stats.num_error_crc_recvs);
	usdf_print_metric("Number of truncation errors",
			stats.num_error_trunc_recvs);
	usdf_print_metric("Number of timeout errors",
			stats.num_error_timeout_recvs);
	usdf_print_metric("Number of internal errors",
			stats.num_error_internal_recvs);
	usdf_print_metric("Number of unknown errors",
			stats.num_error_unknown_recvs);

	usdf_print_metric("Number of successful sends",
			stats.num_success_sends);

	usdf_print_metric("Number of acks sent",
			stats.num_ack_sends);
	usdf_print_metric("Number of naks sent",
			stats.num_nak_sends);

	usdf_print_metric("Number of ack recvs",
			stats.num_ack_recvs);
	usdf_print_metric("Number of nak recvs",
			stats.num_nak_recvs);

	usdf_print_metric("Number of rx overruns",
			stats.num_rx_overruns);

	fprintf(stderr,
		"********************************************************************************\n");
}
