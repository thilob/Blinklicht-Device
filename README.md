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

> Hinweis: Dieses Projekt wurde unter Zuhilfenahme von **KI-Assistenz (ChatGPT)** erstellt.

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
- **Ein großer Elektrolytkondensator** (470 µF bis 1000 µF, 6,3 V oder mehr) **zwischen 3,3 V und GND**, möglichst nah am ESP32.  
- **Ein Keramikkondensator** (100 nF) direkt am Board als schnelle Stütze.  
- Falls die LEDs über externe Treiber/Transistoren geschaltet werden, dort ebenfalls einen Elko einbauen.  

So werden Spannungsschwankungen abgefangen und der ESP32 läuft stabil.

---

## Schaltplan (vereinfacht)

### Prinzipdarstellung (ASCII)

```
        +5V ----+-----------------------------+
                |                             |
              [Regler 3.3V]                   |
                |                             |
               ESP32                          |
          +-----+-----+                       |
          |           |                       |
      GPIOx       GPIOy ...                   |
       |            |                         |
      [R]          [R]                        |
       |            |                         |
      LED          LED                        |
       |            |                         |
      GND---------- GND-----------------------+
                |
          [470µF Elko]
                |
               GND
```

- `[R]` = Vorwiderstand (220–470 Ω)  
- `[470µF Elko]` = Pufferkondensator gegen Brownouts  

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

---

## Fazit

Mit diesem Projekt erhältst du eine **flexible Steuerung für Modellbau-Blaulichter**:  
- Komfortabel über **Web-GUI**  
- Alternativ robust über **CLI**  

Dank externer **Pufferkondensatoren** läuft der ESP32 stabil, auch wenn mehrere LEDs gleichzeitig blinken.  
Die Software ist modular aufgebaut und lässt sich leicht erweitern.  
