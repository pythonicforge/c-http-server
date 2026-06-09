#include<stdio.h>
#include<sys/socket.h>
#include<stdlib.h>
#include<unistd.h>
#include<netinet/in.h>
#include<string.h>
#include "directory_reader.h"

int main(){
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  
  if(server_fd < 0){
    perror("Socket error");
    exit(1);
  }
  
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(8080);
  server_addr.sin_addr.s_addr = INADDR_ANY;
  
  int bind_result = bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
  if(bind_result < 0){
    perror("Bind error\n");
    exit(1);
  }

  int listen_result = listen(server_fd, 10);

  if(listen_result < 0){
    perror("Listen error");
    exit(1);
  }

  printf("Server running on http://localhost:8080\n");

  while(1){
    int client_fd = accept(server_fd, NULL, NULL);

    if(client_fd < 0){
      perror("Accept error");
      continue;
    }

    char buffer[4096];
    recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    char html[8192];
    generate_directory_html("./shared", html, sizeof(html));

    char response[16384];

    snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\n"
        "Content=-Type: text/html\r\n"
        "\r\n"
        "%s",
        html);

    send(client_fd, response, strlen(response), 0);
    close(client_fd);
  }

  close(server_fd);
  return 0;
}

