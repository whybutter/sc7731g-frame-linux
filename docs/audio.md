# Audio on the VT10101

We wanted sound (2026-10-28); a speaker is already on the board's SPK
connector. Mainline Linux has nothing for this SoC's audio, so it is a new
driver, sized like the display one. This file is the plan and, as the work
goes, its log.

## What the hardware is (from the vendor tree and the frame's own Android)

- **Digital side: VBC r2p0**, the AP's voice-band codec interface, at
  0x40020000 (size 0x20000): the DAC/ADC FIFOs, sample-rate converters,
  digital gain. The vendor DT (`vbc-r2p0`) has it `okay`.
- **Analog side: sprd-codec v3** inside the SC2713S PMIC, reached over the
  ADI bus like the regulators (kernel/regulator-sc2713s.c): DAC, mixers,
  speaker driver, mic bias, ADC. Android on this unit ran `sprd-codec-v3`
  (sysfs `devices/sprd-codec-v3.30/power_supply/audio-ldo`), with
  `sprd,audio_power_ver = 3` and `sprd,def_da_fs = 44100` in its DT.
  Samsung's own phone build used v4; ours is v3.
- **Speaker amplifier:** `spk_pa_en_gpios = <85>`, an external PA enabled by
  GPIO 85.
- **DMA:** the AP DMA controller; the vendor's request lines for the VBC are
  DMA_VB_DA0/DA1 (playback) and AD0/AD1 (capture). Its global registers
  match mainline's `sprd-dma` (written for SC9860) field for field so far;
  whether the rest does decides whether we write a DMA driver too.
- **Reference:** the vendor 3.10 kernel's sound/soc/sprd/ (dai/vbc/r2p0,
  codec/sprd/v3, dai/sprd-pcm.c) and drivers/dma/sprd_dma.c, Samsung SM-G531H
  GPL source (docs/mainline.md says where it came from). About 25k lines in
  all, most of it phone features we don't want: voice calls through the
  modem, FM, Bluetooth SCO, tuning tables, headset detection.

## DMA: mainline's sprd-dma, patched (research, 2026-10-28)

The vendor's driver for this chip's DMA is drivers/platform/sprd/dma_r4p0.c
(`sprd,sprd-dma`), not drivers/dma/sprd_dma.c (sharkl64 only, and
CONFIG_DMADEVICES is off in the vendor defconfig). Its register layout is
mainline's sprd-dma.c (SC9860) almost exactly:

- 0x20100000 size 0x4000, GIC SPI 50 (level), 32 channels; channels 0-23
  are "standard" (registers only to 0x1c: no transaction length, no link
  list), 24-31 "full". Enable AP_AHB_EB bit 5, reset AP_AHB_RST bit 8.
  Our clocks dtsi needs that gate.
- Global registers, the channel block (0x1000 + n * 0x40) and its fields,
  and the request-ID table (0x2000 + 4 * (uid - 1), written channel + 1) all
  match. VBC request IDs: DA0 29, DA1 30, AD0 31, AD1 32.
- Differences, to patch behind an `sprd,sc7731g-dma` match: slave channels
  only from 24-31 (mainline hands out channel 0 first, which can't do the
  link-list audio needs); pause status is bit 16, not 2; the INTC status
  bits are likely 8-12, not 16-20 (to confirm on the hardware); no
  REQ_PEND writes and no CFG bit 24 (the vendor never sets them); memcpy
  width capped at 4 bytes.
- The vendor audio path (2 full channels, 16-bit, fragment requests, link
  list) is what mainline's sprd-pcm-dma.c already asks for.

## Plan

1. **Research** (in progress): the DMA controller against mainline's, and the
   minimal register sequence from cold to sound on the speaker.
2. **Analog codec first, by hand**: power the codec and PA from a test
   module/initramfs tool and play the codec's own test tone, or a VBC FIFO
   fed by the CPU (PIO), to prove the speaker path before any DMA.
3. **ASoC drivers**: a VBC DAI driver, a codec component (DAPM for DAC →
   speaker, volume), audio power as regulators where it fits, and the card
   from `simple-audio-card` in the DT. DMA through mainline's sprd-dma and
   sprd-pcm-dma if they fit, else a small DMA driver.
4. **Playback through ALSA** (`aplay`, then a real player), tried with
   kernel-trial.sh like every kernel now: no UART needed.
5. **Capture** (mic) after playback; the board has MIC+/MIC- pads but no mic
   fitted yet.

Kernel changes go through tools/kernel-trial.sh (one boot from recovery,
promote only when healthy), so a broken audio driver costs a power cycle at
worst.

## The speaker path, cold to sound (research, 2026-10-28)

From the vendor's sound/soc/sprd (dai/vbc/r2p0, codec/sprd/v3) and
include/soc/sprd/__sprd_audio_sc8830.h. "ADI x" is an offset in our sprd-adi
regmap (SC2713S slave window 0x40038000): ANA_GLB at 0x800, the codec's
analog block at 0x600. Nothing depends on the modem or on vendor firmware;
the only CP-related step is taking the VBC and the codec IRQ from the CP.

1. **VBC to the AP:** CP0/1/2_WPROT_EN (0x402E3044/48/4C) bit 6 set; AON
   VBC_CTRL (0x402E0020) bits 4-19 (*_SYS_SEL) = 0; AP_WPROT_EN (0x402E3040)
   bit 6 clear. Then APB_EB0 (0x402E0000) bit 19 VBC_EB, PMU SLEEP_CTRL
   (0x402B00CC) bit 8 AP_SLEEP_XTL_ON, PMU MEM_PD_CFG0 (0x402B0138) bits 5:2
   clear; VBC reset APB_RST0 (0x402E0008) bit 20, 10 us.
2. **Codec digital:** APB_EB0 bit 18 AUD_EB; AUDIF auto-gated (APB_EB0 bit 17
   left clear, VBC_CTRL bit 20 set) -- so our clk_audif gate stays off;
   SINDRV_CTRL (0x402E0018) bit 0; APB_RST0 bits 19 and 18 pulsed 10 us.
3. **Codec analog:** ADI 0x800 bit 4 ANA_AUD_EN; ADI 0x80C bits 11:9 pulsed
   10 us; ADI 0x644 bit 2 ICM_PLUS_EN; ADI 0x804 bits 2 and 1; ADI 0x8B4
   bit 1; ADI 0x808 bit 4; ADI 0x88C bit 8 XTL_EN; ADI 0x884 = 0.
4. **Audio power:** VB (ADI 0x640 bit 7), BG (0x644 bit 7), BG_IBIAS (0x644
   bit 5), VCOM (0x644 bit 4, VCM_V bits 13:11 = 2, i.e. 3.2 V: the vendor
   picks it from VBAT and falls back to 3.8 V's code), VCOM_BUF (0x644 bit
   3), VBO (0x640 bit 6); SEL_VCMI and VCMI_FAST_EN (0x644 bits 1, 0);
   ~5 ms. AUD_SDM_CTL0 (0x40000010) = 0x400.
5. **Codec DAC:** AUD_DAC_CTL (0x4000000C) bit 15 (mute) clear, rate in
   3:0 (48 kHz 1, 44.1 kHz 2); CCR (ADI 0x660) bits 3, 0; AUD_TOP_CTL
   (0x40000000) bits 0, 2; AUDIF_ENB (ADI 0x600) bits 0, 2; DACGR (ADI 0x674)
   bits 7, 6 with gain 7 (0 dB) in 14:12 / 10:8.
6. **To the speaker outputs:** DAOCR2 (ADI 0x678) bit 5 DACL->AOL, bit 0
   DACR->AOR; DCR2 (ADI 0x680) bits 4, 3 AOL/AOR_EN; speaker gain DCGR3
   (ADI 0x694), 4 bits per side in 3 dB steps from -24 dB (8 ~ 0 dB).
7. **VBC playback:** FIFOs DA0 0x40020000 (left), DA1 0x40020004 (right),
   16-bit, ping-pong, 160 frames; VBBUFFSIZE (+0x10) 15:8 = 159; VBIISSEL
   (+0x3C) bit 14; DADGCTL (+0x44) and DAPATCHCTL (+0x40) = 0; DMA requests
   29/30, one channel per FIFO (4-byte source step through interleaved
   16-bit stereo, 2-byte fixed destination, 320-byte fragments); VBCHNEN
   (+0xC8) bits 0, 1; VBDABUFFDTA (+0x18) bits 13, 14 (DMA) then 15
   (VBENABLE). The vendor always loads a unity EQ and an ALC profile into
   DAHPCTL (+0x48) and DAALCCTL0-10; bypassing them is untested.
8. **Amplifier, last on, first off:** the internal PA -- PMUR8 (ADI 0x64C)
   15:13 = 7, PMUR6 (ADI 0x648) 13:11 = 7, ADI 0x640 bit 8 OVP_LDO_EN, DCR4
   (ADI 0x684) = 0, class AB with PA_SW_EN (0x640 bit 15), then PA_EN (0x640
   bit 13) -- and GPIO 85 high. The vendor's source never reads
   `spk_pa_en_gpios`, so VTimes added the external-amp code themselves;
   which amp the speaker hangs off, and GPIO 85's polarity, is for the
   hardware to say. Off: GPIO 85 low, PA_EN off, 2 ms, PA_SW/LDO off.

## The speaker is on HPR, through an external amp (bench, 2026-10-28)

tools/audio-poke.py on the frame (/dev/mem; the vendor's order from above),
someone listening:

- The left internal PA (ADI 0x640 bits 15/13, the vendor's whole speaker
  path) gave silence, not even a switch-on pop: it drives SPK-L, which has
  nothing connected. The right PA's bits (12/10) don't stick on this PMIC.
- The speaker is on **SPK-R**, beside an 8-pin amplifier chip. With every
  output on, a CPU-fed tone came out as a crackle with **GPIO 85 high**,
  faint or nothing with it low: GPIO 85 is that amp's enable, active high.
- One output at a time with GPIO 85 high: **HPR** (DCR1 ADI 0x680 bit 6,
  mixer DAOCR1 ADI 0x67C bits 1/0, gain DCGR1 ADI 0x690) crackles, HPL
  doesn't. So the path is DAC -> HPR mixer -> HPR driver -> external amp
  (GPIO 85) -> SPK-R. EAR not tried on its own.
- The crackle is the CPU feeding the FIFOs unpaced (no DMA, no FIFO
  status read): the path is proven, the sound needs the DMA. The driver's
  DAPM: DAC -> HPR, plus a GPIO-switched "Speaker" widget on GPIO 85, and
  no internal PA at all.

## The first driver plays (2026-10-29)

out-25/26 (kernel #43/#44, trialled from p21): the card registers as
`VT10101` (simple-card; sc7731g-vbc + sc2713s-codec + sprd-pcm-dma), with a
`Speaker` volume (HPR gain, -36..+9 dB, set to -6 dB). `aplay -D plughw:0,0`
of a 3 s mono 48 kHz tone takes 3 s, paced by the hardware: DMA channel 24
(device channels start at 24, as the r4p0 driver said), 24 period
interrupts. The INTC status bits are at 8-12 as guessed, but raw -- they
latch fragment and block done whatever is enabled (INTC read 0x314 mid-
playback) -- so mainline's exact match logged "incorrect dma interrupt
type" every period until patch 0004 masked them by the enables. A person's
ear was the test still to pass.

**It sounds right (2026-10-29).** We heard the 3 s 440 Hz tone
"clean and steady" at -6 dB. Kernel #44 (out-26) is promoted to p16. ALSA's
`default` (and a plain `aplay`) takes stereo 44.1 kHz too: alsa-lib's plug
downmixes and resamples to the card's mono 44.1/48 kHz, so players need no
special device. Still to do: capture (no mic fitted), pop checks on
open/close, and suspend. `rootfs/etc/asound/` has the ALSA default and
the mixer state used on the frame.
