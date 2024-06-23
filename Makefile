CXXFLAGS=-g -O2 -Wall

server: server.c++ client.html.h
	clang++ $(CXXFLAGS) -no-pie -std=c++20 server.c++ -o server -DKJ_DEBUG -lavcodec -lavformat -lswscale -lswresample -lavutil -lkj-http -lkj-async -lkj

client.html.h: client.html
	( echo '#include <kj/common.h>'; \
	  echo 'namespace kvmonitor {'; \
	  echo 'static constexpr kj::StringPtr CLIENT_HTML = R"#('; \
	  cat $<; \
	  echo ')#"_kj;'; \
	  echo '}  // namespace kvmonitor' ) > $@
