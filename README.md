# bt-autoconnect

An Atmosphere sysmodule that reconnects your Bluetooth headphones when the
Nintendo Switch boots or wakes from sleep.

When the Switch wakes or reboots, headphones that are already on do not come
back by themselves: you open System Settings → Bluetooth Audio and pick them
again. This module does that for you, right as the console wakes.

Tested on HOS 21.1.0 with Sony WH-1000XM5: connected 1.9 s after boot and
2.9 s after waking from sleep. Needs firmware 13.0.0+ (native Bluetooth audio).

## Install

1. Download the zip from [Releases](../../releases) and copy its
   `atmosphere/` folder to the root of the SD card.
2. Reboot.
3. Connect your headphones once in System Settings → Bluetooth Audio. The
   module remembers them and from then on reconnects them on every boot and
   wake.

To switch to other headphones, connect them once by hand; the module follows
whatever audio device was connected last.

Uninstall: delete `sdmc:/atmosphere/contents/420000000000BA7C/` and reboot.

## How it works

- Program id `420000000000BA7C`, started at boot via `boot2.flag`.
- Connects through `btm:sys` `ConnectAudioDevice`, the same call System
  Settings makes when you pick a paired device.
- Wake detection: a PSC power-state module (id 124) that depends on the
  Bluetooth and btm modules, so the Awake notice arrives after Bluetooth is
  back. Every power-state request is acknowledged before any work, so sleep
  and wake are never delayed.
- After boot it keeps trying for 90 s, after wake for 45 s, and stops as soon
  as an audio device is connected.
- Remembers the target in `sdmc:/config/bt-autoconnect/last_device.txt`
  (updated whenever an audio device connects).
- Log: `sdmc:/config/bt-autoconnect/log.txt`, rotated at 64 KB.

## Limits

- Headphones must be on and in range. While they are off, the module pages
  them for up to 45 s after a wake, then gives up until the next wake.
- Multipoint headphones that already hold two other devices may refuse the
  Switch (not tested).
- One audio device at a time, like the Switch itself.

## Why not SnapBack

[SnapBack](https://github.com/saulotmalo/snapback) aims at the same thing.
In `mode = wake` on HOS 21.1.0 its log showed `pscmGetPmModule failed:
0x0000048A` in a loop and it never tried to connect: it registers its PSC
module under Nintendo's own Bluetooth module id (31). It also connects
through `btdrv` directly instead of `btm`.

## Build

```sh
./scripts/build.sh                          # Docker devkitpro/devkita64 → dist/sd/
./scripts/deploy.py <switch-ip>             # upload over Sphaira's FTP (port 5000)
./scripts/deploy.py <switch-ip> --log       # read the module log
```

## License

MIT
