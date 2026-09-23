#pragma once

struct R5900Context;
class PS2Runtime;

namespace dq8 {
void repairVuBoundsFlags(R5900Context &ctx, unsigned comparison);
bool installVuBoundsComparisons(PS2Runtime &runtime);
}
