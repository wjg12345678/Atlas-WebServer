CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2

endif

server: main.cpp ./timer/lst_timer.cpp ./timer/heap_timer.cpp ./http/http_conn.cpp ./http/http_file_service.cpp ./http/http_auth.cpp ./http/http_utils.cpp ./http/http_response.cpp ./http/http_io.cpp ./http/http_parser.cpp ./http/http_runtime.cpp ./log/log.cpp ./CGImysql/sql_connection_pool.cpp webserver.cpp config.cpp
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -lmysqlclient -lssl -lcrypto

clean:
	rm  -r server
