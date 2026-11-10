cmake_minimum_required(VERSION 3.26)
project(online_reliability_tests LANGUAGES C CXX)
find_package(Threads REQUIRED)
enable_testing()
add_executable(online_reliability_test
    reliability_test.cpp
    ../../src/net/reliable_udp.cpp
    ../../src/net/datagram_scheduler.cpp
    ../../src/net/udp_connection.cpp
    ../../vendor/kcp/ikcp.c)
target_compile_features(online_reliability_test PRIVATE cxx_std_20)
target_link_libraries(online_reliability_test PRIVATE Threads::Threads)
target_include_directories(online_reliability_test PRIVATE ../../include ../../vendor/kcp)
if(WIN32)
    target_link_libraries(online_reliability_test PRIVATE ws2_32)
endif()
add_test(NAME online_reliability COMMAND online_reliability_test)
set_tests_properties(online_reliability PROPERTIES TIMEOUT 30)

add_executable(online_latency_benchmark latency_benchmark.cpp
    ../../src/net/reliable_udp.cpp ../../src/net/datagram_scheduler.cpp
    ../../vendor/kcp/ikcp.c)
target_compile_features(online_latency_benchmark PRIVATE cxx_std_20)
target_include_directories(online_latency_benchmark PRIVATE ../../include ../../vendor/kcp)
