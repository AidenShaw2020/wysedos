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
#include <86box/vid_voodoo_regs.h>
#include <86box/vid_voodoo_banshee_blitter.h>
#include <86box/vid_voodoo_texture.h>
#include <86box/vid_voodoo_fb.h>
#define voodoo_fifo_log(...)
enum {
    CMDFIFO3_PC_MASK_RGB   = (1 << 10),
    CMDFIFO3_PC_MASK_ALPHA = (1 << 11),
    CMDFIFO3_PC_MASK_Z     = (1 << 12),
    CMDFIFO3_PC_MASK_Wb    = (1 << 13),
    CMDFIFO3_PC_MASK_W0    = (1 << 14),
    CMDFIFO3_PC_MASK_S0_T0 = (1 << 15),
    CMDFIFO3_PC_MASK_W1    = (1 << 16),
    CMDFIFO3_PC_MASK_S1_T1 = (1 << 17),

    CMDFIFO3_PC = (1 << 28)
};
void voodoo_port_packet(voodoo_t *voodoo, uint32_t header)
{
    uint32_t addr, mask;
    int smode, num, num_verticies, v_num;
            voodoo->cmd_status &= ~7;
            voodoo->cmd_status |= (header & 7);
            voodoo->cmd_status |= (1 << 11);
            switch (header & 7) {
                case 0:
#if 0
                    voodoo_fifo_log("CMDFIFO0\n");
#endif
                    voodoo->cmd_status = (voodoo->cmd_status & 0xffff8fff) | (((header >> 3) & 7) << 12);
                    switch ((header >> 3) & 7) {
                        case 0: /*NOP*/
                            break;

                        case 1: /*JSR*/
#if 0
                            voodoo_fifo_log("JSR %08x\n", (header >> 4) & 0xfffffc);
#endif
                            voodoo->cmdfifo_ret_addr = voodoo->cmdfifo_rp;
                            voodoo->cmdfifo_rp       = (header >> 4) & 0xfffffc;
                            voodoo->cmdfifo_in_sub   = 1;
                            break;

                        case 2: /*RET*/
                            voodoo->cmdfifo_rp     = voodoo->cmdfifo_ret_addr;
                            voodoo->cmdfifo_in_sub = 0;
                            break;

                        case 3: /*JMP local frame buffer*/
                            voodoo->cmdfifo_rp     = (header >> 4) & 0xfffffc;
                            voodoo->cmdfifo_in_agp = 0;
#if 0
                            voodoo_fifo_log("JMP LFB to %08x %04x\n", voodoo->cmdfifo_rp, header);
#endif
                            break;

                        case 4: /*JMP AGP*/
                            if (UNLIKELY(voodoo->type < VOODOO_BANSHEE)) {
                                voodoo_fifo_log("CMDFIFO0: Not Banshee %08x\n", header);
                                break;
                            }

                            voodoo->cmdfifo_rp     = ((header >> 4) & 0x1fffffc) | (voodoo_port_fifo_word(voodoo) << 25);
                            voodoo->cmdfifo_in_agp = 1;
#if 0
                            voodoo_fifo_log("JMP AGP to %08x %04x\n", voodoo->cmdfifo_rp, header);
#endif
                            break;

                        default:
                            voodoo_fifo_log("Bad CMDFIFO0 %08x\n", header);
                    }
                    voodoo->cmd_status = (voodoo->cmd_status & ~(1 << 27)) | (voodoo->cmdfifo_in_sub << 27);
                    break;

                case 1:
                    num  = header >> 16;
                    addr = (header & 0x7ff8) >> 1;
#if 0
                    voodoo_fifo_log("CMDFIFO1 addr=%08x\n",addr);
#endif
                    while (num--) {
                        uint32_t val = voodoo_port_fifo_word(voodoo);
                        if ((addr & (1 << 13)) && voodoo->type >= VOODOO_BANSHEE) {
#if 0
                            if (voodoo->type != VOODOO_BANSHEE)
                                fatal("CMDFIFO1: Not Banshee\n");
#endif

#if 0
                            voodoo_fifo_log("CMDFIFO1: write %08x %08x\n", addr, val);
#endif
                            voodoo_port_fifo_2d(voodoo, addr, val);
                        } else {
                            if ((addr & 0x3ff) == SST_triangleCMD || (addr & 0x3ff) == SST_ftriangleCMD || (addr & 0x3ff) == SST_fastfillCMD || (addr & 0x3ff) == SST_nopCMD)
                                voodoo->cmd_written_fifo++;

                            if (voodoo->type >= VOODOO_BANSHEE && (addr & 0x3ff) == SST_swapbufferCMD)
                                voodoo->cmd_written_fifo++;
                            voodoo_port_fifo_reg(voodoo, addr, val);
                        }

                        if (header & (1 << 15))
                            addr += 4;
                    }
                    break;

                case 2:
                    if (voodoo->type < VOODOO_2) {
                        voodoo_fifo_log("CMDFIFO2: Not Voodoo 2\n");
                        break;
                    }
                    mask = (header >> 3);
                    addr = (voodoo->type >= VOODOO_BANSHEE) ? 8 : SST_bltSrcBaseAddr;
                    while (mask) {
                        if (mask & 1) {
                            uint32_t val = voodoo_port_fifo_word(voodoo);

                            if (voodoo->type >= VOODOO_BANSHEE)
                                voodoo_port_fifo_2d(voodoo, addr, val);
                            else
                                voodoo_port_fifo_reg(voodoo, addr, val);
                        }

                        addr += 4;
                        mask >>= 1;
                    }
                    break;

                case 3:
                    num   = (header >> 29) & 7;
                    mask  = header; //(header >> 10) & 0xff;
                    smode = (header >> 22) & 0xf;
                    voodoo_port_fifo_reg(voodoo, SST_sSetupMode, ((header >> 10) & 0xff) | (smode << 16));
                    num_verticies = (header >> 6) & 0xf;
                    v_num         = 0;
                    if (((header >> 3) & 7) == 2)
                        v_num = 1;
#if 0
                    voodoo_fifo_log("CMDFIFO3: num=%i verts=%i mask=%02x\n", num, num_verticies, (header >> 10) & 0xff);
                    voodoo_fifo_log("CMDFIFO3 %02x %i\n", (header >> 10), (header >> 3) & 7);
#endif

                    while (num_verticies--) {
                        voodoo->verts[3].sVx = voodoo_port_fifo_float(voodoo);
                        voodoo->verts[3].sVy = voodoo_port_fifo_float(voodoo);
                        /* Packed alpha-only vertices still carry one ARGB word. */
                        if ((mask & CMDFIFO3_PC_MASK_RGB) ||
                            ((header & CMDFIFO3_PC) && (mask & CMDFIFO3_PC_MASK_ALPHA))) {
                            if (header & CMDFIFO3_PC) {
                                uint32_t val            = voodoo_port_fifo_word(voodoo);
                                voodoo->verts[3].sBlue  = (float) (val & 0xff);
                                voodoo->verts[3].sGreen = (float) ((val >> 8) & 0xff);
                                voodoo->verts[3].sRed   = (float) ((val >> 16) & 0xff);
                                voodoo->verts[3].sAlpha = (float) ((val >> 24) & 0xff);
                            } else {
                                voodoo->verts[3].sRed   = voodoo_port_fifo_float(voodoo);
                                voodoo->verts[3].sGreen = voodoo_port_fifo_float(voodoo);
                                voodoo->verts[3].sBlue  = voodoo_port_fifo_float(voodoo);
                            }
                        }
                        if ((mask & CMDFIFO3_PC_MASK_ALPHA) && !(header & CMDFIFO3_PC))
                            voodoo->verts[3].sAlpha = voodoo_port_fifo_float(voodoo);
                        if (mask & CMDFIFO3_PC_MASK_Z)
                            voodoo->verts[3].sVz = voodoo_port_fifo_float(voodoo);
                        if (mask & CMDFIFO3_PC_MASK_Wb)
                            voodoo->verts[3].sWb = voodoo_port_fifo_float(voodoo);
                        if (mask & CMDFIFO3_PC_MASK_W0)
                            voodoo->verts[3].sW0 = voodoo_port_fifo_float(voodoo);
                        if (mask & CMDFIFO3_PC_MASK_S0_T0) {
                            voodoo->verts[3].sS0 = voodoo_port_fifo_float(voodoo);
                            voodoo->verts[3].sT0 = voodoo_port_fifo_float(voodoo);
                        }
                        if (mask & CMDFIFO3_PC_MASK_W1)
                            voodoo->verts[3].sW1 = voodoo_port_fifo_float(voodoo);
                        if (mask & CMDFIFO3_PC_MASK_S1_T1) {
                            voodoo->verts[3].sS1 = voodoo_port_fifo_float(voodoo);
                            voodoo->verts[3].sT1 = voodoo_port_fifo_float(voodoo);
                        }
                        if (v_num)
                            voodoo_port_fifo_reg(voodoo, SST_sDrawTriCMD, 0);
                        else
                            voodoo_port_fifo_reg(voodoo, SST_sBeginTriCMD, 0);
                        v_num++;
                        if (v_num == 3 && ((header >> 3) & 7) == 0)
                            v_num = 0;
                    }
                    while (num--)
                        voodoo_port_fifo_word(voodoo);
                    break;

                case 4:
                    num  = (header >> 29) & 7;
                    mask = (header >> 15) & 0x3fff;
                    addr = (header & 0x7ff8) >> 1;
#if 0
                    voodoo_fifo_log("CMDFIFO4 addr=%08x\n",addr);
#endif
                    while (mask) {
                        if (mask & 1) {
                            uint32_t val = voodoo_port_fifo_word(voodoo);

                            if ((addr & (1 << 13)) && voodoo->type >= VOODOO_BANSHEE) {
                                if (voodoo->type < VOODOO_BANSHEE)
                                    fatal("CMDFIFO1: Not Banshee\n");
#if 0
                                voodoo_fifo_log("CMDFIFO1: write %08x %08x\n", addr, val);
#endif

                                voodoo_port_fifo_2d(voodoo, addr, val);
                            } else {
                                if ((addr & 0x3ff) == SST_triangleCMD || (addr & 0x3ff) == SST_ftriangleCMD || (addr & 0x3ff) == SST_fastfillCMD || (addr & 0x3ff) == SST_nopCMD)
                                    voodoo->cmd_written_fifo++;

                                if (voodoo->type >= VOODOO_BANSHEE && (addr & 0x3ff) == SST_swapbufferCMD)
                                    voodoo->cmd_written_fifo++;
                                voodoo_port_fifo_reg(voodoo, addr, val);
                            }
                        }

                        addr += 4;
                        mask >>= 1;
                    }
                    while (num--)
                        voodoo_port_fifo_word(voodoo);
                    break;

                case 5:
#if 0
                    if (header & 0x3fc00000)
                        fatal("CMDFIFO packet 5 has byte disables set %08x\n", header);
#endif
                    num  = (header >> 3) & 0x7ffff;
                    addr = voodoo_port_fifo_word(voodoo) & 0xffffff;
                    if (!num)
                        num = 1;
#if 0
                    voodoo_fifo_log("CMDFIFO5 addr=%08x num=%i\n", addr, num);
#endif
                    switch (header >> 30) {
                        case 0: /*Linear framebuffer (Banshee)*/
                        case 1: /*Planar YUV*/
                            if (voodoo->texture_present[0][(addr & voodoo->texture_mask) >> TEX_DIRTY_SHIFT]) {
#if 0
                                voodoo_fifo_log("texture_present at %08x %i\n", addr, (addr & voodoo->texture_mask) >> TEX_DIRTY_SHIFT);
#endif
                                flush_texture_cache(voodoo, addr & voodoo->texture_mask, 0);
                            }
                            if (voodoo->texture_present[1][(addr & voodoo->texture_mask) >> TEX_DIRTY_SHIFT]) {
#if 0
                                voodoo_fifo_log("texture_present at %08x %i\n", addr, (addr & voodoo->texture_mask) >> TEX_DIRTY_SHIFT);
#endif
                                flush_texture_cache(voodoo, addr & voodoo->texture_mask, 1);
                            }
                            while (num--) {
                                uint32_t val = voodoo_port_fifo_word(voodoo);
                                if (addr <= voodoo->fb_mask - 3)
                                    voodoo_port_fifo_raw(voodoo, addr, val);
                                addr += 4;
                            }
                            break;
                        case 2: /*Framebuffer*/
                            while (num--) {
                                uint32_t val = voodoo_port_fifo_word(voodoo);
                                voodoo_fb_writel(addr, val, voodoo);
                                addr += 4;
                            }
                            break;
                        case 3: /*Texture*/
                            while (num--) {
                                uint32_t val = voodoo_port_fifo_word(voodoo);
                                voodoo_tex_writel(addr, val, voodoo);
                                addr += 4;
                            }
                            break;

                        default:
                            fatal("CMDFIFO packet 5 bad space %08x %08x\n", header, voodoo->cmdfifo_rp);
                    }
                    break;

                case 6:
                    if (UNLIKELY(voodoo->type < VOODOO_BANSHEE)) {
                        voodoo_fifo_log("CMDFIFO6: Not Banshee %08x %08x\n", header, voodoo->cmdfifo_rp);
                    } else {
                        /* The header holds size and space. Four payload words follow. */
                        uint32_t val = header;
                        banshee_cmd_write(voodoo->priv, 0x00, val >> 5);            /* agpReqSize */
                        banshee_cmd_write(voodoo->priv, 0x04, voodoo_port_fifo_word(voodoo)); /* agpHostAddressLow */
                        banshee_cmd_write(voodoo->priv, 0x08, voodoo_port_fifo_word(voodoo)); /* agpHostAddressHigh */
                        banshee_cmd_write(voodoo->priv, 0x0c, voodoo_port_fifo_word(voodoo)); /* agpGraphicsAddress */
                        banshee_cmd_write(voodoo->priv, 0x10, voodoo_port_fifo_word(voodoo)); /* agpGraphicsStride */
                        banshee_cmd_write(voodoo->priv, 0x14, (val & 0x18) | 0x00); /* agpMoveCMD - start transfer */
#if 0
                        voodoo_fifo_log("CMDFIFO6 addr=%08x num=%i\n", addr, banshee->agpReqSize);
#endif
                    }
                    break;

                default:
                    voodoo_fifo_log("Bad CMDFIFO packet %08x %08x\n", header, voodoo->cmdfifo_rp);
            }

}
