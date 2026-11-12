"""Start: python -m spicetracks [--fake-esp] [--no-spotify] [--no-eq]
One-time Spotify login: python -m spicetracks login"""
import argparse
import asyncio
import logging
import os
import sys

import yaml
from aiohttp import web

from .app import App
from .web import make_app

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_env(path=os.path.expanduser("~/.config/spicetracks.env")):
    """read SPOTIPY_* from ~/.config/spicetracks.env, so nobody has to `source` it by hand."""
    try:
        for line in open(path):
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))
    except OSError:
        pass


async def main(args):
    cfg = yaml.safe_load(open(args.config))
    fake = None
    if args.fake_esp:
        from .fake_esp import FakeEsp
        esp = fake = FakeEsp()
    else:
        from .esp_link import EspLink
        esp = EspLink(cfg["serial"]["port"], cfg["serial"]["baud"])
    await esp.start()

    if args.no_spotify:
        from .spotify_ctl import NoSpotify
        sp = NoSpotify()
    else:
        from .spotify_ctl import Spotify
        sp = Spotify(cfg["spotify"]["device_name"], cfg["spotify"]["volume"])

    app = App(cfg, esp, sp, cfg_path=args.config)
    tasks = [app.esp_loop(), app.spotify_loop(), app.keepalive()]
    if not args.no_eq:
        from .audio_eq import AudioEq
        eq = cfg["eq"]
        tasks.append(AudioEq(eq["source"], eq["fps"], eq["delay_ms"], on_frame=app.on_eq).run())

    runner = web.AppRunner(make_app(app, fake))
    await runner.setup()
    await web.TCPSite(runner, "0.0.0.0", cfg["web"]["port"]).start()
    logging.info("UI on http://localhost:%s", cfg["web"]["port"])
    await asyncio.gather(*tasks)


def cli():
    load_env()
    if len(sys.argv) > 1 and sys.argv[1] == "login":
        from .spotify_ctl import login_cli
        login_cli()
        return
    ap = argparse.ArgumentParser(prog="spicetracks")
    ap.add_argument("--config", default=os.path.join(HERE, "config.yaml"))
    ap.add_argument("--fake-esp", action="store_true", help="run without ESP32 (simulated sensors)")
    ap.add_argument("--no-spotify", action="store_true", help="run without Spotify")
    ap.add_argument("--no-eq", action="store_true", help="no audio analysis")
    ap.add_argument("-v", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.v else logging.INFO, format="%(asctime)s %(name)s %(message)s")
    asyncio.run(main(args))


if __name__ == "__main__":
    cli()
