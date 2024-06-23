CXXFLAGS=-g -O2 -Wall

server: server.c++
	clang++ $(CXXFLAGS) -no-pie -std=c++20 server.c++ -o server -DKJ_DEBUG -lavcodec -lavformat -lswscale -lswresample -lavutil -lkj-http -lkj-async -lkj
