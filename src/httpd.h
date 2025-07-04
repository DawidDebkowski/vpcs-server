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

#ifndef _HTTPD_H_
#define _HTTPD_H_

#define HTTPD_MAX_REQUEST_SIZE 4096
#define HTTPD_MAX_RESPONSE_SIZE 8192
#define HTTPD_MAX_SERVERS 4

typedef struct {
    int enabled;
    int port;
    int pc_id;
} vpcs_httpd_server_t;

extern vpcs_httpd_server_t vpcs_httpd_servers[HTTPD_MAX_SERVERS];

/* VPCS virtual HTTP server functions */
int vpcs_httpd_start(int port);
int vpcs_httpd_stop(int port);
int vpcs_httpd_status(void);
void vpcs_httpd_handle_request(int port, const char *data, int data_len, char *response, int *response_len);

/* HTTP client functions */
int httpd_client_get(const char *host, int port, const char *path);

#endif /* _HTTPD_H_ */
