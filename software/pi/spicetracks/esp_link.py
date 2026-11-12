"""Serial link to the ESP32: one JSON object per line in both directions."""
import asyncio
import json
import logging
import threading
import time

log = logging.getLogger("esp")


class EspLink:
    def __init__(self, port: str, baud: int = 115200):
        self.port, self.baud = port, baud
        self.ser = None
        self.loop = None
        self.inbox: asyncio.Queue = None
        self._wlock = threading.Lock()

    async def start(self):
        self.loop = asyncio.get_running_loop()
        self.inbox = asyncio.Queue()
        threading.Thread(target=self._reader, daemon=True).start()

    def _open(self):
        import serial
        while True:
            try:
                s = serial.Serial(self.port, self.baud, timeout=1, exclusive=True)   # hwtest and app never share the port
                log.info("connected to %s", self.port)
                return s
            except Exception as e:                     # unplugged, wrong port, ESP rebooting
                log.warning("no ESP32 on %s (%s), retrying", self.port, e)
                time.sleep(2)

    def _reader(self):
        buf = b""
        while True:
            if self.ser is None:
                self.ser = self._open()
            try:
                chunk = self.ser.read(256)
            except Exception as e:
                log.warning("serial lost: %s", e)
                self.ser = None
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except ValueError:
                    log.debug("not json: %r", line)       # boot messages of the ESP32
                    continue
                self.loop.call_soon_threadsafe(self.inbox.put_nowait, msg)

    def send(self, **msg):
        data = (json.dumps(msg, separators=(",", ":")) + "\n").encode()
        with self._wlock:
            if self.ser is None:
                return False
            try:
                self.ser.write(data)
                return True
            except Exception as e:
                log.warning("write failed: %s", e)
                return False

    async def messages(self):
        while True:
            yield await self.inbox.get()
