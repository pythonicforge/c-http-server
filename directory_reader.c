#include "directory_reader.h"
#include <ctype.h>
#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void html_escape(const char *src, char *dst, size_t max_size) {
  size_t j = 0;

  for (size_t i = 0; src[i] != '\0' && j + 1 < max_size; i++) {
    const char *rep = NULL;
    switch (src[i]) {
    case '&':
      rep = "&amp;";
      break;
    case '<':
      rep = "&lt;";
      break;
    case '>':
      rep = "&gt;";
      break;
    case '"':
      rep = "&quot;";
      break;
    case '\'':
      rep = "&#39;";
      break;
    default:
      break;
    }

    if (rep != NULL) {
      size_t rlen = strlen(rep);
      if (j + rlen >= max_size) {
        break;
      }
      memcpy(dst + j, rep, rlen);
      j += rlen;
    } else {
      dst[j++] = src[i];
    }
  }
  dst[j] = '\0';
}

static void url_encode(const char *src, char *dst, size_t max_size) {
  static const char hex[] = "0123456789ABCDEF";
  size_t j = 0;

  for (size_t i = 0; src[i] != '\0' && j + 1 < max_size; i++) {
    unsigned char c = (unsigned char)src[i];

    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      dst[j++] = (char)c;
    } else if (c == ' ') {
      if (j + 1 >= max_size) {
        break;
      }
      dst[j++] = '+';
    } else {
      if (j + 3 >= max_size) {
        break;
      }
      dst[j++] = '%';
      dst[j++] = hex[c >> 4];
      dst[j++] = hex[c & 15];
    }
  }

  dst[j] = '\0';
}

void generate_directory_html(const char *directory, char *html,
                             size_t max_size) {
  DIR *dir = opendir(directory);

  if (dir == NULL) {
    snprintf(html, max_size,
             "<html><body><h1>Shared Files</h1>"
             "<p>Could not open directory.</p>"
             "</body></html>");
    return;
  }

  size_t used = snprintf(html, max_size,
                         "<html><body><h1>Shared Files</h1>"
                         "<p>Click a file to download it.</p><ul>");

  if (used >= max_size) {
    closedir(dir);
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char safe_name[512];
    char encoded_name[1024];
    char temp[2048];

    html_escape(entry->d_name, safe_name, sizeof(safe_name));
    url_encode(entry->d_name, encoded_name, sizeof(encoded_name));

    snprintf(temp, sizeof(temp),
             "<li><a href=\"/download?file=%s\">%s</a></li>", encoded_name,
             safe_name);

    size_t remaining = max_size - strlen(html) - 1;
    if (remaining == 0) {
      break;
    }
    strncat(html, temp, remaining);
  }

  strncat(html, "</ul></body></html>", max_size - strlen(html) - 1);
  closedir(dir);
}
