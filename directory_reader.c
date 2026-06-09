#include<stdio.h>
#include<string.h>
#include<dirent.h>

void generate_directory_html(const char *directory, char *html, int max_size){
  DIR *dir = opendir(directory);

  if(dir == NULL) {
    snprintf(html, max_size, "<html><body><p>Could not open directory</p></body></html>");
    return;
  }       

  struct dirent *entry;

  snprintf(html, max_size, "<html><body><h1>Shared Files</h1><ul>");

  while((entry = readdir(dir)) != NULL) {
    if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0){
      continue;
    }

    char temp[512];

    snprintf(temp, sizeof(temp), "<li>%s</li>", entry->d_name);

    strncat(html, temp, max_size - strlen(html) - 1);
  }

  strncat(html, "</ul></body></html>", max_size-strlen(html) - 1);

  closedir(dir);
}
