#include "skai/storage/database.hpp"
#include <iostream>

// Build-only tool: create an empty install seed using production migrations.
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    skai::Database database(argv[1]);
    std::string error;
    if (!database.open(error)) {
        std::cerr << error << '\n';
        return 1;
    }
    database.close();
    return 0;
}
