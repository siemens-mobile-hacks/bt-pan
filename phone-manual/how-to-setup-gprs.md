# Настройка GPRS-профиля на телефоне

[English version](how-to-setup-gprs.en.md)

## 1. Создать точку доступа (NAP)

**1.1.** Открываем `Connectivity → Online settings`.

![Connectivity → Online settings](gprs-setup/261019_213441.jpg)

**1.2.** Выбираем любой свободный профиль (здесь — `Context 05`) и жмём
`Options → Edit`.

![Выбор профиля Context 05](gprs-setup/261019_213452.jpg)

**1.3.** Переходим на последнюю вкладку — `NAP` — и жмём `Edit`.

![Вкладка NAP](gprs-setup/261019_213505.jpg)

**1.4.** В списке выбираем `EGPRS`.

![Выбор EGPRS](gprs-setup/261019_213510.jpg)

**1.5.** Вводим любое имя (`Name`) и любой `APN`. Логин и пароль можно оставить
пустыми. Сохраняем.

![Имя и APN](gprs-setup/261019_213520.jpg)

## 2. Привязать WAP-браузер

**2.1.** Переходим на вкладку `Applications`, выбираем `WAP browser` и жмём `Edit`.

![Applications → WAP browser](gprs-setup/261019_213533.jpg)

**2.2.** Выбираем `Profile one → Options → Edit`.

![Profile one](gprs-setup/261019_213537.jpg)

**2.3.** Вводим любое `Profile name` и открываем `Connect. list`.

![Профиль WAP-браузера](gprs-setup/261019_213546.jpg)

**2.4.** Список пустой — жмём `Options → Select conn.` и выбираем созданную ранее
точку доступа.

![Пустой список подключений](gprs-setup/261019_213551.jpg)
![Options → Select conn.](gprs-setup/261019_213555.jpg)

**2.5.** В списке появляется `01: GPRS` — наша точка доступа.

![Точка доступа добавлена](gprs-setup/261019_213600.jpg)

**2.6.** Обязательно сохраняем: `Options → Save`.

![Options → Save](gprs-setup/261019_213603.jpg)

## 3. Привязать Java

**3.1.** На вкладке `Applications` выбираем `Java` и жмём `Edit`.

![Applications → Java](gprs-setup/261019_213619.jpg)

**3.2.** Так же вводим `Profile name` и открываем список подключений.

![Профиль Java](gprs-setup/261019_213624.jpg)

**3.3.** `Options → Select conn.`, выбираем ту же точку доступа и сохраняем.

![Options → Select conn. для Java](gprs-setup/261019_213629.jpg)

## 4. Если у Java-приложения нет интернета

**4.1.** В опциях самого приложения выбираем `Online settings`.

![Опции приложения → Online settings](gprs-setup/261019_215256.jpg)

**4.2.** Редактируем `Gener. account` (`Edit`).

![Gener. account](gprs-setup/261019_215302.jpg)

**4.3.** Вводим любое имя профиля, выбираем созданную ранее точку доступа и
сохраняем.

![Выбор точки доступа в приложении](gprs-setup/261019_215310.jpg)
