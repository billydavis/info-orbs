#!/usr/bin/env python3
"""Info Orbs setup: asks what you want on your orbs and writes firmware/config/config.h.

Run it from anywhere:   python setup/setup.py
Then build and flash:   pio run -t upload

It opens a setup page in your browser, served from this computer only (nothing else to install).
Prefer questions in the terminal? python setup/setup.py --terminal

Only what you pick is built into the firmware, so the less you pick, the more flash is left over.
Your answers are kept in firmware/config/setup.json and offered as the defaults next time; the
previous config.h is kept as config.h.bak.

Each widget, OrbIt control and screensaver effect is described by a file in setup/features/ - adding
one there is all it takes for this script to offer it. Starting points live in setup/presets/.

Non-interactive use (for CI, or to switch builds quickly):
    python setup/setup.py --preset clock-only --yes
"""

import argparse
import datetime
import http.server
import json
import os
import re
import secrets
import shutil
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FEATURES_DIR = os.path.join(ROOT, "setup", "features")
PRESETS_DIR = os.path.join(ROOT, "setup", "presets")
CONFIG_PATH = os.path.join(ROOT, "firmware", "config", "config.h")
TEMPLATE_PATH = os.path.join(ROOT, "firmware", "config", "config.h.template")
ANSWERS_PATH = os.path.join(ROOT, "firmware", "config", "setup.json")
WEB_PAGE_PATH = os.path.join(ROOT, "setup", "web", "index.html")

# The firmware's share of flash (the app0 partition in partitions.csv), and roughly what a build
# with nothing optional in it takes - the clock-only preset minus the clock. Measured, not exact:
# the estimate is a guide, and the build itself reports the real size.
APP_PARTITION_KB = 2048
BASE_KB = 1324
# WidgetSet can hold this many widgets - see MAX_WIDGETS in firmware/src/core/widget/WidgetSet.h.
MAX_WIDGETS = 5

END_MARKER = "END OF USER CONFIGURATION"


# ===================== features and presets =====================


def load_json_dir(path):
    items = []
    for name in sorted(os.listdir(path)):
        if name.endswith(".json"):
            with open(os.path.join(path, name), encoding="utf-8") as f:
                item = json.load(f)
            item["_file"] = name
            items.append(item)
    return sorted(items, key=lambda item: item.get("order", 0))


def load_features():
    features = load_json_dir(FEATURES_DIR)
    by_id = {f["id"]: f for f in features}
    for f in features:
        f["children"] = [c for c in features if c.get("parent") == f["id"]]
        if f.get("parent") and f["parent"] not in by_id:
            sys.exit("setup/features/%s: unknown parent '%s'" % (f["_file"], f["parent"]))
    return features, by_id


def load_presets(by_id):
    presets = load_json_dir(PRESETS_DIR)
    for p in presets:
        for fid in p["features"]:
            if fid not in by_id:
                sys.exit("setup/presets/%s: unknown feature '%s'" % (p["_file"], fid))
    return presets


def flat(items):
    """A feature's settings, with those inside a group (asked behind one yes/no question) included."""
    for item in items:
        if item["type"] == "group":
            for s in item["settings"]:
                yield s
        else:
            yield item


def all_settings(features):
    for f in features:
        for s in flat(f.get("settings", [])):
            yield f, s


# ===================== reading an existing config.h =====================


def user_section(text):
    """The part of a config.h before the hardware section."""
    index = text.find(END_MARKER)
    if index < 0:
        return text
    return text[: text.rfind("\n", 0, index) + 1]


def hardware_section(text):
    """The hardware section: from the END OF USER CONFIGURATION line to the end of the file."""
    index = text.find(END_MARKER)
    if index < 0:
        return None
    return text[text.rfind("\n", 0, index) + 1 :]


def parse_defines(text):
    """Every #define that isn't commented out: macro -> its value as written, or True if it has none."""
    defines = {}
    for line in text.splitlines():
        m = re.match(r"\s*#define\s+(\w+)(.*)$", line)
        if not m:
            continue
        rest = m.group(2).strip()
        if rest.startswith('"'):
            value, i = "", 1
            while i < len(rest) and rest[i] != '"':
                if rest[i] == "\\" and i + 1 < len(rest):
                    i += 1
                value += rest[i]
                i += 1
            defines[m.group(1)] = '"' + value + '"'
        else:
            token = rest.split("//")[0].strip()
            defines[m.group(1)] = token.split()[0] if token else True
    return defines


def value_from_define(setting, raw):
    """A setting's value from what a config.h #defines it as, or None if it can't be read."""
    kind = setting["type"]
    if kind == "flag":
        return raw is not None
    if raw is None or raw is True:
        return None
    if kind == "bool":
        return {"true": True, "false": False}.get(raw.lower())
    if kind in ("string", "timezone", "location"):
        return raw[1:-1] if raw.startswith('"') else None
    if kind in ("int", "brightness"):
        try:
            return int(raw)
        except ValueError:
            return None
    return raw


# ===================== state =====================


class State:
    """What gets built (features) and how it's set up (settings, by macro)."""

    def __init__(self, features, by_id):
        self.features = features
        self.by_id = by_id
        self.on = {f["id"]: f.get("default", False) for f in features if f.get("switch")}
        self.values = {}
        for _, s in all_settings(features):
            if "derived" not in s:
                self.values[s["macro"]] = s.get("default")

    def enabled(self, fid):
        """Whether a feature ends up in the firmware: switched on, and so is everything above it."""
        f = self.by_id[fid]
        if f.get("switch") and not self.on[fid]:
            return False
        return not f.get("parent") or self.enabled(f["parent"])

    def apply_preset(self, preset):
        for fid in self.on:
            self.on[fid] = fid in preset["features"]
        for macro, value in preset.get("settings", {}).items():
            self.values[macro] = value

    def load_config_h(self, text):
        defines = parse_defines(user_section(text))
        for f in self.features:
            sw = f.get("switch")
            if sw:
                raw = defines.get(sw["macro"])
                if sw["style"] == "bool":
                    if isinstance(raw, str) and raw.lower() in ("true", "false"):
                        self.on[f["id"]] = raw.lower() == "true"
                else:
                    self.on[f["id"]] = raw is not None
            for s in flat(f.get("settings", [])):
                if "derived" in s:
                    continue
                raw = defines.get(s["macro"])
                if raw is None and s["type"] != "flag":
                    if s.get("optional"):
                        self.values[s["macro"]] = None
                    continue
                value = value_from_define(s, raw)
                if value is not None:
                    self.values[s["macro"]] = value

    def load_answers(self, answers):
        for fid, on in answers.get("features", {}).items():
            if fid in self.on:
                self.on[fid] = bool(on)
        for macro, value in answers.get("settings", {}).items():
            if macro in self.values:
                self.values[macro] = value

    def load_payload(self, payload):
        """Answers sent by the setup page, checked against what the feature files allow."""
        for fid, on in payload.get("on", {}).items():
            if fid in self.on:
                self.on[fid] = bool(on)
        for _, s in all_settings(self.features):
            if "derived" in s or s["macro"] not in payload.get("values", {}):
                continue
            value = payload["values"][s["macro"]]
            if s["type"] in ("bool", "flag"):
                value = bool(value)
            elif s["type"] in ("int", "brightness"):
                try:
                    value = None if value in (None, "") else int(value)
                except (TypeError, ValueError):
                    continue
            elif value is not None:
                value = str(value)
                if value == "" and s.get("optional"):
                    value = None
            self.values[s["macro"]] = value

    def answers(self):
        return {
            "note": "Written by setup/setup.py - your answers, offered as the defaults the next time it runs.",
            "features": self.on,
            "settings": self.values,
        }

    def value(self, setting):
        """What gets written for a setting. A choice that needs something this build leaves out is
        written as the first choice still available, but the answer itself is kept, so it comes back
        when that something is built again."""
        if "derived" in setting:
            # Only true while something built needs it - DSEG14 isn't worth 70KB without the clock.
            owner = next(f for f, s in all_settings(self.features) if s is setting)
            if owner.get("switch") and not self.enabled(owner["id"]):
                return False
            if "askWhen" in setting and not any(self.enabled(fid) for fid in setting["askWhen"]):
                return False
            d = setting["derived"]
            # Either one condition ({from, equals}), or anyOf: a list of condition lists, any of which
            # (with all its conditions true) turns it on.
            options = d.get("anyOf", [[d]])
            return any(all(self.value(self.setting(c["from"])) == c["equals"] for c in conds) for conds in options)
        current = self.values.get(setting["macro"])
        if setting["type"] == "choice" and current is not None:
            valid = [c["value"] for c in self.valid_choices(setting)]
            if valid and current not in valid:
                return valid[0]
        return current

    def valid_choices(self, setting):
        return [c for c in setting.get("choices", []) if not c.get("feature") or self.enabled(c["feature"])]

    def widgets(self):
        return [f for f in self.features if f["kind"] == "widget" and self.enabled(f["id"])]

    def estimate_kb(self):
        kb = BASE_KB
        # Code or images several features use (sharedKb in their files) are only counted once.
        shared = {}
        for f in self.features:
            if f.get("switch") and self.enabled(f["id"]):
                kb += f.get("flashKb", 0)
                shared.update(f.get("sharedKb", {}))
            for s in flat(f.get("settings", [])):
                if s.get("flashKb") and self.value(s) is True:
                    kb += s["flashKb"]
        return kb + sum(shared.values())

    def choice_notes(self):
        """Explains each choice that this build writes differently from the answer given (see value())."""
        notes = []
        for _, s in all_settings(self.features):
            if s["type"] != "choice" or "derived" in s:
                continue
            current, written = self.values.get(s["macro"]), self.value(s)
            if current != written:
                label = {c["value"]: c["label"] for c in s["choices"]}
                notes.append("%s will be %s in this build, since %s isn't built." % (s["prompt"], label[written], label.get(current, current)))
        return notes

    def problems(self):
        problems = []
        widgets = self.widgets()
        if not widgets:
            problems.append("Nothing would be shown - turn on at least one widget.")
        if len(widgets) > MAX_WIDGETS:
            problems.append(
                "%d widgets are on, but the firmware only has room for %d - turn some off." % (len(widgets), MAX_WIDGETS)
            )
        for f in self.features:
            if not self.enabled(f["id"]):
                continue
            for c in f.get("conflicts", []):
                if self.values.get(c["macro"]) == c["value"]:
                    problems.append(c["message"])
            sw = f.get("switch")
            if sw and sw["style"] == "value" and not self.values.get(sw["macro"]):
                problems.append("%s needs a value for '%s'." % (f["name"], self.setting(sw["macro"])["prompt"]))
            if f["children"] and all(c["kind"] == "effect" for c in f["children"]):
                if not any(self.enabled(c["id"]) for c in f["children"]):
                    problems.append("%s is on, but none of its effects are - turn one on." % f["name"])
        return problems

    def setting(self, macro):
        for _, s in all_settings(self.features):
            if s["macro"] == macro:
                return s
        return None


# ===================== writing config.h =====================


def c_string(value):
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


def define_line(setting, value, commented=False):
    macro, kind = setting["macro"], setting["type"]
    if kind == "flag":
        line = "#define %s" % macro if value else "// #define %s" % macro
    elif value is None or (value == "" and setting.get("optional")):
        line = "// #define %s" % macro
    else:
        if kind == "bool":
            text = "true" if value else "false"
        elif kind in ("string", "timezone", "location"):
            text = c_string(value)
        else:
            text = str(value)
        line = "#define %s %s" % (macro, text)
        if commented:
            line = "// " + line
    # "comment" is for when the value in config.h isn't in the form the question asks for it.
    return line + " // " + setting.get("comment", setting["prompt"])


def switch_line(feature, state):
    sw = feature["switch"]
    on = state.enabled(feature["id"])
    label = feature["name"]
    if sw["style"] == "bool":
        return "#define %s %s // %s" % (sw["macro"], "true" if on else "false", label)
    if sw["style"] == "flag":
        return ("#define %s" if on else "// #define %s") % sw["macro"] + " // " + label
    setting = state.setting(sw["macro"])
    return define_line(setting, state.value(setting), commented=not on)


def render_feature(feature, state, lines, depth):
    for s in flat(feature.get("settings", [])):
        if feature.get("switch") and s["macro"] == feature["switch"]["macro"]:
            continue
        lines.append(define_line(s, state.value(s)))
    for child in feature["children"]:
        lines.append(switch_line(child, state))
        render_feature(child, state, lines, depth + 1)


def render(state, hardware):
    today = datetime.date.today().isoformat()
    lines = [
        "#ifndef CONFIG_H",
        "#define CONFIG_H",
        "",
        "// Written by setup/setup.py on %s. To change it, run setup/setup.py again - it remembers your" % today,
        "// answers (firmware/config/setup.json) and keeps the previous file as config.h.bak. Edits made",
        "// here by hand are lost the next time it runs.",
        "",
        "// Estimated firmware size: about %dKB of %dKB." % (state.estimate_kb(), APP_PARTITION_KB),
    ]
    for f in state.features:
        if f.get("parent"):
            continue
        lines.append("")
        if not f.get("switch"):
            lines.append("// ----- %s -----" % f["name"])
        elif state.enabled(f["id"]):
            lines.append("// ----- %s: built -----" % f["name"])
        else:
            lines.append("// ----- %s: not built (its settings are kept for when it's turned back on) -----" % f["name"])
        if f.get("switch"):
            lines.append(switch_line(f, state))
        render_feature(f, state, lines, 0)
    lines.append("")
    lines.append("")
    return "\n".join(lines) + hardware


# ===================== asking =====================

def ask(prompt, default=""):
    shown = " [%s]" % default if default != "" else ""
    try:
        answer = input("%s%s: " % (prompt, shown)).strip()
    except EOFError:
        print()
        sys.exit("\nStopped - nothing was written.")
    return answer if answer else default


def ask_yes(prompt, default):
    while True:
        answer = ask(prompt + (" (Y/n)" if default else " (y/N)"), "").lower()
        if answer == "":
            return default
        if answer in ("y", "yes"):
            return True
        if answer in ("n", "no"):
            return False
        print("  Please answer y or n.")


def ask_number(prompt, choices, default_index):
    for i, label in enumerate(choices, 1):
        print("  %d) %s" % (i, label))
    while True:
        answer = ask(prompt, str(default_index + 1))
        if answer.isdigit() and 1 <= int(answer) <= len(choices):
            return int(answer) - 1
        print("  Please pick a number from 1 to %d." % len(choices))


def hex_rgb565(text):
    text = text.strip().lower()
    if re.fullmatch(r"0x[0-9a-f]{1,4}", text):
        return "0x%04x" % int(text, 16)
    m = re.fullmatch(r"#?([0-9a-f]{6})", text)
    if m:
        rgb = int(m.group(1), 16)
        r, g, b = rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF
        return "0x%04x" % (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return None


def api_key(hardware, macro):
    value = parse_defines(hardware or "").get(macro)
    return value[1:-1] if isinstance(value, str) and value.startswith('"') else None


def fetch_json(url):
    with urllib.request.urlopen(url, timeout=10) as response:
        return json.loads(response.read().decode("utf-8"))


_zones = None


def timezone_list(hardware):
    """Every zone timezonedb.com knows (the service the firmware itself uses), or None offline."""
    global _zones
    if _zones is None:
        key = api_key(hardware, "TIMEZONE_API_KEY")
        try:
            data = fetch_json("http://api.timezonedb.com/v2.1/list-time-zone?key=%s&format=json" % key)
            _zones = [(z["zoneName"], z["countryName"]) for z in data.get("zones", [])]
        except (urllib.error.URLError, OSError, ValueError, KeyError):
            _zones = []
    return _zones or None


def match_timezones(answer, hardware):
    """(the zone answer names exactly or None, zones it could mean) - or (None, None) offline."""
    zones = timezone_list(hardware)
    if zones is None:
        return None, None
    exact = [z for z, _ in zones if z.lower() == answer.lower()]
    if exact:
        return exact[0], exact
    needle = answer.lower().replace(" ", "_")
    return None, [z for z, country in zones if needle in z.lower() or needle in country.lower().replace(" ", "_")]


def check_location(answer, hardware):
    """What visualcrossing.com resolves a location to: ("found", address), ("unknown", None), or
    ("offline", None) when it can't be reached."""
    key = api_key(hardware, "WEATHER_API_KEY")
    url = (
        "https://weather.visualcrossing.com/VisualCrossingWebServices/rest/services/timeline/%s/today?key=%s&include=current&elements=datetime"
        % (urllib.parse.quote(answer), key)
    )
    try:
        return "found", fetch_json(url).get("resolvedAddress") or answer
    except urllib.error.HTTPError:
        return "unknown", None
    except (urllib.error.URLError, OSError, ValueError):
        return "offline", None


def ask_timezone(setting, current, hardware):
    while True:
        answer = ask("  " + setting["prompt"], current or "")
        exact, matches = match_timezones(answer, hardware)
        if matches is None:
            if "/" in answer:
                print("  (Couldn't reach timezonedb.com to check it.)")
                return answer
            print("  That doesn't look like a zone name, e.g. America/Vancouver.")
            continue
        if exact:
            return exact
        if not matches:
            print("  No timezone matches '%s'. Try a nearby big city or the country." % answer)
        elif len(matches) == 1:
            if ask_yes("  Use %s?" % matches[0], True):
                return matches[0]
        elif len(matches) <= 15:
            return matches[ask_number("  Which one", matches, 0)]
        else:
            print("  %d timezones match '%s' - be more specific." % (len(matches), answer))


def ask_location(setting, current, hardware):
    while True:
        answer = ask("  " + setting["prompt"], current or "")
        status, resolved = check_location(answer, hardware)
        if status == "unknown":
            print("  visualcrossing.com doesn't know '%s'. Try 'City, Region' or a postal code." % answer)
            continue
        if status == "offline":
            print("  (Couldn't reach visualcrossing.com to check it.)")
            return answer
        if resolved and resolved.lower() != answer.lower():
            if not ask_yes("  That's %s - right?" % resolved, True):
                continue
        return answer


def ask_setting(setting, state, hardware):
    macro, kind = setting["macro"], setting["type"]
    current = state.values.get(macro)
    if setting.get("help"):
        print("  " + setting["help"])
    if kind in ("bool", "flag"):
        state.values[macro] = ask_yes("  " + setting["prompt"] + "?", bool(current))
    elif kind == "choice":
        choices = state.valid_choices(setting)
        labels = [c["label"] for c in choices]
        values = [c["value"] for c in choices]
        if setting.get("optional"):
            labels, values = labels + ["(firmware default)"], values + [None]
        shown = state.value(setting)
        index = values.index(shown) if shown in values else 0
        state.values[macro] = values[ask_number("  " + setting["prompt"], labels, index)]
    elif kind == "int":
        while True:
            shown = current if current is not None else setting.get("suggested")
            answer = ask("  " + setting["prompt"], "" if shown is None else str(shown))
            if answer == "" and setting.get("optional"):
                state.values[macro] = None
                break
            if re.fullmatch(r"-?\d+", answer):
                number = int(answer)
                if setting.get("min", number) <= number <= setting.get("max", number):
                    state.values[macro] = number
                    break
            print("  Please give a whole number" + (" from %s to %s" % (setting["min"], setting["max"]) if "max" in setting else "") + ".")
    elif kind == "brightness":
        # Written as the firmware's 0-255, where 255 is the normal, full brightness - asked as a percent.
        # Full brightness isn't dimming at all, so offer the suggestion instead.
        shown = current if current is not None and current < 255 else setting.get("suggested", 128)
        while True:
            answer = ask("  " + setting["prompt"], str(round(shown * 100 / 255.0)))
            if answer.isdigit() and 1 <= int(answer) <= 100:
                state.values[macro] = round(int(answer) * 255 / 100.0)
                break
            print("  Please give a percent from 1 to 100.")
    elif kind == "color":
        while True:
            answer = hex_rgb565(ask("  " + setting["prompt"] + " (#RRGGBB or RGB565 0x....)", current or ""))
            if answer:
                state.values[macro] = answer
                break
            print("  Please give a colour like #FF8000 or 0xfc80.")
    elif kind == "timezone":
        state.values[macro] = ask_timezone(setting, current, hardware)
    elif kind == "location":
        state.values[macro] = ask_location(setting, current, hardware)
    else:
        answer = ask("  " + setting["prompt"], current or "")
        state.values[macro] = answer if answer or not setting.get("optional") else None


def ask_group(group, state, hardware):
    """A group's settings are all set or all left out, behind one yes/no question."""
    members = group["settings"]
    on = all(state.values.get(s["macro"]) is not None for s in members)
    if group.get("help"):
        print("  " + group["help"])
    if ask_yes("  " + group["prompt"] + "?", on):
        for s in members:
            ask_setting(s, state, hardware)
    else:
        for s in members:
            state.values[s["macro"]] = None


def worth_asking(setting, state):
    if "derived" in setting:
        return False
    # Nothing to ask when what's built leaves only one choice.
    if setting["type"] == "choice" and len(state.valid_choices(setting)) + (1 if setting.get("optional") else 0) < 2:
        return False
    return True


def ask_items(items, state, hardware):
    for item in items:
        if item["type"] == "group":
            ask_group(item, state, hardware)
        elif worth_asking(item, state):
            ask_setting(item, state, hardware)


# ===================== the walk-through =====================


def kb(n):
    return "%dKB" % n if n < 1024 else "%.2fMB" % (n / 1024.0)


def size_note(f):
    total = f.get("flashKb", 0) + sum(f.get("sharedKb", {}).values())
    return " (about %s)" % kb(total) if total else ""


def print_summary(state):
    print()
    print("What will be built:")

    def walk(feature, depth):
        for child in feature["children"]:
            if state.enabled(child["id"]):
                print("  " + "  " * depth + "- " + child["name"])
                walk(child, depth + 1)

    for f in state.features:
        if f["kind"] == "widget" and state.enabled(f["id"]):
            print("  - " + f["name"])
            walk(f, 1)
    used = state.estimate_kb()
    print()
    print("Estimated firmware size: about %s of %s (%d%%)." % (kb(used), kb(APP_PARTITION_KB), round(100.0 * used / APP_PARTITION_KB)))
    if used > APP_PARTITION_KB:
        print("That won't fit - turn something off.")
    elif used > APP_PARTITION_KB * 0.95:
        print("That's very close to full - it may not fit.")


def ask_feature_settings(feature, state, hardware):
    """A feature's own questions, then - behind one more question - its advanced ones."""
    items = [i for i in feature.get("settings", []) if "askWhen" not in i]
    ask_items([i for i in items if not i.get("advanced")], state, hardware)
    advanced = [i for i in items if i.get("advanced") and (i["type"] == "group" or worth_asking(i, state))]
    if advanced:
        label = feature.get("advancedLabel", "the rest")
        if ask_yes("  Change other %s settings (%s)?" % (feature["name"], label), False):
            ask_items(advanced, state, hardware)


def walk_widgets(state, hardware, asked):
    """Each widget in turn: build it? Then what it contains, then its settings."""
    core = next(f for f in state.features if f["kind"] == "core")
    walked = set()

    def walk_children(feature, depth):
        for child in feature["children"]:
            walked.add(child["id"])
            indent = "  " * depth
            print("%s%s - %s%s" % (indent, child["name"], child["description"], size_note(child)))
            state.on[child["id"]] = ask_yes(indent + "Build it?", state.on[child["id"]])
            if state.on[child["id"]]:
                walk_children(child, depth + 1)

    for f in state.features:
        if f["kind"] != "widget":
            continue
        print()
        print("%s - %s%s" % (f["name"], f["description"], size_note(f)))
        state.on[f["id"]] = ask_yes("Build it?", state.on[f["id"]])
        walked.add(f["id"])
        if not state.on[f["id"]]:
            continue
        walk_children(f, 1)
        ask_feature_settings(f, state, hardware)
        # Device settings only something built needs (the weather location, say), asked with the
        # first widget that turns on something needing them.
        for s in core.get("settings", []):
            if "askWhen" not in s or s["macro"] in asked:
                continue
            if any(fid in walked and state.enabled(fid) for fid in s["askWhen"]):
                asked.add(s["macro"])
                if worth_asking(s, state):
                    ask_setting(s, state, hardware)


# ===================== writing =====================


def write_files(state, hardware, hardware_source, output=None):
    """Writes config.h (keeping the previous one as config.h.bak) and the answers; returns what it
    did, for the person running this. With output, writes only that file."""
    text = render(state, hardware)
    if output:
        with open(output, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return ["Wrote %s." % output]

    existing = None
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, encoding="utf-8") as f:
            existing = f.read()
    changed = existing is not None and existing != text
    if changed:
        shutil.copyfile(CONFIG_PATH, CONFIG_PATH + ".bak")
    with open(CONFIG_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    with open(ANSWERS_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(state.answers(), f, indent=2)
        f.write("\n")
    messages = ["Wrote firmware/config/config.h (hardware section from %s)." % hardware_source]
    if changed:
        messages.append("Your previous config.h is in firmware/config/config.h.bak.")
    elif existing is not None:
        messages.append("It's the same as the config.h you already had.")
    return messages


# ===================== the setup page =====================


def serve_page(state, hardware, hardware_source, source, output, open_browser=True):
    """Serves the setup page (setup/web/index.html) on this computer only and opens it in a browser.
    The page holds the answers while you work; nothing is written until it says so."""
    token = secrets.token_urlsafe(16)
    lock = threading.Lock()

    def status(st):
        return {"estimateKb": st.estimate_kb(), "problems": st.problems(), "notes": st.choice_notes()}

    def from_payload(payload):
        st = State(state.features, state.by_id)
        st.on, st.values = dict(state.on), dict(state.values)
        st.load_payload(payload)
        return st

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, code, body, content_type="application/json; charset=utf-8"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def allowed(self):
            # Only the page this run opened may use the API - not some other site in the browser.
            if self.headers.get("X-Setup-Token") == token:
                return True
            self.reply(403, {"error": "forbidden"})
            return False

        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            query = urllib.parse.parse_qs(url.query)
            q = query.get("q", [""])[0].strip()
            if url.path == "/":
                with open(WEB_PAGE_PATH, "rb") as f:
                    self.reply(200, f.read(), "text/html; charset=utf-8")
            elif not url.path.startswith("/api/"):
                self.reply(404, {"error": "not found"})
            elif not self.allowed():
                return
            elif url.path == "/api/model":
                features = [{k: v for k, v in f.items() if k not in ("_file", "children")} for f in state.features]
                with lock:
                    self.reply(200, {
                        "features": features,
                        "on": state.on,
                        "values": state.values,
                        "source": source,
                        "target": os.path.relpath(output) if output else "firmware/config/config.h",
                        "appPartitionKb": APP_PARTITION_KB,
                        "status": status(state),
                    })
            elif url.path == "/api/timezones":
                exact, matches = match_timezones(q, hardware) if q else (None, [])
                self.reply(200, {"exact": exact, "matches": None if matches is None else matches[:20], "count": None if matches is None else len(matches)})
            elif url.path == "/api/location":
                found, resolved = check_location(q, hardware) if q else ("unknown", None)
                self.reply(200, {"status": found, "resolved": resolved})
            else:
                self.reply(404, {"error": "not found"})

        def do_POST(self):
            if not self.allowed():
                return
            length = int(self.headers.get("Content-Length") or 0)
            try:
                payload = json.loads(self.rfile.read(length) or b"{}")
            except ValueError:
                self.reply(400, {"error": "bad JSON"})
                return
            path = urllib.parse.urlparse(self.path).path
            if path == "/api/check":
                self.reply(200, status(from_payload(payload)))
            elif path == "/api/write":
                st = from_payload(payload)
                problems = st.problems()
                if problems:
                    self.reply(400, {"problems": problems})
                    return
                with lock:
                    messages = write_files(st, hardware, hardware_source, output)
                    state.on, state.values = st.on, st.values
                print("\n".join(messages))
                self.reply(200, {"messages": messages})
            elif path == "/api/quit":
                self.reply(200, {"ok": True})
                threading.Thread(target=self.server.shutdown).start()
            else:
                self.reply(404, {"error": "not found"})

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    url = "http://127.0.0.1:%d/#%s" % (server.server_address[1], token)
    print("Info Orbs setup is open in your browser. If it didn't open, go to:")
    print("  " + url, flush=True)
    print("Press Ctrl+C here to stop (or use Done on the page).", flush=True)
    if open_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    server.server_close()


def main():
    parser = argparse.ArgumentParser(description="Choose what to build into the Info Orbs firmware and write firmware/config/config.h.")
    parser.add_argument("--preset", help="start from this preset (see --list-presets)")
    parser.add_argument("--yes", action="store_true", help="don't ask anything: keep every other answer as it is and write the file")
    parser.add_argument("--output", help="write here instead of firmware/config/config.h (nothing else is saved)")
    parser.add_argument("--list-presets", action="store_true", help="list the presets and stop")
    parser.add_argument("--terminal", action="store_true", help="ask the questions here in the terminal instead of opening the setup page")
    parser.add_argument("--no-browser", action="store_true", help="just print the setup page's address instead of opening it")
    args = parser.parse_args()

    features, by_id = load_features()
    presets = load_presets(by_id)
    if args.list_presets:
        for p in presets:
            print("%-12s %s - %s" % (p["id"], p["name"], p["description"]))
        return

    state = State(features, by_id)
    existing = None
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, encoding="utf-8") as f:
            existing = f.read()
    source = None
    if os.path.exists(ANSWERS_PATH):
        with open(ANSWERS_PATH, encoding="utf-8") as f:
            state.load_answers(json.load(f))
        source = "your answers from last time"
    elif existing:
        state.load_config_h(existing)
        source = "your current config.h"

    hardware = hardware_section(existing) if existing else None
    hardware_source = "your current config.h"
    if hardware is None:
        with open(TEMPLATE_PATH, encoding="utf-8") as f:
            hardware = hardware_section(f.read())
        hardware_source = "config.h.template"

    preset = None
    if args.preset:
        preset = next((p for p in presets if p["id"] == args.preset), None)
        if not preset:
            sys.exit("Unknown preset '%s' - see --list-presets." % args.preset)

    if preset:
        state.apply_preset(preset)

    if not args.yes and not args.terminal:
        serve_page(state, hardware, hardware_source, source, args.output, open_browser=not args.no_browser)
        return

    if not args.yes:
        print("Info Orbs setup")
        print("===============")
        print("Answer a few questions and this writes firmware/config/config.h. Only the widgets you pick")
        print("are built into the firmware. Press Enter to keep the answer shown in [brackets]%s." % (" - they're " + source if source else ""))
        core = next(f for f in features if f["kind"] == "core")
        print()
        print("Device")
        ask_feature_settings(core, state, hardware)
        print()
        print("Widgets")
        asked = set()
        while True:
            walk_widgets(state, hardware, asked)
            problems = state.problems()
            if not problems:
                break
            print()
            for p in problems:
                print("! " + p)
            if not ask_yes("Go through the widgets again?", True):
                sys.exit("Nothing was written.")

    for note in state.choice_notes():
        print("Note: " + note)
    problems = state.problems()
    if problems:
        for p in problems:
            print("! " + p)
        sys.exit("Nothing was written.")

    if not args.yes:
        print_summary(state)
        target = args.output or CONFIG_PATH
        if not ask_yes("\nWrite %s?" % os.path.relpath(target), True):
            sys.exit("Nothing was written.")

    print()
    for message in write_files(state, hardware, hardware_source, args.output):
        print(message)
    if not args.output:
        print("Next: build and flash with   pio run -t upload")


if __name__ == "__main__":
    main()
