# FlashDump

FlashDump is an addon that you run from your Casio **fx-CG50**, connect to a device (tested on windows, mac, linux, and android phone through usb OTG), you see it as a usb device and simply copy all of flash to that device, no more having to build fxlink or any extra effort, nothing extra is needed on the receiving side.

FlashDump only reads. The drive is read-only, and nothing on the calculator changes.

## Use

1. Download `FLASHDMP.g3a` from [Releases](https://github.com/fxprgm/flashdump-cg/releases).
   Connect the calculator, choose **USB Flash** (F1), copy the file onto it, eject, unplug.
2. Open **FlashDump** from the MAIN MENU, then connect the USB cable.
3. Open the drive **CG50 FLASH** on your computer and copy **FLASH.BIN** (about 40 seconds).
4. Eject the drive, then press **EXIT** on the calculator.

FLASH.BIN runs in the [fx-CG50 emulator](https://github.com/hexbinoct/casio-cg50) (CG50 Studio,
the Android app). It holds Casio's OS and your own files and settings: keep it to yourself.

**If for some reason this new version is not working for you**, the older FlashDump 0.2 sends the dump with fxlink instead:
[v0.2.0](https://github.com/fxprgm/flashdump-cg/releases/tag/v0.2.0) and its
[guide](https://github.com/fxprgm/flashdump-cg/tree/v0.2.0#readme).

## How to Build

With the fxSDK and [gint](https://gitea.planet-casio.com/Lephenixnoir/gint):
`fxsdk build-cg` → `FLASHDMP.g3a`. The USB driver (`src/usbdev.c`) is FlashDump's own, written from scratch.

Tested on the fx-CG50 (OS 3.60). Pull requests that add other Casio calculators are welcome.

MIT license.
