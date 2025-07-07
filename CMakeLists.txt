cmake_minimum_required(VERSION 3.10)
project(chat_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SRC_FILES
    src/main.cpp
    src/connections.cpp
    src/commands.cpp
    src/validation.cpp
)

set(HEADER_FILES
    include/connections.h
    include/commands.h
    include/validation.h
)

add_executable(server ${SRC_FILES} ${HEADER_FILES})

target_include_directories(server PRIVATE include)

target_compile_options(server PRIVATE 
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

target_link_libraries(server
    pthread
)
