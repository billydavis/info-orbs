#!/usr/bin/env python3
"""Checks that setup/features/ still matches the firmware. Run in CI, and after adding a feature.

Fails when:
- config.h.template or feature_defaults.h has a setting no feature file covers (setup.py would never
  write it), or a feature file names a macro the template doesn't document;
- a feature file's macro isn't used anywhere in the firmware (a switch that does nothing);
- a preset or a feature's parent names a feature that doesn't exist (checked by loading them).
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import setup  # noqa: E402

FIRMWARE = os.path.join(setup.ROOT, "firmware")
DEFAULTS_PATH = os.path.join(FIRMWARE, "config", "feature_defaults.h")


def firmware_sources():
    for top in ("src", "include", "config"):
        for folder, _, files in os.walk(os.path.join(FIRMWARE, top)):
            for name in files:
                if name.endswith((".h", ".cpp")) and name not in ("config.h", "feature_defaults.h"):
                    with open(os.path.join(folder, name), encoding="utf-8", errors="ignore") as f:
                        yield f.read()


def main():
    features, by_id = setup.load_features()
    setup.load_presets(by_id)

    feature_macros = set()
    for f in features:
        if f.get("switch"):
            feature_macros.add(f["switch"]["macro"])
        for s in setup.flat(f.get("settings", [])):
            feature_macros.add(s["macro"])

    with open(setup.TEMPLATE_PATH, encoding="utf-8") as f:
        template = setup.user_section(f.read())
    template_macros = set(re.findall(r"#define\s+(\w+)", template)) - {"CONFIG_H"}
    with open(DEFAULTS_PATH, encoding="utf-8") as f:
        default_macros = set(re.findall(r"#ifndef\s+(\w+)", f.read())) - {"FEATURE_DEFAULTS_H"}

    problems = []
    for macro in sorted((template_macros | default_macros) - feature_macros):
        problems.append("%s is in config.h.template or feature_defaults.h, but no file in setup/features/ covers it" % macro)
    for macro in sorted(feature_macros - template_macros):
        problems.append("%s is in setup/features/, but config.h.template doesn't document it" % macro)
    sources = "\n".join(firmware_sources())
    for macro in sorted(feature_macros):
        if not re.search(r"\b%s\b" % macro, sources):
            problems.append("%s is in setup/features/, but nothing in firmware/ uses it" % macro)

    for p in problems:
        print("! " + p)
    if problems:
        sys.exit(1)
    print("setup/features/ matches the firmware (%d features, %d macros)." % (len(features), len(feature_macros)))


if __name__ == "__main__":
    main()
