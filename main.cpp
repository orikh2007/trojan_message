#include <iostream>
#include <asio/ip/udp.hpp>
#include "network/headers/Comm.h"
#include "network/headers/apiComm.h"
#include "network/headers/Msg.h"
#include "network/headers/Node.h"


int main(int argc, char *argv[]) {
    try {
        int listen_p = 12345;
        int send_p = 12346;
        auto me = std::make_shared<Node>(listen_p);
        me->start();
        std::cout << "getDDNS: " << getDDNS() << endl;
        std::thread net_thread([me] {
            try {
                me->io().run();
            } catch (const std::exception& e) {
                std::cerr << "io_context exception: " << e.what() << "\n";
            }
        });

        std::cout   << "CLI ready.\n"
                    << "Commands:\n"
                    << "  send <ip> <port> <message>\n"
                    << "  quit\n"
                    << std::flush;

        std::string line;
        while (std::getline(std::cin, line)) {
            // Post to io thread so Node state is only touched there
            asio::post(me->io(), [me, line] {
                me->handle_command(line);
            });

            if (line == "quit") break;
        }
        net_thread.join();
        return 0;
    } catch (std::exception &e) {
        std::cerr << "exception: " << e.what() << "\n";
        return 1;
    }
}
