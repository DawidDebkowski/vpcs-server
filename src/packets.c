/*
 * Copyright (c) 2007-2015, Paul Meng (mirnshi@gmail.com)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met:
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE 
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF 
 * THE POSSIBILITY OF SUCH DAMAGE.
**/

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h> /* isprint */
#include <unistd.h> /* usleep */
#include <string.h>
#include <sys/time.h>

#include "packets.h"
#include "vpcs.h"
#include "utils.h"

#define IPFRG_MAXHASH  (1 << 10)
#define IPFRG_HASHMASK (IPFRG_MAXHASH - 1)
#define IPFRG_HASH(x,y) \
        (((((x) & 0xF) | ((((x) >> 8) & 0xF) << 4)) ^ (y)) & IPFRG_HASHMASK)

u_char broadcast[ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        
static struct packet *arp(pcs *pc, u_int dip);
static struct packet *udpReply(struct packet *m0);
static struct packet *icmpReply(struct packet *m0, char icmptype, char icmpcode);
static void save_eaddr(pcs *pc, u_int addr, u_char *mac);
extern int upv6(pcs *pc, struct packet **m);
extern void send6(pcs *pc, struct packet *m);
extern int tcp(pcs *pc, struct packet *m);

/*
static void free_ipfrag(struct ipfrag_head *head, struct ipfrag *fp);
static void free_packet(struct packet *m);
static struct ipfrag *new_ipfrag(struct packet *m, iphdr *ip);
static struct packet *defrag_pkt(struct packet **);
*/

// 1 : ok
// 0 : error
static int fix_dmac(pcs *pc, struct packet *m);

extern u_int time_tick;

/*
 * ipv4 stack
 *
 * code: PKT_UP, send to up layer
 *       PKT_ENQ, in out queue
 *       PKT_DROP, drop the packet
 */
int upv4(pcs *pc, struct packet **m0)
{
	struct packet *m = *m0;
	ethdr *eh = (ethdr *)(m->data);
	u_int *si, *di;
	
	if (eh->type == htons(ETHERTYPE_IPV6))
		return upv6(pc, m0);
		
	/* not ipv4 or arp */
	if ((eh->type != htons(ETHERTYPE_IP)) && 
	    (eh->type != htons(ETHERTYPE_ARP))) {
		// printf("DEBUG: upv4 - dropping packet, unsupported type: 0x%04x\n", ntohs(eh->type));
		return PKT_DROP;
	}
		
	if (etherIsMulticast(eh->src)) {
		// printf("DEBUG: upv4 - dropping multicast packet\n");
		return PKT_DROP;
	}

	if ( (memcmp(eh->dst, pc->ip4.mac, ETH_ALEN) == 0 ||
		memcmp(eh->dst, broadcast, ETH_ALEN) == 0)
		&& ((u_short*)m->data)[6] == htons(ETHERTYPE_IP)) {
			struct packet *p;
			iphdr *ip = (iphdr *)(eh + 1);
			
			// printf("DEBUG: upv4 PC - received IP packet from %s to %s, proto: %d\n", 
			    //    inet_ntoa(*(struct in_addr*)&ip->sip), 
			    //    inet_ntoa(*(struct in_addr*)&ip->dip), 
			    //    ip->proto);

			if (ntohs(ip->len) > pc->mtu) {
				p = icmpReply(m, ICMP_UNREACH, ICMP_UNREACH_NEEDFRAG);
				if (p) {
					fix_dmac(pc, p);
					if (pc->ip4.flags & IPF_FRAG) {
						p = ipfrag(p, pc->mtu);
					}
					enq(&pc->oq, p);
				}
				return PKT_ENQ;
			}

			if (ntohs(ip->frag) & (IP_MF | IP_OFFMASK)) {
				m = ipreass(m);
				if (m == NULL)
					return PKT_ENQ;
				else
					*m0 = m;
				ip = (iphdr *)(m->data + sizeof(ethdr));
			}

			/* ping me, reply */
			if (ip->proto == IPPROTO_ICMP) {
				icmphdr *icmp = (icmphdr *)(ip + 1);

				if (ip->dip != pc->ip4.ip)
					return PKT_DROP;

				/* other type will be sent to application */
				if (icmp->type != ICMP_ECHO)
					return PKT_UP;

				p = icmpReply(m, ICMP_ECHOREPLY, 0);
				if (p != NULL) {
					enq(&pc->bgoq, p);
				}
				return PKT_ENQ;
			} else if (ip->proto == IPPROTO_UDP) {
				udpiphdr *ui;
				char *data = NULL;
				ui = (udpiphdr *)ip;

				if (IN_MULTICAST(ntohl(ip->dip)))
					return PKT_DROP;

				/* dhcp packet */
				if (ui->ui_sport == htons(67) && ui->ui_dport == htons(68))
					return PKT_UP;

				if (ip->dip != pc->ip4.ip)
					return PKT_DROP;

				/* dns response */
				if (ui->ui_sport == htons(53))
					return PKT_UP;

				data = ((char*)(ui + 1));

				/* udp echo reply */
				if (memcmp(data, eh->dst, ETH_ALEN) == 0)
					return PKT_UP;
				else {
					if (ip->ttl == 1)
						p = icmpReply(m, ICMP_UNREACH, ICMP_UNREACH_PORT);
					else
						p = udpReply(m);

					if (p != NULL) {
						enq(&pc->bgoq, p);
					}
				}
				/* anyway tell caller to drop this packet */
				return PKT_DROP;
			} else if (ip->proto == IPPROTO_TCP) {
				if (ip->dip != pc->ip4.ip)
					return PKT_DROP;
				return tcp(pc, m);
			}

	} else if (eh->type == htons(ETHERTYPE_ARP)) {
		vpcs_arphdr *ah = (vpcs_arphdr *)(eh + 1);
		si = (u_int *)ah->sip;
		di = (u_int *)ah->dip;
			
		/* arp reply */
		if (ah->op == htons(ARPOP_REQUEST) && 
		    di[0] == pc->ip4.ip) {
			save_eaddr(pc, si[0], ah->sea);
				
			ah->op = htons(ARPOP_REPLY);
			memcpy(ah->dea, ah->sea, ETH_ALEN);
			memcpy(ah->sea, pc->ip4.mac, ETH_ALEN);
					
			di[0] = si[0];
			si[0] = pc->ip4.ip;
					
			encap_ehead(m->data, pc->ip4.mac, eh->src, 
			    ETHERTYPE_ARP);
	
			enq(&pc->oq, m);
				
			return PKT_ENQ;
		} else if (ah->op == htons(ARPOP_REPLY) && 	
		    sameNet(di[0], pc->ip4.ip, pc->ip4.cidr)) {
		    	save_eaddr(pc, si[0], ah->sea);
		}
		
		return PKT_DROP;
	} else if (strncmp((const char *)eh->dst, (const char *)pc->ip4.mac, 
	    ETH_ALEN) != 0)
		return PKT_DROP;
	
	return PKT_UP;
}

void send4(pcs *pc, struct packet *m)
{
	ethdr *eh = (ethdr *)(m->data);

	if (eh->type == htons(ETHERTYPE_IPV6)) {
		send6(pc, m);
		return;
	}
	
	if (eh->type != htons(ETHERTYPE_IP)) {
		// printf("DEBUG: send4 - dropping non-IP packet, type: 0x%04x\n", ntohs(eh->type));
		del_pkt(m);
		return;
	}
	
	// iphdr *ip = (iphdr *)(eh + 1);
	// printf("DEBUG: send4 PC - sending packet from %s to %s, proto: %d\n", 
	    //    inet_ntoa(*(struct in_addr*)&ip->sip), 
	    //    inet_ntoa(*(struct in_addr*)&ip->dip), 
	    //    ip->proto);
	
	if( ! fix_dmac(pc, m) ){
		// printf("DEBUG: send4 - fix_dmac failed\n");
		del_pkt(m);
		return;
	}
	
	if (pc->ip4.flags & IPF_FRAG) {
		m = ipfrag(m, pc->mtu);
	}

	// printf("DEBUG: send4 - queuing packet for transmission\n");
	enq(&pc->oq, m);
}

int response(struct packet *m, sesscb *sesscb)
{
	ethdr *eh;
	iphdr *ip;
	int n;

	eh = (ethdr *)(m->data);
	ip = (iphdr *)(eh + 1);
	
	/* tracerouter response */
	if (ip->proto == IPPROTO_ICMP) {
		icmphdr *icmp = (icmphdr *)(ip + 1);
		/* redirect for network */
		if (icmp->type == ICMP_REDIRECT) {
			if (icmp->code == ICMP_REDIRECT_NET) {
				icmprdr *rdr = (icmprdr *)icmp;
				
				sesscb->icmptype = icmp->type;
				sesscb->icmpcode = icmp->code;
				/* should check sum */
				sesscb->rdip = rdr->ip;	
				/* should check data */
				return IPPROTO_ICMP;
			}
		}
		if (icmp->type == ICMP_UNREACH || icmp->type == ICMP_TIMXCEED) {
			sesscb->icmptype = icmp->type;
			sesscb->icmpcode = icmp->code;
			sesscb->rttl = ip->ttl;
			sesscb->rdip = ip->sip;
			
			return IPPROTO_ICMP;
		}
	}
	
	if (ip->sip != sesscb->dip)
		return 0;
	
	sesscb->rdsize = ntohs(ip->len);
	
	if (ip->proto == IPPROTO_ICMP && sesscb->proto == IPPROTO_ICMP) {
		icmphdr *icmp = (icmphdr *)(ip + 1);
		sesscb->icmptype = icmp->type;
		sesscb->icmpcode = icmp->code;
		sesscb->rttl = ip->ttl;
		sesscb->rdip = ip->sip;
		
		if (ntohs(icmp->seq) == sesscb->sn)
			return IPPROTO_ICMP;

		return 0;
	}
			
	if (ip->proto == IPPROTO_UDP && sesscb->proto == IPPROTO_UDP) {
		udpiphdr *ui = (udpiphdr *)ip;	
		char *data = ((char*)(ui + 1));
		if (memcmp(data, eh->dst, 6) == 0) {
			sesscb->rttl = ip->ttl;
			return IPPROTO_UDP;
		}
		
		return 0;
	}
	
	if (ip->proto == IPPROTO_TCP && sesscb->proto == IPPROTO_TCP) {
		tcpiphdr *ti = (tcpiphdr *)ip;		
		char *data = ((char*)(ti + 1));
		
		sesscb->rseq = ntohl(ti->ti_seq);
		sesscb->rack = ntohl(ti->ti_ack);
		sesscb->rflags = ti->ti_flags;
		sesscb->rttl = ip->ttl;
		sesscb->data = NULL;

		n = sesscb->rdsize - (ip->ihl << 2) - (ti->ti_off << 2);
		/* try to get MSS from options */
		if (sesscb->flags == TH_SYN && sesscb->rflags == (TH_SYN | TH_ACK) && 
			n > 0) {
			int i = 0;

			while ( i < sesscb->rdsize && data[i] == 0x1 ) i++;
			
			for (;i < n;) {
				if (data[i] == TCPOPT_MAXSEG && 
				    data[i + 1] == TCPOLEN_MAXSEG) {
					sesscb->rmss = (data[i + 2] << 8) + data[i + 3];
					break;
				}
				i += data[i + 1];
			}
		} else {
			sesscb->rdsize = n;  /* Set received data size */
			sesscb->data = ((char*)(ip + 1)) + (ti->ti_off << 2);
		}
		return IPPROTO_TCP;
	}
	return 0;
}

int arpResolve(pcs *pc, u_int ip, u_char *dmac)
{
	int i, c;
	struct packet *m;
	int waittime = 1000;
	struct timeval tv;
		
	c = 0;

	for (i = 0; i < POOL_SIZE; i++) {
		if (pc->ipmac4[i].ip == ip && 
		    (time_tick - pc->ipmac4[i].timeout) <= 120 &&
		    !etherIsZero(pc->ipmac4[i].mac)) {
			memcpy(dmac, pc->ipmac4[i].mac, ETH_ALEN);
			return 1;
		}
	}

	while (c++ < 3){
		m = arp(pc, ip);
		if (m == NULL) {
			printf("out of memory\n");
			return 0;
		}
		enq(&pc->oq, m);
		gettimeofday(&(tv), (void*)0);
		while (!timeout(tv, waittime)) {
			delay_ms(1);
			for (i = 0; i < POOL_SIZE; i++) {
				if (pc->ipmac4[i].ip == ip && 
				    (time_tick - pc->ipmac4[i].timeout) <= 120 &&
				    !etherIsZero(pc->ipmac4[i].mac)) {
					memcpy(dmac, pc->ipmac4[i].mac, ETH_ALEN);
					return 1;
				}
			}	
		}
	}
	return 0;
}

struct packet *packet(pcs *pc)
{
	sesscb *sesscb = &pc->mscb;
	ethdr *eh;
	iphdr *ip;
	int i;
	struct packet *m;
	int dlen = 0; /* the size of payload */
	int hdr_len = 0;
	char b[9];
	
	dlen = sesscb->dsize;

	switch (sesscb->proto) {
		case IPPROTO_ICMP:
			hdr_len = sizeof(iphdr) + sizeof(icmphdr);
			break;
		case IPPROTO_UDP:
			hdr_len = sizeof(iphdr) + sizeof(udphdr);
			break;
		case IPPROTO_TCP:
			if (sesscb->flags != (TH_ACK | TH_PUSH))
				dlen = 0;
			if (sesscb->flags == TH_SYN) {
				/* mss(2 + 2), nop(1 + 1), timestamp ( 2 + 8), 
				 * nop(1), winscale (2 + 1)*/
				dlen = 4 + 2 + 2 + 8 + 1 + 3;	
			} else {
				dlen = dlen + 2 + 2 + 8;
			}
			if (sesscb->rmss != 0 && dlen > sesscb->rmss)
				dlen = sesscb->rmss - sizeof(ethdr) - 
				    sizeof(iphdr) - sizeof(tcphdr);
			
			hdr_len = sizeof(iphdr) + sizeof(tcphdr);
			break;
	}
		
	m = new_pkt(sizeof(ethdr) + hdr_len + dlen);
	
	if (m == NULL)
		return NULL;

	eh = (ethdr *)(m->data);
	ip = (iphdr *)(eh + 1);
	
	ip->ver = 4;
	ip->ihl = sizeof *ip >> 2;
	ip->len = htons(hdr_len + dlen);
	ip->id = htons(sesscb->ipid++);
	if (!sesscb->frag)
		ip->frag = htons(IP_DF);
	ip->ttl = sesscb->ttl;
	ip->proto = sesscb->proto;
	ip->sip = sesscb->sip;
	ip->dip = sesscb->dip;
	
	if (sesscb->proto == IPPROTO_ICMP) {
		icmphdr *icmp = (icmphdr *)(ip + 1);
		char *data = ((char*)(icmp + 1));
		
		icmp->seq = htons(sesscb->sn);
		icmp->cksum = 0;
		icmp->type = ICMP_ECHO;
		icmp->code = 0;
		icmp->id = time(0) & 0xffff;
		
		for (i = 0; i < dlen; i++)
			data[i] = (i + sizeof(icmphdr)) & 0xff;
		
		icmp->cksum = cksum((unsigned short *) (icmp), 
		    hdr_len + dlen - sizeof(iphdr));
	} else if (sesscb->proto == IPPROTO_UDP) {
		udpiphdr *ui = (udpiphdr *)ip;
		char *data = ((char*)(ui + 1));
		
		ui->ui_sport = htons(sesscb->sport);
		ui->ui_dport = htons(sesscb->dport);
		ui->ui_ulen = htons(hdr_len + dlen - sizeof(iphdr));
		ui->ui_sum = 0;
		
		/* this's my footprint */
		if (sesscb->data != NULL) {
			memcpy(data, sesscb->data, dlen);
		} else {
			memcpy(data, sesscb->smac, 6);	
			for (i = 6; i < dlen; i++)
				data[i] = (i + sizeof(udphdr)) & 0xff;
		}
				
		bcopy(((struct ipovly *)ip)->ih_x1, b, 9);
		bzero(((struct ipovly *)ip)->ih_x1, 9);
		
		ui->ui_len = ui->ui_ulen;
		ui->ui_sum = cksum((u_short*)ui, hdr_len + dlen);
		
		bcopy(b, ((struct ipovly *)ip)->ih_x1, 9);
		
	} else if (sesscb->proto == IPPROTO_TCP) {
		tcpiphdr *ti = (tcpiphdr *)ip;
		char *data = ((char*)(ti + 1));
		u_int t = htonl(time(0));
		int optlen = 0;
		
		ti->ti_sport = htons(sesscb->sport);
		ti->ti_dport = htons(sesscb->dport);
		ti->ti_len = htons(hdr_len + dlen - sizeof(iphdr));
		ti->ti_ack = htonl(sesscb->ack);
		ti->ti_seq = htonl(sesscb->seq);
		ti->ti_win = htons(sesscb->winsize);
		ti->ti_sum = 0;
		ti->ti_flags = sesscb->flags;
		
		if (sesscb->flags == TH_SYN) {
			/* mss 1460 */
			*data++ = TCPOPT_MAXSEG;
			*data++ = TCPOLEN_MAXSEG;
			*data++ = 0x5;
			*data++ = 0xb4;
			/* align */
			*data++ = 0x1;
			*data++ = 0x1;
			/* timestamp */
			*data++ = TCPOPT_TIMESTAMP;
			*data++ = TCPOLEN_TIMESTAMP;
			memcpy(data, (char *)&t, 4);
			data += 8;
			/* align */
			*data++ = 0x1;
			*data++ = TCPOPT_WINDOW;
			*data++ = TCPOLEN_WINDOW;
			*data++ = 1;
		} else {
			/* align */
			*data++ = 0x1;
			*data++ = 0x1;
			/* timestamp */
			*data++ = TCPOPT_TIMESTAMP;
			*data++ = TCPOLEN_TIMESTAMP;
			memcpy(data, (char *)&t, 4);
			data += 8;
		}
		
		optlen = data - (char*)(ti + 1);
		ti->ti_off = (sizeof(tcphdr) + optlen) >> 2;
		
		/*  
	 	 * TELNET protcol 
	 	 *               IAC: 0xff
	 	 *      option code
	 	 *              WILL: 0xfb
	 	 *             WON't: 0xfc
	 	 *                DO: 0xfd
	 	 *             DON'T: 0xfe
	 	 * command
	 	 *              ECHO: 0x01
	 	 * SUPPRESS-GO-AHEAD: 0x03
	 	 *     TERMINAL-TYPE: 0x18
	 	 *              NAWS: 0x1f
	 	 */

		/* fill the data */
		if (sesscb->data != NULL && (dlen - optlen) > 0) {
			/* Use actual data if available (for HTTP requests, etc.) */
			int data_len = dlen - optlen;
			memcpy(data, sesscb->data, data_len);
		} else {
			/* Default pattern for testing */
			for (i = optlen; i < dlen; i++) {
				if ((i % 2) == 0)
					*data++ = 0xd;
				else
					*data++ = 0xa;
			}
		}
		
		bcopy(((struct ipovly *)ip)->ih_x1, b, 9);
		bzero(((struct ipovly *)ip)->ih_x1, 9);
		
		ti->ti_sum = cksum((u_short*)ti, hdr_len + dlen);
		bcopy(b, ((struct ipovly *)ip)->ih_x1, 9);
	}
	
	encap_ehead(m->data, sesscb->smac, sesscb->dmac, ETHERTYPE_IP);
	
	/* maybe fragmentation failed, let's do cksum first */
	ip->cksum = 0;
	ip->cksum = cksum((u_short *)ip, sizeof(iphdr));
		
    /* Even when DF bit is set we'll fragment according to our MTU size */
	m = ipfrag(m, sesscb->mtu);
	
	return m;
}

struct packet *arp(pcs *pc, u_int dip)
{
	ethdr *eh;
	vpcs_arphdr *ah;
	struct packet *m;
	u_int *si, *di;
	
	m = new_pkt(ARP_PSIZE);
	if (m == NULL)
		return NULL;

	eh = (ethdr *)(m->data);
	ah = (vpcs_arphdr *)(eh + 1);

	ah->hrd = htons(ARPHRD_ETHER);
	ah->pro = htons(ETHERTYPE_IP);
	ah->hln = 6;
	ah->pln = 4;
	ah->op = htons(ARPOP_REQUEST);
	
	si = (u_int *)ah->sip;
	di = (u_int *)ah->dip;
	si[0] = pc->ip4.ip;
	di[0] = dip;

	memcpy(ah->dea, broadcast, ETH_ALEN);
	memcpy(ah->sea, pc->ip4.mac, ETH_ALEN);
	
	encap_ehead(m->data, pc->ip4.mac, broadcast, ETHERTYPE_ARP);
	
	return m;
}

struct packet *udpReply(struct packet *m0)
{
	ethdr *eh;
	iphdr *ip;
	udpiphdr *ui;
	struct packet *m;
	
	m = new_pkt(m0->len);
	if (m == NULL)
		return NULL;
	
	copy_pkt(m, m0);
	
	eh = (ethdr *)(m->data);
	ip = (iphdr *)(eh + 1);
	ui = (udpiphdr *)ip;
		
	ip->dip ^= ip->sip;
	ip->sip ^= ip->dip;
	ip->dip ^= ip->sip;
	
	ui->ui_sport ^= ui->ui_dport;
	ui->ui_dport ^= ui->ui_sport;
	ui->ui_sport ^= ui->ui_dport;
	
	ip->cksum = cksum_fixup(ip->cksum, ip->ttl, TTL, 0);
	ip->ttl = TTL;
	
	swap_ehead(m->data);
	return m;	
}

struct packet *icmpReply(struct packet *m0, char icmptype, char icmpcode)
{
	struct packet *m = NULL;
	ethdr *eh = NULL;
	iphdr *ip = NULL;
	icmphdr *icmp = NULL;
	
	if (icmptype == ICMP_ECHOREPLY) {
		m = m0;

		eh = (ethdr *)(m->data);
		ip = (iphdr *)(eh + 1);
		icmp = (icmphdr *)(ip + 1);
			
		u_short old_sum;
		u_char old_ttl;
		
		old_ttl = ip->ttl;
		old_sum = icmp->cksum;
		
		icmp->type = ICMP_ECHOREPLY;
		icmp->cksum = cksum_fixup(icmp->cksum, ICMP_ECHO, ICMP_ECHOREPLY, 0);

		ip->dip ^= ip->sip;
		ip->sip ^= ip->dip;
		ip->dip ^= ip->sip;

		ip->ttl = TTL;
		
		ip->cksum = cksum_fixup(cksum_fixup(cksum_fixup(ip->cksum, 
		    old_ttl, ip->ttl, 0), ICMP_ECHO, ICMP_ECHOREPLY, 0),
		    old_sum, icmp->cksum, 0);
	
		swap_ehead(m->data);
		
		return m;	
	} 
	
	if (icmptype == ICMP_UNREACH) {
		int len, len0;
    	
		eh = (ethdr *)(m0->data);
		ip = (iphdr *)(eh + 1);
		
		len0 = ntohs(ip->len);
		if (len0 > 44)
			len0 = 44;

		len = sizeof(ethdr) + sizeof(iphdr) + sizeof(icmphdr) + len0;

		m = new_pkt(len);
		if (m == NULL)
			return NULL;
		
		/* get original etherhdr and iphdr */
		memcpy(m->data, m0->data, sizeof(ethdr) + sizeof(iphdr));
    	
		eh = (ethdr *)(m->data);
		ip = (iphdr *)(eh + 1);
		icmp = (icmphdr *)(ip + 1);
    	
	    	/* copy the origial part */
		memcpy((char*)(icmp + 1), (char *)(m0->data + sizeof(ethdr)), len0);

		ip->len = htons(len - sizeof(ethdr));
		ip->id = time(0) & 0xffff;
		ip->frag = htons(0x4000);
		ip->ttl = TTL;
		ip->proto = IPPROTO_ICMP;
		ip->cksum = 0;
		ip->dip ^= ip->sip;
		ip->sip ^= ip->dip;
		ip->dip ^= ip->sip;
		
		icmp->seq = htons(1);
		icmp->cksum = 0;
		icmp->type = icmptype;
		icmp->code = icmpcode;
		icmp->id = time(0) & 0xffff;
			
		icmp->cksum = cksum((unsigned short *) (icmp), sizeof(icmphdr) + len0);
		
		ip->cksum = cksum((u_short *)ip, sizeof(iphdr));
		
		swap_ehead(m->data);
    	
		return m;
	}
	
	return NULL;
}

static void 
save_eaddr(pcs *pc, u_int addr, u_char *mac)
{
	int i;
	
	if (!sameNet(addr, pc->ip4.ip, pc->ip4.cidr))
		return;
	
	i = 0;
	while (i < POOL_SIZE) {
		if (time_tick - pc->ipmac4[i].timeout <= 120 &&
			pc->ipmac4[i].ip == addr) {
			pc->ipmac4[i].timeout = time_tick;
			break;
		}
		if (pc->ipmac4[i].timeout == 0 || 
		    time_tick - pc->ipmac4[i].timeout > 120) {
			pc->ipmac4[i].ip = addr;
			memcpy(pc->ipmac4[i].mac, mac, ETH_ALEN);
			pc->ipmac4[i].timeout = time_tick;
			break;
		}
		i++;
	}
}

// return 1 : ok
// return 0 : error
int fix_dmac(pcs *pc, struct packet *m)
{
	ethdr *eh = NULL;
	iphdr *ip = NULL;
	u_char mac[6];
	
	eh = (ethdr *)(m->data);
	ip = (iphdr *)(eh + 1);
	
	if (sameNet(ip->dip, pc->ip4.ip, pc->ip4.cidr))
		return 1;

	if( pc->ip4.gw == 0 ) // gw == 0.0.0.0
		return 0;

	if (arpResolve(pc, pc->ip4.gw, mac))
	{
		memcpy(eh->dst, mac, sizeof(mac));
		return 1;
	}

	return 0;
}


#if 0
static void xxpreh(char *e, int c)
{
	int i;
	
	for (i = 0; i < c; i++) {
		printf("%2.2x ", *(e + i));
		if (i % 16 == 0) printf("\n");
	}
	return;
}
#endif

/* end of file */
