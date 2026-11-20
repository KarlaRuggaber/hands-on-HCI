# Spicetracks Software

Stand: Firmware 2.14 und die dazugehörige Pi-Software.

Zwei Programme:

- **`firmware/`** läuft auf dem ESP32: Feuchte, Licht, Touch auf den Pflanzen, Pumpe, Verteiler und der LED-Streifen im Dach. Er entscheidet nichts, er führt aus und schützt (Pumpe stoppt immer selbst nach höchstens 10 s).
- **`pi/`** läuft auf dem Raspberry Pi: Logik, Spotify, Equalizer (zurzeit ausgeschaltet), Bedienoberfläche auf dem Display und die Seite hinter dem NFC-Sticker.

```
Pflanze berühren ─► ESP32 ──{"t":"touch","plant":1}──► Pi ──► Spotify Web API: "spiel Playlist X auf Gerät Spicetracks"
                                                       │                         │
                                                       │                         ▼
LED-Streifen ◄── {"cmd":"mode",...} ◄──────────────────┘      Browser auf dem Display spielt die Musik (Web Playback SDK)
             ◄── {"cmd":"eq",...} ◄── FFT ◄── Ton vom Lautsprecher   (nur mit eq.enabled: true)
```

## Ablauf im Betrieb

| Was passiert | Reaktion |
|---|---|
| Pflanze antippen | Playlist dieser Pflanze für das aktive Profil startet. Ihr Trog leuchtet in ihrer Farbe, im Dach breitet sich das Licht von ihr aus. |
| Spielende Pflanze kurz antippen | Pause (entschieden beim Loslassen), danach Ruhelicht |
| Spielende Pflanze etwa 1 s festhalten | nächster Song |
| Pausierte Pflanze antippen | weiter |
| Andere Pflanze antippen | Wechsel zu deren Playlist, der alte Song stoppt sofort |
| Mehrere Pflanzen gleichzeitig berührt | nur die Pflanze mit dem stärksten Ausschlag zählt |
| Handy an den NFC-Sticker | Seite `/p/<profil>` öffnet sich, Profil wechselt, laufende Musik wechselt auf die Playlist dieser Person |
| Feuchte unter Grenzwert | automatisch giessen, höchstens alle 6 h pro Pflanze, nicht nachts (22 bis 7 Uhr). Zurzeit mit simulierten Werten, siehe Demo-Modus. |
| Keine Musik | Sonnenlauf im Dach, Zustand der Pflanzen am Trog. |
| Pi weg (Absturz, Neustart) | ESP32 fällt nach 30 s ins Ruhelicht zurück, Pumpe bleibt aus |

## Protokoll Pi ↔ ESP32

USB-Kabel, 115200 Baud, eine JSON-Nachricht pro Zeile.

ESP32 an Pi:
```
{"t":"hello","fw":"spicetracks-2.14","core":"<Arduino-ESP32-Version>","idf":"<ESP-IDF-Version>"}
{"t":"status","moist":[1612,2840,2210,1500],"moistAge":240,"sense":false,"light":2300,"tank":1,
 "touch":[0,0,1,0],"tst":["ok","ok","ok","stuck"],"pump":false,"homed":true}   alle 2 s
   moist = letzte Messung (null bis zur ersten), moistAge = Sekunden seither, sense = Feuchtesensoren gerade an,
   tst = Touch je Pflanze: ok, cal (kalibriert), pause (Feuchte misst), lock (nach dem Giessen), stuck (Messung hängt)
{"t":"touch","plant":2,"ms":0}   {"t":"release","plant":2,"ms":1200}   (ms = wie lange berührt)
{"t":"stuck","plant":3}   {"t":"unstuck","plant":3}
{"t":"watered","plant":3,"ms":4000}
{"t":"error","msg":"tank_empty"}        weitere: busy, bad_plant, home_failed, bad_json
```
Pi an ESP32:
```
{"cmd":"water","plant":3,"ms":4000}     {"cmd":"stop"}     {"cmd":"home"}
{"cmd":"mode","mode":"zone","plant":2}  Modi: off, idle, zone, eq
{"cmd":"eq","b":[200,150,90,60,40,20,10,5],"beat":1}     8 Bänder 0..255, 30 mal pro Sekunde (nur mit Equalizer)
{"cmd":"theme","plant":0,"dark":[0,45,8],"main":[0,200,35],"bright":[90,255,50]}   {"cmd":"bright","level":1.0}
{"cmd":"ledcfg","white":false,"max_ma":1500}   {"cmd":"steps","pos":[515,345,170,0]}
{"cmd":"touchcfg","factor":3}   {"cmd":"touchmap","pins":[11,13,14,12]}   {"cmd":"touchcal"}
{"cmd":"sensecfg","every_s":600,"after_water_s":45}   {"cmd":"moist"} (Feuchte jetzt messen)
{"cmd":"time","h":13.5}   {"cmd":"plant","plant":1,"state":"dry"}  (ok, dry, wet, none)   {"cmd":"flash","plant":2}
{"cmd":"pump","ms":1000}   {"cmd":"goto","plant":2}   {"cmd":"ledtest"}   {"cmd":"touchmon","s":60}   nur zum Testen
{"cmd":"ping"}
```
Kommen keine `eq`-Pakete mehr (Pause, Stille, Equalizer aus), bleibt der ESP32 in der Pflanzenzone.

## Lichtkonzept

| | ohne Musik | mit Musik |
|---|---|---|
| Dach (28 LEDs) | Sonnenlauf: eine weiche Sonne wandert über den Tag von links nach rechts, morgens rosa-warm, mittags warmweiss, abends orange, nachts ein blauer Mond. Die Uhrzeit schickt der Pi jede Minute. | Licht breitet sich von der Stelle über der angetippten Pflanze nach beiden Seiten aus, danach ziehen die Farben der Pflanze langsam über das Dach. Mit eingeschaltetem Equalizer liegt der Bass in der Mitte, die Höhen aussen. Bei Pause zieht sich das Licht in die Pflanze zurück. |
| Pflanzen (je 3 LEDs) | Zustand: gut = Grün, durstig = oranges Pulsieren, sehr nass = Blau, kein Messwert = weiches Weiss | Die spielende Pflanze leuchtet hell in ihrer Farbe (mit Equalizer heller beim Bass), die anderen sind aus. |
| Antippen | kurzer Begrüssungsblitz in der Pflanzenfarbe, im Trog und im Dach darüber | Blitz nur im Trog |
| Giessen | blaue Tropfen laufen über die 3 LEDs der Pflanze | |

## LED-Streifen

Eine Kette mit 40 LEDs an GPIO 40 (330 Ω). Strom kommt aus der eigenen Powerbank (Intenso B10000, höchstens 2,4 A) mit 1000-µF-Kondensator am Eingang, nicht vom Breadboard-Netzteil. Gemeinsame Masse mit dem ESP32, wie im Steckplan:
- LED 0 bis 27 in der Nut des Dachs, LED 0 rechts am Kabelanfang. Jede LED kennt ihre Position und damit die nächste Pflanze (5, 9, 9, 5 LEDs).
- LED 28 bis 39 als 3er-Stücke innen am Trog, je eines pro Pflanze.

Strom ist in der Firmware auf 1,5 A begrenzt (`light.max_ma` in `config.yaml`). Helligkeit ohne und mit Musik steht unter `light.idle_brightness` und `light.music_brightness`, zurzeit beide auf 1.0.

Farben und Einstellungen: Alles steht in `pi/config.yaml`, nichts muss in die Firmware. Pro Pflanze `color` (Bildschirm) und `led` mit drei Farben (`dark` leise, `main`, `bright` laut), unter `light` die Helligkeit, `use_white_chip` und `max_ma`, unter `spotify` die Lautstärke, unter `touch` die Empfindlichkeit und unter `sensors` der Demo-Modus. Das laufende Programm liest die Datei alle 10 s neu ein und schickt die Werte an den ESP32, ein Neustart ist nicht nötig. Nur `serial`, `eq` und der Spotify-Gerätename brauchen einen Neustart. Ältere `config.yaml` einmal mit `python add_led_config.py` ergänzen.

Equalizer: Spotify gibt keine Audio-Analyse mehr heraus, deshalb kann der Pi mithören, was der Lautsprecher spielt (PipeWire-Monitor), 8 Frequenzbänder von 40 Hz bis 12 kHz rechnen und Bass-Schläge erkennen. Bluetooth-Lautsprecher hängen 150 bis 250 ms nach, `eq.delay_ms` gleicht das aus. **Zurzeit ist er aus** (`eq.enabled: false`), weil am Monitor bisher nur Stille ankommt. Das spart auch Rechenleistung auf dem Pi. Zum Testen `eq.enabled: true` setzen und das Programm neu starten.

## Einrichten

### 1. ESP32
1. Arduino IDE, Boardverwaltung: „esp32 by Espressif“ (Version 3.x). Board: „ESP32S3 Dev Module“, „USB CDC On Boot: Disabled“. Das USB-Kabel steckt in der Buchse „UART“ (COM), wie im Steckplan.
2. Bibliotheken: **Adafruit NeoPixel** und **ArduinoJson** (Version 7). FastLED wird nicht mehr gebraucht. Die Streifen sind RGBW (SK6812, 4 Byte pro LED). Sind Rot und Grün vertauscht, in der Firmware `NEO_GRBW` durch `NEO_RGBW` ersetzen. Beim Start zeigt der Streifen kurz Rot, Grün, Blau, Weiss: Daran sieht man, dass die Ansteuerung funktioniert.
3. `firmware/spicetracks_esp32/spicetracks_esp32.ino` öffnen, hochladen.
4. Serieller Monitor auf 115200: alle 2 s eine `status`-Zeile. Mit `{"cmd":"mode","mode":"zone","plant":1}` + Enter testen, ob Zone 2 leuchtet.

Feuchte und Touch: Ein eingeschalteter Feuchtesensor im Topf stört die Touch-Messung so stark, dass sie abbricht. Darum schaltet die Firmware die vier Feuchtesensoren nur kurz ein: Eingeschaltet wird alle 10 min und 45 s nach jedem Giessen für etwa 2 s, Touch pausiert in dieser Zeit. Hängt ein Touch-Kanal trotzdem, meldet die Firmware `stuck`, die anderen Pflanzen laufen weiter. Auf dem Bildschirm zeigt jede Pflanze zwei Symbole: Touch (an, kalibriert, Pause, hängt) und Feuchte (misst gerade oder aus).

Demo-Modus der Feuchte: Solange die Feuchtesensoren nicht kalibriert sind, steht in `config.yaml` `sensors.demo: true`. Dann rechnet der Pi mit erfundenen Werten: Jede Pflanze startet mit einem zufälligen, für sie typischen Wert, jedes Giessen erhöht ihn um 3 bis 6 %. Die echten Sensoren werden ignoriert. Nach dem Kalibrieren (siehe unten) auf `false` setzen.

Touch: `touch_lukas.h` ist Lukas' getestete Erkennung (v4). Sie läuft auf dem zweiten Kern, damit LEDs und Pi-Verbindung flüssig bleiben. Nach jedem Start kalibriert sie 30 s, in dieser Zeit die Pflanzen nicht berühren (die Board-LED blinkt blau). Nach dem Giessen pausiert sie 60 s und kalibriert dann neu. Seit Firmware 2.14 gilt immer nur eine Pflanze als berührt, die mit dem stärksten Ausschlag über ihrer Schwelle. Eine andere übernimmt erst, wenn sie 1,5-mal stärker ausschlägt. `touch.threshold_factor` in `config.yaml` multipliziert alle Schwellen: höher = unempfindlicher (1.0 = wie kalibriert, zurzeit 3). Auf dem Pi wird dieselbe Pflanze 2 s nach einem Musikbefehl ignoriert, damit ein nachfedernder Kontakt nicht gleich wieder pausiert.

Verteiler: Positionen pro Pflanze in `config.yaml` unter `distributor.steps`, zurzeit 515, 345, 170 und 0 Schritte (ohne Pi nutzt die Firmware 495, 330, 165, 0). Solange es keinen Endschalter gibt (`HAS_ENDSTOP = false`), zählt die Firmware ab der Stellung beim Einschalten: Verteiler vor dem Einschalten von Hand auf Pflanze 4 drehen (Schritt 0).

### 2. Raspberry Pi: Ton und Spotify-Player
Die Musik spielt im Browser auf dem Display (offizielles Spotify Web Playback SDK). librespot funktioniert mit vielen Konten nicht mehr (Spotify verweigert den Audio-Schlüssel) und wird nicht gebraucht.
1. Raspberry Pi OS mit Desktop. Bluetooth-Lautsprecher über das Bluetooth-Symbol koppeln und über das Lautsprecher-Symbol als Ausgabe wählen.
2. Chromium braucht Widevine, um geschützte Musik abzuspielen:
   ```
   sudo apt install -y libwidevinecdm0 pipewire-alsa
   ```
3. Falls librespot/raspotify von früher noch installiert ist, abschalten, sonst gibt es zwei Geräte mit demselben Namen:
   ```
   systemctl --user disable --now librespot 2>/dev/null; sudo systemctl disable --now raspotify 2>/dev/null
   ```
4. Das Dashboard muss auf dem Pi selbst unter `http://localhost:8080` offen sein (Kiosk, siehe Autostart). Nach dem Laden erscheint im Spotify-Gerätemenü „Spicetracks“; im Ereignis-Log steht „Spotify-Player bereit“. Auf dem Pi selbst schaltet die Seite automatisch in eine schlichte Variante ohne Unschärfe und bewegte Farbflächen, weil diese Effekte den Pi 4 so stark belasten, dass die Musik stockt.

Der Pi 4 läuft trotzdem am Limit: Die Reaktion auf eine Berührung ist leicht verzögert, und die Musik kann ab und zu stocken.

### 3. Spotify-App für die Steuerung
1. developer.spotify.com, Dashboard, „Create app“. Redirect URI: `http://127.0.0.1:8888/callback`, API: Web API.
2. Seit Februar 2026 gilt für solche Apps: Der Besitzer braucht Premium, höchstens 5 Nutzer, die unter „User Management“ eingetragen werden. Für euer Team reicht das.
3. `cp pi/systemd/spicetracks.env.example ~/.config/spicetracks.env` und Client ID und Secret eintragen.

### 4. Pi-Programm
```
sudo apt install -y python3-venv pulseaudio-utils chromium-browser
cd ~/spicetracks-software/pi
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
set -a; . ~/.config/spicetracks.env; set +a
python -m spicetracks login        # einmalig, Anleitung erscheint
ls /dev/serial/by-id/              # Namen in config.yaml bei serial.port eintragen
python -m spicetracks -v           # Probelauf
```
Dann `config.yaml` füllen:
- **Playlists:** In Spotify bei einer Playlist „Teilen, Link kopieren“, die ID zwischen `playlist/` und `?` als `spotify:playlist:<ID>` eintragen. Eigene Playlists nehmen, dann klappt es sicher.
- **Feuchte kalibrieren:** Die Oberfläche zeigt unter jeder Pflanze den Sensor-Rohwert. Sensor in der Luft ablesen = `dry_raw`, im Wasserglas = `wet_raw`. Danach `sensors.demo: false` setzen.
- **Touch:** Löst eine Pflanze zu leicht oder zu schwer aus, `touch.threshold_factor` anpassen (etwa 1.5 bis 3).
- **Licht:** Sobald der Lichtsensor angeschlossen ist, `light.ldr: true` setzen und `light.dark_below` auf den Wert, den die Statuszeile bei Dämmerung zeigt.

Autostart:
```
cp pi/systemd/spicetracks.service ~/.config/systemd/user/ && systemctl --user enable --now spicetracks
echo 'chromium --kiosk --noerrdialogs --autoplay-policy=no-user-gesture-required http://localhost:8080 &' >> ~/.config/labwc/autostart
```

### Nachtmodus
Der Mond-Knopf oben im Player pausiert die Musik, schaltet alle LEDs aus und dimmt das Display auf schwarz. Ein Tipp auf den Bildschirm oder auf eine Pflanze weckt alles wieder; der erste Tipp auf den Bildschirm drückt dabei keinen Knopf.

Damit das Programm die Display-Beleuchtung schalten darf, einmalig:
```
echo 'SUBSYSTEM=="backlight", RUN+="/bin/chmod 666 /sys/class/backlight/%k/brightness"' | sudo tee /etc/udev/rules.d/90-spicetracks-backlight.rules
sudo reboot
```

## Auf der Hardware testen

Schritt für Schritt, jedes Teil einzeln. Erst wenn ein Schritt klappt, den nächsten.

1. **ESP32 allein am Laptop:** Firmware hochladen, seriellen Monitor auf 115200 und „Neue Zeile“. Alle 2 s kommt eine `status`-Zeile.
2. **ESP32 am Pi:** USB einstecken, `ls /dev/serial/by-id/`, Namen in `config.yaml` eintragen. Dann das Testprogramm:
   ```
   cd ~/spicetracks-software/pi && source .venv/bin/activate
   python -m spicetracks.hwtest
   ```
   Menü: `s` Sensorwerte, `l` LED-Test (Zonen, Equalizer-Welle, Ruhelicht), `p` Pumpe 1 s (Schlauch in ein Glas), `h` und `g` Verteiler, `w` giessen, `t` Berührungen beobachten, `m` Touch-Werte live, `c` Touch neu kalibrieren, `f` Feuchte jetzt messen. Das Testprogramm übernimmt Verteiler-Positionen, Touch-Pins und Touch-Empfindlichkeit aus `config.yaml`.
3. **Pi-Programm ohne Spotify:** `python -m spicetracks --no-spotify --no-eq -v`. Pflanze berühren: im Log erscheint `touch`, die Zone leuchtet.
4. **Mit Spotify:** `python -m spicetracks --no-eq -v`. Pflanze berühren: Musik startet auf dem Lautsprecher.
5. **Mit Equalizer:** `eq.enabled: true` in `config.yaml`, dann `python -m spicetracks -v`. Im Log steht alle 10 s der Pegel; „nur Stille“ heisst, dass am Monitor kein Ton ankommt. Passt der Takt nicht, `eq.delay_ms` anpassen (Licht zu früh: Wert erhöhen).
6. **Dauerlauf:** eine Stunde laufen lassen, dabei giessen, Musik wechseln, Pi neu starten.

Läuft der Dienst schon, zuerst `systemctl --user stop spicetracks`, sonst ist der USB-Port besetzt.

## Deployen

- **Code vom Laptop auf den Pi:** `pi/deploy.sh` (Standard-Ziel `gruppe4@raspberrypi.local`, ein anderes als Argument angeben) kopiert alles, installiert neue Pakete und startet den Dienst neu. Alternativ den Ordner in ein Git-Repo legen und auf dem Pi `git pull` + `systemctl --user restart spicetracks`.
- **Firmware vom Pi aus flashen:** einmalig arduino-cli auf dem Pi einrichten (Befehle oben in `pi/flash.sh`), danach genügt `pi/flash.sh`. Das Skript pausiert den Dienst, lädt hoch und startet ihn wieder.
- **Logs:** `journalctl --user -u spicetracks -f`. Fehler des Spotify-Players im Browser landen ebenfalls dort.
- **Neustart:** `systemctl --user restart spicetracks`.

## Ohne Hardware testen
```
python -m spicetracks --fake-esp --no-spotify --no-eq
```
Dann `http://localhost:8080/?test` öffnen: Jede Pflanze hat einen Knopf „Blatt berühren (Test)“, im Terminal gehen auch die Tasten 1 bis 4 + Enter.

## Dateien
```
firmware/spicetracks_esp32/spicetracks_esp32.ino   ESP32 (Firmware 2.14)
firmware/spicetracks_esp32/touch_lukas.h           Touch-Erkennung v4
firmware/spicetracks_esp32/led_colors.h            Farbrechnung für die LEDs
pi/config.yaml                 Pflanzen, LED-Farben, Helligkeit, Lautstärke, Profile, Playlists, Grenzwerte,
                               Touch-Empfindlichkeit, Equalizer an/aus, Demo-Modus der Feuchte
pi/spicetracks/app.py          Logik: Touch, Musik, Giessen, Licht, Demo-Feuchte
pi/spicetracks/esp_link.py     serielle Verbindung
pi/spicetracks/spotify_ctl.py  Spotify Web API
pi/spicetracks/audio_eq.py     Equalizer (FFT, Beat)
pi/spicetracks/web.py          Web-Server
pi/spicetracks/hwtest.py       Testmenü für die echte Hardware
pi/deploy.sh, pi/flash.sh      Code und Firmware auf die Box bringen
pi/web/index.html              Oberfläche auf dem Display
pi/systemd/                    Autostart
```
