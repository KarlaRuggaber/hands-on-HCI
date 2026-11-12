"""Adds the LED settings to an existing config.yaml, keeps everything else (port, playlists, ...) as it is.
Run once on the Pi:  python add_led_config.py   (it does nothing if the settings are already there)"""
import re
import shutil
import sys

import yaml

PATH = sys.argv[1] if len(sys.argv) > 1 else "config.yaml"

THEMES = [  # dark (quiet), main, bright (loud) per plant, in the order of the plants
    ([0, 45, 8], [0, 200, 35], [90, 255, 50]),      # Basilikum: deep green, green, fresh lime
    ([35, 55, 0], [120, 200, 0], [215, 255, 20]),   # Oregano: olive, yellow-green, light yellow-green
    ([110, 50, 0], [255, 150, 0], [255, 215, 40]),  # Currykraut: amber, gold, sunny yellow
    ([80, 10, 0], [230, 45, 0], [255, 95, 25]),     # Zimtblätter: dark red-brown, red-orange, coral
]

text = open(PATH).read()
if re.search(r"^\s+(led|use_white_chip|max_ma):", text, re.M):
    sys.exit("LED-Einstellungen sind schon drin, nichts geändert.")

lines, out, k = text.split("\n"), [], 0
for line in lines:
    out.append(line)
    m = re.match(r"^(\s+)color:\s*\[", line)
    if m and k < len(THEMES):                       # a plant's screen colour: LED colours go right below
        ind = m.group(1)
        d, mn, b = THEMES[k]
        out += [f"{ind}led:                 # LED colours: dark = quiet, main, bright = loud (equalizer, touch flash)",
                f"{ind}  dark: {d}",
                f"{ind}  main: {mn}",
                f"{ind}  bright: {b}"]
        k += 1
    if re.match(r"^\s+music_brightness:", line):     # end of the light: block
        ind = re.match(r"^(\s+)", line).group(1)
        out += [f"{ind}use_white_chip: false   # true: white part of a colour on the white chip (colours get paler)",
                f"{ind}max_ma: 1500            # current limit for all LEDs together",
                f"{ind}music_theme:            # music started from the phone, no plant chosen",
                f"{ind}  dark: [60, 25, 5]",
                f"{ind}  main: [255, 140, 50]",
                f"{ind}  bright: [255, 200, 120]"]

new = "\n".join(out)
cfg = yaml.safe_load(new)                           # check before writing
assert all("led" in p for p in cfg["plants"]) and "max_ma" in cfg["light"], "konnte nicht alles einfügen"
shutil.copy(PATH, PATH + ".bak")
open(PATH, "w").write(new)
print(f"{k} Pflanzen + Licht ergänzt. Alte Datei: {PATH}.bak")
