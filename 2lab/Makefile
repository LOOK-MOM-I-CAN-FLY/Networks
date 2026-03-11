# Makefile — build client and server
CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
LDLIBS :=
UNAME := $(shell uname -s)

# pthread for POSIX (needed for std::thread)
ifeq ($(UNAME),Linux)
    CXXFLAGS += -pthread
endif
ifeq ($(UNAME),Darwin)
    CXXFLAGS += -pthread
endif

# On Windows (MSYS/MinGW) we need ws2_32
ifeq ($(OS),Windows_NT)
    LDLIBS += -lws2_32
endif

SRCS_CLIENT := client.cpp
SRCS_SERVER := server.cpp
OBJS_CLIENT := $(SRCS_CLIENT:.cpp=.o)
OBJS_SERVER := $(SRCS_SERVER:.cpp=.o)
TARGETS := client server

.PHONY: all clean run-server run-client

all: $(TARGETS)

client: $(SRCS_CLIENT)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

server: $(SRCS_SERVER)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

# Generic .o rule (not strictly necessary here, kept for extension)
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@rm -f $(TARGETS) *.o

# Convenience targets
run-server: server
	./server

run-client: client
	./client
