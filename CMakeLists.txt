cmake_minimum_required(VERSION 3.20)
project(chaosproxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(CHAOS_SANITIZE "" CACHE STRING "Sanitizer to build with: address, thread, or empty")
option(CHAOS_WERROR "Treat warnings as errors" OFF)

find_package(Threads REQUIRED)

add_library(chaos_core STATIC
  src/util/log.cpp
  src/net/socket.cpp
  src/net/poller_kqueue.cpp
  src/net/poller_epoll.cpp
  src/chaos/rules.cpp
  src/chaos/pipeline.cpp
  src/core/stats.cpp
  src/core/timer_service.cpp
  src/core/session.cpp
  src/core/worker.cpp
  src/core/acceptor.cpp
  src/core/proxy.cpp
  src/control/commands.cpp
  src/control/control_server.cpp
)
target_include_directories(chaos_core PUBLIC src)
target_link_libraries(chaos_core PUBLIC Threads::Threads)
target_compile_options(chaos_core PUBLIC -Wall -Wextra -Wpedantic $<$<BOOL:${CHAOS_WERROR}>:-Werror>)

if(CHAOS_SANITIZE STREQUAL "address")
  target_compile_options(chaos_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(chaos_core PUBLIC -fsanitize=address,undefined)
elseif(CHAOS_SANITIZE STREQUAL "thread")
  target_compile_options(chaos_core PUBLIC -fsanitize=thread)
  target_link_options(chaos_core PUBLIC -fsanitize=thread)
elseif(NOT CHAOS_SANITIZE STREQUAL "")
  message(FATAL_ERROR "CHAOS_SANITIZE must be address, thread or empty")
endif()

add_executable(chaosproxy src/main.cpp)
target_link_libraries(chaosproxy PRIVATE chaos_core)

include(CTest)
if(BUILD_TESTING)
  add_executable(chaos_tests
    tests/test_main.cpp
    tests/test_rules.cpp
    tests/test_pipeline.cpp
    tests/test_timer.cpp
    tests/test_proxy.cpp
  )
  target_link_libraries(chaos_tests PRIVATE chaos_core)
  add_test(NAME chaos_tests COMMAND chaos_tests)
endif()
