#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "directory_reader.h"

#define PORT 8080
#define BUFFER_SIZE 4096
#define HTML_SIZE 65536
#define RESPONSE_SIZE 70000

static int send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int is_safe_filename(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return 0;
    }

    if (strstr(name, "..") != NULL) {
        return 0;
    }

    for (const char *p = name; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || c == '\\' || c == '"' || c < 32 || c == 127) {
            return 0;
        }
    }

    return 1;
}

static int url_decode(const char *src, char *dst, size_t max_size) {
    size_t j = 0;

    for (size_t i = 0; src[i] != '\0'; i++) {
        if (j + 1 >= max_size) {
            return -1;
        }

        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' &&
                   src[i + 1] != '\0' &&
                   src[i + 2] != '\0') {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            char *end = NULL;
            long value = strtol(hex, &end, 16);
            if (end == NULL || *end != '\0') {
                return -1;
            }
            dst[j++] = (char)value;
            i += 2;
        } else {
            dst[j++] = src[i];
        }
    }

    dst[j] = '\0';
    return 0;
}

static void send_simple_response(int client_fd, const char *status, const char *content_type, const char *body) {
    char response[RESPONSE_SIZE];
    int written = snprintf(response, sizeof(response),
                           "HTTP/1.1 %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "%s",
                           status, content_type, strlen(body), body);

    if (written > 0) {
        send_all(client_fd, response, (size_t)written);
    }
}

static void send_404(int client_fd) {
    send_simple_response(client_fd, "404 Not Found", "text/html",
                         "<html><body><h1>404 Not Found</h1></body></html>");
}

static void send_400(int client_fd) {
    send_simple_response(client_fd, "400 Bad Request", "text/html",
                         "<html><body><h1>400 Bad Request</h1></body></html>");
}

static void send_405(int client_fd) {
    send_simple_response(client_fd, "405 Method Not Allowed", "text/html",
                         "<html><body><h1>405 Method Not Allowed</h1></body></html>");
}

static void send_homepage(int client_fd) {
    char html[HTML_SIZE];
    generate_directory_html("./shared", html, sizeof(html));
    send_simple_response(client_fd, "200 OK", "text/html", html);
}

static void send_file(int client_fd, const char *filename) {
    if (!is_safe_filename(filename)) {
        send_400(client_fd);
        return;
    }

    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "./shared/%s", filename);

    FILE *fp = fopen(filepath, "rb");
    if (fp == NULL) {
        send_404(client_fd);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(fp);
        send_500:
        send_simple_response(client_fd, "500 Internal Server Error", "text/html",
                             "<html><body><h1>500 Internal Server Error</h1></body></html>");
        return;
    }

    char header[1024];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/octet-stream\r\n"
                              "Content-Disposition: attachment; filename=\"%s\"\r\n"
                              "Content-Length: %ld\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              filename, file_size);

    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        fclose(fp);
        goto send_500;
    }

    if (send_all(client_fd, header, (size_t)header_len) < 0) {
        fclose(fp);
        return;
    }

    char buffer[BUFFER_SIZE];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (send_all(client_fd, buffer, n) < 0) {
            break;
        }
    }

    fclose(fp);
}

int main(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        exit(1);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(1);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        exit(1);
    }

    printf("Server running on http://localhost:%d\n", PORT);
    printf("On your phone, use your computer's LAN IP, not localhost.\n");

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        char request[BUFFER_SIZE];
        ssize_t received = recv(client_fd, request, sizeof(request) - 1, 0);
        if (received <= 0) {
            close(client_fd);
            continue;
        }
        request[received] = '\0';

        char method[16] = {0};
        char path[1024] = {0};

        if (sscanf(request, "%15s %1023s", method, path) != 2) {
            send_400(client_fd);
            close(client_fd);
            continue;
        }

        if (strcmp(method, "GET") != 0) {
            send_405(client_fd);
        } else if (strcmp(path, "/") == 0) {
            send_homepage(client_fd);
        } else if (strncmp(path, "/download?file=", 15) == 0) {
            char filename[512];
            if (url_decode(path + 15, filename, sizeof(filename)) == 0) {
                send_file(client_fd, filename);
            } else {
                send_400(client_fd);
            }
        } else {
            send_404(client_fd);
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}
