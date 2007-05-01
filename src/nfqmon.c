/* nfqmon.c
 * Copyright (C) 2006 Tillmann Werner <tillmann.werner@gmx.de>
 *
 * This file is free software; as a special exception the author gives
 * unlimited permission to copy and/or distribute it, with or without
 * modifications, as long as this notice is preserved.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "honeytrap.h"
#ifdef USE_NFQ_MON

#include <errno.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "logging.h"
#include "dynsrv.h"
#include "ctrl.h"
#include "nfqmon.h"

/* Set BUFSIZE to 1500 (ethernet frame size) to prevent
 * errors with ipq_read due to truncated messages.
 * This is only necessary for UDP. A buffer size of
 * 256 seems to be enough to hanlde TCP when there is
 * no data on the SYNs */
#define BUFSIZE 1500


static int server_wrapper(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfa, void *data) {
	int ret;
	struct ip_header *ip;
	struct udp_header *udp;
	struct tcp_header *tcp;
	char *payload;
	uint16_t sport, dport;
	u_int8_t port_mode;
	struct nfqnl_msg_packet_hdr *ph;

	ret		= -1;
	id		= 0;
	sport		= 0;
	dport		= 0;
	port_mode	= PORTCONF_IGNORE;
	ip		= NULL;
	udp		= NULL;
	tcp		= NULL;


	if ((ph = nfq_get_msg_packet_hdr(nfa)) > 0) id = ntohl(ph->packet_id);

	if ((ret = nfq_get_payload(nfa, &payload)) >= 0) {
		ip = (struct ip_header*) payload;
		if (ip->ip_p == TCP) {
			tcp	= (struct tcp_header*) (payload + (4 * ip->ip_hlen));
			sport	= ntohs(tcp->th_sport);
			dport	= ntohs(tcp->th_dport);
			port_mode = port_flags[dport].tcp;
		} else if (ip->ip_p == UDP) {
			udp	= (struct udp_header*) (payload + (4 * ip->ip_hlen));
			sport	= ntohs(udp->uh_sport);
			dport	= ntohs(udp->uh_dport);
			port_mode = port_flags[dport].udp;
		} else {
			logmsg(LOG_ERR, 1, "Error - Protocol %u is not supported.\n", ip->ip_p);
			return(-1);
		}
	}

	switch (port_mode) {
	case PORTCONF_NONE:
		logmsg(LOG_DEBUG, 1, "Port %u/%s has no explicit configuration.\n", dport, PROTO(ip->ip_p));
		break;
	case PORTCONF_IGNORE:
		logmsg(LOG_DEBUG, 1, "Port %u/%s is configured to be ignored.\n", dport, PROTO(ip->ip_p));
		nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
		return(0);
	case PORTCONF_NORMAL:
		logmsg(LOG_DEBUG, 1, "Port %u/%s is configured to be handled in normal mode.\n", dport, PROTO(ip->ip_p));
		break;
	case PORTCONF_MIRROR:
		logmsg(LOG_DEBUG, 1, "Port %u/%s is configured to be handled in mirror mode.\n", dport, PROTO(ip->ip_p));
		break;
	case PORTCONF_PROXY:
		logmsg(LOG_DEBUG, 1, "Port %u/%s is configured to be handled in proxy mode\n", dport, PROTO(ip->ip_p));
		break;
	default:
		logmsg(LOG_ERR, 1, "Error - Invalid explicit configuration for port %u/%s.\n", dport, PROTO(ip->ip_p));
		nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
		return(0);
	}

	logmsg(LOG_INFO, 1, "Connection request on port %d/%s.\n", dport, PROTO(ip->ip_p));
	start_dynamic_server(ip->ip_src, htons(sport), ip->ip_dst, htons(dport), ip->ip_p);
	
	/* nfq_set_verdict()'s return value is really confusing and documented nowhere */
//	nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL); 

	return(1);
}



int start_nfq_mon(void) {
	struct nfq_handle *h;
	struct nfnl_handle *nh;
	int fd, rv;
	char buf[4096];

	h	= NULL;
	qh	= NULL;
	nh	= NULL;
	fd	= -1;
	rv	= -1;

	logmsg(LOG_DEBUG, 1, "Creating NFQ connection monitor.\n");
	if ((h = nfq_open()) < 0) {
		logmsg(LOG_ERR, 1, "Error - Could not create NFQ handle: %s\n", strerror(errno));
		clean_exit(0);
	}

	if (nfq_unbind_pf(h, AF_INET) < 0) {
		logmsg(LOG_ERR, 1, "Error - Could not unbind existing NFQ handle: %s\n", strerror(errno));
		clean_exit(0);
	}

	if (nfq_bind_pf(h, AF_INET) < 0) {
		logmsg(LOG_ERR, 1, "Error - Could not unbind existing NFQ handle: %s\n", strerror(errno));
		clean_exit(0);
	}

	if ((qh = nfq_create_queue(h,  0, &server_wrapper, NULL)) == NULL) {
		logmsg(LOG_ERR, 1, "Error - Could not create NFQ queue handle: %s\n", strerror(errno));
		clean_exit(0);
	}

	if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
		logmsg(LOG_ERR, 1, "Error - Could not set NFQ mode.\n");
		clean_exit(0);
	}

	/* to what is publicly documented checking retvals is unnecessary here
	 * because these funcs do not perform any tests on validity of passed arguments */
	nh = nfq_nfnlh(h);
	fd = nfnl_fd(nh);

	logmsg(LOG_NOTICE, 1, "---- Trapping attacks via NFQ. ----\n");

	/* receive packets */
	while ((rv = recv(fd, buf, sizeof(buf), 0)) && rv >= 0) nfq_handle_packet(h, buf, rv);

	/* never reached */
	nfq_destroy_queue(qh);
	nfq_close(h);
	return(1);
}

#endif