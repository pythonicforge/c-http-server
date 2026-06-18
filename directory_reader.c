#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

static void url_encode(const char *in, char *out, size_t out_sz) {
  const char *hex = "0123456789ABCDEF";
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

void generate_directory_html(const char *directory, char *html, int max_size) {
  DIR *dir = opendir(directory);

  if (dir == NULL) {
    snprintf(html, max_size,
             "<html><body><p>Could not open directory</p></body></html>");
    return;
  }

  struct dirent *entry;

  int written = snprintf(html, max_size,
                         "<html><body>"
                         "<h1>Shared Files</h1>"
                         "<form method=\"POST\" action=\"/upload\" "
                         "enctype=\"multipart/form-data\">"
                         "<input type=\"file\" name=\"file\">"
                         "<button type=\"submit\">Upload</button>"
                         "</form>"
                         "<p>Click a file to download it.</p><ul>");

  if (written < 0 || written >= max_size) {
    closedir(dir);
    return;
  }

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char encoded[1024];
    url_encode(entry->d_name, encoded, sizeof(encoded));

    char temp[1536];
    snprintf(temp, sizeof(temp),
             "<li><a href=\"/download?file=%s\">%s</a></li>", encoded,
             entry->d_name);

    size_t current_len = strlen(html);
    if (current_len < (size_t)max_size - 1) {
      strncat(html, temp, max_size - current_len - 1);
    }
  }

  strncat(html, "</ul></body></html>", max_size - strlen(html) - 1);

  closedir(dir);
}
