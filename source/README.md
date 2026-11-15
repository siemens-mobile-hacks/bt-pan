# bt-pan — интернет на телефоне через Bluetooth PAN

Эльф для BenQ-Siemens EL71 sw45 и E71 sw45 (и S65 sw58, см. ниже). Телефон подключается к ПК по Bluetooth PAN, ПК
раздаёт интернет. Браузер, Java и эльфы ходят в сеть как через GPRS.

Основан на PoC от ARMenian Developer.

## Сборка
```sh
cmake -B build -S . && cmake --build build
```
Получаются `build/bt-pan_ELKA.elf` (EL71/E71 sw45), `build/bt-pan_SG.elf` (S65 sw58, SK65 sw50) и
отладочные `bt-pan_SG_TRACE.elf`, `bt-pan_SG_LOG.elf`, `bt-pan_ELKA_TRACE.elf`.

## S65 sw58 и SK65 sw50
- Патчи: `library-s65-sw58-extended.vkp`, `bt-pan-mtu_S65v58.vkp`, для браузера без SIM —
  `bt-pan-browser_S65v58.vkp`; на SK65 то же с `_SK65v50` (`patches/sk65/`).
- Без патча MTU: на ПК `sudo MTU=320 ./nap.sh`.
- На S65/SK65 нет слоя BTA, Bluetooth через объекты CA (`src/bt_sg.h`).

## Запуск
1. Поставить патчи из релиза: `patches/el71/` (EL71), `patches/e71/` (E71), `patches/s65/` (S65) или `patches/sk65/` (SK65).
2. ПК: `sudo ./nap.sh`
3. Телефон: запустить эльф, выбрать ПК, "Connect".

Что в каком файле — `SOURCES.txt`.
