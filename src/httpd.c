/*
 * Copyright (c) 2025, Dawid Dębkowski
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "httpd.h"
#include "vpcs.h"
#include "tcp.h"
#include "utils.h"
#include "queue.h"
#include "packets.h"
#include "ip.h"

/* VPCS virtual HTTP servers */
httpd_server_t httpd_servers[HTTPD_MAX_SERVERS] = {0};

/* VPCS Virtual HTTP Server Implementation */

int httpd_start(int port)
{
    extern int pcid; // moge usunac pcid bo kazdy pc ma unikalne ip
    int i;
    
    /* Check if server already running on this port */
    for (i = 0; i < HTTPD_MAX_SERVERS; i++) {
        if (httpd_servers[i].enabled && httpd_servers[i].port == port) {
            printf("VPCS HTTP server already running on port %d\n", port);
            return 0;
        }
    }
    
    /* Find free slot */
    for (i = 0; i < HTTPD_MAX_SERVERS; i++) {
        if (!httpd_servers[i].enabled) {
            httpd_servers[i].enabled = 1;
            httpd_servers[i].port = port;
            httpd_servers[i].pc_id = pcid;
            
            printf("VPCS HTTP server started on PC%d port %d\n", pcid + 1, port);
            printf("Server will echo back incoming HTTP request headers\n");
            return 1;
        }
    }
    
    printf("Maximum number of VPCS HTTP servers reached (%d)\n", HTTPD_MAX_SERVERS);
    return 0;
}

int httpd_stop(int port)
{
    int i;
    
    for (i = 0; i < HTTPD_MAX_SERVERS; i++) {
        if (httpd_servers[i].enabled && 
            httpd_servers[i].port == port) {
            httpd_servers[i].enabled = 0;
            printf("VPCS HTTP server on port %d stopped\n", port);
            return 1;
        }
    }
    
    printf("No VPCS HTTP server running on port %d\n", port);
    return 0;
}

int httpd_status(void)
{
    int i, count = 0;
    
    printf("\nVPCS HTTP Server Status:\n");
    
    for (i = 0; i < HTTPD_MAX_SERVERS; i++) {
        if (httpd_servers[i].enabled) {
            printf("  PC%d: Port %d - Running\n", 
                   httpd_servers[i].pc_id + 1, 
                   httpd_servers[i].port);
            count++;
        }
    }
    
    if (count == 0) {
        printf("  No VPCS HTTP servers running\n");
    }
    
    return 1;
}

// hook in the tcp protocol to handle httpd data if a server is present at the given ip+port
void httpd_handle_request(int port, const char *data, int data_len, char *response, int *response_len)
{
    /* Validate HTTP request - check if it starts with a valid HTTP method */
    if (data_len < 4 || 
        (strncmp(data, "GET ", 4) != 0 &&
         strncmp(data, "POST", 4) != 0 &&
         strncmp(data, "HEAD", 4) != 0 &&
         strncmp(data, "PUT ", 4) != 0)) {
        printf("VPCS HTTP server port %d - received non-HTTP data (%d bytes)\n", port, data_len);
        *response_len = 0;
        return;
    }
    
    printf("VPCS HTTP server port %d - received request (%d bytes):\n", port, data_len);
    printf("--- Start of received data ---\n");
    for (int i = 0; i < data_len && i < 512; i++) {
        if (data[i] >= 32 && data[i] <= 126) {
            printf("%c", data[i]);
        } else if (data[i] == '\r') {
            printf("\\r");
        } else if (data[i] == '\n') {
            printf("\\n\n");
        } else {
            printf("\\x%02x", (unsigned char)data[i]);
        }
    }
    if (data_len > 512) {
        printf("\n... (%d more bytes truncated)", data_len - 512);
    }
    printf("\n--- End of received data ---\n");
    
    /* Generate HTTP response with echo */
    *response_len = snprintf(response, HTTPD_MAX_RESPONSE_SIZE,
        "HTTP/1.0 200 OK\r\n"
        "Server: VPCS-HTTP/1.0\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        data);

    printf("VPCS HTTP server port %d - served request (%d bytes response)\n", 
           port, *response_len);
}

/* HTTP client implementation using VPCS virtual TCP stack */
int httpd_client_get(const char *host, int port, const char *path)
{
    extern int pcid;
    extern pcs vpc[];
    
    pcs *pc = &vpc[pcid];
    struct in_addr addr;
    char request[512];
    int k;
    struct timeval ts, ts0;
    int usec;
    int gip;
    unsigned int gwip;
    
    printf("Connecting to %s:%d%s using VPCS virtual TCP stack\n", host, port, path);
    
    /* Parse IP address */
    if (inet_aton(host, &addr) == 0) {
        printf("Invalid IP address: %s\n", host);
        return -1;
    }
    
    /* Set up connection parameters - following ping pattern */
    // mscb - my session control block
    pc->mscb.frag = true;             /* Allow fragmentation */
    pc->mscb.mtu = pc->mtu;           /* Max Transmission Unit */
    pc->mscb.waittime = 5000;         /* 5 second timeout */
    pc->mscb.ipid = time(0) & 0xffff; /* IP ID */
    pc->mscb.seq = time(0);           /* Sequence */
    pc->mscb.proto = IPPROTO_TCP;     /* Protocol number*/
    pc->mscb.ttl = TTL;               /* TTL */
    pc->mscb.dsize = 0;               /* No data in SYN */
    pc->mscb.sport = 1024 + (rand() % 64511); /* Random source port */
    pc->mscb.dport = port;            /* Destination port */
    pc->mscb.sip = pc->ip4.ip;        /* Source IP: our own IP */
    pc->mscb.dip = addr.s_addr;       /* Destination IP */
    memcpy(pc->mscb.smac, pc->ip4.mac, ETH_ALEN); /* Source MAC */
    pc->mscb.sock = 1;                /* Mark socket as open */
    pc->mscb.winsize = 0xb68;         /* Window size (1460 * 4) */
    pc->mscb.timeout = 0;             /* Reset timeout */
    
    /* Resolve destination MAC address - following ping pattern */
    gwip = pc->ip4.gw;
    if (sameNet(pc->mscb.dip, pc->ip4.ip, pc->ip4.cidr))
        gip = pc->mscb.dip;
    else {
        if (gwip == 0) {
            printf("No gateway found\n");
            return -1;
        }
        gip = gwip;
    }
    
    /* Get the ether address of the destination */
    if (!arpResolve(pc, gip, pc->mscb.dmac)) {
        struct in_addr in;
        in.s_addr = gip;
        printf("host (%s) not reachable\n", inet_ntoa(in));
        return -1;
    }
    
    /* Prepare HTTP request */
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: VPCS-HTTP-Client/1.0\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port);
    
    printf("HTTP request prepared (%zu bytes)\n", strlen(request));
    
    /* Clear input queue before connection */
    struct packet *m;
    while ((m = deq(&pc->iq)) != NULL)
        del_pkt(m);
    
    /* Establish TCP connection */
    gettimeofday(&ts, NULL);
    k = tcp_open(pc, 4);
    
    gettimeofday(&ts0, NULL);
    usec = (ts0.tv_sec - ts.tv_sec) * 1000000 + ts0.tv_usec - ts.tv_usec;
    
    if (k == 0) {
        printf("Connect to %s:%d timeout\n", host, port);
        return -1;
    } else if (k == 2) {
        printf("Connect to %s:%d failed - ICMP error\n", host, port);
        return -1;
    } else if (k == 3) {
        printf("Connect to %s:%d failed - RST returned\n", host, port);
        return -1;
    }
    
    printf("Connected to %s:%d (time=%.3f ms)\n", host, port, usec / 1000.0);
    
    /* Send HTTP request */
    gettimeofday(&ts, NULL);
    
    /* Set up the HTTP request data */
    pc->mscb.dsize = strlen(request);
    pc->mscb.data = request;  /* Point to the request buffer, don't copy */
    
    k = tcp_send(pc, 4);
    
    gettimeofday(&ts0, NULL);
    usec = (ts0.tv_sec - ts.tv_sec) * 1000000 + ts0.tv_usec - ts.tv_usec;
    
    if (k == 0) {
        printf("Send request to %s:%d timeout\n", host, port);
        tcp_close(pc, 4);
        return -1;
    }
    
    printf("HTTP request sent (time=%.3f ms)\n", usec / 1000.0);
    
    /* The server will respond with ACK+PUSH+DATA, which tcp_send already handled */
    /* Check if we received response data */
    if (k == 1 && pc->mscb.rdsize > 0 && pc->mscb.data != NULL) {
        printf("\n--- HTTP Response (%d bytes) ---\n", pc->mscb.rdsize);
        
        /* Print response with proper character handling */
        for (int i = 0; i < pc->mscb.rdsize && i < 4096; i++) {
            char c = pc->mscb.data[i];
            if (c >= 32 && c <= 126) {
                printf("%c", c);
            } else if (c == '\r') {
                printf("\\r");
            } else if (c == '\n') {
                printf("\\n\n");
            } else {
                printf("\\x%02x", (unsigned char)c);
            }
        }
        printf("\n--- End of HTTP Response ---\n");
    } else {
        printf("No HTTP response received\n");
    }
    
    /* Close connection */
    delay_ms(100);
    gettimeofday(&ts, NULL);
    k = tcp_close(pc, 4);
    gettimeofday(&ts0, NULL);
    usec = (ts0.tv_sec - ts.tv_sec) * 1000000 + ts0.tv_usec - ts.tv_usec;
    
    if (k == 0) {
        printf("Close connection to %s:%d timeout\n", host, port);
    } else {
        printf("Connection closed (time=%.3f ms)\n", usec / 1000.0);
    }
    
    return 0;
}
