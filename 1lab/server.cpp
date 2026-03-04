#include <iostream>     // Для вывода в консоль (std::cout)
#include <cstring>      // Для работы с массивами символов (хотя тут тоже почти не используется)
#include <arpa/inet.h>  // Для сетевых преобразований: htons, ntohs, inet_ntoa
#include <sys/socket.h> // Главная библиотека сокетов: socket, bind, recvfrom, sendto
#include <netinet/in.h> // Сетевые константы и структуры: AF_INET, INADDR_ANY, sockaddr_in
#include <unistd.h>     // Для функции close()

int main() {
    
    // Создаем "сетевую розетку". 
    // AF_INET - IPv4 (IP-адреса вида 192.168.x.x).
    // SOCK_DGRAM - протокол UDP (отправка данных кусками-датаграммами без гарантии доставки).
    // 0 - протокол по умолчанию (UDP).
    // Переменная 's' - это файловый дескриптор (число), номер нашего сокета в системе.
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    
    // sockaddr_in a{} - структура для адреса сервера. 
    // {} зануляет всю память структуры, чтобы там не было мусора.
    sockaddr_in a{}; 
    
    // Указываем семейство адресов (IPv4).
    a.sin_family = AF_INET; 
    
    // a.sin_addr.s_addr = INADDR_ANY; - ОЧЕНЬ ВАЖНАЯ СТРОКА!
    // INADDR_ANY (0.0.0.0) означает: "слушать на всех сетевых интерфейсах компьютера". 
    // Если у сервера есть Wi-Fi, локальная сеть по проводу и интернет, 
    // INADDR_ANY заставит сервер принимать пакеты отовсюду.
    a.sin_addr.s_addr = INADDR_ANY; 
    
    // a.sin_port = htons(12345); - указываем порт 12345.
    // htons (Host TO Network Short) - переворачивает порядок байт числа 12345 
    // из формата процессора в единый сетевой стандарт.
    a.sin_port = htons(12345);
    
    // bind(...) - функция привязки. Мы говорим операционной системе: 
    // "Забронируй порт 12345 за моим сокетом 's'".
    // (sockaddr*)&a - снова приведение типов из мира языка Си. 
    // Превращаем указатель на sockaddr_in в указатель на базовый sockaddr.
    // sizeof(a) - размер структуры в байтах.
    bind(s, (sockaddr*)&a, sizeof(a));
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
