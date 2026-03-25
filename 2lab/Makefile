CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++11 -pthread

all: server client

server: server.cpp protocol.hpp
	$(CXX) $(CXXFLAGS) server.cpp -o server

client: client.cpp protocol.hpp
	$(CXX) $(CXXFLAGS) client.cpp -o client

clean:
	rm -f server client

# -Wall (Warnings: all) — вопреки названию, включает не «все-все» предупреждения, а основной набор самых важных и общепринятых. Сюда входят сообщения о неиспользованных переменных, функциях без возвращаемого значения, подозрительных преобразованиях типов и т.д.
# -Wextra — включает дополнительные предупреждения, которые не вошли в -Wall. Например, предупреждения о сравнении знаковых и беззнаковых целых чисел или об отсутствии инициализации некоторых членов структуры.
