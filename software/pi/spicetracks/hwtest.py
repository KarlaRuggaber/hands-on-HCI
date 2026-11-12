"""Hardware test on the real box: python -m spicetracks.hwtest [port]
Stop the service first (systemctl --user stop spicetracks), the port can only be used once."""
import json
import sys
import threading
import time

import serial
import yaml

MENU = """
  s  Status zeigen (Feuchte, Licht, Tank, Touch-Grundwerte)
  l  LED-Test: jede Zone, Equalizer-Welle, Ruhelicht, aus
  p  Pumpe 1 s laufen lassen (Schlauch in ein Glas!)
  h  Verteiler: Nullpunkt suchen
  g  Verteiler: Pflanze 1 bis 4 anfahren
  w  Pflanze giessen (Verteiler + Pumpe)
  t  30 s lang Berührungen anzeigen
  m  Touch live: Werte jeder Pflanze 60 s lang (ab Firmware 2.7)
  c  Touch neu kalibrieren (30 s nichts berühren)
  f  Feuchte jetzt messen (Sensoren 2 s an, Touch pausiert)
  q  Ende
"""
last_status = {}
NAMES = ["Basilikum ", "Oregano   ", "Currykraut", "Zimt      "]


def touch_line(m):
    """one line per update: how far above the resting value, against the threshold."""
    if m.get("cal"):
        return "   kalibriert gerade, nichts berühren ..."
    if m.get("lock"):
        return "   gesperrt (60 s nach dem Giessen)"
    if m.get("pause"):
        return "   Pause, Feuchtesensoren messen gerade"
    out = []
    stuck = m.get("stuck") or [0] * len(m["d"])
    raw = m.get("raw") or [0] * len(m["d"])
    pin = m.get("pin") or ["?"] * len(m["d"])
    for n, d, t, on, st, r, p in zip(NAMES, m["d"], m["thr"], m["on"], stuck, raw, pin):
        head = f"{n[:4]}(T{p}) {r / 1000:6.0f}k"   # roh: Schraube in der Erde ca. 1000k bis 2000k, Luft ca. 10k bis 20k
        if st:
            out.append(f"{head} HAENGT              ")
            continue
        k = max(0.0, min(2.0, d / t if t else 0))
        bar = "#" * int(k * 5) + "." * (10 - int(k * 5))
        out.append(f"{head} {d:7.0f}/{t:5.0f} [{bar[:5]}|{bar[5:]}]{' BERUEHRT' if on else ''}")
    return "  ".join(out)


def reader(ser):
    global last_status
    while True:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            print("   ESP:", line)
            continue
        if msg.get("t") == "status":
            last_status = msg
        elif msg.get("t") == "tm":
            print(touch_line(msg))
        else:
            print("   <-", msg)


def send(ser, **m):
    ser.write((json.dumps(m) + "\n").encode())


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else yaml.safe_load(open("config.yaml"))["serial"]["port"]
    try:
        ser = serial.Serial(port, 115200, timeout=1, exclusive=True)   # nobody else may write at the same time
    except serial.SerialException as e:
        sys.exit(f"Port belegt ({e}). Zuerst: systemctl --user stop spicetracks, und kein zweites Programm offen lassen.")
    threading.Thread(target=reader, args=(ser,), daemon=True).start()
    time.sleep(2.5)
    cfg = yaml.safe_load(open("config.yaml"))
    steps = cfg.get("distributor", {}).get("steps")
    if steps:                                   # same distributor positions as the app
        send(ser, cmd="steps", pos=[int(x) for x in steps])
    pins = [p.get("touch_pin") for p in cfg["plants"]]
    if all(isinstance(x, int) for x in pins):   # same touch pin per plant as the app
        send(ser, cmd="touchmap", pins=pins)
    print("verbunden mit", port, MENU)
    while True:
        c = input("> ").strip().lower()
        if c == "q":
            send(ser, cmd="mode", mode="idle")
            break
        elif c == "s":
            print(json.dumps(last_status, indent=1) if last_status else "noch kein Status (2 s warten)")
        elif c == "l":
            send(ser, cmd="bright", level=0.5)
            for p in range(4):
                print("   Zone", p + 1); send(ser, cmd="mode", mode="zone", plant=p); time.sleep(2)
            print("   Equalizer-Welle")
            for k in range(150):
                b = [int(127 + 127 * __import__("math").sin(k / 5 + i)) for i in range(8)]
                send(ser, cmd="eq", b=b, beat=1 if k % 15 == 0 else 0); time.sleep(1 / 30)
            print("   Ruhelicht"); send(ser, cmd="mode", mode="idle"); time.sleep(3)
            print("   aus"); send(ser, cmd="mode", mode="off")
        elif c == "p":
            send(ser, cmd="pump", ms=1000)
        elif c == "h":
            send(ser, cmd="home")
        elif c == "g":
            send(ser, cmd="goto", plant=int(input("   Pflanze 1-4: ")) - 1)
        elif c == "w":
            send(ser, cmd="water", plant=int(input("   Pflanze 1-4: ")) - 1, ms=int(input("   ms (z.B. 2000): ")))
        elif c == "m":
            print("   Wert / Schwelle. Ab | (Schwelle erreicht) gilt es als Berührung. Ctrl+C beendet alles.")
            send(ser, cmd="touchmon", s=60)
            time.sleep(61)
        elif c == "c":
            send(ser, cmd="touchcal")
            print("   Kalibrierung läuft 30 s, nichts berühren")
        elif c == "f":
            send(ser, cmd="moist")
            print("   misst ... (Werte mit 's' in 3 s)")
        elif c == "t":
            print("   Pflanzen berühren, Ereignisse erscheinen hier ...")
            time.sleep(30)
        print(MENU)


if __name__ == "__main__":
    main()
