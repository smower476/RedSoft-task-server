cmake_minimum_required(VERSION 3.10)
project(chat_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(server
    main.cpp
)

target_compile_options(server PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(server
    pthread
)
