#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include <time.h>

#include "httpd.h"

/* Global HTTP server instance */
httpd_server_t httpd_server = {
    .running = 0,
    .port = HTTPD_DEFAULT_PORT,
    .server_fd = -1,
    .thread = 0,
    .header_echo = 0,
    .docroot = ".",
    .active_connections = 0
};

int httpd_start(int port, const char *docroot)
{
    struct sockaddr_in addr;
    int opt = 1;
    
    if (httpd_server.running) {
        printf("HTTP server is already running on port %d\n", httpd_server.port);
        return 0;
    }
    
    /* Create socket */
    // AF_INET specifies that the socket will use the IPv4 protocol.
    // SOCK_STREAM indicates that the socket will provide sequenced, reliable, two-way, connection-based byte streams (typically used for TCP).
    httpd_server.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (httpd_server.server_fd == -1) {
        printf("Failed to create socket: %s\n", strerror(errno));
        return -1;
    }
    
    /* Set socket options */
    if (setsockopt(httpd_server.server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        printf("Failed to set socket options: %s\n", strerror(errno));
        close(httpd_server.server_fd);
        return -1;
    }
    
    /* Bind socket */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(httpd_server.server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("Failed to bind to port %d: %s\n", port, strerror(errno));
        close(httpd_server.server_fd);
        return -1;
    }
    
    /* Listen */
    if (listen(httpd_server.server_fd, 2) < 0) {
        printf("Failed to listen: %s\n", strerror(errno));
        close(httpd_server.server_fd);
        return -1;
    }
    
    /* Set server parameters */
    httpd_server.port = port;
    if (docroot) {
        strncpy(httpd_server.docroot, docroot, sizeof(httpd_server.docroot) - 1);
        httpd_server.docroot[sizeof(httpd_server.docroot) - 1] = '\0';
    }
    
    /* Start server thread */
    httpd_server.running = 1;
    if (pthread_create(&httpd_server.thread, NULL, httpd_thread, NULL) != 0) {
        printf("Failed to create HTTP server thread: %s\n", strerror(errno));
        httpd_server.running = 0;
        close(httpd_server.server_fd);
        return -1;
    }
    
    printf("HTTP server started on port %d\n", port);
    if (docroot) {
        printf("Document root: %s\n", docroot);
    }
    
    return 1;
}

int httpd_stop(void)
{
    if (!httpd_server.running) {
        printf("HTTP server is not running\n");
        return 0;
    }
    
    httpd_server.running = 0;
    
    /* Close server socket to wake up accept() */
    if (httpd_server.server_fd != -1) {
        close(httpd_server.server_fd);
        httpd_server.server_fd = -1;
    }
    
    /* Wait for thread to finish */
    if (httpd_server.thread) {
        pthread_join(httpd_server.thread, NULL);
        httpd_server.thread = 0;
    }
    
    printf("HTTP server stopped\n");
    return 1;
}

int httpd_status(void)
{
    printf("\nHTTP Server Status:\n");
    printf("Running: %s\n", httpd_server.running ? "Yes" : "No");
    if (httpd_server.running) {
        printf("Port: %d\n", httpd_server.port);
        printf("Document root: %s\n", httpd_server.docroot);
        printf("Header echo: %s\n", httpd_server.header_echo ? "Enabled" : "Disabled");
        printf("Active connections: %d\n", httpd_server.active_connections);
    }
    return 1;
}

void httpd_set_header_echo(int enable)
{
    httpd_server.header_echo = enable;
    printf("Header echo %s\n", enable ? "enabled" : "disabled");
}

void *httpd_thread(void *arg)
{
    struct sockaddr_in client_addr;
    socklen_t client_len;
    int client_fd;
    fd_set read_fds;
    struct timeval timeout;
    
    printf("HTTP server thread started\n");
    
    while (httpd_server.running) {
        FD_ZERO(&read_fds);
        FD_SET(httpd_server.server_fd, &read_fds);
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int activity = select(httpd_server.server_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (activity < 0) {
            if (errno != EINTR) {
                printf("HTTP server select error: %s\n", strerror(errno));
                break;
            }
            continue;
        }
        
        if (activity == 0) {
            /* Timeout - continue loop to check running flag */
            continue;
        }
        
        if (FD_ISSET(httpd_server.server_fd, &read_fds)) {
            client_len = sizeof(client_addr);
            client_fd = accept(httpd_server.server_fd, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_fd < 0) {
                if (errno != EINTR && httpd_server.running) {
                    printf("HTTP server accept error: %s\n", strerror(errno));
                }
                continue;
            }
            
            httpd_server.active_connections++;
            printf("HTTP client connected from %s:%d\n", 
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            
            /* Handle client in the same thread for simplicity */
            httpd_handle_client(client_fd);
            
            close(client_fd);
            httpd_server.active_connections--;
        }
    }
    
    printf("HTTP server thread stopped\n");
    return NULL;
}

void httpd_handle_client(int client_fd)
{
    char request[HTTPD_MAX_REQUEST_SIZE];
    char method[16], path[256], version[16];
    int bytes_received;
    char *line, *saveptr;
    
    /* Receive request */
    bytes_received = recv(client_fd, request, sizeof(request) - 1, 0);
    if (bytes_received <= 0) {
        return;
    }
    
    request[bytes_received] = '\0';
    
    /* Parse first line */
    line = strtok_r(request, "\r\n", &saveptr);
    if (!line) {
        httpd_send_response(client_fd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }
    
    if (sscanf(line, "%15s %255s %15s", method, path, version) != 3) {
        httpd_send_response(client_fd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }
    
    printf("HTTP %s %s %s\n", method, path, version);
    
    /* Handle different methods */
    if (strcmp(method, "GET") == 0) {
        if (httpd_server.header_echo) {
            /* Send back the original request headers */
            httpd_send_headers_echo(client_fd, request);
        } else {
            /* Send simple response */
            char response_body[512];
            snprintf(response_body, sizeof(response_body),
                "<html><head><title>VPCS HTTP Server</title></head>"
                "<body><h1>VPCS HTTP Server</h1>"
                "<p>Request: %s %s</p>"
                "<p>Server running on port %d</p>"
                "</body></html>",
                method, path, httpd_server.port);
            
            httpd_send_response(client_fd, "200 OK", "text/html", response_body);
        }
    } else {
        httpd_send_response(client_fd, "405 Method Not Allowed", "text/plain", "Method Not Allowed");
    }
}

void httpd_send_response(int client_fd, const char *status, const char *content_type, const char *body)
{
    char response[HTTPD_MAX_RESPONSE_SIZE];
    time_t now;
    struct tm *tm_info;
    char time_str[64];
    
    time(&now);
    tm_info = gmtime(&now);
    strftime(time_str, sizeof(time_str), "%a, %d %b %Y %H:%M:%S GMT", tm_info);
    
    snprintf(response, sizeof(response),
        "HTTP/1.0 %s\r\n"
        "Date: %s\r\n"
        "Server: VPCS-HTTP/1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, time_str, content_type, strlen(body), body);
    
    send(client_fd, response, strlen(response), 0);
}

void httpd_send_headers_echo(int client_fd, const char *request)
{
    char response_body[HTTPD_MAX_RESPONSE_SIZE];
    char escaped_request[HTTPD_MAX_REQUEST_SIZE * 2];
    const char *src = request;
    char *dst = escaped_request;
    
    /* HTML escape the request */
    while (*src && (dst - escaped_request) < sizeof(escaped_request) - 10) {
        switch (*src) {
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
            default:
                *dst++ = *src;
                break;
        }
        src++;
    }
    *dst = '\0';
    
    snprintf(response_body, sizeof(response_body),
        "<html><head><title>VPCS HTTP Server - Headers Echo</title></head>"
        "<body><h1>Your Request Headers</h1>"
        "<pre>%s</pre>"
        "</body></html>",
        escaped_request);
    
    httpd_send_response(client_fd, "200 OK", "text/html", response_body);
}
