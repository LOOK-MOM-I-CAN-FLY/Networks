CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++11 -pthread

all: server client

server: server.cpp protocol.h
	$(CXX) $(CXXFLAGS) server.cpp -o server

client: client.cpp protocol.h
	$(CXX) $(CXXFLAGS) client.cpp -o client

clean:
	rm -f server client
