// THIS IS THE RECEIVE PART!!!
int tcp(pcs *pc, struct packet *m)
{
	printf("tcp RECEIVE\n");
	// print_trace();
	iphdr *ip_header = (iphdr *)(m->data + sizeof(ethdr)); // Get pointer to IP header (after Ethernet header)
	tcpiphdr *tcp_ip_header = (tcpiphdr *)(ip_header); // Get pointer to TCP/IP header (usually starts at IP header)
	sesscb *session_control_block = NULL;
	struct packet *packet = NULL;
	int i;
	
	if (ip_header->dip != pc->ip4.ip)
		return PKT_DROP;

	/* response packet 
	 * 1. socket opened
	 * 2. same port
	 * 3. destination is me
	 * 4. mscb.proto is TCP
	 */
	printf("--- SESSION LOOKUP START ---\n");
	// CHECK IF THIS IS THE MSCB SO MAIN SESSION!!!!
	if (pc->mscb.sock && ntohs(tcp_ip_header->ti_dport) == pc->mscb.sport &&	// socket opened
	    ntohs(tcp_ip_header->ti_sport) == pc->mscb.dport && 				  	// same port
	    ip_header->sip == pc->mscb.dip && 									// destination is me
		pc->mscb.proto == ip_header->proto									// protocol is tcp
	) {
		printf("MSBC ACTIVATED!\n");
		/* mscb is actived, up to the upper application */
		if (time_tick - pc->mscb.timeout <= TCP_TIMEOUT) {
			return PKT_UP;
		}

		/* not mine, reset the request */
		sesscb rcb;
		
		// this should never happen I think ? I think it's detecting that tcp session is broken and it tries to reset
		rcb.seq = random();
		rcb.sip = ip_header->sip;
		rcb.dip = ip_header->dip;
		rcb.sport = tcp_ip_header->ti_sport;
		rcb.dport = tcp_ip_header->ti_dport;
		rcb.flags = TH_RST | TH_FIN | TH_ACK;
		
		packet = tcpReply(m, &rcb);
		
		/* push m into the background output queue which is watched by pth_output */
		if (packet != NULL) {
			// push packet p which is the response ? to pc->bgoq
			enq(&pc->bgoq, packet);			
		} else
			printf("reply error\n");
		
		/* drop the request packet */
		return PKT_DROP;
	}


	
	/* request process
	 * find control block 
	 */
	// find control block for that session ?
	for (i = 0; i < MAX_SESSIONS; i++) {
		if (tcp_ip_header->ti_flags == TH_SYN) { // if this packet wants to SYN
			printf("packet wants to sync\n");
			// use session i if:
			// timeout == 0  // free slot for a session
			// > TCP_TIMEOUT // free slot for a session
			// if sip dip sport dport match find the corresponding connection
			if (pc->sesscb[i].timeout == 0 || 
			    time_tick - pc->sesscb[i].timeout > TCP_TIMEOUT ||
			    (ip_header->sip == pc->sesscb[i].sip &&  // same source ip
			     ip_header->dip == pc->sesscb[i].dip && // same destination ip
			     tcp_ip_header->ti_sport == pc->sesscb[i].sport && // same source port
				 tcp_ip_header->ti_dport == pc->sesscb[i].dport)) { // same destination ip

				/* get new scb */
				session_control_block = &pc->sesscb[i]; 
				// start assigning new scb cuz now session control is the pointer to session control block
				session_control_block->timeout = time_tick;
				session_control_block->seq = random();
				session_control_block->sip = ip_header->sip;
				session_control_block->dip = ip_header->dip;
				session_control_block->sport = tcp_ip_header->ti_sport;
				session_control_block->dport = tcp_ip_header->ti_dport;
				printf("session found at index %d\n", i);
				break;
			}
		} else { // if not sync?
			printf("packet does not want to sync\n");
			// if TCP did not timeout and
			// sip dip sport dport match
			// find corresponding session control block
			if ((time_tick - 
			    pc->sesscb[i].timeout <= TCP_TIMEOUT) && 
			    ip_header->sip == pc->sesscb[i].sip && 
			    ip_header->dip == pc->sesscb[i].dip &&
			    tcp_ip_header->ti_sport == pc->sesscb[i].sport &&
			    tcp_ip_header->ti_dport == pc->sesscb[i].dport) {
				/* get the scb */
				printf("session found at index %d\n", i);
				session_control_block = &pc->sesscb[i];
				break;
			}	
		}
	}
	printf("--- SESSION LOOKUP END ---\n");

	
	// if it a SYN but could not find a free control block this is due to shit implementation
	if (tcp_ip_header->ti_flags == TH_SYN && session_control_block == NULL) {
		printf("VPCS %d out of session\n", pc->id);
		return PKT_DROP;
	}
	
	if (session_control_block != NULL) { // if control block found/made or whatever
		printf("CONTROL BLOCK WAS FOUND!\n");
		if (tcp_ip_header->ti_flags == TH_ACK && session_control_block->flags == TH_FIN) { // session end confirmation
			/* clear session */
			memset(session_control_block, 0, sizeof(sesscb));
		} else { // respond to the mfing packet
			session_control_block->timeout = time_tick;
			packet = tcpReply(m, session_control_block);
			
			/* push m into the background output queue which is watched by pth_output */
			if (packet != NULL) { // Send packet out into the world
				enq(&pc->bgoq, packet);
			}
			
			/* send FIN after ACK if got FIN */
			if ((session_control_block->rflags & TH_FIN) == TH_FIN && // current session response FIN flag is 1
			    session_control_block->flags == (TH_ACK | TH_FIN)) { // send ACK and FIN to kill the session
				packet = tcpReply(m, session_control_block);
				
				/* push m into the background output queue which is watched by pth_output */
				if (packet != NULL) {
					enq(&pc->bgoq, packet);
				}
			}
		}
	}

	// this should prolly never be reached
	/* anyway tell caller to drop this packet */
	return PKT_DROP;	
}