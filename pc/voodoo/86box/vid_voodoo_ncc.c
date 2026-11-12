/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          3DFX Voodoo emulation.
 *
 * Authors: Sarah Walker, <https://pcem-emulator.co.uk/>
 *
 *          Copyright 2008-2020 Sarah Walker.
 */
#include "../voodoo_port.h"
#include <86box/vid_voodoo_common.h>
void
voodoo_update_ncc(voodoo_t *voodoo, int tmu)
{
    for (uint8_t tbl = 0; tbl < 2; tbl++) {
        for (uint16_t col = 0; col < 256; col++) {
            int y = (col >> 4);
            int i = (col >> 2) & 3;
            int q = col & 3;
            int i_r;
            int i_g;
            int i_b;
            int q_r;
            int q_g;
            int q_b;

            y = (voodoo->nccTable[tmu][tbl].y[y >> 2] >> ((y & 3) * 8)) & 0xff;

            i_r = (voodoo->nccTable[tmu][tbl].i[i] >> 18) & 0x1ff;
            if (i_r & 0x100)
                i_r |= 0xfffffe00;
            i_g = (voodoo->nccTable[tmu][tbl].i[i] >> 9) & 0x1ff;
            if (i_g & 0x100)
                i_g |= 0xfffffe00;
            i_b = voodoo->nccTable[tmu][tbl].i[i] & 0x1ff;
            if (i_b & 0x100)
                i_b |= 0xfffffe00;

            q_r = (voodoo->nccTable[tmu][tbl].q[q] >> 18) & 0x1ff;
            if (q_r & 0x100)
                q_r |= 0xfffffe00;
            q_g = (voodoo->nccTable[tmu][tbl].q[q] >> 9) & 0x1ff;
            if (q_g & 0x100)
                q_g |= 0xfffffe00;
            q_b = voodoo->nccTable[tmu][tbl].q[q] & 0x1ff;
            if (q_b & 0x100)
                q_b |= 0xfffffe00;

            voodoo->ncc_lookup[tmu][tbl][col].rgba.r = CLAMP(y + i_r + q_r);
            voodoo->ncc_lookup[tmu][tbl][col].rgba.g = CLAMP(y + i_g + q_g);
            voodoo->ncc_lookup[tmu][tbl][col].rgba.b = CLAMP(y + i_b + q_b);
            voodoo->ncc_lookup[tmu][tbl][col].rgba.a = 0xff;
        }
    }
}

