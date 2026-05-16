cmake_minimum_required(VERSION 4.0)
project(trojan_message)

set(CMAKE_CXX_STANDARD 20)

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
        network/source/Node.cpp
        network/headers/Msg.h
        network/headers/log.h
        encryption/Crypto.cpp
        encryption/Crypto.h
        gui/UserGUI.cpp
        gui/UserGUI.h
        admin/Shell.cpp
        admin/Shell.h
)
add_executable(admin
        admin_main.cpp
        admin/Admin.h
        admin/Admin.cpp
        network/source/apiComm.cpp
        network/headers/apiComm.h
        network/headers/networkSettings.h
        network/headers/Node.h
        network/source/Node.cpp
        network/headers/Msg.h
        network/headers/log.h
        encryption/Crypto.cpp
        encryption/Crypto.h
        admin/Shell.cpp
        admin/Shell.h
)


find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(imgui CONFIG REQUIRED)

target_compile_definitions(trojan_message PRIVATE ASIO_STANDALONE)

if (WIN32)
    target_link_libraries(trojan_message PRIVATE ws2_32 mswsock)
endif()

target_link_libraries(trojan_message
        PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto
        imgui::imgui
        d3d11
        dxgi
        d3dcompiler
)

target_compile_definitions(admin PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0601)

if (WIN32)
    target_link_libraries(admin PRIVATE ws2_32 mswsock)
endif()

target_link_libraries(admin
        PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto
)
