#include "MiniTest.h"

void register_ps2_vu1_tests();
void register_ps2_vu1_upper_engine_tests();
void register_ps2_vu1_program_tests();

int main() {
    register_ps2_vu1_tests();
    register_ps2_vu1_upper_engine_tests();
    register_ps2_vu1_program_tests();
    return MiniTest::Run();
}
