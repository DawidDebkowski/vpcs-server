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

#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define HTTPD_MAX_REQUEST_SIZE 4096
#define HTTPD_MAX_RESPONSE_SIZE 8192
#define HTTPD_DEFAULT_PORT 8080

typedef struct {
    int running;
    int port;
    int server_fd;
    pthread_t thread;
    int header_echo;
    char docroot[256];
    int active_connections;
} httpd_server_t;

extern httpd_server_t httpd_server;

/* HTTP server functions */
int httpd_start(int port, const char *docroot);
int httpd_stop(void);
int httpd_status(void);
void httpd_set_header_echo(int enable);

/* Internal functions */
void *httpd_thread(void *arg);
void httpd_handle_client(int client_fd);
void httpd_send_response(int client_fd, const char *status, const char *content_type, const char *body);
void httpd_send_headers_echo(int client_fd, const char *request);

#endif /* _HTTPD_H_ */
