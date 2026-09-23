#include "MiniTest.h"

void register_ps2_runtime_kernel_tests();
void register_ps2_runtime_interrupt_tests();
void register_ps2_runtime_io_tests();
void register_ps2_sif_rpc_tests();

int main() {
    register_ps2_runtime_kernel_tests();
    register_ps2_runtime_interrupt_tests();
    register_ps2_runtime_io_tests();
    register_ps2_sif_rpc_tests();
    return MiniTest::Run();
}
