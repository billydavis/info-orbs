# orbit-api specification

This is the REST control surface for `OrbItWidget` — a widget that treats each of the 5 physical screens as an independently assignable "slot."

Status: **implemented and verified on real hardware** — every control (`time`, `weather`, `ticker`, `custom`, `blank`), both single and bulk `POST`, and the validation/error paths have been tested end-to-end against a flashed device. Source: `firmware/src/widgets/orbitwidget/OrbItWidget.h`/`.cpp`.

## Base path

```
/orbit/api/v1
```

Versioned from day one (`v1`) so the schema can evolve later without breaking whatever is talking to a device in the field.

## Screens are addressed by index

Screens are numbered `0`-`4`, matching `ScreenManager`'s existing `selectScreen(int)` numbering (`NUM_SCREENS = 5`, `ScreenManager.h:11`). No aliasing/naming layer on top — orbit-api uses the same index space the firmware already uses internally.

## Slot config schema

Each screen holds one **slot config**: a `control` name plus a `params` object whose shape depends on the control. This mirrors how `WebDataWidget`'s JSON already separates "what kind of thing" (`type`) from "its specific fields" — same idea, applied one level up (a whole screen's content, not just one drawn primitive).

```json
{
  "control": "time",
  "params": { "showDate": true, "showDay": true, "format24Hour": false }
}
```

```json
{
  "control": "weather",
  "params": { "element": "icon" }
}
```

```json
{
  "control": "ticker",
  "params": { "symbol": "BTC/USD" }
}
```

```json
{
  "control": "custom",
  "params": {
    "data": [
      { "type": "text", "x": 120, "y": 120, "text": "Hi", "color": "white", "alignment": "mc" }
    ]
  }
}
```

### `control` enum (v1)

| control   | backed by                                              | notes |
|-----------|---------------------------------------------------------|-------|
| `time`    | `TimeControl` (`firmware/src/widgets/orbitwidget/controls/`) | Draws the full current time (HH:MM, not zero-padded) centered on the screen, mirroring `WeatherWidget`'s own clock-face screen — not a single digit, that turned out to be useless on a per-orb basis. `params.showDate` and `params.showDay` (both optional, default `false`) add the date and day-of-week above/below the time. `params.format24Hour` (optional, default `false`) picks military vs. 12-hour display — independent per slot, computed from `GlobalTime::getHour24()` rather than the device's own global `FORMAT_24_HOUR`/`setFormat24Hour()` setting, so it doesn't affect (or get affected by) anything else on the device. |
| `analogClock` | `AnalogClockControl` (`firmware/src/widgets/orbitwidget/controls/`) | Draws a classic analog clock face (circle, 12 hour ticks, smoothly-moving hour/minute/second hands) — a genuinely new rendering, not adapted from any existing widget. All `params` are optional color names (parsed via the same `Utils::stringToColor()` the rest of the codebase already uses): `background` (default black), `tickColor` (default white), `hourColor`/`minuteColor` (default white), `secondColor` (default red). Reading colors back via `GET` reports raw numeric RGB565 values, not names — `Utils::stringToColor()` has no reverse lookup, the same lossy-read-back tradeoff already accepted for a rich `custom` slot. Only the hands are erased-and-redrawn each second (not a full-screen `fillScreen()`), which is what keeps it flicker-free — hand lengths stay well short of the tick marks so this can't accidentally erase the face. |
| `weather` | `WeatherControl` (`firmware/src/widgets/orbitwidget/controls/`) | `params.element` picks which piece (e.g. `icon`, `temperature`, `condition`) |
| `ticker`  | `TickerControl` (`firmware/src/widgets/orbitwidget/controls/`) | `params.symbol` — any symbol `StockWidget`/twelvedata already accepts, including crypto/forex (e.g. `BTC/USD`) per the existing widget's convention |
| `custom`  | inline drawing, reusing `WebDataModel`/`WebDataElementModel` classes directly | `params` is exactly one `WebDataWidget` "displays" entry (`label`/`data`/`color`/`labelColor`/`background`/`fullDraw`) — either a plain string in `data` for word-wrapped centered text, or an array of drawing primitives (`type: text\|line\|rectangle\|triangle\|circle\|arc\|character`). No new drawing DSL invented. Note: reading a rich (element-array) custom slot back via `GET` is lossy — it reports `elementCount` rather than the original primitives, since `WebDataModel` doesn't expose a way to reconstruct them. |
| `blank`   | clears the screen to black, no content                 | explicit "nothing assigned here" state, distinct from a slot that's never been configured |

All controls are self-contained under `firmware/src/widgets/orbitwidget/` — OrbIt does not include or depend on `ClockWidget`/`WeatherWidget`/`StockWidget`/`WebDataWidget`; it only reuses `WeatherDataModel`/`StockDataModel`/`WebDataModel` as small, unmodified data/rendering classes, and owns its own HTTP fetching independently of those widgets.

This enum is expected to grow; adding a new `control` value should not require changing the endpoint shapes below.

## Endpoints

### `GET /orbit/api/v1/screens`

Returns all 5 current slot configs as an array, index-aligned with screen number.

```json
{
  "screens": [
    { "screen": 0, "control": "time", "params": { "showDate": true, "showDay": true, "format24Hour": false }, "updatedAt": 1234567890 },
    { "screen": 1, "control": "weather", "params": { "element": "temperature" }, "updatedAt": 0 },
    { "screen": 2, "control": "custom", "params": { "label": "", "data": "OrbIt Widget" }, "updatedAt": 0 },
    { "screen": 3, "control": "weather", "params": { "element": "icon" }, "updatedAt": 1234567890 },
    { "screen": 4, "control": "ticker", "params": { "symbol": "BTC/USD" }, "updatedAt": 1234567999 }
  ]
}
```

`updatedAt` is a `millis()`-based timestamp (device uptime, not wall clock — this device doesn't necessarily have a reliable epoch until NTP sync) of the last time that slot's config was written via the API. Included so a client can tell whether its last write actually landed, and to leave room for a future "only tell me what changed" mode.

### `GET /orbit/api/v1/screens/{n}`

Returns one slot config, same shape as one entry of the array above. `404` if `n` is outside `0`-`4`.

### `POST /orbit/api/v1/screens`

Body: an array of up to 5 slot configs (bulk replace), mirroring the shape `WebDataWidget` already uses for its `"displays"` array:

```json
{
  "screens": [
    { "screen": 3, "control": "weather", "params": { "element": "temperature" } },
    { "screen": 4, "control": "ticker", "params": { "symbol": "ETH/USD" } }
  ]
}
```

Only screens present in the array are changed; omitted screens keep their current config. (Explicitly set `"control": "blank"` to clear a screen — omitting it is not the same as blanking it.)

### `POST /orbit/api/v1/screens/{n}`

Body: one slot config (no `screen` field needed, it's in the URL):

```json
{ "control": "ticker", "params": { "symbol": "BTC/USD" } }
```

Replaces screen `n`'s config entirely (not a merge/patch of `params`).

### Response on successful write

Both POST endpoints echo back what was actually applied (post-validation), same shape as the GET responses. This lets a client confirm the write took effect without a follow-up GET.

### Effect of a write

If `OrbItWidget` is the currently active widget (the one the physical device is showing), a successful write immediately triggers a redraw of just the affected screen(s) — it does not wait for the widget's normal update loop. If OrbIt isn't currently on-screen, the new config is stored and takes effect the next time the device cycles to it. (See the "data update vs. redraw" note below for how this interacts with data-driven controls like `ticker`.)

## Validation & error responses

All of the following are rejected with a `4xx` status and a JSON error body — never silently ignored, never allowed to corrupt in-memory state or crash the device (this is the specific mistake `firmware-review.md` flagged in `MQTTWidget`'s remote-input handling, and it should not be repeated here):

- `n` outside `0`-`4` → `404`
- `control` not one of the known enum values → `400`
- `params` missing a field the given `control` requires, or a field of the wrong type → `400`
- Malformed JSON body → `400`

Error body shape:

```json
{ "error": "unknown control 'foo'" }
```

## Data update vs. full redraw (for `ticker`, and any future data-backed control)

Per the earlier discussion: a data-backed control like `ticker` separates "did the underlying value change" from "should we repaint." `TickerControl` fetches/holds its own data and exposes an `isChanged()`-style flag (matching `StockWidget`'s existing pattern); the widget's draw loop only repaints a screen when that flag is set, and does a full-screen repaint when it does (not a partial/pixel-diff redraw like `ClockWidget` does for digits — not worth the complexity for card-style content). A `POST` that assigns `ticker` to a screen triggers an immediate first fetch+draw; after that, the normal update loop's polling interval governs redraws, same as `StockWidget` today.

## Persistence

Every successful `POST` (single or bulk) saves the complete 5-slot layout as one JSON blob to NVS via the ESP32 `Preferences` library (namespace `"orbit"`, key `"layout"` — same JSON shape as `GET /screens`). On boot, `OrbItWidget`'s constructor restores this before WiFi is even up (NVS is local flash, no network needed) and it overrides the compile-time default layout. A ticker slot restored this way does *not* trigger an immediate fetch (no network yet); it picks up fresh data on the normal polling cycle once WiFi connects. `updatedAt` is not meaningfully persisted — it's a `millis()`-based uptime value that resets every reboot, so a restored slot reports `updatedAt: 0` until it's next written via the API in the current session. A corrupt or unparseable saved layout is logged and ignored (compile-time defaults apply instead), never crashes the boot.

## Explicitly out of scope for v1

- **No auth/TLS.** Anyone on the local network can call this API. This matches the project's existing security posture elsewhere (e.g. `WebDataWidget`'s outbound calls, `MQTTWidget`'s broker connection) but is called out here explicitly as a tradeoff being accepted, not an oversight.
- **No partial `params` patching.** A `POST` always replaces a slot's entire config; there's no "just change the symbol, leave everything else" merge endpoint in v1.

## Resolved decisions

1. **`updatedAt` stays** in responses as specified above.
2. **An unconfigured screen reads back as `control: "blank"`** — same state as an explicitly cleared screen, no separate "unset" state.
3. **Bulk `POST /screens` merges by omission** — screens not present in the array keep their current config, as specified above. (Explicitly send `"control": "blank"` for a screen to clear it.)
