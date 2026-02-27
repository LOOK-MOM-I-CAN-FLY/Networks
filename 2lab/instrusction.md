g++ server.cpp -o server.exe -std=c++11 -Wall -O2 -lws2_32
g++ client.cpp -o client.exe -std=c++11 -Wall -O2 -lws2_32 -pthread