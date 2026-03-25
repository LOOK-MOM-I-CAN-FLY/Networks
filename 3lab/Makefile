CXX = g++
CXXFLAGS = -Wall -Wextra -pthread -std=c++11

all: server client

server: server.cpp protocol.hpp
	$(CXX) $(CXXFLAGS) -o server server.cpp

client: client.cpp protocol.hpp
	$(CXX) $(CXXFLAGS) -o client client.cpp

clean:
	rm -f server client
