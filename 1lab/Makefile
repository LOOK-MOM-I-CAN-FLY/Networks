CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
LDFLAGS :=
TARGETS := server client
SRCS := $(TARGETS:=.cpp)
OBJS := $(SRCS:.cpp=.o)

all: $(TARGETS)

server: server.o
	$(CXX) $(LDFLAGS) -o $@ $^

client: client.o
	$(CXX) $(LDFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(TARGETS) $(OBJS)

.PHONY: all clean