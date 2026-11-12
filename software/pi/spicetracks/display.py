"""Backlight of the Touch Display 2. Off = black screen, touch still works, so a tap can wake it.
Needs write access to /sys/class/backlight, see README (udev rule)."""
import glob
import logging
import os

log = logging.getLogger("display")


class Backlight:
    def __init__(self):
        found = sorted(glob.glob("/sys/class/backlight/*"))
        self.dev = found[0] if found else None
        self.saved = None
        if not self.dev:
            log.info("no backlight found (laptop or HDMI): night mode only darkens the page")

    def _read(self, name):
        with open(os.path.join(self.dev, name)) as f:
            return int(f.read().strip())

    def _write(self, name, value):
        try:
            with open(os.path.join(self.dev, name), "w") as f:
                f.write(str(value))
        except OSError as e:
            log.warning("backlight %s: %s (udev rule missing?)", name, e)

    def off(self):
        if not self.dev:
            return
        try:
            self.saved = self._read("brightness") or self._read("max_brightness")
        except OSError:
            self.saved = None
        self._write("brightness", 0)

    def on(self):
        if not self.dev:
            return
        try:
            level = self.saved or self._read("max_brightness")
        except OSError:
            level = 255
        self._write("brightness", level)
