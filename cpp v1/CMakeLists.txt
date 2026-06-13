cmake_minimum_required(VERSION 3.14)
project(OrderBookCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(orderbook
    main.cpp
    behave/axob_init.cpp
    behave/axob_order.cpp
    behave/axob_trade.cpp
    behave/axob_cage.cpp
    behave/axob_snap.cpp
)

target_include_directories(orderbook PRIVATE ${CMAKE_SOURCE_DIR})
