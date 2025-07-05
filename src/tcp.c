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

#include <stdio.h>
#include <unistd.h> /* usleep */
#include <stdlib.h> /* random */
#include <string.h> /* string op */

#include "vpcs.h"
#include "tcp.h"
#include "packets.h"
#include "packets6.h"
#include "utils.h"
#include "httpd.h"

extern int pcid;
extern int ctrl_c;
extern u_int time_tick;
extern int dmpflag;

/*******************************************************
 *      client                  server
 *                 SYN  ->
 *                      <- SYN + ACK
 *                 ACk  ->
 *             (sseq+1)  
 *    ACK + PUSH + data ->
 *             sseq + 1
 *                      <- ACK 
 *                         cseq + sizeof(data)
 *                  FIN ->
 *                sseq+1   
 *         wait1
 *                      <- ACK
 *         wait2           sseq+1
 *                      <- FIN
 *                            close wait
 *                  ACk ->
 *******************************************************/
int tcp_ack(pcs *pc, int ipv)
{
	struct packet *m = NULL;
	
	struct packet * (*fpacket)(pcs *pc);
	
	if (ipv == IPV6_VERSION)
		fpacket = packet6;
	else
		fpacket = packet;
	
	pc->mscb.flags = TH_ACK;

	m = fpacket(pc);
	
	if (m == NULL) {
		printf("out of memory\n");
		return 0;
	}
	
	/* push m into the background output queue 
	   which is watched by pth_output */
	enq(&pc->bgoq, m);
	
	return 1;
}

int tcp_open(pcs *pc, int ipv)
{
	struct packet *m, *p;
	int i = 0, ok;
	int state = 0;
	struct packet * (*fpacket)(pcs *pc);
	int (*fresponse)(struct packet *pkt, sesscb *sesscb);
	
	if (ipv == IPV6_VERSION) {
		fpacket = packet6;
		fresponse = response6;
	} else {
		fpacket = packet;
		fresponse = response;
	}
	
	/* try to connect */
	//printf("DEBUG: tcp_open - attempting to connect, ipv=%d\n", ipv);
	while (i++ < 3 && ctrl_c == 0) {
		struct timeval tv;
		
		pc->mscb.flags = TH_SYN;
		pc->mscb.timeout = time_tick;
		pc->mscb.seq = rand();
		pc->mscb.ack = 0;

		// printf("DEBUG: tcp_open - sending SYN attempt %d, seq=%u\n", i, pc->mscb.seq);
		
		m = fpacket(pc);
	
		if (m == NULL) {
			printf("out of memory\n");
			return 0;
		}
		/* push m into the background output queue 
		   which is watched by pth_output */
		enq(&pc->bgoq, m);
		
		//k = 0;
		ok = 0;
		gettimeofday(&(tv), (void*)0);
		while (!timeout(tv, pc->mscb.waittime) && !ctrl_c) {
			delay_ms(1);

			while ((p = deq(&pc->iq)) != NULL && 
			    !timeout(tv, pc->mscb.waittime) && !ctrl_c) {	
				
				// printf("DEBUG: tcp_open - received response packet\n");
				ok = fresponse(p, &pc->mscb);
				del_pkt(p);

				if (!ok) {
					// printf("DEBUG: tcp_open - fresponse failed\n");
					continue;
				}

				if (ok == IPPROTO_ICMP) {
					// printf("DEBUG: tcp_open - received ICMP response\n");
					return 2;
				}
				
				if (pc->mscb.rack == (pc->mscb.seq + 1) && 
					pc->mscb.rflags == (TH_SYN | TH_ACK)) {
					// printf("DEBUG: tcp_open - received SYN+ACK, connection established\n");
					state = 1;
					tv.tv_sec = 0;
					break;
				} 
				
				if ((pc->mscb.rflags & TH_RST) != TH_RST) {
					pc->mscb.flags = TH_RST | TH_ACK;
					pc->mscb.seq = pc->mscb.rack;
					pc->mscb.ack = pc->mscb.rseq;
					
					m = fpacket(pc);
					if (m == NULL) {
						printf("out of memory\n");
						return 0;
					}
					/* push m into the background output 
					   queue which is watched 
					   by pth_output */
					enq(&pc->bgoq, m);
					
					delay_ms(1);
					tv.tv_sec = 0;
					break;
				}
				if ((pc->mscb.rflags & TH_RST) == TH_RST)
					return 3;	
				
				tv.tv_sec = 0;
				break;
			}
		}
		
		if (state != 1) {
			// printf("DEBUG: tcp_open - attempt %d failed, state=%d\n", i, state);
			continue;
		}
		
		/* reply ACK , ack+1 */
		pc->mscb.seq = pc->mscb.rack;
		pc->mscb.ack = pc->mscb.rseq + 1;
		// printf("DEBUG: tcp_open - sending final ACK to complete handshake\n");
		tcp_ack(pc, ipv);

		// printf("DEBUG: tcp_open - connection established successfully\n");
		return 1;
	}
	// printf("DEBUG: tcp_open - all connection attempts failed\n");
	return 0;
}
/*
 * return 1 if ACK, 2 if FIN|PUSH
 */
int tcp_send(pcs *pc, int ipv)
{
	struct packet *m, *p;
	int i = 0, ok;
	int state = 0;
	
	struct packet * (*fpacket)(pcs *pc);
	int (*fresponse)(struct packet *pkt, sesscb *sesscb);
	
	if (ipv == IPV6_VERSION) {
		fpacket = packet6;
		fresponse = response6;
	} else {
		fpacket = packet;
		fresponse = response;
	}
	
	/* drop the response if any, but update the ack */
	while ((p = deq(&pc->iq)) != NULL) {	
		ok = fresponse(p, &pc->mscb);
		del_pkt(p);
		
		if (!ok)
			continue;
			
		if (pc->mscb.rflags == (TH_ACK | TH_PUSH) &&
			pc->mscb.seq == pc->mscb.rack) {
			pc->mscb.ack = pc->mscb.rseq + pc->mscb.rdsize;
			tcp_ack(pc, ipv);
			delay_ms(1);
		}
	}	
	 
	/* try to send */ 
	while (i++ < 3 && ctrl_c == 0) {
		struct timeval tv;
		
		pc->mscb.flags = TH_ACK | TH_PUSH;
		m = fpacket(pc);
	
		if (m == NULL) {
			printf("out of memory\n");
			return 0;
		}
		/* push m into the background output queue 
		   which is watched by pth_output */
		enq(&pc->bgoq, m);
		
		//k = 0;
		ok = 0;
		gettimeofday(&(tv), (void*)0);
		while (!timeout(tv, pc->mscb.waittime) && !ctrl_c) {
			delay_ms(1);
			while ((p = deq(&pc->iq)) != NULL) {	
				ok = fresponse(p, &pc->mscb);
				del_pkt(p);

				if (!ok)
					continue;

				if (pc->mscb.rflags == (TH_ACK) && 
				    pc->mscb.rack == pc->mscb.seq + pc->mscb.dsize) {
					pc->mscb.seq = pc->mscb.rack;
					pc->mscb.ack = pc->mscb.rseq;		
					state = 1;
					
					return 1;
				}
				if (pc->mscb.rflags == (TH_ACK | TH_PUSH)) {
					/* Server sent ACK+PUSH+DATA (HTTP response) */
					pc->mscb.seq = pc->mscb.rack;
					pc->mscb.ack = pc->mscb.rseq + pc->mscb.rdsize;
					
					/* Send ACK to acknowledge the response data */
					tcp_ack(pc, ipv);
					
					/* HTTP transaction complete */
					return 1;
				}
					
				/* the remote does not like me, closing the connection */	
				if (pc->mscb.rflags == (TH_ACK | TH_PUSH | TH_FIN)) {
					pc->mscb.seq = pc->mscb.rack;
					pc->mscb.ack = pc->mscb.rseq + pc->mscb.rdsize;

					state = 1;
				
					return 2;
				}
				tv.tv_sec = 0;	
				break;
			}
		}
		
		if (state != 1)
			continue;
					
		return 1;
	}
	return 0;
}

int tcp_close(pcs *pc, int ipv)
{
	struct packet *m, *p;
	int i = 0, ok;
	int state = 0;
	int rfin = 0;
	
	struct packet * (*fpacket)(pcs *pc);
	int (*fresponse)(struct packet *pkt, sesscb *sesscb);
	
	if (ipv == IPV6_VERSION) {
		fpacket = packet6;
		fresponse = response6;
	} else {
		fpacket = packet;
		fresponse = response;
	}
	
	/* drop the response if any, but update the ack */
	while ((p = deq(&pc->iq)) != NULL) {	
		ok = fresponse(p, &pc->mscb);
		del_pkt(p);
		
		if (!ok)
			continue;
		
		if (pc->mscb.rflags == (TH_ACK | TH_PUSH) &&
			pc->mscb.seq == pc->mscb.rack) {
			pc->mscb.ack = pc->mscb.rseq + pc->mscb.rdsize;
			tcp_ack(pc, ipv);
			delay_ms(1);
			continue;
		}
		if (pc->mscb.rflags == TH_ACK) {
			pc->mscb.seq = pc->mscb.rack;
			pc->mscb.ack = pc->mscb.rseq;
			break;
		}
		if ((pc->mscb.rflags & (TH_ACK | TH_FIN)) == (TH_ACK | TH_FIN)) {
			pc->mscb.seq = pc->mscb.rack;
			pc->mscb.ack = pc->mscb.rseq;
			pc->mscb.ack++;
			
			tcp_ack(pc, ipv);
			
			delay_ms(1);
			rfin = 1;
			
			continue;
		}
	}
		
	/* try to close */
	while (i++ < 3 && ctrl_c == 0) {
		struct timeval tv;
		
		state = 0;
		
		pc->mscb.flags = TH_FIN | TH_ACK | TH_PUSH;
		m = fpacket(pc);
	
		if (m == NULL) {
			printf("out of memory\n");
			return 0;
		}
		
		/* push m into the background output queue which is watched by pth_output */
		enq(&pc->bgoq, m);   
		
		/* expect ACK */
		gettimeofday(&(tv), (void*)0);
		while (!timeout(tv, pc->mscb.waittime) && !ctrl_c) {
			delay_ms(1);
			while ((p = deq(&pc->iq)) != NULL) {
				ok = fresponse(p, &pc->mscb);
				del_pkt(p);
				
				if (!ok)
					continue;
					
				if ((pc->mscb.rflags & (TH_ACK | TH_FIN) ) == (TH_ACK | TH_FIN)) 
					state = 1;
				else if (pc->mscb.rflags == TH_ACK) 
					state = 2;

				tv.tv_sec = 0;
				break;
			}
		}
		
		if (state == 0)
			continue;
		
		/* both side sent and received FIN/ACK, closed! */
		if (rfin == 1 && state == 2)
			return 1;
		
		/* local send FIN/ACK first */
		if (state == 2) {
			/* expect FIN */
			state = 0;
			//k = 0;
			gettimeofday(&(tv), (void*)0);
			while (!timeout(tv, pc->mscb.waittime) && !ctrl_c) {
				delay_ms(1);
				while ((p = deq(&pc->iq)) != NULL) {	
					ok = fresponse(p, &pc->mscb);
					del_pkt(p);
					if (!ok)
						continue;
						
					if ((pc->mscb.rflags & TH_FIN) == TH_FIN)  {
						/* change my seq, seq += 1 */
						pc->mscb.seq = pc->mscb.rack;
						state = 1;
					}
					tv.tv_sec = 0;
					break;
				}
			}
		}
		
		/* ACK was not received in the time */
		if (state == 0)
			return 0;
		
		/* the remote sent FIN/ACK, response the ACK */
		pc->mscb.ack++;
		tcp_ack(pc, ipv);
		
		return 1;
	}
	return 0;			
}

/* tcp processor
 * return PKT_DROP/PKT_UP
 */
int tcpReplyPacket(tcphdr *th, sesscb *cb, int tcplen)
{
	int clientfinack = 0; /* ack for fin was reply if 1 */
	int dsize = 0;

	th->th_sport ^= th->th_dport;
	th->th_dport ^= th->th_sport;
	th->th_sport ^= th->th_dport;
	
	cb->ack = ntohl(th->th_seq);
	cb->rflags = th->th_flags;
	cb->winsize = ntohs(th->th_win);
	
	// printf("DEBUG: tcpReplyPacket - incoming flags: 0x%02x, seq: %u, ack: %u\n", 
	       // th->th_flags, ntohl(th->th_seq), ntohl(th->th_ack));
	
	if (cb->flags != (TH_RST | TH_FIN)) {		
		switch (th->th_flags) {
			case TH_SYN:
				// printf("DEBUG: Processing SYN - will reply with SYN+ACK\n");
				cb->flags = TH_ACK | TH_SYN;
				cb->ack++;
				break;
			case TH_ACK | TH_PUSH:
				// printf("DEBUG: Processing ACK+PUSH - data size: %d\n", tcplen - (th->th_off << 2));
				/* HTTP-style response: send ACK+PUSH+DATA (combined acknowledgment and response) */
				cb->flags = TH_ACK | TH_PUSH;  /* Send ACK+PUSH+DATA */
				dsize = tcplen - (th->th_off << 2);
				cb->ack += dsize;  /* Acknowledge received data */
				break;
			case TH_ACK | TH_FIN:
				// printf("DEBUG: Processing ACK+FIN\n");
				cb->flags = (TH_ACK | TH_FIN);
				cb->ack++;
				break;
			case TH_ACK | TH_FIN | TH_PUSH:
			case TH_FIN | TH_PUSH:
				// printf("DEBUG: Processing FIN+PUSH\n");
				dsize = tcplen - (th->th_off << 2);

			case TH_FIN:
				// printf("DEBUG: Processing FIN\n");
				if (cb->flags == (TH_ACK | TH_FIN))
					cb->flags = TH_FIN | TH_ACK;
				else
					cb->flags = TH_ACK;
				if (dsize == 0)
					cb->ack++;
				clientfinack = 1;
				break;
			default:
				// printf("DEBUG: Unknown/unsupported flags: 0x%02x\n", th->th_flags);
				return 0;	
		}
	}
	if (th->th_flags != TH_SYN)
		cb->seq = ntohl(th->th_ack);
	th->th_ack = htonl(cb->ack + dsize);
	th->th_seq = htonl(cb->seq);
	th->th_flags = cb->flags;
	
	// printf("DEBUG: tcpReplyPacket - outgoing flags: 0x%02x, seq: %u, ack: %u\n", 
	//        cb->flags, cb->seq, cb->ack + dsize);
	
	/* ignore the tcp options */
	if ((th->th_off << 2) > sizeof(tcphdr))
		th->th_off = sizeof(tcphdr) >> 2;
		
	if (clientfinack == 1)
		return 2;
	
	return 1;
}

int tcp(pcs *pc, struct packet *m)
{
	// printf("TCP\n");
	iphdr *ip = (iphdr *)(m->data + sizeof(ethdr));
	tcpiphdr *ti = (tcpiphdr *)(ip);
	sesscb *cb = NULL;
	struct packet *p = NULL;
	int i;
	
	if (ip->dip != pc->ip4.ip) {
		// printf("DEBUG: Packet not for us - dst: %s, our IP: %s\n", 
		//        inet_ntoa(*(struct in_addr*)&ip->dip), 
		//        inet_ntoa(*(struct in_addr*)&pc->ip4.ip));
		return PKT_DROP;
	}
	
	printf("DEBUG: TCP packet received - src: %s:%d -> dst: %s:%d, flags: 0x%02x\n",
	       inet_ntoa(*(struct in_addr*)&ip->sip), ntohs(ti->ti_sport),
	       inet_ntoa(*(struct in_addr*)&ip->dip), ntohs(ti->ti_dport),
	       ti->ti_flags);

	/* response packet 
	 * 1. socket opened
	 * 2. same port
	 * 3. destination is me
	 * 4. mscb.proto is TCP
	 */
	if (pc->mscb.sock && ntohs(ti->ti_dport) == pc->mscb.sport && 
	    ntohs(ti->ti_sport) == pc->mscb.dport && 
	    ip->sip == pc->mscb.dip && pc->mscb.proto == ip->proto) {
		// printf("DEBUG: This is a response to our outgoing connection\n");
		/* mscb is actived, up to the upper application */
		if (time_tick - pc->mscb.timeout <= TCP_TIMEOUT)
			return PKT_UP;

		// printf("DEBUG: Client connection timed out, sending RST\n");
		/* not mine, reset the request */
		sesscb rcb;
		
		rcb.seq = random();
		rcb.sip = ip->sip;
		rcb.dip = ip->dip;
		rcb.sport = ti->ti_sport;
		rcb.dport = ti->ti_dport;
		rcb.flags = TH_RST | TH_FIN | TH_ACK;
		
		p = tcpReply(m, &rcb);
		
		/* push m into the background output queue which is watched by pth_output */
		if (p != NULL) {
			enq(&pc->bgoq, p);			
		} else
			printf("reply error\n");
		
		/* drop the request packet */
		return PKT_DROP;
	}

	/* request process
	 * find control block 
	 */
	// printf("DEBUG: Looking for existing session or creating new one\n");
	for (i = 0; i < MAX_SESSIONS; i++) {
		if (ti->ti_flags == TH_SYN) {
			if (pc->sesscb[i].timeout == 0 || 
			    time_tick - pc->sesscb[i].timeout > TCP_TIMEOUT ||
			    (ip->sip == pc->sesscb[i].sip && 
			     ip->dip == pc->sesscb[i].dip &&
			     ti->ti_sport == pc->sesscb[i].sport &&
			     ti->ti_dport == pc->sesscb[i].dport)) {
				/* get new scb */
				printf("DEBUG: Found/created session %d for incoming SYN\n", i);
				cb = &pc->sesscb[i];
				cb->timeout = time_tick;
				cb->seq = random();
				cb->sip = ip->sip;
				cb->dip = ip->dip;
				cb->sport = ti->ti_sport;
				cb->dport = ti->ti_dport;
				
				break;
			}
		} else {
			if ((time_tick - 
			    pc->sesscb[i].timeout <= TCP_TIMEOUT) && 
			    ip->sip == pc->sesscb[i].sip && 
			    ip->dip == pc->sesscb[i].dip &&
			    ti->ti_sport == pc->sesscb[i].sport &&
			    ti->ti_dport == pc->sesscb[i].dport) {
				/* get the scb */
				printf("DEBUG: Found existing session %d for packet\n", i);
				cb = &pc->sesscb[i];
				break;
			}	
		}
	}
	
	if (ti->ti_flags == TH_SYN && cb == NULL) {
		/* Check if we have an HTTP server on this port */
		extern vpcs_httpd_server_t vpcs_httpd_servers[];
		int dest_port = ntohs(ti->ti_dport);
		int server_found = 0;
		
		// printf("DEBUG: SYN received on port %d from %s, checking for HTTP server...\n", 
		    //    dest_port, inet_ntoa(*(struct in_addr*)&ip->sip));
		
		for (int j = 0; j < HTTPD_MAX_SERVERS; j++) {
			if (vpcs_httpd_servers[j].enabled && 
			    vpcs_httpd_servers[j].port == dest_port && 
			    vpcs_httpd_servers[j].pc_id == pcid) {
				server_found = 1;
				// printf("DEBUG: Found HTTP server on port %d for PC %d\n", dest_port, pcid);
				break;
			}
		}
		
		if (!server_found) {
			// printf("DEBUG: No HTTP server found on port %d for PC %d\n", dest_port, pcid);
			printf("VPCS %d out of session\n", pc->id);
			return PKT_DROP;
		}
		
		// printf("DEBUG: Allocating session for HTTP server on port %d\n", dest_port);
		
		/* Try to find a free session for the HTTP server */
		for (i = 0; i < MAX_SESSIONS; i++) {
			if (pc->sesscb[i].timeout == 0 || 
			    time_tick - pc->sesscb[i].timeout > TCP_TIMEOUT) {
				cb = &pc->sesscb[i];
				cb->timeout = time_tick;
				cb->seq = random();
				cb->sip = ip->sip;
				cb->dip = ip->dip;
				cb->sport = ti->ti_sport;
				cb->dport = ti->ti_dport;
				break;
			}
		}
		
		if (cb == NULL) {
			// printf("DEBUG: VPCS %d out of session - no free sessions available\n", pc->id);
			return PKT_DROP;
		}
		
		// printf("DEBUG: Session allocated for HTTP server - session %d\n", (int)(cb - pc->sesscb));
	}
	
	if (cb != NULL) {
		if (ti->ti_flags == TH_ACK && cb->flags == TH_FIN) {
			/* clear session */
			// printf("DEBUG: Clearing session - received final ACK\n");
			memset(cb, 0, sizeof(sesscb));
		} else {
			cb->timeout = time_tick;
			// printf("DEBUG: Processing packet with flags 0x%02x, generating reply\n", ti->ti_flags);
			p = tcpReply(m, cb);
			
			/* push m into the background output queue which is watched by pth_output */
			if (p != NULL) {
				// printf("DEBUG: Reply packet queued successfully\n");
				enq(&pc->bgoq, p);
			} else {
				// printf("DEBUG: Failed to create reply packet\n");
			}
			
			/* send FIN after ACK if got FIN */
			if ((cb->rflags & TH_FIN) == TH_FIN && 
			    cb->flags == (TH_ACK | TH_FIN)) {
				// printf("DEBUG: Sending FIN after ACK\n");
				p = tcpReply(m, cb);
				
				/* push m into the background output queue which is watched by pth_output */
				if (p != NULL) {
					enq(&pc->bgoq, p);
				}
			}
		}
	} else {
		// printf("DEBUG: No session control block found for packet\n");
	}

	/* anyway tell caller to drop this packet */
	return PKT_DROP;	
}

struct packet *tcpReply(struct packet *m0, sesscb *cb)
{
	ethdr *eh;
	iphdr *ip;
	tcpiphdr *ti;
	tcphdr *th;
	struct packet *m;
	char b[9];
	int len;

	int tcplen = 0;
	
	/* Check if this is HTTP data (ACK + PUSH) */
	ethdr *orig_eh = (ethdr *)(m0->data);
	iphdr *orig_ip = (iphdr *)(orig_eh + 1);
	tcphdr *orig_th = (tcphdr *)(orig_ip + 1);
	
	int orig_tcplen = ntohs(orig_ip->len) - sizeof(iphdr);
	int orig_dsize = orig_tcplen - (orig_th->th_off << 2);
	
	/* Check if this is HTTP data packet and we have a server on this port */
	if ((orig_th->th_flags & (TH_ACK | TH_PUSH)) == (TH_ACK | TH_PUSH) && orig_dsize > 0) {
		int dest_port = ntohs(orig_th->th_dport);
		char *http_data = (char *)orig_th + (orig_th->th_off << 2);
		
		// printf("DEBUG: Received HTTP data on port %d, size: %d\n", dest_port, orig_dsize);
		
		/* Check if we have HTTP server on this port */
		char response_buffer[HTTPD_MAX_RESPONSE_SIZE];
		int response_len = 0;
		
		vpcs_httpd_handle_request(dest_port, http_data, orig_dsize, response_buffer, &response_len);
		
		if (response_len > 0) {
			// printf("DEBUG: Generated HTTP response, size: %d\n", response_len);
			/* Create HTTP response packet */
			len = sizeof(ethdr) + sizeof(iphdr) + sizeof(tcphdr) + response_len;
			m = new_pkt(len);
			if (m == NULL)
				return NULL;
			
			memcpy(m->data, m0->data, sizeof(ethdr) + sizeof(iphdr) + sizeof(tcphdr));
			
			eh = (ethdr *)(m->data);
			ip = (iphdr *)(eh + 1);
			ti = (tcpiphdr *)ip;
			th = (tcphdr *)(ip + 1);
			
			/* Copy HTTP response data */
			memcpy((char *)th + sizeof(tcphdr), response_buffer, response_len);
			
			tcplen = ntohs(orig_ip->len) - sizeof(iphdr);
			ip->len = htons(len - sizeof(ethdr));
			
			ip->dip ^= ip->sip;
			ip->sip ^= ip->dip;
			ip->dip ^= ip->sip;
			ip->ttl = TTL;
			
			/* Set data size for tcpReplyPacket */
			cb->dsize = response_len;
			
			int rt = tcpReplyPacket(th, cb, tcplen);
			if (rt == 0) {
				// printf("DEBUG: tcpReplyPacket failed for HTTP response\n");
				del_pkt(m);
				return NULL;
			}
			
			// printf("DEBUG: HTTP response packet created successfully\n");
			
			/* Update TCP header data size */
			ti->ti_len = htons(len - sizeof(iphdr));
			
			bcopy(((struct ipovly *)ip)->ih_x1, b, 9);
			bzero(((struct ipovly *)ip)->ih_x1, 9);
			
			ti->ti_sum = 0;
			ti->ti_sum = cksum((unsigned short *)ti, len - sizeof(ethdr));
			bcopy(b, ((struct ipovly *)ip)->ih_x1, 9);

			ip->cksum = 0;
			ip->cksum = cksum((unsigned short *)ip, sizeof(iphdr));
			
			swap_ehead(m->data);
			
			/* save the status, ACK for TH_FIN of client was sent 
			 * so send FIN on the next time
			 */
			if (rt == 2)
				cb->flags = (TH_ACK | TH_FIN);
				
			return m;
		} else {
			// printf("DEBUG: No HTTP response generated\n");
		}
	} else {
		// printf("DEBUG: Not HTTP data packet - flags: 0x%02x, dsize: %d\n", orig_th->th_flags, orig_dsize);
	}
	
	/* Default TCP reply (no HTTP data) */
	// printf("DEBUG: Creating default TCP reply packet\n");
	len = sizeof(ethdr) + sizeof(iphdr) + sizeof(tcphdr);
	m = new_pkt(len);
	if (m == NULL)
		return NULL;
	memcpy(m->data, m0->data, m->len);
	
	eh = (ethdr *)(m->data);
	ip = (iphdr *)(eh + 1);
	ti = (tcpiphdr *)ip;
	th = (tcphdr *)(ip + 1);
	
	tcplen = ntohs(ip->len) - sizeof(iphdr);
	ip->len = htons(len - sizeof(ethdr));
	
	ip->dip ^= ip->sip;
	ip->sip ^= ip->dip;
	ip->dip ^= ip->sip;
	ip->ttl = TTL;
	
	int rt = tcpReplyPacket(th, cb, tcplen);
	if (rt == 0) {
		// printf("DEBUG: tcpReplyPacket failed for default TCP reply\n");
		del_pkt(m);
		return NULL;
	} 
	
	// printf("DEBUG: Default TCP reply packet created successfully\n");
			
	ti->ti_len = htons(len - sizeof(iphdr));
	
	bcopy(((struct ipovly *)ip)->ih_x1, b, 9);
	bzero(((struct ipovly *)ip)->ih_x1, 9);
	
	ti->ti_sum = 0;
	ti->ti_sum = cksum((unsigned short *)ti, len);
	bcopy(b, ((struct ipovly *)ip)->ih_x1, 9);

	ip->cksum = 0;
	ip->cksum = cksum((unsigned short *)ip, sizeof(iphdr));
	
	swap_ehead(m->data);
	
	/* save the status, ACK for TH_FIN of client was sent 
	 * so send FIN on the next time
	 */
	if (rt == 2)
		cb->flags = (TH_ACK | TH_FIN);
		
	return m;	
}

int tcp6(pcs *pc, struct packet *m)
{
	ip6hdr *ip = (ip6hdr *)(m->data + sizeof(ethdr));
	struct tcphdr *th = (struct tcphdr *)(ip + 1);
	sesscb *cb = NULL;
	struct packet *p = NULL;
	int i;

	/* from linklocal */
	if (ip->src.addr16[0] == IPV6_ADDR_INT16_ULL) {
		if (!IP6EQ(&(pc->link6.ip), &(ip->dst)))// || th->th_sport != th->th_dport)
			return PKT_DROP;
	} else {
		if (!IP6EQ(&(pc->ip6.ip), &(ip->dst)))// || th->th_sport != th->th_dport)
			return PKT_DROP;
	}

	/* response packet 
	 * 1. socket opened
	 * 2. same port
	 * 3. destination is me
	 * 4. mscb.proto is TCP
	 */
	if (pc->mscb.sock && pc->mscb.proto == ip->ip6_nxt &&
	    ntohs(th->th_dport) == pc->mscb.sport && 
	    ntohs(th->th_sport) == pc->mscb.dport &&
	    IP6EQ(&(pc->mscb.dip6), &(ip->src))) {
		/* mscb is actived, up to the upper application */
		if (time_tick - pc->mscb.timeout <= TCP_TIMEOUT)
			return PKT_UP;

		/* not mine, reset the request*/
		sesscb rcb;
		
		rcb.seq = random();
		memcpy(rcb.sip6.addr8, ip->src.addr8, 16);
		memcpy(rcb.dip6.addr8, ip->dst.addr8, 16);
		rcb.sport = th->th_sport;
		rcb.dport = th->th_dport;
		rcb.flags = TH_RST | TH_FIN | TH_ACK;
		rcb.seq = time(0);
		
		p = tcp6Reply(m, &rcb);
		
		/* push m into the background output queue which is watched by pth_output */
		if (p != NULL) {
			enq(&pc->bgoq, p);
		} else
			printf("reply error\n");
		
		/* drop the request packet */
		return PKT_DROP;
	}

	/* request process
	 * find control block 
	 */
	for (i = 0; i < MAX_SESSIONS; i++) {
		if (th->th_flags == TH_SYN) {
			if (pc->sesscb[i].timeout == 0 || 
				(IP6EQ(&(pc->sesscb[i].sip6), &(ip->src)) && 
				 IP6EQ(&(pc->sesscb[i].dip6), &(ip->dst)) &&
				 th->th_sport == pc->sesscb[i].sport &&
				 th->th_dport == pc->sesscb[i].dport)) {
				/* get new scb */
				cb = &pc->sesscb[i];
				cb->timeout = time_tick;
				cb->seq = random();
				memcpy(cb->sip6.addr8, ip->src.addr8, 16);
				memcpy(cb->dip6.addr8, ip->dst.addr8, 16);
				cb->sport = th->th_sport;
				cb->dport = th->th_dport;

				break;
			}
		} else {

	
			if ((time_tick - 
			    pc->sesscb[i].timeout <= TCP_TIMEOUT) && 
			    IP6EQ(&(pc->sesscb[i].sip6), &(ip->src)) && 
			    IP6EQ(&(pc->sesscb[i].dip6), &(ip->dst)) &&
			    th->th_sport == pc->sesscb[i].sport &&
			    th->th_dport == pc->sesscb[i].dport) {
				/* get the scb */
				cb = &pc->sesscb[i];
				break;
			}	
		}
	}

	if (th->th_flags == TH_SYN && cb == NULL) {
		printf("VPCS %d out of session\n", pc->id);
		return PKT_DROP;
	}

	if (cb != NULL) {
		if (th->th_flags == TH_ACK && cb->flags == TH_FIN) {
			/* clear session */
			memset(cb, 0, sizeof(sesscb));
		} else {
			cb->timeout = time_tick;
			p = tcp6Reply(m, cb);
			
			/* push m into the background output queue which is watched by pth_output */
			if (p != NULL) {
				enq(&pc->bgoq, p);
			}

			/* send FIN after ACK if got FIN */	
			if ((cb->rflags & TH_FIN) == TH_FIN && cb->flags == (TH_ACK | TH_FIN)) {	
				p = tcp6Reply(m, cb);
				/* push m into the background output queue which is watched by pth_output */
				if (p != NULL) {
					enq(&pc->bgoq, p);
				}
			}
		}
	}

	/* anyway tell caller to drop this packet */
	return PKT_DROP;	
}

struct packet *tcp6Reply(struct packet *m0, sesscb *cb)
{
	ethdr *eh;
	ip6hdr *ip;
	tcphdr *th;
	struct packet *m;
	int len;
	int tcplen = 0;
	
	len = sizeof(ethdr) + sizeof(ip6hdr) + sizeof(tcphdr);
	m = new_pkt(len);
	if (m == NULL)
		return NULL;
		
	memcpy(m->data, m0->data, m->len);
	
	eh = (ethdr *)(m->data);
	ip = (ip6hdr *)(eh + 1);
	th = (struct tcphdr *)(ip + 1);
		
	swap_ehead(m->data);
	swap_ip6head(m);

	ip->ip6_hlim = TTL;
	tcplen = ntohs(ip->ip6_plen);
	ip->ip6_plen = htons(sizeof(tcphdr));
	
	int rt = tcpReplyPacket(th, cb, tcplen);
	if (rt == 0) {
		del_pkt(m);
		return NULL;
	} 

	th->th_sum = 0;
	th->th_sum = cksum6(ip, IPPROTO_TCP, len);
	
	/* save the status, ACK for TH_FIN of client was sent 
	 * so send FIN on the next time
	 */
	if (rt == 2)
		cb->flags = (TH_ACK | TH_FIN);
		
	return m;	
}
/* end of file */
