#include "MiniTest.h"

void register_ps2_memory_tests();

int main() {
    register_ps2_memory_tests();
    return MiniTest::Run();
}
