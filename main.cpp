#include <iostream>
#include "headerFiles/networking/apiComm.h"

int main(int argc, char *argv[]) {
    setRoot(getIP());
    getDDNS();
    return 0;
}
