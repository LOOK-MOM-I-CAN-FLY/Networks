// iostream: Нужна для ввода и вывода текста в консоль (std::cin и std::cout).
#include <iostream>

// string: Подключает класс строк (std::string), чтобы удобно работать с текстом.
#include <string>

// cstring: Старая Си-библиотека для работы с массивами символов. 
// В этом коде она фактически не нужна, но часто идет "прицепом" в сетевых программах.
#include <cstring>

// arpa/inet.h: Содержит функции для перевода IP-адресов из понятного нам вида ("127.0.0.1") 
// в понятный компьютеру (набор нулей и единиц). Например: inet_pton, htons.
#include <arpa/inet.h>

// sys/socket.h: Самая главная сетевая библиотека. Содержит базовые функции сокетов: 
// socket() - создать, sendto() - отправить, recvfrom() - получить.
#include <sys/socket.h>

// netinet/in.h: Содержит структуры данных для интернет-адресов (например, sockaddr_in), 
// а также константы вроде AF_INET (IPv4).
#include <netinet/in.h>

// unistd.h: Библиотека операционной системы (POSIX). Здесь она нужна для функции close(), 
// чтобы правильно закрыть сокет в конце.
#include <unistd.h>

// cstdlib: Стандартная библиотека языка Си. Нужна здесь ради одной функции: atoi(), 
// которая превращает текст "123" в число 123.
#include <cstdlib>

int main(int argc,char**argv){
    const char* ip = argc>1?argv[1]:"127.0.0.1";
    int port = argc>2?atoi(argv[2]):12345;
    int s=socket(AF_INET,SOCK_DGRAM,0);
    sockaddr_in srv{}; srv.sin_family=AF_INET; srv.sin_port=htons(port); inet_pton(AF_INET,ip,&srv.sin_addr);
    std::string line;
    while(std::getline(std::cin,line)){
        sendto(s,line.data(),line.size(),0,(sockaddr*)&srv,sizeof(srv));
        char b[1024];
        socklen_t sl=sizeof(srv);
        ssize_t n=recvfrom(s,b,sizeof(b)-1,0,(sockaddr*)&srv,&sl);
        if(n>0){ b[n]=0; std::cout<<b<<"\n"; }
    }
    close(s);
}
