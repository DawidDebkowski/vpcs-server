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
vpcs_httpd_server_t vpcs_httpd_servers[HTTPD_MAX_SERVERS] = {0};

/* VPCS Virtual HTTP Server Implementation */

int vpcs_httpd_start(int port)
{
    extern int pcid;
    int i;
    
    /* Check if server already running on this port */
    for (i = 0; i < HTTPD_MAX_SERVERS; i++) {
        if (vpcs_httpd_servers[i].enabled && vpcs_httpd_servers[i].port == port) {
            printf("VPCS HTTP server already running on port %d\n", port);
            return 0;
        }
    }
    
    /* Find free slot */
    for (i = 0; i < HTTPD_MAX_SERVERS; i++) {
        if (!vpcs_httpd_servers[i].enabled) {
            vpcs_httpd_servers[i].enabled = 1;
            vpcs_httpd_servers[i].port = port;
            vpcs_httpd_servers[i].pc_id = pcid;
            
            printf("VPCS HTTP server started on PC%d port %d\n", pcid + 1, port);
            printf("Server will echo back incoming HTTP request headers\n");
            return 1;
        }
    }
    
    printf("Maximum number of VPCS HTTP servers reached (%d)\n", HTTPD_MAX_SERVERS);
    return 0;
}

int vpcs_httpd_stop(int port)
{
    int i;
    
    for (i = 0; i < HTTPD_MAX_SERVERS; i++) {
        if (vpcs_httpd_servers[i].enabled && vpcs_httpd_servers[i].port == port) {
            vpcs_httpd_servers[i].enabled = 0;
            printf("VPCS HTTP server on port %d stopped\n", port);
            return 1;
        }
    }
    
    printf("No VPCS HTTP server running on port %d\n", port);
    return 0;
}

int vpcs_httpd_status(void)
{
    int i, count = 0;
    
    printf("\nVPCS HTTP Server Status:\n");
    
    for (i = 0; i < HTTPD_MAX_SERVERS; i++) {
        if (vpcs_httpd_servers[i].enabled) {
            printf("  PC%d: Port %d - Running\n", 
                   vpcs_httpd_servers[i].pc_id + 1, 
                   vpcs_httpd_servers[i].port);
            count++;
        }
    }
    
    if (count == 0) {
        printf("  No VPCS HTTP servers running\n");
    }
    
    return 1;
}

void vpcs_httpd_handle_request(int port, const char *data, int data_len, char *response, int *response_len)
{
    extern int pcid;
    char escaped_request[HTTPD_MAX_REQUEST_SIZE * 2];
    const char *src = data;
    char *dst = escaped_request;
    int i;
    
    printf("VPCS HTTP server PC%d:%d - received request (%d bytes):\n", 
           pcid + 1, port, data_len);
    printf("--- Start of received data ---\n");
    for (i = 0; i < data_len && i < 512; i++) {
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
    
    /* Check if we have a server on this port */
    int server_found = 0;
    for (i = 0; i < HTTPD_MAX_SERVERS; i++) {
        if (vpcs_httpd_servers[i].enabled && 
            vpcs_httpd_servers[i].port == port && 
            vpcs_httpd_servers[i].pc_id == pcid) {
            server_found = 1;
            break;
        }
    }
    
    if (!server_found) {
        *response_len = 0;
        return;
    }
    
    /* HTML escape the request */
    int src_len = (data_len < HTTPD_MAX_REQUEST_SIZE) ? data_len : HTTPD_MAX_REQUEST_SIZE;
    for (i = 0; i < src_len && (dst - escaped_request) < sizeof(escaped_request) - 10; i++) {
        switch (src[i]) {
            case '<':
                strcpy(dst, "&lt;");
                dst += 4;
                break;
            case '>':
                strcpy(dst, "&gt;");
                dst += 4;
                break;
            case '&':
                strcpy(dst, "&amp;");
                dst += 5;
                break;
            case '"':
                strcpy(dst, "&quot;");
                dst += 6;
                break;
            case '\0':
                goto escape_done;
            default:
                *dst++ = src[i];
                break;
        }
    }
escape_done:
    *dst = '\0';
    
    /* Generate HTTP response with echo */
    *response_len = snprintf(response, HTTPD_MAX_RESPONSE_SIZE,
        "HTTP/1.0 200 OK\r\n"
        "Server: VPCS-Virtual-HTTP/1.0\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html><head><title>VPCS Virtual HTTP Server</title></head>"
        "<body><h1>VPCS Virtual HTTP Server - PC%d:%d</h1>"
        "<h2>Your Request Headers:</h2>"
        "<pre>%s</pre>"
        "</body></html>",
        pcid + 1, port, escaped_request);
    
    printf("VPCS HTTP server PC%d:%d - served request (%d bytes response)\n", 
           pcid + 1, port, *response_len);
}

/* HTTP client implementation using VPCS virtual TCP stack */
int httpd_client_get(const char *host, int port, const char *path)
{
    extern int pcid;
    extern pcs vpc[];
    extern int ctrl_c;
    
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
    pc->mscb.frag = true;             /* Allow fragmentation */
    pc->mscb.mtu = pc->mtu;           /* MTU */
    pc->mscb.waittime = 5000;         /* 5 second timeout */
    pc->mscb.ipid = time(0) & 0xffff; /* IP ID */
    pc->mscb.seq = time(0);           /* Sequence */
    pc->mscb.proto = IPPROTO_TCP;     /* Protocol */
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
    
    /* Clear input queue */
    struct packet *m;
    while ((m = deq(&pc->iq)) != NULL)
        del_pkt(m);
    
    /* Connect to server */
    gettimeofday(&ts, NULL);
    
    /* Clear data size for SYN packet */
    pc->mscb.dsize = 0; /* No data in SYN */
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
    
    /* Wait for and process HTTP response */
    printf("Waiting for HTTP response...\n");
    gettimeofday(&ts, NULL);
    
    int response_received = 0;
    char response_buffer[4096];
    int total_response_len = 0;
    
    /* Wait up to 5 seconds for response */
    while (!timeout(ts, 5000) && !response_received && !ctrl_c) {
        delay_ms(10);
        
        struct packet *resp_pkt;
        while ((resp_pkt = deq(&pc->iq)) != NULL) {
            /* Check if this is a TCP response packet */
            iphdr *resp_ip = (iphdr *)(resp_pkt->data + sizeof(ethdr));
            
            if (resp_ip->proto == IPPROTO_TCP && 
                resp_ip->sip == pc->mscb.dip && 
                resp_ip->dip == pc->mscb.sip) {
                
                tcpiphdr *resp_ti = (tcpiphdr *)resp_ip;
                
                if (ntohs(resp_ti->ti_sport) == pc->mscb.dport && 
                    ntohs(resp_ti->ti_dport) == pc->mscb.sport) {
                    
                    /* Check if this packet contains data (ACK + PUSH) */
                    if ((resp_ti->ti_flags & (TH_ACK | TH_PUSH)) == (TH_ACK | TH_PUSH)) {
                        int resp_tcplen = ntohs(resp_ip->len) - sizeof(iphdr);
                        int resp_data_len = resp_tcplen - (resp_ti->ti_off << 2);
                        
                        if (resp_data_len > 0) {
                            char *resp_data = (char *)resp_ti + (resp_ti->ti_off << 2);
                            
                            /* Copy response data to buffer */
                            int copy_len = resp_data_len;
                            if (total_response_len + copy_len > sizeof(response_buffer) - 1) {
                                copy_len = sizeof(response_buffer) - 1 - total_response_len;
                            }
                            
                            if (copy_len > 0) {
                                memcpy(response_buffer + total_response_len, resp_data, copy_len);
                                total_response_len += copy_len;
                            }
                            
                            printf("Received HTTP response data (%d bytes)\n", resp_data_len);
                            response_received = 1;
                        }
                    }
                }
            }
            del_pkt(resp_pkt);
        }
    }
    
    /* Display the HTTP response */
    if (response_received && total_response_len > 0) {
        response_buffer[total_response_len] = '\0';
        printf("\n--- HTTP Response (%d bytes) ---\n", total_response_len);
        printf("%s", response_buffer);
        printf("\n--- End of HTTP Response ---\n");
    } else {
        printf("No HTTP response received (timeout or no data)\n");
    }
    
    /* Wait a bit more before closing */
    delay_ms(100);
    
    /* Close connection */
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
