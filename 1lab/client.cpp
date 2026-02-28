#include <iostream>
#include <string>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

int main(int argc,char**argv){
    const char* ip = argc>1?argv[1]:"127.0.0.1";
    int port = argc>2?atoi(argv[2]):12345;
    int s=socket(AF_INET,SOCK_DGRAM,0);
    sockaddr_in srv{}; srv.sin_family=AF_INET; srv.sin_port=htons(port); inet_pton(AF_INET,ip,&srv.sin_addr);
    std::string line;
    std::getline(std::cin,line);
    sendto(s,line.data(),line.size(),0,(sockaddr*)&srv,sizeof(srv));
    char b[1024];
    socklen_t sl=sizeof(srv);
    ssize_t n=recvfrom(s,b,sizeof(b)-1,0,(sockaddr*)&srv,&sl);
    if(n>0){ b[n]=0; std::cout<<b<<"\n"; }
    close(s);
}