# bt-pan — интернет на телефоне через Bluetooth PAN

Эльф для BenQ-Siemens EL71 sw45 и E71 sw45. Телефон подключается к ПК по Bluetooth PAN, ПК
раздаёт интернет. Браузер, Java и эльфы ходят в сеть как через GPRS.

Основан на PoC от ARMenian Developer.

## Сборка
```sh
cmake -B build -S . && cmake --build build
```

## Запуск
1. Поставить патчи из `patches/` (EL71) или `patches/e71/` (E71).
2. ПК: `sudo ./nap.sh`
3. Телефон: запустить эльф, выбрать ПК, "Connect".
