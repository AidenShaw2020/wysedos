/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Freestanding unsigned division support for the kernel-mode NT display driver.
 * Bit division avoids depending on a user-mode C runtime in a kernel driver. */
typedef unsigned long long u64;
u64 __udivdi3(u64 numerator, u64 denominator)
{
    u64 quotient = 0, remainder = 0;
    if (!denominator) __builtin_trap();
    for (int bit = 63; bit >= 0; --bit) {
        unsigned carry = (unsigned)(remainder >> 63);
        remainder = (remainder << 1) | ((numerator >> bit) & 1);
        if (carry || remainder >= denominator) {
            remainder -= denominator;
            quotient |= (u64)1 << bit;
        }
    }
    return quotient;
}
