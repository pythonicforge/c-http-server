#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PORT 8080
#define SHARED_DIR "./shared"
#define MAX_REQUEST_SIZE (100 * 1024 * 1024)
#define MAX_HTML_SIZE 65536
#define MAX_FILE_SIZE (100 * 1024 * 1024)

#define MAX_NAME_LEN 256
#define MAX_PATH_LEN 512

typedef struct {
  char name[MAX_NAME_LEN];
  off_t size;
  time_t mtime;
} file_info;

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

static void appendf(char *dst, size_t dst_sz, size_t *used, const char *fmt, ...) {
  if (*used >= dst_sz) {
    return;
  }

  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(dst + *used, dst_sz - *used, fmt, ap);
  va_end(ap);

  if (n < 0) {
    return;
  }

  size_t written = (size_t)n;
  if (written >= dst_sz - *used) {
    *used = dst_sz - 1;
  } else {
    *used += written;
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

static void format_size(off_t bytes, char *out, size_t out_sz) {
  double v = (double)bytes;
  const char *unit = "B";

  if (v >= 1024.0) {
    v /= 1024.0;
    unit = "KB";
  }
  if (v >= 1024.0) {
    v /= 1024.0;
    unit = "MB";
  }
  if (v >= 1024.0) {
    v /= 1024.0;
    unit = "GB";
  }

  if (strcmp(unit, "B") == 0) {
    snprintf(out, out_sz, "%.0f %s", v, unit);
  } else {
    snprintf(out, out_sz, "%.1f %s", v, unit);
  }
}

static void format_time(time_t t, char *out, size_t out_sz) {
  struct tm tm_buf;
  struct tm *tm = localtime_r(&t, &tm_buf);
  if (!tm) {
    snprintf(out, out_sz, "unknown");
    return;
  }
  strftime(out, out_sz, "%Y-%m-%d %H:%M", tm);
}

static int compare_files_newest_first(const void *a, const void *b) {
  const file_info *fa = (const file_info *)a;
  const file_info *fb = (const file_info *)b;

  if (fa->mtime > fb->mtime) {
    return -1;
  }
  if (fa->mtime < fb->mtime) {
    return 1;
  }
  return strcmp(fa->name, fb->name);
}

static int collect_files(const char *directory, file_info **out_files,
                         size_t *out_count, size_t *out_total_bytes) {
  DIR *dir = opendir(directory);
  if (!dir) {
    *out_files = NULL;
    *out_count = 0;
    *out_total_bytes = 0;
    return -1;
  }

  size_t cap = 16;
  size_t count = 0;
  size_t total_bytes = 0;
  file_info *files = malloc(cap * sizeof(*files));
  if (!files) {
    closedir(dir);
    return -1;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);

    struct stat st;
    if (stat(path, &st) != 0) {
      continue;
    }

    if (!S_ISREG(st.st_mode)) {
      continue;
    }

    if (count == cap) {
      cap *= 2;
      file_info *new_files = realloc(files, cap * sizeof(*files));
      if (!new_files) {
        free(files);
        closedir(dir);
        return -1;
      }
      files = new_files;
    }

    snprintf(files[count].name, sizeof(files[count].name), "%s", entry->d_name);
    files[count].size = st.st_size;
    files[count].mtime = st.st_mtime;
    total_bytes += (size_t)st.st_size;
    count++;
  }

  closedir(dir);

  qsort(files, count, sizeof(*files), compare_files_newest_first);

  *out_files = files;
  *out_count = count;
  *out_total_bytes = total_bytes;
  return 0;
}

static void generate_home_html(char *html, size_t max_size) {
  file_info *files = NULL;
  size_t file_count = 0;
  size_t total_bytes = 0;
  bool opened = collect_files(SHARED_DIR, &files, &file_count, &total_bytes) == 0;

  char host[256] = "this device";
  gethostname(host, sizeof(host));
  host[sizeof(host) - 1] = '\0';

  char total_used[64];
  char upload_limit[64];
  format_size((off_t)total_bytes, total_used, sizeof(total_used));
  format_size((off_t)MAX_FILE_SIZE, upload_limit, sizeof(upload_limit));

  size_t used = 0;
  appendf(html, max_size, &used,
          "<html><body>"
          "<h1>Shared Files</h1>"
          "<p><b>Host:</b> %s &nbsp; <b>Port:</b> %d</p>"
          "<p><b>Files:</b> %zu &nbsp; <b>Used:</b> %s &nbsp; <b>Max upload per request:</b> %s</p>"
          "<p>Select one or more files. The page shows the total selected size before upload.</p>"
          "<form method=\"POST\" action=\"/upload\" enctype=\"multipart/form-data\">"
          "<input type=\"file\" name=\"file\" id=\"fileInput\" multiple>"
          "<button type=\"submit\">Upload</button>"
          "<p id=\"selectedInfo\">No file selected</p>"
          "</form>"
          "<ul>",
          host, PORT, file_count, total_used, upload_limit);

  if (!opened) {
    appendf(html, max_size, &used,
            "<li>Could not open <code>%s</code></li>", SHARED_DIR);
  } else if (file_count == 0) {
    appendf(html, max_size, &used, "<li>No files yet.</li>");
  } else {
    for (size_t i = 0; i < file_count; i++) {
      char escaped_name[512];
      char encoded_name[1024];
      char size_str[64];
      char time_str[64];
      char delete_url[1100];
      char download_url[1100];

      html_escape(files[i].name, escaped_name, sizeof(escaped_name));
      url_encode(files[i].name, encoded_name, sizeof(encoded_name));
      format_size(files[i].size, size_str, sizeof(size_str));
      format_time(files[i].mtime, time_str, sizeof(time_str));
      snprintf(download_url, sizeof(download_url), "/download?file=%s", encoded_name);
      snprintf(delete_url, sizeof(delete_url), "/delete?file=%s", encoded_name);

      appendf(html, max_size, &used,
              "<li><a href=\"%s\">%s</a> &nbsp; (%s, %s) &nbsp; <a href=\"%s\">delete</a></li>",
              download_url, escaped_name, size_str, time_str, delete_url);
    }
  }

  appendf(html, max_size, &used,
          "</ul>"
          "<script>"
          "const input = document.getElementById('fileInput');"
          "const info = document.getElementById('selectedInfo');"
          "input.addEventListener('change', () => {"
          "  let total = 0;"
          "  for (const f of input.files) total += f.size;"
          "  if (input.files.length === 0) {"
          "    info.textContent = 'No file selected';"
          "  } else {"
          "    info.textContent = `${input.files.length} file(s) selected, ${(total / 1024 / 1024).toFixed(2)} MB total`;"
          "  }"
          "});"
          "</script>"
          "</body></html>");

  free(files);
}

static void build_unique_filepath(const char *directory, const char *name,
                                  char *out, size_t out_sz) {
  char base[MAX_NAME_LEN];
  char ext[MAX_NAME_LEN];
  const char *dot = strrchr(name, '.');

  if (dot && dot != name) {
    size_t base_len = (size_t)(dot - name);
    if (base_len >= sizeof(base)) {
      base_len = sizeof(base) - 1;
    }
    memcpy(base, name, base_len);
    base[base_len] = '\0';
    snprintf(ext, sizeof(ext), "%s", dot);
  } else {
    snprintf(base, sizeof(base), "%s", name);
    ext[0] = '\0';
  }

  snprintf(out, out_sz, "%s/%s%s", directory, base, ext);
  if (access(out, F_OK) != 0) {
    return;
  }

  for (int i = 1; i < 10000; i++) {
    snprintf(out, out_sz, "%s/%s_%d%s", directory, base, i, ext);
    if (access(out, F_OK) != 0) {
      return;
    }
  }

  snprintf(out, out_sz, "%s/%s_%ld%s", directory, base, (long)time(NULL), ext);
}

static void handle_download(int client_fd, const char *path) {
  const char *file_param = strstr(path, "/download?file=");
  if (!file_param) {
    send_simple_response(client_fd, "400 Bad Request", "text/plain",
                         "Bad download request");
    return;
  }

  file_param += strlen("/download?file=");

  char filename[MAX_NAME_LEN];
  snprintf(filename, sizeof(filename), "%s", file_param);
  url_decode_inplace(filename);

  char safe_name[MAX_NAME_LEN];
  sanitize_filename(filename, safe_name, sizeof(safe_name));

  char filepath[MAX_PATH_LEN];
  snprintf(filepath, sizeof(filepath), "%s/%s", SHARED_DIR, safe_name);

  FILE *fp = fopen(filepath, "rb");
  if (!fp) {
    send_simple_response(client_fd, "404 Not Found", "text/plain",
                         "File not found");
    return;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    send_simple_response(client_fd, "500 Internal Server Error", "text/plain",
                         "Could not read file");
    return;
  }

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

static void handle_delete(int client_fd, const char *path) {
  const char *file_param = strstr(path, "/delete?file=");
  if (!file_param) {
    send_simple_response(client_fd, "400 Bad Request", "text/plain",
                         "Bad delete request");
    return;
  }

  file_param += strlen("/delete?file=");

  char filename[MAX_NAME_LEN];
  snprintf(filename, sizeof(filename), "%s", file_param);
  url_decode_inplace(filename);

  char safe_name[MAX_NAME_LEN];
  sanitize_filename(filename, safe_name, sizeof(safe_name));

  char filepath[MAX_PATH_LEN];
  snprintf(filepath, sizeof(filepath), "%s/%s", SHARED_DIR, safe_name);

  if (remove(filepath) != 0) {
    send_simple_response(client_fd, "404 Not Found", "text/plain",
                         "File not found or could not be deleted");
    return;
  }

  send_redirect(client_fd, "/");
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
  char next_boundary[300];
  snprintf(first_boundary, sizeof(first_boundary), "--%s", boundary);
  snprintf(next_boundary, sizeof(next_boundary), "\r\n--%s", boundary);

  const char *cursor = body;
  size_t cursor_len = body_len;
  int saved_files = 0;

  mkdir(SHARED_DIR, 0755);

  while (1) {
    const char *part = find_bytes(cursor, cursor_len, first_boundary,
                                  strlen(first_boundary));
    if (!part) {
      break;
    }

    part += strlen(first_boundary);
    size_t remaining = (size_t)((body + body_len) - part);

    if (remaining >= 2 && part[0] == '-' && part[1] == '-') {
      break;
    }

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

    const char *filename_pos = find_bytes(part, (size_t)(part_headers_end - part),
                                          "filename=\"", 10);

    const char *file_data = part_headers_end + 4;
    size_t file_data_remaining = (size_t)((body + body_len) - file_data);
    const char *boundary_hit = find_bytes(file_data, file_data_remaining,
                                          next_boundary, strlen(next_boundary));
    if (!boundary_hit) {
      send_simple_response(client_fd, "400 Bad Request", "text/plain",
                           "Could not find file end");
      return;
    }

    size_t file_size = (size_t)(boundary_hit - file_data);
    if (file_size >= 2 && file_data[file_size - 2] == '\r' &&
        file_data[file_size - 1] == '\n') {
      file_size -= 2;
    }

    if (filename_pos && filename_pos < part_headers_end) {
      filename_pos += strlen("filename=\"");
      char filename[MAX_NAME_LEN] = {0};
      size_t i = 0;
      while (filename_pos[i] && filename_pos[i] != '"' && i + 1 < sizeof(filename)) {
        filename[i] = filename_pos[i];
        i++;
      }
      filename[i] = '\0';

      char safe_name[MAX_NAME_LEN];
      sanitize_filename(filename, safe_name, sizeof(safe_name));

      if (file_size == 0) {
        cursor = boundary_hit + 2;
        cursor_len = (size_t)((body + body_len) - cursor);
        continue;
      }

      if (file_size > MAX_FILE_SIZE) {
        send_simple_response(client_fd, "413 Payload Too Large", "text/plain",
                             "File too large");
        return;
      }

      char final_path[MAX_PATH_LEN];
      build_unique_filepath(SHARED_DIR, safe_name, final_path, sizeof(final_path));

      FILE *fp = fopen(final_path, "wb");
      if (!fp) {
        send_simple_response(client_fd, "500 Internal Server Error", "text/plain",
                             "Could not save file");
        return;
      }

      fwrite(file_data, 1, file_size, fp);
      fclose(fp);
      saved_files++;
    }

    cursor = boundary_hit + 2;
    cursor_len = (size_t)((body + body_len) - cursor);
  }

  if (saved_files == 0) {
    send_simple_response(client_fd, "400 Bad Request", "text/plain",
                         "No files were uploaded");
    return;
  }

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

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
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
  mkdir(SHARED_DIR, 0755);

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
    } else if (strcmp(method, "GET") == 0 &&
               strncmp(path, "/delete?file=", 13) == 0) {
      handle_delete(client_fd, path);
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
