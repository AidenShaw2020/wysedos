#include <assert.h>
#include <stdio.h>
#include "cpu_speed_policy.h"

int main(void)
{
    int g, v, active_g, active_v;
    CpuWindows3DBoost timer = {0};
    assert(!cpu_windows_3d_boost_step(&timer, 10, 0, 0, 1, 0));
    assert(!cpu_windows_3d_boost_step(&timer, 100, 0, 0, 1, 1)); /* VBE alone */
    assert(!cpu_windows_3d_boost_step(&timer, 90000000, 0, 0, 1, 1)); /* desktop idle */
    assert(cpu_windows_3d_boost_step(&timer, 90000001, 0, 1, 1, 1));
    assert(cpu_windows_3d_boost_step(&timer, 93000000, 0, 1, 1, 1));
    assert(!cpu_windows_3d_boost_step(&timer, 93000001, 0, 1, 1, 1)); /* idle expiry */
    assert(cpu_windows_3d_boost_step(&timer, 94000000, 0, 2, 1, 1));
    assert(!cpu_windows_3d_boost_step(&timer, 94000001, 1, 2, 1, 1)); /* text round trip */
    assert(!cpu_windows_3d_boost_step(&timer, 94000002, 1, 2, 1, 1));
    assert(cpu_windows_3d_boost_step(&timer, 95000000, 1, 3, 1, 1));
    assert(!cpu_windows_3d_boost_step(&timer, 95000001, 1, 4, 0, 1)); /* disabled ignores work */
    assert(!cpu_windows_3d_boost_step(&timer, 95000002, 1, 4, 1, 1));
    assert(cpu_windows_3d_boost_step(&timer, 0xfffffff0u, 1, 5, 1, 1));
    assert(cpu_windows_3d_boost_step(&timer, 20, 1, 5, 1, 1)); /* time wrap */
    assert(!cpu_windows_3d_boost_step(&timer, 3000000, 1, 5, 1, 1));
    assert(!cpu_windows_3d_boost_step(&timer, 3000001, 1, 6, 1, 0));
    assert(!cpu_windows_3d_boost_step(&timer, 3000002, 1, 6, 1, 1));
    for (g = 0; g <= 1; ++g) {
        for (v = 0; v <= 1; ++v) {
            for (active_g = 0; active_g <= 1; ++active_g) {
                for (active_v = 0; active_v <= 1; ++active_v) {
                    int expected = (g && active_g) || (v && active_v) ? 0 : 133;
                    assert(cpu_speed_effective(133, g, active_g, v, active_v) == expected);
                }
            }
        }
    }
    assert(cpu_speed_effective(166, 1, 0, 1, 0) == 166);
    assert(cpu_speed_effective(166, 1, 1, 1, 0) == 0);
    assert(cpu_speed_effective(166, 1, 0, 1, 1) == 0);
    assert(cpu_speed_effective(0, 0, 0, 0, 0) == 0);
    puts("Tagged Windows 3D lease, idle expiry, text return, wrap and independent DOS Glide: PASS");
    return 0;
}
