#include "hpserver.h"
#include <cstdio>

int main() {
    hpserver server;
    if (server.listen() == -1) {
        perror("Failed to start server");
        return 1;
    }
    return 0;
}
