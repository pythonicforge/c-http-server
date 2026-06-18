#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PORT 8080
#define SHARED_DIR "./shared"
#define MAX_HEADER_SIZE (64 * 1024)
#define MAX_HTML_SIZE 65536
#define MAX_UPLOAD_SIZE (1ULL << 60) /* effectively no fixed cap for normal use */

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

static bool get_query_param(const char *path, const char *key, char *out, size_t out_sz) {
  const char *q = strchr(path, '?');
  if (!q) {
    return false;
  }
  q++;

  size_t key_len = strlen(key);
  while (*q) {
    const char *amp = strchr(q, '&');
    size_t pair_len = amp ? (size_t)(amp - q) : strlen(q);
    const char *eq = memchr(q, '=', pair_len);

    if (eq && (size_t)(eq - q) == key_len && memcmp(q, key, key_len) == 0) {
      size_t val_len = pair_len - (size_t)(eq - q) - 1;
      if (val_len >= out_sz) {
        val_len = out_sz - 1;
      }
      memcpy(out, eq + 1, val_len);
      out[val_len] = '\0';
      url_decode_inplace(out);
      return true;
    }

    if (!amp) {
      break;
    }
    q = amp + 1;
  }

  return false;
}

static bool get_content_length(const char *headers, unsigned long long *out) {
  const char *p = strstr(headers, "Content-Length:");
  if (!p) {
    return false;
  }

  p += strlen("Content-Length:");
  while (*p == ' ' || *p == '\t') {
    p++;
  }

  *out = strtoull(p, NULL, 10);
  return true;
}

static int read_http_headers(int client_fd, char **out_buf, size_t *out_len) {
  size_t cap = 8192;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    return -1;
  }

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

    if (len > MAX_HEADER_SIZE) {
      free(buf);
      return -1;
    }

    const char *end = find_bytes(buf, len, "\r\n\r\n", 4);
    if (end) {
      *out_buf = buf;
      *out_len = len;
      return 0;
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
  format_size((off_t)MAX_UPLOAD_SIZE, upload_limit, sizeof(upload_limit));

  size_t used = 0;
  appendf(html, max_size, &used,
          "<html><body>"
          "<h1>ShareIt</h1>"
          "<p><b>Host:</b> %s &nbsp; <b>Port:</b> %d</p>"
          "<p><b>Files:</b> %zu &nbsp; <b>Used:</b> %s &nbsp;"
          "<div>"
          "<input type=\"file\" id=\"fileInput\" multiple>"
          "<button id=\"uploadBtn\" type=\"button\">Upload selected files</button>"
          "<p id=\"selectedInfo\">No file selected</p>"
          "<pre id=\"statusBox\">Idle</pre>"
          "</div>"
          "<ul>",
          host, PORT, file_count, total_used);

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
              "<li><a href=\"%s\">%s</a> &nbsp; (%s, %s) &nbsp; "
              "<a href=\"%s\" onclick=\"return confirm('Delete this file?')\">delete</a></li>",
              download_url, escaped_name, size_str, time_str, delete_url);
    }
  }

  appendf(html, max_size, &used,
          "</ul>"
          "<script>"
          "const input = document.getElementById('fileInput');"
          "const info = document.getElementById('selectedInfo');"
          "const statusBox = document.getElementById('statusBox');"
          "const uploadBtn = document.getElementById('uploadBtn');"
          "input.addEventListener('change', () => {"
          "  let total = 0;"
          "  for (const f of input.files) total += f.size;"
          "  if (input.files.length === 0) {"
          "    info.textContent = 'No file selected';"
          "  } else {"
          "    info.textContent = `${input.files.length} file(s) selected, ${(total / 1024 / 1024).toFixed(2)} MB total`;"
          "  }"
          "});"
          "uploadBtn.addEventListener('click', async () => {"
          "  const files = Array.from(input.files);"
          "  if (files.length === 0) {"
          "    statusBox.textContent = 'Pick at least one file first.';"
          "    return;"
          "  }"
          "  uploadBtn.disabled = true;"
          "  try {"
          "    for (let i = 0; i < files.length; i++) {"
          "      const f = files[i];"
          "      statusBox.textContent = `Uploading ${i + 1}/${files.length}: ${f.name}`;"
          "      const res = await fetch(`/upload?name=${encodeURIComponent(f.name)}`, {"
          "        method: 'POST',"
          "        headers: { 'Content-Type': 'application/octet-stream' },"
          "        body: f"
          "      });"
          "      if (!res.ok) {"
          "        throw new Error(await res.text());"
          "      }"
          "    }"
          "    statusBox.textContent = 'Upload complete. Refreshing...';"
          "    window.location.reload();"
          "  } catch (err) {"
          "    statusBox.textContent = `Upload failed: ${err.message}`;"
          "    uploadBtn.disabled = false;"
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
  char filename[MAX_NAME_LEN] = "downloaded_file";
  if (!get_query_param(path, "file", filename, sizeof(filename))) {
    send_simple_response(client_fd, "400 Bad Request", "text/plain",
                         "Bad download request");
    return;
  }

  char safe_name[MAX_NAME_LEN];
  sanitize_filename(filename, safe_name, sizeof(safe_name));

  char filepath[MAX_PATH_LEN];
  snprintf(filepath, sizeof(filepath), "%s/%s", SHARED_DIR, safe_name);

  struct stat st;
  if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode)) {
    send_simple_response(client_fd, "404 Not Found", "text/plain",
                         "File not found");
    return;
  }

  FILE *fp = fopen(filepath, "rb");
  if (!fp) {
    send_simple_response(client_fd, "404 Not Found", "text/plain",
                         "File not found");
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
                   safe_name, (long)st.st_size);

  if (n > 0) {
    send_all(client_fd, header, (size_t)n);
  }

  char chunk[8192];
  size_t bytes;
  while ((bytes = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
    send_all(client_fd, chunk, bytes);
  }

  fclose(fp);
}

static void handle_delete(int client_fd, const char *path) {
  char filename[MAX_NAME_LEN] = "deleted_file";
  if (!get_query_param(path, "file", filename, sizeof(filename))) {
    send_simple_response(client_fd, "400 Bad Request", "text/plain",
                         "Bad delete request");
    return;
  }

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

static void handle_raw_upload(int client_fd, const char *path, const char *headers,
                              const char *body, size_t body_len) {
  unsigned long long content_length = 0;
  if (!get_content_length(headers, &content_length)) {
    send_simple_response(client_fd, "411 Length Required", "text/plain",
                         "Content-Length is required");
    return;
  }

  if (content_length > MAX_UPLOAD_SIZE) {
    send_simple_response(client_fd, "413 Payload Too Large", "text/plain",
                         "File too large");
    return;
  }

  char name[MAX_NAME_LEN] = "uploaded_file.bin";
  if (!get_query_param(path, "name", name, sizeof(name))) {
    snprintf(name, sizeof(name), "uploaded_file.bin");
  }

  char safe_name[MAX_NAME_LEN];
  sanitize_filename(name, safe_name, sizeof(safe_name));

  mkdir(SHARED_DIR, 0755);

  char final_path[MAX_PATH_LEN];
  build_unique_filepath(SHARED_DIR, safe_name, final_path, sizeof(final_path));

  FILE *fp = fopen(final_path, "wb");
  if (!fp) {
    send_simple_response(client_fd, "500 Internal Server Error", "text/plain",
                         "Could not save file");
    return;
  }

  size_t initial = body_len;
  if ((unsigned long long)initial > content_length) {
    initial = (size_t)content_length;
  }

  if (initial > 0) {
    if (fwrite(body, 1, initial, fp) != initial) {
      fclose(fp);
      remove(final_path);
      send_simple_response(client_fd, "500 Internal Server Error", "text/plain",
                           "Could not save file");
      return;
    }
  }

  unsigned long long remaining = content_length - (unsigned long long)initial;
  char chunk[8192];

  while (remaining > 0) {
    size_t want = remaining < sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
    ssize_t n = recv(client_fd, chunk, want, 0);
    if (n <= 0) {
      fclose(fp);
      remove(final_path);
      send_simple_response(client_fd, "400 Bad Request", "text/plain",
                           "Upload interrupted");
      return;
    }

    if (fwrite(chunk, 1, (size_t)n, fp) != (size_t)n) {
      fclose(fp);
      remove(final_path);
      send_simple_response(client_fd, "500 Internal Server Error", "text/plain",
                           "Could not save file");
      return;
    }

    remaining -= (unsigned long long)n;
  }

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
    if (read_http_headers(client_fd, &request, &request_len) < 0 || !request) {
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
      send_simple_response(client_fd, "200 OK", "text/html; charset=utf-8", html);
    } else if (strcmp(method, "GET") == 0 &&
               strncmp(path, "/download?file=", 15) == 0) {
      handle_download(client_fd, path);
    } else if (strcmp(method, "GET") == 0 &&
               strncmp(path, "/delete?file=", 13) == 0) {
      handle_delete(client_fd, path);
    } else if ((strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) &&
               strncmp(path, "/upload", 7) == 0) {
      handle_raw_upload(client_fd, path, headers, body, body_len);
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
