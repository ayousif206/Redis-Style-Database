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
                std::cout << buffer << '\n';

                if(buffer[0] == '*'){
                    char* p = buffer;

                    char* dollarsign = strchr(p, '$');
                    char* stringstart = strchr(dollarsign, '\n') + 1;

                    if(strncmp(stringstart, "ECHO", 4) == 0){
                        char* nextdollar = strchr(stringstart, '$');
                        if(nextdollar){
                            int lenmessage = atoi(nextdollar+1);
                            char* messagestart = strchr(nextdollar, '\n') + 1;

                            std::string reply = "$" + std::to_string(lenmessage) + "\r\n";
                            reply.append(messagestart, lenmessage);
                            reply += "\r\n";
                            
                            write(connfd, reply.c_str(), reply.length());
                            continue;
                        }
                    }
                }

                if(strstr(buffer, "PING")){
                    const char* reply = "+PONG\r\n";
                    write(connfd, reply, strlen(reply));
                }
                else{
                    const char* err = "-ERR unknown command\r\n";
                    write(connfd, err, strlen(err));
                }
            }
            close(connfd);
            exit(0);
        }
        close(connfd);
    }

    return 0;
}