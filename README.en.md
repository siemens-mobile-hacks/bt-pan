# BT PAN for BenQ-Siemens EL71 sw45, E71 sw45, Siemens S65 sw58, SK65 sw50 and CX75 sw25

[Русская версия](README.md)

Internet on the phone over Bluetooth: a PC (with `nap.sh`) or an Android phone with «Bluetooth
tethering» on shares its connection, and the browser, Java and elfs go online as if over GPRS. Not tested on the E71.

## What's in the folder

| File | What it is |
|---|---|
| `bt-pan_ELKA.elf` | the elf for EL71 and E71 |
| `bt-pan_SG.elf` | the elf for S65 sw58, SK65 sw50 and CX75 sw25 |
| `nap.sh` | PC side |
| `speedtest-pc.py` | speed test server (`nap.sh` starts it; keep it next to `nap.sh`) |
| `patches/` | `patches/el71/` for EL71, `patches/e71/` for E71, `patches/s65/` for S65, `patches/sk65/` for SK65, `patches/cx75/` for CX75 |
| `source/` | source code |
| `phone-manual/` | guides with screenshots: GPRS profile, connecting the elf |

## Patches (in this order)

1. `library-...vkp` — function library. The elf takes every firmware function from it (entries 420–471).
   If an older version of this library is installed, undo it and install the new one;
   otherwise the elf reports at startup that the library doesn't fit.2. `bt-pan-mtu...vkp` — required.
3. `btnet_...vkp` — needed by the browser and elfs. Changes nothing while the elf isn't running.
   If the elf says «Old hook patch», undo `patches/el71/old/...` and install the new one.

### S65 sw58 (`patches/s65/`)

1. `library-s65-sw58-extended.vkp` — extended function library with Bluetooth functions.
2. `bt-pan-mtu_S65v58.vkp` — recommended: without it packets are at most 320 bytes and the PC
   needs `sudo MTU=320 ./nap.sh`.
3. `bt-pan-browser_S65v58.vkp` — only for the built-in browser without a SIM card.
   Changes nothing while the elf isn't running.

### SK65 sw50 (`patches/sk65/`)

1. `library-sk65-sw50-extended.vkp` — extended function library with Bluetooth functions.
2. `bt-pan-mtu_SK65v50.vkp` — recommended: without it packets are at most 320 bytes and the PC
   needs `sudo MTU=320 ./nap.sh`.
3. `bt-pan-browser_SK65v50.vkp` — only for the built-in browser without a SIM card.
   Changes nothing while the elf isn't running.

### CX75 sw25 (`patches/cx75/`)

Needs ElfPack 3.3.2.

1. `library-cx75-sw25-extended.vkp` — extended function library with Bluetooth functions.
2. `bt-pan-mtu_CX75v25.vkp` — recommended: without it packets are at most 320 bytes and the PC
   needs `sudo MTU=320 ./nap.sh`. The patch also gives 96 KB of general memory to the Bluetooth
   buffers (without that the phone does not boot).
3. `bt-pan-browser_CX75v25.vkp` — only for the built-in browser without a SIM card.
   Does nothing while the elf is not running.

Browser on S65/SK65/CX75: in the profile set protocol HTTP, empty proxy, port 80, connection = the GPRS
profile, home page starting with `http://`.

## PC

1. Pair the phone with the PC.
2. Install the packages:
   ```sh
   sudo apt install -y bluez bluez-tools dnsmasq iptables python3
   ```
3. Run:
   ```sh
   sudo ./nap.sh        # stop: ./nap.sh stop
   ```

## Phone

1. You need a GPRS internet profile (any settings). Point the browser and Java at it.
   How to create one: [phone-manual/how-to-setup-gprs.en.md](phone-manual/how-to-setup-gprs.en.md).
2. Start the elf. In the menu: «PC» → pick the PC → «Connect».
   Step by step with screenshots: [phone-manual/how-to-setup-elf.en.md](phone-manual/how-to-setup-elf.en.md).
3. «Speed test...» — speed tests, «Ping...» — ping. Speed tests work only while the phone is connected to a PC running `nap.sh`.
4. «Disconnect» drops the link, «Exit» quits.

Settings (the PC and auto-reconnect) can be changed in the elf itself (the «PC» and «Auto-reconnect» rows),
or in the BCFG editor: `zbin\etc\BTPAN.bcfg`.

## Limitations

- While the elf is running, Java Bluetooth (JSR-82) doesn't work.
- While the elf is connected to the PC, all of the phone's internet (browser, Java, elfs) goes
  through the PC; mobile internet (GPRS) is not used.
- If it doesn't work after «Disconnect» → «Connect» or an elf restart, reboot the phone.
- Not tested with a SIM card.

## Building

ElfLoader 3 SDK: original [siemens-mobile-hacks/sdk](https://github.com/siemens-mobile-hacks/sdk); this build uses the modified fork [dm468/sdk](https://github.com/dm468/sdk) (adds the BT/LMAN/ENIP/socket SWI prototypes bt-pan needs), cloned to `../sdk` relative to `source/`. Then:

```sh
cd source && cmake -B build -S . && cmake --build build
```
