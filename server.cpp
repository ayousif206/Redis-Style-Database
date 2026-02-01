#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <iostream>
#include <cerrno>
#include <cstring>
#include <signal.h>

int main(){
    signal(SIGCHLD, SIG_IGN);

    int val = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr  = htonl(INADDR_ANY);
    
    bind(fd, (struct sockaddr *) &addr, sizeof(addr));
    listen(fd, SOMAXCONN);
    
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);

    while(true){
        int connfd = accept(fd, (struct sockaddr *) &client_addr, &socklen);
        if(connfd < 0){
            continue;
        }

        int pid = fork();
        if (pid == 0){
            close(fd);
            char buffer[64] {};
            while(1){
                ssize_t bytes_received = read(connfd, buffer, sizeof(buffer)-1);
                if(bytes_received <= 0){
                    break;
                }
                buffer[bytes_received] = '\0';
                std::cout << buffer;
                write(connfd, buffer, bytes_received);
            }
            close(connfd);
            exit(0);
        }
        close(connfd);
    }

    return 0;
}