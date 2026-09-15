#!/usr/bin/env python3
"""
Standalone demo client for OrbIt's "gauge" control (firmware/src/widgets/orbitwidget/controls/GaugeControl.h).

Unlike webdata_test_server.py (which WebDataWidget *pulls* from on its own polling schedule),
orbit-api is a REST server running ON the device - this script is a small *client* that pushes
POST updates to it, simulating a live CPU/memory-style gauge with a random walk (bounded random
steps each tick, so it drifts believably rather than jumping erratically). No dependencies beyond
the Python standard library.

Usage:
    python gauge_demo.py <device-ip> [--screen 4] [--interval 2] [--label CPU] [--style ring] [--color cyan]

Example:
    python gauge_demo.py 192.168.4.41 --screen 4 --label CPU --style instrument --color gold

Find <device-ip> the same way as for webdata_test_server.py: check the serial monitor's WiFi
connection line, or your router's DHCP client list.
"""

import argparse
import json
import random
import time
import urllib.error
import urllib.request


def post_gauge(host, screen, label, value, style, color, min_value, max_value):
    url = "http://{}/orbit/api/v1/screens/{}".format(host, screen)
    body = json.dumps({
        "control": "gauge",
        "params": {
            "label": label,
            "value": round(value, 1),
            "min": min_value,
            "max": max_value,
            "style": style,
            "color": color,
        },
    }).encode("utf-8")

    request = urllib.request.Request(url, data=body, method="POST", headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def random_walk(current, minimum, maximum, max_step):
    step = random.uniform(-max_step, max_step)
    return max(minimum, min(maximum, current + step))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host", help="the device's LAN IP, e.g. 192.168.4.41")
    parser.add_argument("--screen", type=int, default=4, help="which screen (0-4) to drive")
    parser.add_argument("--interval", type=float, default=2.0, help="seconds between updates")
    parser.add_argument("--label", default="CPU")
    parser.add_argument("--style", choices=["ring", "speedometer", "instrument"], default="ring")
    parser.add_argument("--color", default="cyan")
    parser.add_argument("--min", type=float, default=0.0, dest="min_value")
    parser.add_argument("--max", type=float, default=100.0, dest="max_value")
    parser.add_argument("--max-step", type=float, default=6.0, help="largest random change per tick, keeps the walk believable")
    args = parser.parse_args()

    value = (args.min_value + args.max_value) / 2.0
    print("Driving screen {} on {} as a '{}' gauge (Ctrl+C to stop)".format(args.screen, args.host, args.label))

    try:
        while True:
            value = random_walk(value, args.min_value, args.max_value, args.max_step)
            try:
                result = post_gauge(args.host, args.screen, args.label, value, args.style, args.color, args.min_value, args.max_value)
                print("[{}] {} -> {}".format(time.strftime("%H:%M:%S"), args.label, result["params"]["value"]))
            except (urllib.error.URLError, ConnectionError, TimeoutError) as error:
                print("[{}] request failed: {}".format(time.strftime("%H:%M:%S"), error))
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
