# BT PAN for BenQ-Siemens EL71 sw45 and E71 sw45

[Русская версия](README.md)

Internet on the phone over Bluetooth: a PC (with `nap.sh`) or an Android phone with «Bluetooth
tethering» on shares its connection, and the browser, Java and elfs go online as if over GPRS. Not tested on the E71.

## What's in the folder

| File | What it is |
|---|---|
| `bt-pan_ELKA.elf` | the elf |
| `nap.sh` | PC side |
| `speedtest-pc.py` | speed test server (`nap.sh` starts it; keep it next to `nap.sh`) |
| `patches/` | patches for EL71, `patches/e71/` for E71 |
| `source/` | source code |
| `phone-manual/` | guides with screenshots: GPRS profile, connecting the elf |

## Patches (in this order)

1. `library-...vkp` — function library.
2. `bt-pan-mtu...vkp` — required.
3. `btnet_...vkp` — needed by the browser and elfs. Changes nothing while the elf isn't running.
   If the elf says «Old hook patch», undo `patches/old/...` and install the new one.

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
2. Start the elf. Tab «Link»: «Computer» → pick the PC → «Connect».
   Step by step with screenshots: [phone-manual/how-to-setup-elf.en.md](phone-manual/how-to-setup-elf.en.md).
3. Tab «Test» — speed tests. They work only while the phone is connected to a PC running `nap.sh`.
4. «Disconnect» drops the link, «Exit» quits.

Settings (the PC and auto-reconnect) can be changed in the elf itself, on the «Link» tab,
or in the BCFG editor: `zbin\etc\BTPAN.bcfg`.

## Limitations

- While the elf is running, Java Bluetooth (JSR-82) doesn't work.
- While the elf is connected to the PC, all of the phone's internet (browser, Java, elfs) goes
  through the PC; mobile internet (GPRS) is not used.
- If it doesn't work after «Disconnect» → «Connect» or an elf restart, reboot the phone.
- Not tested with a SIM card.

## Building

ElfLoader 3 SDK in `../sdk` relative to `source/`; in it, replace
`swilib/include/swilib/socket.h` with the file from `source/sdk-patch/`. Then:

```sh
cd source && cmake -B build -S . && cmake --build build
```
