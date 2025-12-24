cmake_minimum_required(VERSION 4.0)
project(trojan_message)

set(CMAKE_CXX_STANDARD 20)


if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if (MSVC)
    add_compile_options(/W4 /permissive- /EHsc)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(trojan_message
        main.cpp
        network/source/apiComm.cpp
        network/headers/apiComm.h
        network/headers/networkSettings.h
        network/headers/Node.h
        network/headers/msg.h
        network/source/Node.cpp
        network/source/Node.cpp
)
target_compile_definitions(trojan_message PRIVATE ASIO_STANDALONE)

if (WIN32)
    target_link_libraries(trojan_message PRIVATE ws2_32)
    # Often needed for async networking on Windows:
    target_link_libraries(trojan_message PRIVATE mswsock)
endif()

find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(trojan_message
        PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
)
