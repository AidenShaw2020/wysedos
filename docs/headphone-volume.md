# Analog headphone volume

The Settings **Volume** control scales the mixed PCM sent to both display audio
and the analog output. **Headphones** controls only the analog RT5672 output.
It must not change the HDMI/DisplayPort PCM stream. Both are global monitor
controls, separate from a game's own Sound Blaster mixer.

The codec has two paths from DAC1 to the headphone amplifier: one direct and
one through the headphone volume stage (HPOVOL). Initialization previously
enabled the direct path, leaving HPOVOL disconnected. Writes to `HP_VOL` (02h)
therefore changed register values without attenuating the active signal.

Initialization now selects DAC L1/R1 into their powered HPOVOL stages, enables
HPOVOL into HPO MIX, and mutes the direct DAC1 bypass, DAC2 and analog inputs.
HPMIX (45h) uses active-high mute bits: the route mask/value are E00Fh/A00Ah.
It preserves the unrelated mixer-gain bit. This fixes the signal route rather
than adding a second software gain to the shared PCM output.

The register definitions and independent signal graph are documented in the
primary Linux driver: [RT5670/72 definitions](https://github.com/torvalds/linux/blob/v6.6/sound/soc/codecs/rt5670.h)
and [HPO/HPOVOL DAPM routes](https://github.com/torvalds/linux/blob/v6.6/sound/soc/codecs/rt5670.c).

Menu level 16 selects +12 dB, each lower nonzero level subtracts 3 dB, and 0
mutes. Thus 15 to 2 is a 39 dB change on the analog path at constant master
Volume. The existing mapping is retained. Volume writes now read back HP_VOL
and report a failure instead of claiming success after an unsuccessful write.

## Local regression

`tools/tests/analog_volume.c` executes the real codec initialization and volume
functions against a mock DesignWare I2C/register device. It checks the decoded
signal paths: both volume stages powered and connected, no direct bypass or
unwanted analog/DAC2 source, attenuation, mute/unmute, clamping and rejected
write readback. Restoring the old initialization fails the bypass assertion.

```powershell
clang -O2 -ffunction-sections tools/tests/analog_volume.c '-Wl,/opt:ref' -o work/analog_volume.exe
./work/analog_volume.exe
```

Hardware qualification must additionally verify actual headphone loudness and
unchanged HDMI PCM at fixed master Volume; register readback alone does not
prove the analog level. These are separate observations.

## Hardware validation, 2026-11-05

- Wyse 3040 reported HPMIX A00Ah after initialization. The user confirmed a
  large decrease at menu Headphones 15 to 2 with master Volume held at 6.
- HDMI capture at constant master 12 used the same 1 kHz tone and hardware
  volume steps corresponding to menu 15, 2 and mute. Each four-second recording
  was measured over its middle two seconds. RMS was 1085.602 / 1085.601 /
  1085.602 and peak 1536 in all three. Level differences were below 0.001 dB.
- Initial silent capture attempts were excluded: the original 1.1.9 monitor
  also produced silence through the capture device. Reconnecting the grabber's
  USB restored valid audio before the comparisons above.
