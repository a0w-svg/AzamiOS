/* ============================================================================
 * AzamiOS Userspace — Lightweight HTTP Micro Web Server (httpd.elf)
 * File: userland/apps/httpd/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define DEFAULT_PORT 80
#define BUFFER_SIZE  4096

static void send_response(int client_fd, int status_code, const char *status_text,
                          const char *content_type, const char *body, size_t body_len)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: AzamiOS-httpd/1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len
    );

    send(client_fd, header, (size_t)hlen, 0);
    if (body && body_len > 0) {
        send(client_fd, body, body_len, 0);
    }
}

#include <sys/ioctl.h>

static void handle_client(int client_fd)
{
    char req_buf[BUFFER_SIZE];
    ssize_t n = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    req_buf[n] = '\0';

    char method[16];
    char path[256];
    if (sscanf(req_buf, "%15s %255s", method, path) != 2) {
        send_response(client_fd, 400, "Bad Request", "text/plain", "Bad Request", 11);
        close(client_fd);
        return;
    }

    /* Query real active IP and Gateway from kernel */
    unsigned char host_ip[4] = { 0, 0, 0, 0 };
    unsigned char host_gw[4] = { 0, 0, 0, 0 };
    int net_fd = open("/dev/net0", O_RDWR, 0);
    if (net_fd >= 0) {
        ioctl(net_fd, 0x8915 /* SIOCGIFADDR */, (unsigned long)host_ip);
        ioctl(net_fd, 0x891d /* SIOCGIFGW */, (unsigned long)host_gw);
        close(net_fd);
    }

    char ip_str[32], gw_str[32];
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", host_ip[0], host_ip[1], host_ip[2], host_ip[3]);
    snprintf(gw_str, sizeof(gw_str), "%u.%u.%u.%u", host_gw[0], host_gw[1], host_gw[2], host_gw[3]);

    /* JSON API Endpoint */
    if (strcmp(path, "/api/status") == 0) {
        char json[256];
        int jlen = snprintf(json, sizeof(json),
            "{\"os\":\"AzamiOS\",\"version\":\"7.0\",\"arch\":\"x86_64\",\"status\":\"running\",\"network\":{\"ip\":\"%s\",\"gateway\":\"%s\"}}\n",
            ip_str, gw_str
        );
        send_response(client_fd, 200, "OK", "application/json", json, (size_t)jlen);
        close(client_fd);
        return;
    }

    /* HTML Dashboard */
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        char html[4096];
        int hlen = snprintf(html, sizeof(html),
            "<!DOCTYPE html>\n"
            "<html>\n"
            "<head>\n"
            "  <meta charset=\"utf-8\">\n"
            "  <title>AzamiOS Web Dashboard</title>\n"
            "  <style>\n"
            "    body { background: #0f172a; color: #f8fafc; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; padding: 40px; margin: 0; }\n"
            "    .container { max-width: 800px; margin: 0 auto; background: #1e293b; border-radius: 12px; padding: 32px; box-shadow: 0 10px 25px rgba(0,0,0,0.5); border: 1px solid #334155; }\n"
            "    h1 { color: #38bdf8; margin-top: 0; font-size: 28px; border-bottom: 1px solid #334155; padding-bottom: 16px; }\n"
            "    .badge { background: #0284c7; color: white; padding: 4px 12px; border-radius: 9999px; font-size: 14px; display: inline-block; margin-bottom: 20px; }\n"
            "    .card { background: #0f172a; padding: 16px; border-radius: 8px; margin-bottom: 16px; border: 1px solid #1e293b; }\n"
            "    .card-title { color: #94a3b8; font-size: 13px; text-transform: uppercase; font-weight: 600; margin-bottom: 8px; }\n"
            "    .card-value { font-size: 18px; font-weight: 500; color: #e2e8f0; }\n"
            "    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }\n"
            "    .status-dot { width: 10px; height: 10px; background: #22c55e; border-radius: 50%%; display: inline-block; margin-right: 6px; }\n"
            "  </style>\n"
            "</head>\n"
            "<body>\n"
            "  <div class=\"container\">\n"
            "    <span class=\"badge\"><span class=\"status-dot\"></span>AzamiOS HTTP Server Online</span>\n"
            "    <h1>AzamiOS Microkernel System Dashboard</h1>\n"
            "    <div class=\"grid\">\n"
            "      <div class=\"card\"><div class=\"card-title\">Host IP Address</div><div class=\"card-value\">%s</div></div>\n"
            "      <div class=\"card\"><div class=\"card-title\">Default Gateway</div><div class=\"card-value\">%s</div></div>\n"
            "      <div class=\"card\"><div class=\"card-title\">Architecture</div><div class=\"card-value\">x86_64 SMP (4 Cores)</div></div>\n"
            "      <div class=\"card\"><div class=\"card-title\">Kernel Version</div><div class=\"card-value\">AzamiOS v7.0 Microkernel</div></div>\n"
            "      <div class=\"card\"><div class=\"card-title\">Networking Stack</div><div class=\"card-value\">IPv4 / ICMP / UDP / TCP / DHCP Client</div></div>\n"
            "      <div class=\"card\"><div class=\"card-title\">Display Engine</div><div class=\"card-value\">X11 / Xorg Server active</div></div>\n"
            "    </div>\n"
            "  </div>\n"
            "</body>\n"
            "</html>\n",
            ip_str, gw_str
        );

        send_response(client_fd, 200, "OK", "text/html", html, (size_t)hlen);
        close(client_fd);
        return;
    }

    send_response(client_fd, 404, "Not Found", "text/plain", "404 Not Found\n", 14);
    close(client_fd);
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        }
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd < 0) {
        perror("httpd: socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((unsigned short)port);
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("httpd: bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("httpd: listen");
        close(server_fd);
        return 1;
    }

    printf("httpd: listening on http://0.0.0.0:%d/\n", port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);
        if (client_fd < 0) {
            continue;
        }

        handle_client(client_fd);
    }

    close(server_fd);
    return 0;
}
