#include <iostream>     // Для вывода в консоль (std::cout)
#include <cstring>      // Для работы с массивами символов (хотя тут тоже почти не используется)
#include <arpa/inet.h>  // Для сетевых преобразований: htons, ntohs, inet_ntoa
#include <sys/socket.h> // Главная библиотека сокетов: socket, bind, recvfrom, sendto
#include <netinet/in.h> // Сетевые константы и структуры: AF_INET, INADDR_ANY, sockaddr_in
#include <unistd.h>     // Для функции close()

int main(){
    int s=socket(AF_INET,SOCK_DGRAM,0);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(12345);
    bind(s,(sockaddr*)&a,sizeof(a));
    while(true){
        char b[1024];
        sockaddr_in c{}; socklen_t cl=sizeof(c);
        ssize_t n=recvfrom(s,b,sizeof(b)-1,0,(sockaddr*)&c,&cl);
        if(n>0){
            b[n]=0;
            std::cout<<inet_ntoa(c.sin_addr)<<":"<<ntohs(c.sin_port)<<" "<<b<<"\n";
            sendto(s,b,n,0,(sockaddr*)&c,cl);
        }
    }
    close(s);
}
