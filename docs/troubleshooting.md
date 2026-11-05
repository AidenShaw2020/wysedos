# Troubleshooting

[Documentation index](index.md)

## Boot and installation

**USB will not start:** write the image raw, select its UEFI entry with Dell
F12, and check unsigned application boot policy. Another PC is not an equivalent
test of the Wyse-specific installer.

**C: exists but does not boot:** installation creates FAT32 and DRIVERS, not
DOS system files. Boot your DOS media and transfer the matching system. Early
DOS versions may not support FAT32.

**Black screen:** identify whether it occurs before Dell, at wyseDOS or inside
the guest. Try a direct display connection without an extender. Record adapter,
resolution and whether input/animation still works. Do not immediately format.

## Graphics and power saving

**Windows offers only 16 colours:** install [VMDisp9x](../drivers/windows9x/vmdisp9x/README.md)
for PCI 1234:1111, not an Intel physical GPU driver.

**Broken moving objects:** report the title/version, scene and build. v1.1.8
fixes Bash page lifetime and Keen's wide intro. A static clean screen is not
evidence that page-flipped movement is correct.

**Only the grabber is corrupt:** compare direct output or raw capture. The
development grabber has produced damaged MJPEG blocks with a correct framebuffer.

**Black display after the screen saver:** Windows' later monitor-power timer
can blank the guest. Current DPMS suppresses both pixels and text cursor,
although the physical HDMI signal remains. A persistent blinking cursor in
this state is a bug report, not the intended screen saver.

## Sound and speed

**Wrong tempo:** note whether animation and sound accelerate together and
whether display changes trigger it. CPU pacing and display-audio clocks are
independent; v1.1.6 corrected the output-clock problem.

**Distorted PCM:** check SB16 220h, IRQ 5, DMA 1/5 and 8/16-bit selection.
v1.1.7 fixes the DMA-stop race in Teenagent. New reports need a reproducible
scene/time and output type (analog or HDMI).

## Input, USB and network

**DOS mouse missing:** load CuteMouse or another PS/2 driver. Host HID support
does not itself install a guest DOS mouse driver.

**KVM input pause:** firmware enumeration can take seconds. Record whether it
recovers, which devices stop and whether the guest continues. The long-idle
PS/2 deadline issue was corrected in v1.1.4; check the installed version.

**Windows USB disk missing:** check the guest mass-storage driver and Disk
Manager ownership. Public releases exclude Windows USB storage packages.

**Disk disappears after image mounting:** expected exclusive ownership.
Eject every image from that physical source; a successful flush returns it to
guest USB. Use Windows Stop/Eject before takeover or physical removal.

**NE2000 resource dialog:** the model uses 300h–31Fh, IRQ 3 and ISA PnP.
A setup dialog alone does not identify a conflict. Record the conflicting
device/range or error code before changing resources.

## Bug reports

Include release/hash, Wyse firmware, guest OS/drivers, CPU/RAM, peripherals,
storage backend, exact steps, expected/actual behavior, and direct-output versus
capture observations. Keep OS keys, personal files and copyrighted media out
of reports.
