#!/usr/bin/env python3
"""
Standalone test server for WebDataWidget (firmware/src/widgets/webdatawidget/).

WebDataWidget polls a URL you configure via WEB_DATA_WIDGET_URL in config.h and
draws whatever JSON comes back, independently across all 5 physical screens
(indexes 0-4, one entry per screen in the "displays" array). This script serves
that JSON with no dependencies beyond the Python standard library, so you can
point a real device at your dev machine and see it react without deploying any
backend.

Usage:
    python webdata_test_server.py [--port 8000] [--interval-ms 4000]

Then in firmware/config/config.h:
    #define WEB_DATA_WIDGET_URL "http://<this-machine-ip>:8000/"

Every "interval-ms" (also reported to the device via the "interval" field, so
it knows how often to re-poll), the server advances to the next demo scene.
Each scene shows a different combination of screens so you can see:
  - plain text with a label (screen 0)
  - custom colors, no label (screen 1)
  - a mix of drawing primitives: rectangle + text + triangle, similar to the
    stock ticker examples in stocks.php (screen 2)
  - circle / line / arc / character primitives together (screen 3)
  - fullDraw forcing a full repaint every poll vs. a screen that reports no
    change so the firmware skips repainting it (screen 4)

Watch the server's console log: it prints which scene is being served on each
request, so you can correlate "device just repainted" with "server just
changed the payload".
"""

import argparse
import http.server
import json
import time

SCREEN_SIZE = 240  # matches SCREEN_SIZE in firmware/config/config.h.template


def scene_text_with_label(tick):
    return {
        "label": "Uptime",
        "data": f"{tick} polls",
        "labelColor": "cyan",
        "color": "white",
        "background": "black",
        "fullDraw": True,
    }


def scene_plain_colored_text(tick):
    # Alternates color each tick to make redraws obvious on this screen only.
    color = "green" if tick % 2 == 0 else "red"
    return {
        "data": f"tick {tick}",
        "color": color,
        "background": "black",
        "fullDraw": True,
    }


def scene_stock_style_card(tick):
    up = tick % 2 == 0
    price = 100 + (tick % 20)
    return {
        "fullDraw": True,
        "data": [
            {"type": "rectangle", "x": 0, "y": 0, "width": SCREEN_SIZE, "height": 50, "filled": True, "color": "blue"},
            {"type": "text", "x": 120, "y": 25, "font": 1, "size": 3, "alignment": "mc", "text": "DEMO", "color": "white", "background": "blue"},
            {"type": "rectangle", "x": 0, "y": 50, "width": SCREEN_SIZE, "height": 190, "filled": True, "color": "black"},
            {"type": "text", "x": 120, "y": 110, "font": 1, "size": 4, "alignment": "mc", "text": f"${price}", "color": "white", "background": "black"},
            {
                "type": "triangle",
                "x": 120, "y": 160 if up else 200,
                "x2": 140, "y2": 200 if up else 160,
                "x3": 100, "y3": 200 if up else 160,
                "filled": True,
                "color": "green" if up else "red",
            },
        ],
    }


def scene_shape_playground(tick):
    # Layout note: these are round (GC9A01) displays -- only a circle inscribed
    # in the 240x240 logical canvas is actually visible, so anything placed near
    # a corner of that square gets clipped by the physical bezel. Keep shapes
    # within roughly 90px of screen center (120,120) to stay fully visible.
    # TFT_eSPI's drawArc() angle convention: x,y is the arc's CENTER, and angles
    # are measured in degrees clockwise starting at the 6 o'clock (bottom)
    # position -- angleStart=0/angleEnd=270 below draws 3/4 of a ring, leaving
    # the bottom-right slice open.
    return {
        "label": "",
        "fullDraw": True,
        "data": [
            {"type": "circle", "x": 70, "y": 70, "radius": 25, "color": "red", "filled": tick % 2 == 0},
            {"type": "line", "x": 40, "y": 120, "x2": 200, "y2": 120, "color": "cyan"},
            {"type": "arc", "x": 150, "y": 70, "radius": 25, "innerRadius": 15, "angleStart": 0, "angleEnd": 270, "color": "yellow", "background": "black"},
            {"type": "character", "x": 100, "y": 140, "character": chr(ord("A") + (tick % 26)), "font": 4, "size": 3, "color": "white", "background": "black"},
        ],
    }


def scene_static_no_change(tick):
    # Same payload every tick (ignores tick) -> WebDataModel.isChanged() stays
    # false after the first draw, so the firmware should stop repainting this
    # screen even though the server keeps getting polled. fullDraw only matters
    # on a call that actually happens, so it's safe to set unconditionally here
    # too -- it still won't cause extra redraws once the data stops changing.
    return {
        "label": "Static",
        "data": "I never change",
        "color": "silver",
        "fullDraw": True,
    }


SCENE_BUILDERS = [
    scene_text_with_label,
    scene_plain_colored_text,
    scene_stock_style_card,
    scene_shape_playground,
    scene_static_no_change,
]


def build_payload(tick, interval_ms):
    return {
        "interval": interval_ms,
        "displays": [builder(tick) for builder in SCENE_BUILDERS],
    }


def make_handler(interval_ms):
    start_time = time.time()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            tick = int((time.time() - start_time) * 1000 // interval_ms)
            payload = build_payload(tick, interval_ms)
            body = json.dumps(payload).encode("utf-8")

            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

            print(f"[{time.strftime('%H:%M:%S')}] served tick={tick} to {self.client_address[0]}")

        def log_message(self, format, *args):
            pass  # quiet default logging; we print our own line above

    return Handler


def guess_lan_ip():
    """Best-effort guess at this machine's LAN IP (the one reachable from the ESP32),
    without actually sending any traffic. Falls back to a placeholder if detection fails
    (e.g. no network connectivity) -- always double check against `ipconfig`/`ifconfig`
    if multiple network adapters are present."""
    import socket

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))  # no packets sent, just picks the outbound interface
            return s.getsockname()[0]
    except OSError:
        return "<your-machine-lan-ip>"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--interval-ms", type=int, default=4000, help="how often scenes advance and how often the device should re-poll")
    args = parser.parse_args()

    lan_ip = guess_lan_ip()
    server = http.server.HTTPServer(("0.0.0.0", args.port), make_handler(args.interval_ms))
    print(f"Serving WebDataWidget demo payloads on http://0.0.0.0:{args.port}/")
    print(f"Point WEB_DATA_WIDGET_URL at this machine's LAN IP: http://{lan_ip}:{args.port}/")
    print("(if this machine has multiple network adapters, verify against `ipconfig`/`ifconfig` -- pick the one on the same network as the ESP32)")
    print(f"Scenes advance every {args.interval_ms}ms. Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
