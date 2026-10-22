# Setting up a GPRS profile on the phone

[Русская версия](how-to-setup-gprs.md)

## 1. Create an access point (NAP)

**1.1.** Open `Connectivity → Online settings`.

![Connectivity → Online settings](gprs-setup/261019_213441.jpg)

**1.2.** Pick any free profile (here `Context 05`) and press `Options → Edit`.

![Choosing profile Context 05](gprs-setup/261019_213452.jpg)

**1.3.** Go to the last tab, `NAP`, and press `Edit`.

![NAP tab](gprs-setup/261019_213505.jpg)

**1.4.** Choose `EGPRS` in the list.

![Choosing EGPRS](gprs-setup/261019_213510.jpg)

**1.5.** Enter any `Name` and any `APN`. Login and password can stay empty. Save.

![Name and APN](gprs-setup/261019_213520.jpg)

## 2. Assign it to the WAP browser

**2.1.** Go to the `Applications` tab, select `WAP browser` and press `Edit`.

![Applications → WAP browser](gprs-setup/261019_213533.jpg)

**2.2.** Select `Profile one → Options → Edit`.

![Profile one](gprs-setup/261019_213537.jpg)

**2.3.** Enter any `Profile name` and open `Connect. list`.

![WAP browser profile](gprs-setup/261019_213546.jpg)

**2.4.** The list is empty: press `Options → Select conn.` and choose the access point created above.

![Empty connection list](gprs-setup/261019_213551.jpg)
![Options → Select conn.](gprs-setup/261019_213555.jpg)

**2.5.** `01: GPRS` appears in the list — that's our access point.

![Access point added](gprs-setup/261019_213600.jpg)

**2.6.** Be sure to save: `Options → Save`.

![Options → Save](gprs-setup/261019_213603.jpg)

## 3. Assign it to Java

**3.1.** On the `Applications` tab select `Java` and press `Edit`.

![Applications → Java](gprs-setup/261019_213619.jpg)

**3.2.** Again enter a `Profile name` and open the connection list.

![Java profile](gprs-setup/261019_213624.jpg)

**3.3.** `Options → Select conn.`, choose the same access point and save.

![Options → Select conn. for Java](gprs-setup/261019_213629.jpg)

## 4. If a Java app has no internet

**4.1.** In the app's own options choose `Online settings`.

![App options → Online settings](gprs-setup/261019_215256.jpg)

**4.2.** Edit `Gener. account` (`Edit`).

![Gener. account](gprs-setup/261019_215302.jpg)

**4.3.** Enter any profile name, choose the access point created above and save.

![Choosing the access point in the app](gprs-setup/261019_215310.jpg)
