cmake_minimum_required(VERSION 4.0)
project(trojan_message)

set(CMAKE_CXX_STANDARD 20)

if (MSVC)
    add_compile_options(/W4 /permissive- /EHsc)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(trojan_message main.cpp
        network/source/apiComm.cpp
        network/headers/apiComm.h
        network/headers/networkSettings.h
        network/headers/Node.h
        network/headers/protocol.h
        network/headers/msg.h
)


find_package(nlohmann_json REQUIRED)
target_link_libraries(trojan_message PRIVATE nlohmann_json::nlohmann_json)


find_package(CURL REQUIRED)
target_link_libraries(trojan_message PRIVATE CURL::libcurl)

find_package(asio CONFIG REQUIRED)
target_link_libraries(trojan_message PRIVATE asio::asio)