# cpp_linux — GCC 编译的订单簿引擎共享库 (libacmt_orderbook.so)
# 数据源: ClickHouse 直连 (无本地文件)。引擎源码通过符号链接 engine -> ../cpp v2 引用。
# 首次构建前执行: ln -s "../cpp v2" engine

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -march=native -fPIC -Wall -Wextra -fvisibility=hidden
DEFS     := -DUSE_FLAT_HASHMAP=1

INC := -Iinclude \
       -Iengine \
       -Iengine/source \
       -Iengine/third_party/unordered_dense/include

ENGINE_SRC := \
	engine/behave/axob_init.cpp \
	engine/behave/axob_order.cpp \
	engine/behave/axob_trade.cpp \
	engine/behave/axob_cage.cpp \
	engine/behave/axob_snap.cpp \
	engine/source/ch_client.cpp \
	engine/source/clickhouse_source.cpp

API_SRC := src/api.cpp
API_HDR := include/acmt_orderbook.h

LIB := libacmt_orderbook.so

.PHONY: all clean demo

all: $(LIB) demo

$(LIB): $(API_SRC) $(API_HDR) $(ENGINE_SRC)
	$(CXX) $(CXXFLAGS) $(DEFS) $(INC) -shared -o $@ $(API_SRC) $(ENGINE_SRC)

demo: $(LIB) demo/replay_ch.cpp
	$(CXX) $(CXXFLAGS) $(DEFS) $(INC) -o demo/replay_ch demo/replay_ch.cpp \
		-L. -lacmt_orderbook -Wl,-rpath,'$$ORIGIN/..'

clean:
	rm -f $(LIB) demo/replay_ch
