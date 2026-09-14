# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Info Orbs is an ESP32-based firmware project (Arduino framework, built via PlatformIO) that drives five small TFT displays and cycles through configurable "widgets" (Clock, Weather, Stocks, MQTT, generic web data, Parqet portfolio, Makapix) using three physical buttons.

## Build / dev commands

Build system is PlatformIO, not a Makefile/npm project. There is a single environment: `esp32doit-devkit-v1`.

```
pio run                     # build firmware
pio run -t upload           # build and flash (device must be in bootloader mode)
pio run -t monitor          # serial monitor (115200 baud)
pio check                   # static analysis (if configured)
```

**Required one-time setup before anything will compile**: copy `firmware/config/config.h.template` to `firmware/config/config.h`. `config_helper.h` deliberately fails the build with a `#error` if this file is missing — this is not a bug, it's the guard rail for per-user secrets/settings (WiFi creds, API keys, timezone, enabled widgets, pin config).

There are no unit tests in this repo (firmware-only, hardware-dependent). CI (`.github/workflows/platformio.yml`) just does `cp config/config.h.template config/config.h` then `pio run` on Linux/macOS/Windows — that's the practical smoke test.

Wokwi simulation is supported (`wokwi.toml` points at the built `.pio/build/esp32doit-devkit-v1/firmware.{bin,elf}`).

Linting: MegaLinter runs `CPP_CLANG_FORMAT` (config in `.clang-format`: LLVM-based, 4-space indent, no tabs, no column limit) and Markdown linters. Format C++ changes with clang-format using the repo's `.clang-format` before committing.

## Architecture

### Source layout (see `platformio.ini` for the actual include paths)

- `firmware/src/main.cpp` — entry point: sets up buttons/ISRs, `ScreenManager`, `WidgetSet`, WiFi, then in `loop()` drives WiFi connection state, button polling, and widget update/draw/cycle.
- `firmware/src/core/screenmanager/` — `ScreenManager` wraps `TFT_eSPI` + `OpenFontRender` (TTF text) across the 5 physical screens (selected via per-screen chip-select pins). All drawing (shapes, text, JPEG/pushImage) goes through this class; nothing in widget code touches `TFT_eSPI` directly.
- `firmware/src/core/widget/` — the widget framework:
  - `Widget` is the abstract base every widget implements (`setup()`, `update()`, `draw()`, `buttonPressed()`, `getName()`).
  - `WidgetSet` owns an array of `Widget*` (capacity `MAX_WIDGETS`, currently 8), tracks which is current, and dispatches button presses / draw / update / cycling to it. Widgets are registered in `main.cpp::setup()`, conditionally via `#ifdef` based on which features are configured (e.g. `#ifdef STOCK_TICKER_LIST`).
- `firmware/src/core/button/` — `Button` handles debouncing/short/medium/long press detection; ISR handlers in `main.cpp` call into it from `attachInterrupt`.
- `firmware/src/core/globaltime/` — `GlobalTime` singleton (NTP-backed) used across widgets for current time/timezone.
- `firmware/src/core/utils/` — shared helpers (e.g. RGB565 dimming used for screen brightness/dimming effects).
- `firmware/src/widgets/<name>widget/` — one directory per widget, each with its own `.cpp/.h` (and sometimes a data-model class, e.g. `WeatherDataModel`). New widgets follow this pattern: subclass `Widget`, live in their own directory, get conditionally included/instantiated in `main.cpp` behind a config macro if the feature is optional.
- `firmware/config/config.h.template` — the master list of all compile-time configuration macros (WiFi, timezone, per-widget settings, which widgets are enabled, hardware/display driver flags like `GC9A01_DRIVER`, `HARDWARE == WOKWI`). `firmware/config/config_helper.h` just includes the user's real `config.h` or fails the build.
- `firmware/lib/` — vendored/symlinked libraries (e.g. `OpenFontRender`, `webp` via `symlink://firmware/lib/webp` in `platformio.ini`) not pulled from the PlatformIO registry.
- `firmware/include/` — generated/static headers embedded into the binary (icons, TTF font tables).
- `images/` and `fonts/` — assets embedded into flash via `board_build.embed_files` in `platformio.ini`. Adding a new embedded image/font requires adding it both to the filesystem and to that `embed_files` list (many entries are commented out to save flash — read the comments in `platformio.ini` before uncommenting/adding to understand the flash-size tradeoffs, e.g. Nixie clock assets vs. custom clock faces vs. DSEG14 font).

### Key runtime concepts

- **Widgets are compile-time optional.** Most non-core widgets (Stocks, Parqet, MQTT, WebData, Makapix) only exist in the binary if their config macro is `#define`d in `config.h`; check for the corresponding `#ifdef` in `main.cpp` before assuming a widget is active.
- **All screen brightness/dimming is applied at the pixel level** (`Utils::rgb565dim`) in `ScreenManager` and in the `tft_output` JPEG callback in `main.cpp` — there's no hardware backlight PWM dimming, it's RGB565 attenuation.
- **Config macros drive both firmware behavior and flash usage** — e.g. `USE_CLOCK_NIXIE`, `USE_CLOCK_CUSTOM`, `CLOCK_INCLUDE_DSEG14`, `WEATHER_INCLUDE_LIGHT_ICONS` gate both `config.h` behavior and which assets get embedded via `platformio.ini`'s `board_build.embed_files` — changing one without the other will either waste flash or break at runtime.
