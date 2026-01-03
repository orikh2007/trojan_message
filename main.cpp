#include "network/headers/networkSettings.h"
#include "network/headers/apiComm.h"
#include "network/headers/Msg.h"
#include "network/headers/Node.h"

int main(int argc, char* argv[]) {
    try {
        std::cout << getIP(v4) << "\n";
        int listen_p;
        do {
            std::cout << "enter port: " << std::flush;
            std::cin >> listen_p;
        } while (listen_p > UINT16_MAX || listen_p <= 0);

        std::cout << "Creating Node on port " << listen_p << "...\n" << std::flush;

        auto me = std::make_shared<Node>(listen_p);
        me->start();
        std::cout << "Current root: " << getDDNS() << std::endl;
        std::thread net_thread([me] {
            try {
                me->io().run();
            } catch (const std::exception& e) {
                std::cerr << "io_context exception: " << e.what() << "\n";
            }
        });

        std::cout << "CLI ready.\n";

        std::string line;
        while (std::getline(std::cin, line)) {
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
