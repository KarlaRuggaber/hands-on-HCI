"""Spotify Web API: start, pause and switch music on the Pi's own Spotify Connect device (librespot).

The music itself never goes through this code. librespot plays it; the Web API only says
"play this playlist on the device called Spicetracks". Needs Spotify Premium.
"""
import asyncio
import logging
import os
import time

log = logging.getLogger("spotify")

SCOPES = ("user-modify-playback-state user-read-playback-state user-read-currently-playing "
          "streaming user-read-email user-read-private")      # streaming: the player in the display's browser
CACHE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".spotify_token")


def make_auth():
    from spotipy.oauth2 import SpotifyOAuth
    return SpotifyOAuth(scope=SCOPES, cache_path=CACHE, open_browser=False,
                        redirect_uri=os.environ.get("SPOTIPY_REDIRECT_URI", "http://127.0.0.1:8888/callback"))


def login_cli():
    """One-time login: python -m spicetracks login"""
    auth = make_auth()
    print("1. Diese Adresse im Browser öffnen und bei Spotify anmelden:\n")
    print(auth.get_authorize_url())
    print("\n2. Danach landet der Browser auf einer Seite, die nicht lädt (127.0.0.1...).")
    url = input("   Die ganze Adresse aus der Adresszeile hier einfügen: ").strip()
    code = auth.parse_response_code(url)
    auth.get_access_token(code, as_dict=False)
    print("Gespeichert in", CACHE)


class Spotify:
    def __init__(self, device_name: str, volume: int = 60):
        import spotipy
        self.sp = spotipy.Spotify(auth_manager=make_auth(), requests_timeout=8, retries=2)
        self.device_name = device_name
        self.volume = volume
        self.device_id = None
        self._dev_checked = 0.0
        self._lock = asyncio.Lock()        # one command after the other, never two plays at once
        self._verify_task = None
        self.on_stuck = None               # set by the app: called when the player does not start

    async def _run(self, fn, *a, **kw):
        return await asyncio.get_running_loop().run_in_executor(None, lambda: fn(*a, **kw))

    def token(self):
        """access token for the Web Playback SDK in the browser (refreshed automatically)."""
        return self.sp.auth_manager.get_access_token(as_dict=False)

    def set_device(self, dev_id):
        """the browser player reports its device id as soon as it is ready."""
        self.device_id = dev_id
        self._dev_checked = time.time()

    async def device(self):
        """id of the librespot device; looked up again at most every 30 s."""
        if self.device_id and time.time() - self._dev_checked < 30:
            return self.device_id
        res = await self._run(self.sp.devices)
        self._dev_checked = time.time()
        for d in res.get("devices", []):
            if d["name"].lower() == self.device_name.lower():
                self.device_id = d["id"]
                return self.device_id
        log.warning("Spotify device %r not found, is librespot running?", self.device_name)
        self.device_id = None
        return None

    async def play(self, uri: str):
        async with self._lock:
            dev = await self.device()
            if not dev:
                return False
            kw = {"context_uri": uri} if ":track:" not in uri else {"uris": [uri]}
            try:
                await self._run(self.sp.start_playback, device_id=dev, **kw)
                await self._run(self.sp.shuffle, True, device_id=dev)
                await self._run(self.sp.volume, self.volume, device_id=dev)
            except Exception as e:
                log.error("play failed: %s", e)
                self.device_id = None
                return False
        self._cancel_verify()
        self._verify_task = asyncio.create_task(self._verify(uri, dev, kw))
        return True

    def _cancel_verify(self):
        if self._verify_task and not self._verify_task.done():
            self._verify_task.cancel()
        self._verify_task = None

    async def _verify(self, uri, dev, kw):
        """Spotify answers 'ok' even when the player on the display is stuck. Check, try once more, then report."""
        for attempt in (1, 2):
            await asyncio.sleep(3)
            try:
                pb = await self._run(self.sp.current_playback)
            except Exception:
                return
            if pb and pb.get("is_playing") and (pb.get("context") or {}).get("uri", uri) == uri:
                return
            log.warning("Spotify spielt %s nicht (Prüfung %d)", uri, attempt)
            if attempt == 1:
                async with self._lock:
                    try:
                        await self._run(self.sp.transfer_playback, dev, force_play=False)
                        await self._run(self.sp.start_playback, device_id=dev, **kw)
                    except Exception as e:
                        log.warning("zweiter Versuch: %s", e)
        if self.on_stuck:
            self.on_stuck(uri)

    async def pause(self):
        self._cancel_verify()
        try:
            await self._run(self.sp.pause_playback, device_id=self.device_id)
        except Exception as e:
            log.debug("pause: %s", e)

    async def resume(self):
        dev = await self.device()
        try:
            await self._run(self.sp.start_playback, device_id=dev)
        except Exception as e:
            log.debug("resume: %s", e)

    async def next(self):
        try:
            await self._run(self.sp.next_track, device_id=self.device_id)
        except Exception as e:
            log.debug("next: %s", e)

    async def set_volume(self, v: int):
        self.volume = max(0, min(100, int(v)))
        try:
            await self._run(self.sp.volume, self.volume, device_id=self.device_id)
        except Exception as e:
            log.debug("volume: %s", e)

    async def state(self):
        """what is playing right now (also when someone pauses from the phone)."""
        try:
            pb = await self._run(self.sp.current_playback)
        except Exception as e:
            log.debug("state: %s", e)
            return None
        if not pb:
            return {"playing": False}
        item = pb.get("item") or {}
        album = item.get("album") or {}
        images = album.get("images") or []
        return {
            "playing": pb.get("is_playing", False),
            "on_box": (pb.get("device") or {}).get("name", "").lower() == self.device_name.lower(),
            "context": (pb.get("context") or {}).get("uri"),
            "track": item.get("name"),
            "artist": ", ".join(a["name"] for a in item.get("artists", [])),
            "album": album.get("name"),
            "image": images[0]["url"] if images else None,          # largest cover first
            "progress_ms": pb.get("progress_ms") or 0,
            "duration_ms": item.get("duration_ms") or 0,
        }

    async def previous(self):
        """like a CD player: back to the start of the song, or the song before when already at the start."""
        try:
            pb = await self._run(self.sp.current_playback)
            if pb and (pb.get("progress_ms") or 0) > 4000:
                await self._run(self.sp.seek_track, 0, device_id=self.device_id)
            else:
                await self._run(self.sp.previous_track, device_id=self.device_id)
        except Exception as e:
            log.debug("previous: %s", e)


class NoSpotify:
    """used with --no-spotify: pretends to play a few songs, so the screen can be tried without Spotify."""
    SONGS = [("Erster Song", "Testband", "Kräuteralbum", 214000), ("Zweiter Song", "Testband", "Kräuteralbum", 187000),
             ("Dritter Song", "Andere Band", "Gartenmusik", 245000)]

    def __init__(self):
        self.i, self.playing, self.started, self.pos = 0, False, time.time(), 0

    def _progress(self):
        return self.pos + (int((time.time() - self.started) * 1000) if self.playing else 0)

    async def play(self, uri):
        log.info("[spotify] play %s", uri)
        self.i, self.pos, self.started, self.playing = (self.i + 1) % 3, 0, time.time(), True
        return True

    async def pause(self):
        self.pos, self.playing = self._progress(), False

    async def resume(self):
        self.started, self.playing = time.time(), True

    async def next(self):
        self.i, self.pos, self.started = (self.i + 1) % 3, 0, time.time()

    async def previous(self):
        if self._progress() < 4000:
            self.i = (self.i - 1) % 3
        self.pos, self.started = 0, time.time()

    async def set_volume(self, v):
        log.info("[spotify] volume %s", v)

    async def state(self):
        t, a, al, d = self.SONGS[self.i]
        return {"playing": self.playing, "on_box": True, "track": t, "artist": a, "album": al, "image": "/static/demo-cover.svg",
                "progress_ms": min(self._progress(), d), "duration_ms": d}
