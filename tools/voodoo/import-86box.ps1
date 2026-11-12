$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$upstream = Join-Path $root 'work/upstream/86box'
$revision = 'a1e92633baf178ff0791cb5624e406da3e8eb257'
$actual = (& git -C $upstream rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actual -ne $revision) { throw 'Wrong 86Box revision' }
if (& git -C $upstream status --porcelain) { throw 'Modified upstream checkout' }
$destination = Join-Path $root 'pc/voodoo/86box'
New-Item -ItemType Directory -Force "$destination/86box" | Out-Null
$sources = @('vid_voodoo_render.c', 'vid_voodoo_reg.c', 'vid_voodoo_texture.c',
             'vid_voodoo_setup.c', 'vid_voodoo_fb.c', 'vid_voodoo_blitter.c')
$utf8 = New-Object System.Text.UTF8Encoding($false)
foreach ($name in $sources) {
    $text = [IO.File]::ReadAllText((Join-Path $upstream "src/video/$name")).Replace("`r`n", "`n")
    $text = $text.Replace('#include <86box/86box.h>', '#include "../voodoo_port.h"')
    $text = $text.Replace("#include <wchar.h>`n", '')
    $text = [regex]::Replace($text, '(?m)^#include (?:"cpu\.h"|<86box/(?!vid_voodoo_)[^>]+>)\r?\n', '')
    if ($name -eq 'vid_voodoo_fb.c') {
        $text = $text.Replace('if (read_addr > voodoo->fb_mask)', 'if (read_addr > voodoo->fb_mask - (sizeof(temp) - 1))')
        $anchor = '    if (voodoo->lfbMode & 0x100) {'
        if ([regex]::Matches($text, [regex]::Escape($anchor)).Count -ne 2) { throw 'LFB write boundaries changed' }
        $text = $text.Replace($anchor, "    voodoo_port_vram_written(voodoo, write_addr, 4);`n    voodoo_port_vram_written(voodoo, write_addr_aux, 4);`n$anchor")
    }
    if ($name -eq 'vid_voodoo_texture.c') {
        $anchor = "    params->tex_entry[tmu] = c;`n    voodoo->texture_cache[tmu][c].refcount++;`n}`n`nvoid`nflush_texture_cache"
        if (-not $text.Contains($anchor)) { throw 'Texture decode boundary changed' }
        $text = $text.Replace($anchor, "    voodoo_port_texture_decoded(voodoo, tmu, c, texture_offset[lod_min], texture_offset[lod_max + 1]);`n$anchor")
    }
    if ($name -eq 'vid_voodoo_render.c') {
        # DEPTH_TEST() does not parenthesize its argument: with FBZ_DEPTH_SOURCE the
        # comparison became "source ? zaColor : (new_depth OP old_depth)", which
        # passed every pixel (the recompiler compares correctly).
        $depth = 'DEPTH_TEST((params->fbzMode & FBZ_DEPTH_SOURCE) ? (params->zaColor & 0xffff) : new_depth);'
        if (-not $text.Contains($depth)) { throw 'Depth test call changed' }
        $text = $text.Replace($depth, 'DEPTH_TEST(((params->fbzMode & FBZ_DEPTH_SOURCE) ? (params->zaColor & 0xffff) : new_depth));')
        $text = $text.Replace('#if (defined __amd64__ || defined _M_X64)', "#if defined NO_CODEGEN`nint voodoo_recomp = 0;`n#elif (defined __amd64__ || defined _M_X64)")
        $start = "voodoo_queue_triangle(voodoo_t *voodoo, voodoo_params_t *params)`n{"
        if (-not $text.Contains($start)) { throw 'Triangle function boundary changed' }
        $sync = @'
#if defined VOODOO_PORT_SYNCHRONOUS
    voodoo_use_texture(voodoo, params, 0);
    if (voodoo->dual_tmus)
        voodoo_use_texture(voodoo, params, 1);
    voodoo_port_triangle(voodoo, params);
#else
'@
        $text = $text.Replace($start, "$start`n$sync")
        $text = $text.TrimEnd()
        $last = $text.LastIndexOf('}')
        $text = $text.Insert($last, "#endif`n") + "`n"
    }
    [IO.File]::WriteAllText((Join-Path $destination $name), $text.Replace("`r`n", "`n"), $utf8)
}
Copy-Item "$upstream/src/include/86box/vid_voodoo_*.h" "$destination/86box/" -Force
# The render-idle wait is where queued GPU work must reach the frame buffer
# (the header keeps upstream's line endings).
$renderHeader = Join-Path $destination '86box/vid_voodoo_render.h'
$header = [IO.File]::ReadAllText($renderHeader)
$nl = if ($header.Contains("`r`n")) { "`r`n" } else { "`n" }
$idle = "voodoo_wait_for_render_thread_idle(voodoo_t *voodoo)$nl{$nl"
if (-not $header.Contains($idle)) { throw 'Render idle wait changed' }
$header = $header.Replace($idle, "$idle    voodoo_port_render_idle(voodoo);$nl")
[IO.File]::WriteAllText($renderHeader, $header, $utf8)
Copy-Item "$upstream/COPYING" "$destination/COPYING" -Force
$display = [IO.File]::ReadAllText("$upstream/src/video/vid_voodoo_display.c").Replace("`r`n", "`n")
$start = $display.IndexOf("void`nvoodoo_update_ncc(")
$end = $display.IndexOf("void`nvoodoo_pixelclock_update(", $start)
if ($start -lt 0 -or $end -le $start) { throw 'NCC function boundaries changed' }
$notice = $display.Substring(0, $display.IndexOf('*/') + 2)
$ncc = $notice + "`n#include `"../voodoo_port.h`"`n#include <86box/vid_voodoo_common.h>`n" + $display.Substring($start, $end - $start)
[IO.File]::WriteAllText("$destination/vid_voodoo_ncc.c", $ncc, $utf8)
$main = [IO.File]::ReadAllText("$upstream/src/video/vid_voodoo.c").Replace("`r`n", "`n")
$anchor = $main.IndexOf('rgb332[c].r = c & 0xe0;')
if ($anchor -lt 0) { throw 'Color table anchor changed' }
$start = $main.LastIndexOf('    for (c = 0; c < 0x100; c++) {', $anchor)
$end = $main.IndexOf('#ifndef NO_CODEGEN', $anchor)
if ($start -lt 0 -or $end -le $start) { throw 'Color table boundaries changed' }
$notice = $main.Substring(0, $main.IndexOf('*/') + 2)
$definitions = [regex]::Matches($main, '(?m)^rgba8_t [^\n]+;') | ForEach-Object { $_.Value }
$tables = $notice + "`n#include `"../voodoo_port.h`"`n#include <86box/vid_voodoo_common.h>`n" + ($definitions -join "`n") + "`nvoid voodoo_port_tables_init(void)`n{`n    int c;`n" + $main.Substring($start, $end - $start) + "}`n"
[IO.File]::WriteAllText("$destination/vid_voodoo_tables.c", $tables, $utf8)
$fifo = [IO.File]::ReadAllText("$upstream/src/video/vid_voodoo_fifo.c").Replace("`r`n", "`n")
$start = $fifo.IndexOf('            voodoo->cmd_status &= ~7;')
if ($start -lt 0) { throw 'FIFO packet anchor changed' }
$end = $fifo.IndexOf('            end_time = plat_timer_read();', $start)
if ($end -le $start) { throw 'FIFO packet boundary changed' }
$notice = $fifo.Substring(0, $fifo.IndexOf('*/') + 2)
$enumStart = $fifo.LastIndexOf('enum {', $start)
$enumEnd = $fifo.IndexOf('};', $enumStart) + 2
if ($enumStart -lt 0 -or $enumEnd -le $enumStart) { throw 'FIFO vertex definitions changed' }
$vertexDefinitions = $fifo.Substring($enumStart, $enumEnd - $enumStart)
$body = $fifo.Substring($start, $end - $start)
$body = $body.Replace('voodoo_cmdfifo_reg_writel(', 'voodoo_port_fifo_reg(')
$body = $body.Replace('voodoo_2d_reg_writel(', 'voodoo_port_fifo_2d(')
$body = $body.Replace('cmdfifo_get_f(', 'voodoo_port_fifo_float(').Replace('cmdfifo_get(', 'voodoo_port_fifo_word(')
$body = $body.Replace('if (addr <= voodoo->fb_mask)', 'if (addr <= voodoo->fb_mask - 3)')
$body = $body.Replace('*(uint32_t *) &voodoo->fb_mem[addr] = val;', 'voodoo_port_fifo_raw(voodoo, addr, val);')
$packet = $notice + "`n#include `"../voodoo_port.h`"`n#include <86box/vid_voodoo_common.h>`n#include <86box/vid_voodoo_regs.h>`n#include <86box/vid_voodoo_banshee_blitter.h>`n#include <86box/vid_voodoo_texture.h>`n#include <86box/vid_voodoo_fb.h>`n#define voodoo_fifo_log(...)`n" + $vertexDefinitions + "`nvoid voodoo_port_packet(voodoo_t *voodoo, uint32_t header)`n{`n    uint32_t addr, mask;`n    int smode, num, num_verticies, v_num;`n" + $body + "}`n"
[IO.File]::WriteAllText("$destination/vid_voodoo_packet.c", $packet, $utf8)
Write-Output "Imported 86Box Voodoo core at $revision"