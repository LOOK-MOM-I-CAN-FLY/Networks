CXX = g++
CXXFLAGS = -Wall -Wextra -pthread -std=c++11
LDLIBS = -lssl -lcrypto -lpthread

CERT_DAYS = 365
CERT_SUBJ = /C=RU/ST=Local/L=Local/O=Lab7/OU=Dev/CN=localhost

all: server client

server: server.cpp protocol.hpp
	$(CXX) $(CXXFLAGS) -o server server.cpp $(LDLIBS)

client: client.cpp protocol.hpp
	$(CXX) $(CXXFLAGS) -o client client.cpp $(LDLIBS)

certs: server.crt server.key

server.crt server.key:
	openssl req -x509 -newkey rsa:2048 \
		-keyout server.key \
		-out server.crt \
		-days $(CERT_DAYS) \
		-nodes \
		-subj "$(CERT_SUBJ)"

clean:
	rm -f server client

distclean: clean
	rm -f server.crt server.key history.json net_diag_*.json

.PHONY: all certs clean distclean
