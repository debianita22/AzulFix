AzulFix
=======

[Lilu](https://github.com/acidanthera/Lilu) plugin for `AppleIntelFramebufferAzul`, the Haswell
(HD 4600/5000) framebuffer that OpenCore Legacy Patcher restores on macOS 13 and later. Written for
a ThinkPad W541 running [ouroBIOS](https://github.com/debianita22/ourobios) (coreboot + EDK2), where
the built-in panel is driven through a DisplayPort port instead of eDP and the ME (HECI) PCI device
is hidden.

The fixes are applied in memory when the framebuffer loads. The kext on disk stays exactly as
OCLP installs it, so nothing has to be patched again after a macOS update or an OCLP root patch.

| Fix | Without AzulFix | With AzulFix |
|-----|-----------------|--------------|
| ASR | white flashes every ~1 s, no picture with acceleration | `FeatureControl` > `ASRDisable` = 1 |
| ME wait | 40 s of boot spent waiting for the ME, slow wake with a green screen | wait of 1 ms instead of 10 s per device name |
| Panel | black screen at S3 wake when the firmware leaves the display to the OS; backlight left on during display sleep | panel power sequence on when the framebuffer wakes, backlight on after `LightUpDP`, backlight off before `DisableDisplay` (with the backlight off delay) |

Tested build: `AppleIntelFramebufferAzul` 18.0.8 (OCLP, UUID `FA074475…`), macOS 14 Sonoma.

## Details

**Alternate Scrambler Reset.** `GetDPCDInfo` keeps bits 1:0 of the panel's DPCD 0x00D. When the
panel reports ASSR, `LinkTraining`/`FastLinkTraining` set bit 6 of `DP_TP_CTL` (alternate scrambler
seed on the source) unless the controller's `ASRDisable` flag (controller+0x1a5e) is set, but they
write DPCD 0x10A (the same mode on the sink) only when the port type is eDP (3). With the panel on
a DP port (type 2), source and sink scramble with different seeds: training passes, the video stream
is garbage. `AppleIntelAzulController::getFeatureControl` loads the flag from the `FeatureControl`
dictionary of the controller personality (default 0). AzulFix routes that function and puts
`ASRDisable = 1` in a copy of the dictionary just before the original reads it.

**ME lookup.** `AppleIntelAzulController::setupMEIDriverClassPointer` calls `findMEDevice` for
`pci8086,8c3a`, `9c3a`, `8cba` and `9cba`. Each call is
`waitForMatchingService(nameMatching(name), 10 s)`; with the HECI device hidden all four time out,
at boot and again for every framebuffer at wake. AzulFix changes the timeout immediate
(`48 be 00 e4 0b 54 02 00 00 00` → `48 be 40 42 0f 00 00 00 00 00`, unique in 18.0.8) to 1 ms.
A device that is already registered is still found.

**Panel backlight.** To Azul the panel on DDI D is a DP display, so it never drives the PCH panel
power sequencer (`PP_CONTROL`), which coreboot uses for it (`GFX_GMA_PANEL_1_PORT "DP3"`). At S3
resume with ouroBIOS "Light the display up at wake" off, the firmware gives the panel only VDD
(`PP_CONTROL` 0x8): Azul trains the link and sends the picture, but the backlight stays off. On the
framebuffer with `port-number` 7, AzulFix turns the power sequence on when the framebuffer wakes
(`AppleIntelFramebuffer::doSetPowerState`), before Azul wakes the sink over AUX and trains the link,
waits for it (power-up delay, at most 1 s), and turns the backlight on after it, as libgfxinit
and i915 do; before
`DisableDisplay` it turns the backlight off and waits the backlight off delay (`PP_OFF_DELAYS`, 1 ms
on the W541) before the video goes. The panel keeps its power, so its HPD stays high for the
next probe. Registers go through `AppleIntelAzulController::ReadRegister32`/`WriteRegister32`.

## Install

1. Lilu 1.2.0 or newer (built against 1.7.2); works together with WhateverGreen.
2. Copy `AzulFix.kext` to `EFI/OC/Kexts` and add it to `Kernel > Add` after Lilu and WhateverGreen:
   `BundlePath` `AzulFix.kext`, `ExecutablePath` `Contents/MacOS/AzulFix`, `PlistPath`
   `Contents/Info.plist`, `Enabled` true.
3. If the Azul kext on disk was patched by hand before, AzulFix reports
   `1 already in the kext Info.plist` and `bytes not found`: harmless, but restore the original kext
   to rely on AzulFix alone.

## Boot arguments

| Argument | Effect |
|----------|--------|
| `-azulfixoff` | disable AzulFix |
| `-azulfixnoasr` | skip the ASR fix |
| `-azulfixnome` | skip the ME wait patch |
| `-azulfixnopanel` | skip the panel backlight handling |
| `-azulfixnoef` | normal framing on the DP link instead of enhanced framing, which Azul enables whatever the panel declares (DPCD 0x002 bit 7) |
| `-azulfixdbg` | debug log (DEBUG build) |
| `-azulfixbeta` | load on macOS versions newer than supported |

## Check

```sh
ioreg -rc AzulFix -d1 -w0 | grep -E 'ASRDisable|MEWait|Panel|VersionInfo'
ioreg -rc AppleIntelAzulController -d1 -w0 | grep -o '"ASRDisable"=[0-9]*'
sudo dmesg | grep AzulFix
```

Expected: `ASRDisable` = `set to 1`, `MEWait` = `10 s -> 1 ms`, `Panel` = `power with doSetPowerState, backlight with LightUpDP and DisableDisplay`, and `"ASRDisable"=1` in the
controller's `FeatureControl`. Lilu logs through `IOLog("%s", ...)`, which the unified log stores
as `<private>`, so `log show` does not show the messages; the kernel message buffer (`dmesg`) does,
while they are still in it.

Verified on the W541 (2026-10-26) with the original OCLP kext: `set to 1`, `10 s -> 1 ms`, the ME
lookup at boot takes 108 ms instead of 40 s.

## Build

GitHub Actions builds DEBUG and RELEASE on a macOS runner with Xcode, against the Lilu 1.7.2 DEBUG
SDK (checksum verified) and a pinned [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK)
commit. The zips are in the run's artifacts, and attached to a release when one is published.

Locally on macOS: put `Lilu.kext` (DEBUG) and `MacKernelSDK` in the project root, then
`xcodebuild -configuration Release`.

## Credits

Lilu and MacKernelSDK by vit9696 and the acidanthera team. The project layout follows acidanthera's
DebugEnhancer, the `FeatureControl` wrapper follows WhateverGreen's display data buffer optimizer.
