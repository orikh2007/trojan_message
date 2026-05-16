#include "admin/Admin.h"

int main() {
    try {
        int port;
        do {
            std::cout << "enter port: " << std::flush;
            std::cin >> port;
            std::cin.ignore();
        } while (port <= 0 || port > UINT16_MAX);

        Admin admin(port);
        admin.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
