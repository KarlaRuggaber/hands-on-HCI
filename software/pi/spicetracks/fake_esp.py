"""Stand-in for the ESP32, so the Pi software runs on a laptop without hardware.
Touches are simulated from the web page (buttons "Berühren") or the keyboard (keys 1-4 + Enter)."""
import asyncio
import logging
import random
import sys

log = logging.getLogger("fake-esp")


class FakeEsp:
    def __init__(self):
        self.inbox: asyncio.Queue = None
        self.moist = [2200, 1800, 2500, 2000]
        self.pump_until = 0.0
        self.sent = []                      # last commands, handy in tests

    async def start(self):
        self.inbox = asyncio.Queue()
        asyncio.create_task(self._status())
        try:
            asyncio.get_running_loop().add_reader(sys.stdin, self._stdin)
        except (NotImplementedError, ValueError, OSError):
            pass
        self.inbox.put_nowait({"t": "hello", "fw": "fake"})

    def _stdin(self):
        line = sys.stdin.readline().strip()
        if line in ("1", "2", "3", "4"):
            self.touch(int(line) - 1)

    def touch(self, plant):
        self.inbox.put_nowait({"t": "touch", "plant": plant, "raw": 0})

    async def _status(self):
        while True:
            await asyncio.sleep(2)
            self.moist = [max(1300, min(2900, m + random.randint(-5, 15))) for m in self.moist]
            self.n = getattr(self, "n", 0) + 1
            sensing = self.n % 15 == 0                  # moisture sensors briefly on, touch pauses meanwhile
            tst = ["pause" if sensing else "ok"] * 4
            self.inbox.put_nowait({"t": "status", "moist": self.moist, "light": random.randint(800, 2000),
                                   "tank": 1, "pump": False, "homed": True, "sense": sensing,
                                   "moistAge": (self.n % 15) * 2, "tst": tst})

    def send(self, **msg):
        self.sent = (self.sent + [msg])[-50:]
        if msg.get("cmd") == "water":
            p, ms = msg["plant"], min(msg.get("ms", 3000), 10000)
            self.moist[p] = max(1300, self.moist[p] - ms // 10)

            async def done():
                await asyncio.sleep(ms / 1000)
                self.inbox.put_nowait({"t": "watered", "plant": p, "ms": ms})
            asyncio.get_running_loop().create_task(done())
        elif msg.get("cmd") != "eq":
            log.info("-> ESP %s", msg)
        return True

    async def messages(self):
        while True:
            yield await self.inbox.get()
