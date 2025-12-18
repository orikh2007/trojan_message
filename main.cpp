#include <iostream>
#include <asio/ip/udp.hpp>
#include "network/headers/CommV6.h"
#include "network/headers/apiComm.h"

int main(int argc, char *argv[]) {
    asio::io_context io;
    CommV6(io, 12345);
    return 0;
}
