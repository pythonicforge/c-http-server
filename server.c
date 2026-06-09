#include<stdio.h>
#include<sys/socket.h>
#include<stdlib.h>
#include<unistd.h>
#include<netinet/in.h>
#include<string.h>

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

  while(1){
    printf("Connecting...\n");
    int client_fd = accept(server_fd, NULL, NULL);

    if(client_fd < 0){
      perror("Accept error");
      continue;
    }

    char buffer[4096];

    int bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if(bytes_received > 0){
      buffer[bytes_received] = '\0';
      printf("%s\n", buffer);

    char *response = "HTTP/1.1 200 OK\r\n"
      "\r\n"
      "Hello World";

    send(client_fd, response, strlen(response), 0);
    }
    close(client_fd);
  }
  close(server_fd);
  return 0;
}

