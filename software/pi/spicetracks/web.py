"""Small web server: touch screen UI, phone page behind the NFC sticker, live updates over a websocket."""
import asyncio
import json
import os

from aiohttp import web

WEB_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web")


def make_app(app, fake_esp=None):
    routes = web.RouteTableDef()

    @routes.get("/")
    async def index(_):
        return web.FileResponse(os.path.join(WEB_DIR, "index.html"))

    @routes.get("/api/state")
    async def state(_):
        return web.json_response(app.snapshot())

    @routes.get("/p/{pid}")                 # NFC sticker: http://spicetracks.local:8080/p/thierry
    async def profile(req):
        await app.set_profile(req.match_info["pid"])
        raise web.HTTPFound("/")

    @routes.post("/api/profile/{pid}")
    async def profile_post(req):
        ok = await app.set_profile(req.match_info["pid"])
        return web.json_response({"ok": ok})

    @routes.post("/api/water/{i}")
    async def water(req):
        return web.json_response({"ok": app.water(int(req.match_info["i"]))})

    @routes.post("/api/play/{i}")           # same as touching the plant
    async def play(req):
        await app.on_touch(int(req.match_info["i"]))
        return web.json_response({"ok": True})

    @routes.post("/api/touch/{i}")          # only with --fake-esp: simulates a finger on the plant
    async def touch(req):
        if fake_esp:
            fake_esp.touch(int(req.match_info["i"]))
        return web.json_response({"ok": bool(fake_esp)})

    @routes.post("/api/pause")
    async def pause(_):
        await app.pause()
        return web.json_response({"ok": True})

    @routes.post("/api/music/{what}")      # toggle, next, previous
    async def music(req):
        what = req.match_info["what"]
        if what not in ("toggle", "next", "previous"):
            raise web.HTTPNotFound()
        await app.control(what)
        return web.json_response({"ok": True})

    @routes.get("/api/token")              # only for the player on the display itself
    async def token(req):
        if req.remote not in ("127.0.0.1", "::1") or not hasattr(app.sp, "token"):
            raise web.HTTPForbidden()
        tok = await asyncio.get_running_loop().run_in_executor(None, app.sp.token)
        return web.json_response({"token": tok})

    @routes.post("/api/device/{dev}")
    async def device(req):
        if hasattr(app.sp, "set_device"):
            app.sp.set_device(req.match_info["dev"])
            app.event("Spotify-Player bereit")
            asyncio.create_task(app.player_ready())
        return web.json_response({"ok": True})

    @routes.post("/api/sdklog")             # errors of the Spotify player in the display's browser
    async def sdklog(req):
        d = await req.json()
        app.event(f"Spotify-Player {d.get('kind', '?')}: {str(d.get('msg', ''))[:100]}")
        return web.json_response({"ok": True})

    @routes.post("/api/night")
    async def night(_):
        await app.sleep()
        return web.json_response({"ok": True})

    @routes.post("/api/wake")
    async def wake(_):
        app.wake()
        return web.json_response({"ok": True})

    @routes.post("/api/volume/{v}")
    async def volume(req):
        await app.sp.set_volume(int(req.match_info["v"]))
        return web.json_response({"ok": True})

    @routes.get("/ws")
    async def ws(req):
        sock = web.WebSocketResponse(heartbeat=20)
        await sock.prepare(req)
        q = asyncio.Queue()
        app.listeners.add(q)
        try:
            await sock.send_str(json.dumps(app.snapshot()))
            while not sock.closed:
                snap = await q.get()
                while not q.empty():            # only the newest state matters
                    snap = q.get_nowait()
                await sock.send_str(json.dumps(snap))
        finally:
            app.listeners.discard(q)
        return sock

    w = web.Application()
    w.add_routes(routes)
    w.router.add_static("/static", WEB_DIR)
    w["fake"] = bool(fake_esp)
    return w
