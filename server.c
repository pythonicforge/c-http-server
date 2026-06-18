#include <ctype.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define PORT 8080
#define MAX_REQUEST_SIZE (50 * 1024 * 1024)
#define MAX_HTML_SIZE 32768
#define MAX_FILE_SIZE (50 * 1024 * 1024)

static void send_all(int fd, const char *buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, buf + sent, len - sent, 0);
    if (n <= 0) {
      return;
    }
    sent += (size_t)n;
  }
}

static const char *find_bytes(const char *haystack, size_t hay_len,
                              const char *needle, size_t needle_len) {
  if (needle_len == 0 || hay_len < needle_len) {
    return NULL;
  }

  for (size_t i = 0; i + needle_len <= hay_len; i++) {
    if (memcmp(haystack + i, needle, needle_len) == 0) {
      return haystack + i;
    }
  }

  return NULL;
}

static void html_escape(const char *in, char *out, size_t out_sz) {
  size_t j = 0;
  for (size_t i = 0; in[i] != '\0' && j + 1 < out_sz; i++) {
    switch (in[i]) {
    case '&':
      if (j + 5 < out_sz) {
        memcpy(out + j, "&amp;", 5);
        j += 5;
      }
      break;
    case '<':
      if (j + 4 < out_sz) {
        memcpy(out + j, "&lt;", 4);
        j += 4;
      }
      break;
    case '>':
      if (j + 4 < out_sz) {
        memcpy(out + j, "&gt;", 4);
        j += 4;
      }
      break;
    case '"':
      if (j + 6 < out_sz) {
        memcpy(out + j, "&quot;", 6);
        j += 6;
      }
      break;
    case '\'':
      if (j + 5 < out_sz) {
        memcpy(out + j, "&#39;", 5);
        j += 5;
      }
      break;
    default:
      out[j++] = in[i];
      break;
    }
  }
  out[j] = '\0';
}

static void url_encode(const char *in, char *out, size_t out_sz) {
  static const char hex[] = "0123456789ABCDEF";
  size_t j = 0;

  for (size_t i = 0; in[i] != '\0' && j + 1 < out_sz; i++) {
    unsigned char c = (unsigned char)in[i];

    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out[j++] = (char)c;
    } else if (c == ' ' && j + 3 < out_sz) {
      out[j++] = '%';
      out[j++] = '2';
      out[j++] = '0';
    } else if (j + 3 < out_sz) {
      out[j++] = '%';
      out[j++] = hex[c >> 4];
      out[j++] = hex[c & 0x0F];
    } else {
      break;
    }
  }

  out[j] = '\0';
}

static void url_decode_inplace(char *s) {
  char *src = s;
  char *dst = s;

  while (*src) {
    if (*src == '%' && isxdigit((unsigned char)src[1]) &&
        isxdigit((unsigned char)src[2])) {
      char hex[3] = {src[1], src[2], '\0'};
      *dst++ = (char)strtol(hex, NULL, 16);
      src += 3;
    } else if (*src == '+') {
      *dst++ = ' ';
      src++;
    } else {
      *dst++ = *src++;
    }
  }

  *dst = '\0';
}

static void sanitize_filename(const char *in, char *out, size_t out_sz) {
  size_t j = 0;
  const char *base = strrchr(in, '/');
  const char *base2 = strrchr(in, '\\');
  if (base2 && (!base || base2 > base)) {
    base = base2;
  }
  base = base ? base + 1 : in;

  for (size_t i = 0; base[i] != '\0' && j + 1 < out_sz; i++) {
    unsigned char c = (unsigned char)base[i];
    if (isalnum(c) || c == '.' || c == '_' || c == '-') {
      out[j++] = (char)c;
    } else {
      out[j++] = '_';
    }
  }
  out[j] = '\0';

  if (out[0] == '\0') {
    snprintf(out, out_sz, "uploaded_file.bin");
  }
}

static size_t parse_content_length(const char *headers) {
  const char *p = strstr(headers, "Content-Length:");
  if (!p) {
    return 0;
  }

  p += strlen("Content-Length:");
  while (*p == ' ' || *p == '\t') {
    p++;
  }

  return (size_t)strtoull(p, NULL, 10);
}

static int read_http_request(int client_fd, char **out_buf, size_t *out_len) {
  size_t cap = 8192;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    return -1;
  }

  size_t header_end = 0;
  size_t content_length = 0;
  int have_headers = 0;

  while (1) {
    char tmp[4096];
    ssize_t n = recv(client_fd, tmp, sizeof(tmp), 0);
    if (n < 0) {
      free(buf);
      return -1;
    }
    if (n == 0) {
      break;
    }

    if (len + (size_t)n > cap) {
      size_t new_cap = cap * 2;
      while (new_cap < len + (size_t)n) {
        new_cap *= 2;
      }

      char *new_buf = realloc(buf, new_cap);
      if (!new_buf) {
        free(buf);
        return -1;
      }
      buf = new_buf;
      cap = new_cap;
    }

    memcpy(buf + len, tmp, (size_t)n);
    len += (size_t)n;

    if (!have_headers) {
      const char *end = find_bytes(buf, len, "\r\n\r\n", 4);
      if (end) {
        header_end = (size_t)(end - buf) + 4;
        content_length = parse_content_length(buf);
        have_headers = 1;

        if (content_length == 0) {
          break;
        }
        if (len >= header_end + content_length) {
          break;
        }
      }
    } else {
      if (len >= header_end + content_length) {
        break;
      }
    }

    if (len > MAX_REQUEST_SIZE) {
      free(buf);
      return -1;
    }
  }

  *out_buf = buf;
  *out_len = len;
  return 0;
}

static void send_simple_response(int client_fd, const char *status,
                                 const char *content_type, const char *body) {
  char header[1024];
  size_t body_len = strlen(body);

  int n = snprintf(header, sizeof(header),
                   "HTTP/1.1 %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   status, content_type, body_len);

  if (n > 0) {
    send_all(client_fd, header, (size_t)n);
  }
  send_all(client_fd, body, body_len);
}

static void send_redirect(int client_fd, const char *location) {
  char header[512];
  int n = snprintf(header, sizeof(header),
                   "HTTP/1.1 303 See Other\r\n"
                   "Location: %s\r\n"
                   "Content-Length: 0\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   location);

  if (n > 0) {
    send_all(client_fd, header, (size_t)n);
  }
}

static void generate_home_html(char *html, size_t max_size) {
  DIR *dir = opendir("./shared");
  if (!dir) {
    snprintf(html, max_size,
             "<html><body><h1>Shared Files</h1>"
             "<p>Could not open ./shared</p>"
             "<form method=\"POST\" action=\"/upload\" "
             "enctype=\"multipart/form-data\">"
             "<input type=\"file\" name=\"file\">"
             "<button type=\"submit\">Upload</button>"
             "</form></body></html>");
    return;
  }

  size_t used = 0;
  used += snprintf(html + used, max_size - used,
                   "<html><body><h1>Shared Files</h1>"
                   "<form method=\"POST\" action=\"/upload\" "
                   "enctype=\"multipart/form-data\">"
                   "<input type=\"file\" name=\"file\">"
                   "<button type=\"submit\">Upload</button>"
                   "</form>"
                   "<p>Click a file to download it.</p><ul>");

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char escaped[512];
    char encoded[1024];
    html_escape(entry->d_name, escaped, sizeof(escaped));
    url_encode(entry->d_name, encoded, sizeof(encoded));

    if (used < max_size) {
      used += snprintf(html + used, max_size - used,
                       "<li><a href=\"/download?file=%s\">%s</a></li>", encoded,
                       escaped);
    }
  }

  if (used < max_size) {
    snprintf(html + used, max_size - used, "</ul></body></html>");
  }

  closedir(dir);
}

static void handle_download(int client_fd, const char *path) {
  const char *file_param = strstr(path, "/download?file=");
  if (!file_param) {
    send_simple_response(client_fd, "400 Bad Request", "text/plain",
                         "Bad download request");
    return;
  }

  file_param += strlen("/download?file=");

  char filename[256];
  snprintf(filename, sizeof(filename), "%s", file_param);
  url_decode_inplace(filename);

  char safe_name[256];
  sanitize_filename(filename, safe_name, sizeof(safe_name));

  char filepath[512];
  snprintf(filepath, sizeof(filepath), "./shared/%s", safe_name);

  FILE *fp = fopen(filepath, "rb");
  if (!fp) {
    send_simple_response(client_fd, "404 Not Found", "text/plain",
                         "File not found");
    return;
  }

  fseek(fp, 0, SEEK_END);
  long fsize = ftell(fp);
  rewind(fp);

  if (fsize < 0) {
    fclose(fp);
    send_simple_response(client_fd, "500 Internal Server Error", "text/plain",
                         "Could not read file");
    return;
  }

  char header[1024];
  int n = snprintf(header, sizeof(header),
                   "HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Content-Disposition: attachment; filename=\"%s\"\r\n"
                   "Content-Length: %ld\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   safe_name, fsize);

  if (n > 0) {
    send_all(client_fd, header, (size_t)n);
  }

  char chunk[4096];
  size_t bytes;
  while ((bytes = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
    send_all(client_fd, chunk, bytes);
  }

  fclose(fp);
}

static void handle_upload(int client_fd, const char *headers, const char *body,
                          size_t body_len) {
  const char *ct = strstr(headers, "Content-Type:");
  if (!ct || !strstr(ct, "multipart/form-data")) {
    send_simple_response(client_fd, "400 Bad Request", "text/plain",
                         "Expected multipart/form-data");
    return;
  }

  const char *boundary_pos = strstr(ct, "boundary=");
  if (!boundary_pos) {
    send_simple_response(client_fd, "400 Bad Request", "text/plain",
                         "Missing boundary");
    return;
  }

  boundary_pos += strlen("boundary=");

  char boundary[256];
  size_t bi = 0;
  while (boundary_pos[bi] && boundary_pos[bi] != '\r' &&
         boundary_pos[bi] != '\n' && boundary_pos[bi] != ';' &&
         bi + 1 < sizeof(boundary)) {
    boundary[bi] = boundary_pos[bi];
    bi++;
  }
  boundary[bi] = '\0';

  char first_boundary[300];
  char closing_boundary[300];
  snprintf(first_boundary, sizeof(first_boundary), "--%s", boundary);
  snprintf(closing_boundary, sizeof(closing_boundary), "\r\n--%s", boundary);

  const char *part =
      find_bytes(body, body_len, first_boundary, strlen(first_boundary));
  if (!part) {
    send_simple_response(client_fd, "400 Bad Request", "text/plain",
                         "Boundary not found");
    return;
  }

  part += strlen(first_boundary);
  size_t remaining = (size_t)((body + body_len) - part);

  if (remaining >= 2 && part[0] == '\r' && part[1] == '\n') {
    part += 2;
    remaining -= 2;
  }

  const char *part_headers_end = find_bytes(part, remaining, "\r\n\r\n", 4);
  if (!part_headers_end) {
    send_simple_response(client_fd, "400 Bad Request", "text/plain",
                         "Invalid multipart body");
    return;
  }

  const char *fn = strstr(part, "filename=\"");
  char filename[256] = "uploaded_file.bin";

  if (fn && fn < part_headers_end) {
    fn += strlen("filename=\"");
    size_t i = 0;
    while (fn[i] && fn[i] != '"' && i + 1 < sizeof(filename)) {
      filename[i] = fn[i];
      i++;
    }
    filename[i] = '\0';
  }

  char safe_name[256];
  sanitize_filename(filename, safe_name, sizeof(safe_name));

  const char *file_data = part_headers_end + 4;
  size_t file_data_len = (size_t)((body + body_len) - file_data);

  const char *end = find_bytes(file_data, file_data_len, closing_boundary,
                               strlen(closing_boundary));
  if (!end) {
    send_simple_response(client_fd, "400 Bad Request", "text/plain",
                         "Could not find file end");
    return;
  }

  size_t file_size = (size_t)(end - file_data);
  if (file_size >= 2 && file_data[file_size - 2] == '\r' &&
      file_data[file_size - 1] == '\n') {
    file_size -= 2;
  }

  if (file_size > MAX_FILE_SIZE) {
    send_simple_response(client_fd, "413 Payload Too Large", "text/plain",
                         "File too large");
    return;
  }

  mkdir("./shared", 0755);

  char filepath[512];
  snprintf(filepath, sizeof(filepath), "./shared/%s", safe_name);

  FILE *fp = fopen(filepath, "wb");
  if (!fp) {
    send_simple_response(client_fd, "500 Internal Server Error", "text/plain",
                         "Could not save file");
    return;
  }

  fwrite(file_data, 1, file_size, fp);
  fclose(fp);

  send_redirect(client_fd, "/");
}

int main(void) {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket");
    return 1;
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(PORT);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("bind");
    close(server_fd);
    return 1;
  }

  if (listen(server_fd, 10) < 0) {
    perror("listen");
    close(server_fd);
    return 1;
  }

  printf("Listening on http://0.0.0.0:%d\n", PORT);

  while (1) {
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
      perror("accept");
      continue;
    }

    char *request = NULL;
    size_t request_len = 0;

    if (read_http_request(client_fd, &request, &request_len) < 0 || !request) {
      free(request);
      close(client_fd);
      continue;
    }

    const char *header_end = find_bytes(request, request_len, "\r\n\r\n", 4);
    if (!header_end) {
      send_simple_response(client_fd, "400 Bad Request", "text/plain",
                           "Bad request");
      free(request);
      close(client_fd);
      continue;
    }

    size_t headers_len = (size_t)(header_end - request) + 4;
    size_t body_len = request_len - headers_len;
    const char *body = request + headers_len;

    char *headers = malloc(headers_len + 1);
    if (!headers) {
      free(request);
      close(client_fd);
      continue;
    }
    memcpy(headers, request, headers_len);
    headers[headers_len] = '\0';

    char method[16] = {0};
    char path[1024] = {0};
    char version[16] = {0};
    sscanf(request, "%15s %1023s %15s", method, path, version);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
      char html[MAX_HTML_SIZE];
      generate_home_html(html, sizeof(html));
      send_simple_response(client_fd, "200 OK", "text/html; charset=utf-8",
                           html);
    } else if (strcmp(method, "GET") == 0 &&
               strncmp(path, "/download?file=", 15) == 0) {
      handle_download(client_fd, path);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/upload") == 0) {
      handle_upload(client_fd, headers, body, body_len);
    } else {
      send_simple_response(client_fd, "404 Not Found", "text/plain",
                           "Not found");
    }

    free(headers);
    free(request);
    close(client_fd);
  }

  close(server_fd);
  return 0;
}
