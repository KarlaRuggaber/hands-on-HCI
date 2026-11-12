"""The brain: reacts to touches, sensor values and Spotify, and tells the ESP32 what to do."""
import asyncio
import datetime as dt
import json
import logging
import os
import time

log = logging.getLogger("app")

STATE_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "state.json")
DOUBLE_TAP_S = 1.2            # Lukas' touch needs 0.2 s on + 0.3 s off, so a double tap takes about a second
TOUCH_LOCK_S = 1.5            # one hand often triggers several plants at once: after a touch, other plants are ignored this long


class App:
    def __init__(self, cfg, esp, spotify, cfg_path=None):
        self.cfg, self.esp, self.sp = cfg, esp, spotify
        self.cfg_path = cfg_path                # config.yaml is re-read when it changes
        self._cfg_mtime = os.path.getmtime(cfg_path) if cfg_path and os.path.exists(cfg_path) else None
        # touch: ok, cal, pause, lock, stuck (from the ESP32); sensing: moisture sensors are on right now
        self.plants = [dict(name=p["name"], raw=None, pct=None, last_water=0.0, touch=None, sensing=False, moist_at=None)
                       for p in cfg["plants"]]
        self.profile = cfg["default_profile"]
        self.playing_plant = None          # plant whose playlist runs (or ran, when paused)
        self.playing = False
        self.track = None
        self.light_raw = None
        self.tank_ok = True
        self.esp_ok = False
        self.events = []                   # short log for the screen
        self.listeners = set()             # websocket queues
        self._pending_pause = None
        self._last_touch = (None, 0.0)
        self._sent_state = {}
        self._touch_accept = (None, 0.0)
        self.player_reset = 0              # counted up: the display reconnects its Spotify player
        self._replay_plant = None          # play this again once the player is back
        if hasattr(spotify, "on_stuck"):
            spotify.on_stuck = self._spotify_stuck
        self.night = False
        from .display import Backlight
        self.backlight = Backlight()
        self._load()

    # ---------- persistence ----------
    def _load(self):
        try:
            data = json.load(open(STATE_FILE))
            for p, t in zip(self.plants, data.get("last_water", [])):
                p["last_water"] = t
            self.profile = data.get("profile", self.profile)
        except (OSError, ValueError):
            pass

    def _save(self):
        json.dump({"last_water": [p["last_water"] for p in self.plants], "profile": self.profile}, open(STATE_FILE, "w"))

    # ---------- helpers ----------
    def event(self, text):
        log.info(text)
        self.events = ([dt.datetime.now().strftime("%H:%M ") + text] + self.events)[:8]
        self.push()

    def snapshot(self):
        prof = self.cfg["profiles"].get(self.profile, {})
        return dict(plants=self.plants, profile=self.profile, profile_name=prof.get("name", self.profile),
                    profiles={k: v.get("name", k) for k, v in self.cfg["profiles"].items()},
                    playing=self.playing, playing_plant=self.playing_plant, track=self.track,
                    thresholds=[p["water_below"] for p in self.cfg["plants"]], volume=getattr(self.sp, "volume", 60),
                    tank_ok=self.tank_ok, esp_ok=self.esp_ok, dark=self.is_dark(), events=self.events,
                    colors=[p["color"] for p in self.cfg["plants"]], night=self.night, player_reset=self.player_reset)

    def push(self):
        snap = self.snapshot()
        for q in list(self.listeners):
            q.put_nowait(snap)

    def is_dark(self):
        if not self.cfg["light"].get("ldr", True):
            return True
        return self.light_raw is not None and self.light_raw < self.cfg["light"]["dark_below"]

    def pct(self, i, raw):
        c = self.cfg["plants"][i]
        span = c["dry_raw"] - c["wet_raw"]          # works both ways: wetter = lower (capacitive) or higher value
        if raw is None or abs(span) < 1:
            return None
        p = (c["dry_raw"] - raw) / span * 100
        return max(0, min(100, round(p)))

    def light_rest(self):
        """no music: sun across the roof, plant pieces show the state. A bright room makes it a bit stronger."""
        if self.night:
            self.esp.send(cmd="mode", mode="off")
            return
        lvl = self.cfg["light"]["idle_brightness"] * (1.0 if self.is_dark() else 1.5)
        self.esp.send(cmd="bright", level=round(min(1.0, lvl), 2))
        self.esp.send(cmd="mode", mode="idle")

    def plant_state(self, i):
        p, thr = self.plants[i]["pct"], self.cfg["plants"][i]["water_below"]
        if p is None:
            return "none"
        return "dry" if p < thr else "wet" if p > 85 else "ok"

    def send_states(self, force=False):
        for i in range(len(self.plants)):
            st = self.plant_state(i)
            if force or st != self._sent_state.get(i):
                self._sent_state[i] = st
                self.esp.send(cmd="plant", plant=i, state=st)

    def send_time(self):
        now = dt.datetime.now()
        self.esp.send(cmd="time", h=round(now.hour + now.minute / 60, 3))

    def send_setup(self):
        """everything the ESP32 takes from config.yaml: LED colours per plant, LED settings, time, states, light."""
        self._setup_at = time.time()
        light = self.cfg["light"]
        self.esp.send(cmd="ledcfg", white=bool(light.get("use_white_chip", False)), max_ma=int(light.get("max_ma", 1500)))
        for i, p in enumerate(self.cfg["plants"]):
            led = p.get("led")
            if led:
                self.esp.send(cmd="theme", plant=i, dark=led["dark"], main=led["main"], bright=led["bright"])
        if light.get("music_theme"):                      # music started from the phone, no plant chosen
            m = light["music_theme"]
            self.esp.send(cmd="theme", plant=4, dark=m["dark"], main=m["main"], bright=m["bright"])
        pins = [p.get("touch_pin") for p in self.cfg["plants"]]
        if all(isinstance(x, int) for x in pins):          # which touch screw belongs to which plant
            self.esp.send(cmd="touchmap", pins=pins)
        sens = self.cfg.get("sensors", {})                 # moisture sensors only on for short measurements
        self.esp.send(cmd="sensecfg", every_s=int(float(sens.get("moist_every_min", 10)) * 60),
                      after_water_s=int(sens.get("after_water_s", 45)))
        steps = self.cfg.get("distributor", {}).get("steps")
        if steps:                                          # stepper position per plant
            self.esp.send(cmd="steps", pos=[int(x) for x in steps])
        self.send_time()
        self.send_states(force=True)
        if self.playing and self.playing_plant is not None and not self.night:
            self.esp.send(cmd="bright", level=light["music_brightness"])
            self.esp.send(cmd="mode", mode="zone", plant=self.playing_plant)
        else:
            self.light_rest()

    def reload_config(self):
        """config.yaml was saved: take over colours, brightness and volume without a restart."""
        if not self.cfg_path:
            return
        try:
            mtime = os.path.getmtime(self.cfg_path)
        except OSError:
            return
        if mtime == self._cfg_mtime:
            return
        self._cfg_mtime = mtime
        import yaml
        try:
            new = yaml.safe_load(open(self.cfg_path))
            assert len(new["plants"]) == len(self.plants), "Anzahl Pflanzen geändert, bitte neu starten"
        except Exception as e:
            self.event(f"config.yaml nicht übernommen: {e}")
            return
        self.cfg = new
        if self.profile not in new["profiles"]:
            self.profile = new["default_profile"]
        vol = new.get("spotify", {}).get("volume")
        if vol is not None:
            asyncio.create_task(self.sp.set_volume(vol))
        self.send_setup()
        self.event("config.yaml neu geladen")

    # ---------- music ----------
    async def play_plant(self, i):
        uri = self.cfg["profiles"][self.profile]["playlists"][i]
        self.esp.send(cmd="bright", level=self.cfg["light"]["music_brightness"])
        self.esp.send(cmd="mode", mode="zone", plant=i)            # instant feedback, EQ takes over once sound runs
        ok = await self.sp.play(uri)
        if ok:
            self.playing_plant, self.playing = i, True
            self.event(f"{self.plants[i]['name']}: Musik an")
            asyncio.create_task(self.refresh_track(1.2))
        else:
            self.event("Spotify nicht erreichbar")
            self.light_rest()

    async def pause(self):
        await self.sp.pause()
        self.playing = False
        self.event("Pause")
        self.light_rest()

    async def sleep(self):
        """night mode: music off, LEDs off, screen dark. A tap on the screen or a plant wakes everything."""
        if self.playing:
            await self.pause()
        self.night = True
        self.esp.send(cmd="mode", mode="off")
        self.backlight.off()
        self.event("Nachtmodus")

    def wake(self):
        if not self.night:
            return
        self.night = False
        self.backlight.on()
        self.light_rest()
        self.event("Guten Morgen")

    async def on_plant_touch(self, i):
        """touch from the ESP32. Several plants within a moment = one hand: only the first one counts."""
        now = time.time()
        last_i, last_t = self._touch_accept
        name = self.plants[i]["name"] if 0 <= i < len(self.plants) else i
        if last_i is not None and last_i != i and now - last_t < TOUCH_LOCK_S:
            log.info("Touch %s ignoriert (kam mit %s)", name, self.plants[last_i]["name"])
            return
        self._touch_accept = (i, now)
        log.info("Touch %s", name)
        await self.on_touch(i)

    def _spotify_stuck(self, uri):
        """Spotify says ok, but the player on the display does not play: reconnect it, then play again."""
        self._replay_plant = self.playing_plant
        self.player_reset += 1
        self.event("Spotify-Player hängt, wird neu verbunden")

    async def player_ready(self):
        if self._replay_plant is not None:
            i, self._replay_plant = self._replay_plant, None
            await asyncio.sleep(1)
            await self.play_plant(i)

    async def on_touch(self, i):
        if self.night:
            self.wake()                      # a plant wakes the box, the touch then plays as usual
        now = time.time()
        last_i, last_t = self._last_touch
        self._last_touch = (i, now)
        # double tap on the playing plant: next song
        if self._pending_pause and last_i == i and now - last_t < DOUBLE_TAP_S:
            self._pending_pause.cancel()
            self._pending_pause = None
            await self.sp.next()
            self.event(f"{self.plants[i]['name']}: nächster Song")
            return
        if self.playing and self.playing_plant == i:
            # single tap on the playing plant: pause, but wait a moment for a possible second tap
            async def later():
                await asyncio.sleep(DOUBLE_TAP_S)
                self._pending_pause = None
                await self.pause()
            self._pending_pause = asyncio.create_task(later())
        elif not self.playing and self.playing_plant == i:
            await self.sp.resume()
            self.playing = True
            self.esp.send(cmd="bright", level=self.cfg["light"]["music_brightness"])
            self.esp.send(cmd="mode", mode="zone", plant=i)
            self.event(f"{self.plants[i]['name']}: weiter")
        else:
            await self.play_plant(i)                              # other plant: switch playlist

    async def set_profile(self, pid):
        if pid not in self.cfg["profiles"]:
            return False
        self.profile = pid
        self._save()
        self.event(f"Profil: {self.cfg['profiles'][pid].get('name', pid)}")
        if self.playing and self.playing_plant is not None:
            await self.play_plant(self.playing_plant)              # same plant, this person's music
        return True

    def on_eq(self, bands, beat):
        if self.playing:
            self.esp.send(cmd="eq", b=bands, beat=1 if beat else 0)
        elif time.time() - getattr(self, "_eq_note", 0) > 30:
            self._eq_note = time.time()
            log.info("eq: Ton da, aber Spotify spielt laut App nicht auf der Box, Lichter bleiben ruhig")

    # ---------- water ----------
    def water(self, i, ms=None, why="von Hand"):
        if not self.tank_ok:
            self.event("Tank leer, nicht gegossen")
            return False
        ms = ms or self.cfg["plants"][i]["water_ms"]
        self.esp.send(cmd="water", plant=i, ms=int(ms))
        self.event(f"{self.plants[i]['name']}: giessen ({why})")
        return True

    def auto_water(self):
        w = self.cfg["watering"]
        if not w["auto"]:
            return
        h = dt.datetime.now().hour
        q0, q1 = w["quiet_hours"]
        if (q0 > q1 and (h >= q0 or h < q1)) or (q0 < q1 and q0 <= h < q1):
            return
        for i, p in enumerate(self.plants):
            c = self.cfg["plants"][i]
            if p["pct"] is not None and p["pct"] < c["water_below"] \
                    and time.time() - p["last_water"] > w["min_gap_hours"] * 3600:
                p["last_water"] = time.time()      # set now, so the next status does not trigger again
                self._save()
                self.water(i, why=f"{p['pct']} %")
                return                             # one plant at a time

    # ---------- loops ----------
    async def esp_loop(self):
        async for msg in self.esp.messages():
            t = msg.get("t")
            if t == "hello":
                self.esp_ok = True
                self.send_setup()
                self.event("ESP32 verbunden, Touch kalibriert 30 s: Pflanzen nicht berühren")
            elif t == "status":
                if not self.esp_ok:                         # ESP32 was already running (no restart on connect)
                    self.esp_ok = True
                    self.send_setup()
                for i, raw in enumerate(msg.get("moist", [])[:4]):
                    self.plants[i]["raw"] = raw                     # None until the first measurement
                    self.plants[i]["pct"] = self.pct(i, raw)
                age = msg.get("moistAge", -1)
                tst = msg.get("tst") or []
                for i, p in enumerate(self.plants):
                    p["sensing"] = bool(msg.get("sense", False))
                    p["touch"] = tst[i] if i < len(tst) else None
                    p["moist_at"] = time.time() - age if isinstance(age, (int, float)) and age >= 0 else None
                self.send_states()
                was_dark = self.is_dark()
                self.light_raw = msg.get("light")
                self.tank_ok = bool(msg.get("tank", 1))
                if not self.playing and was_dark != self.is_dark():
                    self.light_rest()
                self.auto_water()
                self.push()
            elif t == "touch":
                await self.on_plant_touch(int(msg["plant"]))
            elif t == "watered":
                i = int(msg["plant"])
                if not 0 <= i < len(self.plants):        # pump test without a plant
                    continue
                self.plants[i]["last_water"] = time.time()
                self._save()
                self.event(f"{self.plants[i]['name']}: {msg.get('ms', 0) / 1000:.1f} s gegossen")
            elif t in ("stuck", "unstuck"):
                i = int(msg.get("plant", -1))
                if 0 <= i < len(self.plants):
                    self.plants[i]["touch"] = "stuck" if t == "stuck" else "ok"
                    self.event(f"{self.plants[i]['name']}: Touch-Messung hängt" if t == "stuck"
                               else f"{self.plants[i]['name']}: Touch läuft wieder")
            elif t == "cal" and int(msg.get("plant", 0)) == len(self.plants) - 1:
                self.event("Touch kalibriert")
            elif t == "error":
                if msg.get("msg") == "bad_json" and time.time() - getattr(self, "_setup_at", 0) < 5:
                    log.info("ESP32 hat beim Verbinden eine Zeile verloren (%s), Einstellungen nochmals", msg.get("line"))
                    self._setup_at = 0                       # only once
                    self.send_setup()
                else:
                    self.event(f"ESP32: {msg.get('msg')} {msg.get('line', '')}".strip())

    async def spotify_loop(self):
        """keeps the box in sync when someone pauses or changes music from the phone."""
        while True:
            await asyncio.sleep(5)
            st = await self.sp.state()
            if not st:
                continue
            playing = st["playing"] and st.get("on_box", True)
            if playing != self.playing:
                self.playing = playing
                if not playing:
                    self.light_rest()
            self.set_track(st)

    def set_track(self, st):
        if st.get("track"):
            self.track = {k: st.get(k) for k in ("track", "artist", "album", "image", "progress_ms", "duration_ms")}
            self.track["at"] = time.time()          # the screen counts the progress on from here
        self.push()

    async def refresh_track(self, wait=0.8):
        """after a button: Spotify needs a moment, then show the new song right away."""
        await asyncio.sleep(wait)
        st = await self.sp.state()
        if st:
            self.playing = st["playing"] and st.get("on_box", True)
            self.set_track(st)

    async def control(self, what):
        if what == "toggle":
            if self.playing:
                await self.pause()
            elif self.playing_plant is not None:
                await self.on_touch(self.playing_plant)
            else:
                await self.play_plant(0)
        elif what == "next":
            await self.sp.next()
        elif what == "previous":
            await self.sp.previous()
        asyncio.create_task(self.refresh_track())

    async def keepalive(self):
        n = 0
        while True:
            await asyncio.sleep(10)
            self.esp.send(cmd="ping")
            self.reload_config()
            n += 1
            if n % 6 == 0:                      # every minute: the clock for the sun in the roof
                self.send_time()
