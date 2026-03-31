# ESP32 Blaulicht-Controller mit Web-UI & CLI

Dieses Projekt ist ein **Blaulicht-Controller für Modellbau und ähnliche Anwendungen**.  
Die Software läuft auf einem ESP32-DevKit und steuert bis zu **8 LEDs (Blaulichter)** über PWM-Ausgänge.  

---

## Features

- Bis zu **8 Ausgänge** (PWM, LEDC-Hardware des ESP32, 5 kHz, 8 Bit Helligkeit)  
- **Mehrere Blinkmuster (Patterns)**, die frei definierbar sind  
- **Gruppen-Funktion**: LEDs lassen sich zu Gruppen zusammenfassen und gemeinsam ein-/ausschalten  
- **Phasenverschiebung**: gleiche Muster können zeitlich versetzt starten  
- **Weboberfläche (GUI)**: läuft direkt im Browser (Handy, Tablet, PC), responsive gestaltet  
- **Serielle Konsole (CLI)**: alle Funktionen auch über USB-Terminal steuerbar  
- **Speicherung im Flash** (LittleFS): Konfiguration überlebt Neustarts  
- **WLAN-Betrieb**: Access Point (AP) oder Station (STA, DHCP) mit mDNS (z. B. `http://blaulicht-xxxx.local`)  
- **Captive Portal**: Im AP-Modus wird die Weboberfläche automatisch geöffnet  
- **Brownout-Schutz**: externe Kondensatoren empfohlen (siehe unten)  

---

## Hardware-Aufbau

### ESP32-DevKit
- Verwendet wird ein Standard-ESP32-DevKit (mit USB-Anschluss und Spannungsregler).  

### LED-Ausgänge
- Bis zu 8 Pins frei wählbar (z. B. GPIO21, 22, 18, 19, 23, 25, 26, 27).  
- Jeder Ausgang steuert eine LED oder LED-Baugruppe.  
- Vor jede LED gehört ein **Vorwiderstand** (typisch 220–470 Ω bei 5 V Betrieb).  

### Versorgung & Brownout-Schutz
Der ESP32 ist empfindlich gegenüber Spannungseinbrüchen („Brownout“).  
Das passiert besonders dann, wenn mehrere LEDs gleichzeitig leuchten oder wenn das WLAN viel Strom zieht.

👉 Deshalb unbedingt folgende Maßnahmen:
- **Ein großer Elektrolytkondensator** (470 µF bis 1000 µF, 6,3 V oder mehr) **zwischen 3,3 V und GND**, möglichst nah am ESP32 bzw. am `3V3`-Pin des Devkits.  
- **Ein Keramikkondensator** (100 nF) direkt am Board bzw. an `3V3`/`GND` als schnelle Stütze.  
- Wenn die Schaltung über USB versorgt wird und die LED-Last die `5V`-Schiene stark belastet, zusätzlich ein weiterer Elko **zwischen `USB 5V` und `GND`** nahe am Devkit oder an der Last.  

So werden Spannungsschwankungen abgefangen und der ESP32 läuft stabil.

---

## Schaltplan (vereinfacht)

### Prinzipdarstellung (ASCII)

```  
USB 5V ----+-----------------------------+
           |                             |
      [DevKit-Regler 3.3V]               |
           |                             |
          3V3 ---+---- ESP32             |
                 |      +-----+-----+    |
             [470uF]    |           |    |
                 |  GPIOx       GPIOy ...|
                GND  |            |      |
                    [R]          [R]     |
                     |            |      |
                    LED          LED     |
                     |            |      |
GND -----------------+------------+------+

Optional bei hoher LED-Last auf USB/5V:

USB 5V ---+--- [zus. Elko 470uF..1000uF] --- GND
```

- `[R]` = Vorwiderstand (220–470 Ω)  
- `[470uF]` = Pufferkondensator zwischen `3.3V` und `GND` gegen Brownouts  
- `[zus. Elko 470uF..1000uF]` = optionaler Puffer zwischen `USB 5V` und `GND`, wenn die `5V`-Versorgung durch LED-Last stark einbricht  

---

## Software-Prinzip

### Patterns (Blinkmuster)
Ein **Pattern** ist eine Liste von Schritten:
- `duration_ms` – Dauer in Millisekunden  
- `level` – Ziel-Helligkeit (0..255)  
- `fade` – weicher Übergang (true) oder harter Sprung (false)  

Beispiel für ein Doppelblitz-Pattern:

```cpp
{30, 255, false},  // LED an
{30,   0, false},  // aus
{30, 255, false},  // LED an
{150, 0,  true},   // langsam ausblenden
{250, 0,  false},  // Pause
```

- Der **letzte Schritt ist die Pause**, bevor das Muster neu startet.  
- Pro Ausgang kann das Pattern individuell gewählt werden.  
- Jeder Ausgang kann zusätzlich eine **eigene Pausenlänge** erzwingen.  

### Gruppen
- Mehrere Ausgänge können zu **Gruppen** zusammengefasst werden.  
- Eine Gruppe kann komplett **ein- oder ausgeschaltet** werden.  
- Ist eine Gruppe aus, bleiben die LEDs dunkel (das Pattern pausiert).  

---

## CLI (Serielle Konsole)

Über USB (115200 Baud) öffnet sich ein Terminal.  
Man sieht, was man tippt (Echo-Funktion ist aktiv).

### Wichtige Befehle:

- `help` → alle Befehle anzeigen  
- `status` → Status von WLAN, Patterns, Gruppen und Ausgängen  
- `wifi on|off` → WLAN/Webserver ein-/ausschalten  
- `mode ap|sta` → WLAN-Modus wählen  
- `sta <ssid> <pass>` → Zugangsdaten für Station-Modus setzen  
- `ap <ssid> [pass]` → SSID/PW für Access Point setzen  
- `dhcp` → im STA-Modus neue DHCP-Adresse beziehen  
- `pat list` → Patterns anzeigen  
- `pat add <name>` → neues Pattern anlegen  
- `pat step add <pidx> <dur> <lvl> <fade>` → Step zu Pattern hinzufügen  
- `group list` → Gruppen anzeigen  
- `group add <id> <name>` → Gruppe anlegen  
- `light list` → Ausgänge anzeigen  
- `light add <pin> <ch> <pidx> <rest> <gid> <phase>` → Ausgang hinzufügen  

---

## Weboberfläche (GUI)

Die **Web-UI** wird aus dem Flash geladen (`LittleFS`).  
- Im AP-Modus erreichbar unter: `http://192.168.4.1`  
  (Captive Portal öffnet die Seite oft automatisch)  
- Im STA-Modus unter: `http://<hostname>.local` oder über die vergebene DHCP-IP  

### Funktionen:
- WLAN konfigurieren (AP oder STA)  
- Patterns anlegen, bearbeiten, löschen  
- Gruppen verwalten und schalten  
- Ausgänge (Lights) hinzufügen, ändern, löschen  

Die Oberfläche ist **responsive** (Bootstrap-Layout) → funktioniert gut auf Handy, Tablet und PC.  

---

## Build & Flash (PlatformIO)

1. Projekt in **VS Code / PlatformIO** öffnen  
2. **Build** → Firmware kompilieren  
3. **Upload** → ESP32 flashen  
4. **Upload Filesystem Image** → Weboberfläche (index.html) ins LittleFS laden  
5. Serielle Konsole (115200 Baud) starten → Logausgaben prüfen  

### CLI-Beispiele

```bash
# Firmware für ESP32 DevKit V1 / ESP32-WROOM bauen
$HOME/.platformio/penv/bin/pio run -e devkit-v1

# Firmware hochladen
$HOME/.platformio/penv/bin/pio run -e devkit-v1 -t upload

# LittleFS-Image hochladen
$HOME/.platformio/penv/bin/pio run -e devkit-v1 -t uploadfs
```

### Upload-Port / udev-Hinweise unter Linux

Beim Upload kann PlatformIO z. B. folgende Meldungen anzeigen:

```text
Looking for upload port...

Warning! Please install `99-platformio-udev.rules`.
More details: https://docs.platformio.org/en/latest/core/installation/udev-rules.html

Auto-detected: /dev/ttyS0
Uploading .pio/build/devkit-v1/littlefs.bin
esptool.py v4.11.0
Serial port /dev/ttyS0
```

Das bedeutet meist:
- Die empfohlenen PlatformIO-`udev`-Regeln für USB-Seriell-Geräte sind noch nicht installiert.
- PlatformIO hat vermutlich den falschen Port erkannt.
- `/dev/ttyS0` ist oft **nicht** der USB-Port des ESP32, sondern eine interne serielle Schnittstelle des PCs.

Prüfe in so einem Fall die tatsächlich vorhandenen USB-Ports, z. B.:

```bash
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
```

Und gib den Upload-Port dann explizit an:

```bash
$HOME/.platformio/penv/bin/pio run -e devkit-v1 -t upload --upload-port /dev/ttyUSB0
$HOME/.platformio/penv/bin/pio run -e devkit-v1 -t uploadfs --upload-port /dev/ttyUSB0
```

Wenn du unter Linux häufiger Upload-Probleme oder Zugriffsfehler auf serielle Geräte hast, installiere die PlatformIO-`udev`-Regeln:
- Doku: https://docs.platformio.org/en/latest/core/installation/udev-rules.html

### Richtigen Port beim Einstecken finden

Wenn unklar ist, welches Device zu deinem ESP32 gehört, öffne vor dem Einstecken ein Kernel-Log:

```bash
journalctl -k -f
```

Alternativ:

```bash
dmesg --follow
```

Stecke dann den ESP32 an. Typische Meldungen nennen danach den neu angelegten Port, z. B.:

```text
cdc_acm 1-1.3:1.0: ttyACM0: USB ACM device
```

oder:

```text
ch341-uart converter now attached to ttyUSB0
```

Diesen Port kannst du anschließend direkt für Upload und Monitor verwenden:

```bash
$HOME/.platformio/penv/bin/pio run -e devkit-v1 -t upload --upload-port /dev/ttyACM0
$HOME/.platformio/penv/bin/pio device monitor --port /dev/ttyACM0 --baud 115200
```

---

## Fazit

Mit diesem Projekt erhältst du eine **flexible Steuerung für Modellbau-Blaulichter**:  
- Komfortabel über **Web-GUI**  
- Alternativ robust über **CLI**  

Dank externer **Pufferkondensatoren** läuft der ESP32 stabil, auch wenn mehrere LEDs gleichzeitig blinken.  
Die Software ist modular aufgebaut und lässt sich leicht erweitern.  
