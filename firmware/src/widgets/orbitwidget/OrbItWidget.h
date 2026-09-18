#ifndef ORBIT_WIDGET_H
#define ORBIT_WIDGET_H

#include "GlobalTime.h"
#include "Utils.h"
#include "Widget.h"
#include "controls/AnalogClockControl.h"
#include "controls/AsteroidsControl.h"
#include "controls/CountdownControl.h"
#include "controls/GaugeControl.h"
#include "controls/SysMonitorControl.h"
#include "controls/TickerControl.h"
#include "controls/TimeControl.h"
#include "controls/WeatherControl.h"
#include "stockwidget/StockDataModel.h"
#include "weatherwidget/WeatherDataModel.h"
#include "webdatawidget/WebDataModel.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>

// OrbIt treats each of the 5 physical screens as an independently assignable "slot" instead of
// one widget owning all 5 screens the way ClockWidget/WeatherWidget/StockWidget do. Slot
// assignment is controlled at runtime via a small REST API ("orbit-api", see
// firmware/src/widgets/orbitwidget/docs/orbit-api.md for the full spec this implements).
//
// Deliberately self-contained: OrbIt does not include or depend on ClockWidget/WeatherWidget/
// StockWidget. It reuses their small per-screen renderers (TimeControl/WeatherControl/
// TickerControl, under this widget's own controls/ directory) and their plain data model classes
// (StockDataModel/WeatherDataModel), but owns its own data fetching, so nothing here changes if
// those widgets change.
enum class OrbItSource {
    BLANK,
    TIME,
    // A genuinely new rendering, not adapted from an existing widget - see AnalogClockControl.
    ANALOG_CLOCK,
    // Another genuinely new rendering - see GaugeControl.
    GAUGE,
    // Yet another genuinely new rendering - see SysMonitorControl.
    SYS_MONITOR,
    // Purely decorative screensaver - see AsteroidsControl. No config; OrbIt's default for screen 2.
    ASTEROIDS,
    // Set/pause/resume/stop/restart via orbit-api params.action - see CountdownControl.
    COUNTDOWN,
    WEATHER,
    TICKER,
    // Reuses WebDataModel/WebDataElementModel (from webdatawidget/, read-only reuse - not modified,
    // not depended on beyond these two small data classes) so OrbIt doesn't need its own drawing
    // primitive dialect: "params" for this control is exactly one WebDataWidget "displays" entry
    // (label/data/color/labelColor/background/fullDraw), see firmware/src/widgets/orbitwidget/docs/orbit-api.md.
    CUSTOM,
};

enum class WeatherElement {
    ICON,
    TEMPERATURE,
    CONDITION,
};

struct OrbItSlot {
    OrbItSource source = OrbItSource::BLANK;
    bool showDate = false; // TIME control only
    bool showDay = false; // TIME control only
    bool format24Hour = false; // TIME control only - independent of GlobalTime's own device-wide setting
    WeatherElement weatherElement = WeatherElement::ICON;
    String tickerSymbol;
    // TICKER control only. Independent per slot (not shared with StockWidget's own timer, nor with
    // any other OrbIt ticker slot) so orbit-api can give each screen its own cadence. Defaults to
    // 15 minutes, matching StockWidget's own default. See MIN_TICKER_POLL_INTERVAL_MS in
    // OrbItWidget.cpp for why this can't be set arbitrarily low.
    unsigned long tickerPollIntervalMs = 900000;
    unsigned long tickerDelayPrev = 0; // millis() at this slot's last fetch, 0 = never fetched yet
    AnalogClockColors analogColors; // ANALOG_CLOCK control only
    GaugeConfig gaugeConfig; // GAUGE control only
    SysMonitorConfig sysMonitorConfig; // SYS_MONITOR control only
    // COUNTDOWN control only. The last-configured duration/label/color - in memory only, not
    // persisted (see OrbItWidget::persistLayout()'s comment on why countdown slots are skipped
    // entirely). The live running/paused/remaining-time state lives separately in OrbItWidget's
    // m_countdownRuntimes.
    CountdownConfig countdownConfig;

    // Change-tracking, mirroring the isChanged()/lastValue pattern already used elsewhere in this
    // codebase (ClockWidget's m_lastDisplayNDigit, StockDataModel::isChanged()): only repaint a
    // screen when its underlying value actually changed, computed in update() and compared in draw().
    String pendingValue;
    String lastRenderedValue;
    bool everDrawn = false;

    unsigned long updatedAt = 0; // millis() at last successful orbit-api write, 0 if never configured via API
};

class OrbItWidget : public Widget {
public:
    OrbItWidget(ScreenManager &manager);
    void setup() override;
    void update(bool force = false) override;
    void draw(bool force = false) override;
    void buttonPressed(uint8_t buttonId, ButtonState state) override;
    String getName() override;

    // Starts the orbit-api server on first call and services pending requests. Must only be called
    // once WiFi is connected (main.cpp already gates this the same way it gates widget updates).
    // Intentionally called every loop iteration regardless of which widget is currently displayed,
    // BEFORE WidgetSet::updateCurrent()/drawCurrent() - so a config change applied this tick is
    // picked up by the very same tick's draw pass if OrbIt happens to be current, with no need for
    // this widget to know whether it's "the current one" (WidgetSet already only calls this
    // widget's own draw() while it is).
    void serviceApi();

private:
    bool anySlotUses(OrbItSource source);

    void drawSlot(int displayIndex, OrbItSlot &slot, bool force);
    void drawTimeSlot(int displayIndex, OrbItSlot &slot, bool force);
    void drawAnalogClockSlot(int displayIndex, OrbItSlot &slot, bool force);
    void drawGaugeSlot(int displayIndex, OrbItSlot &slot, bool force);
    void drawSysMonitorSlot(int displayIndex, OrbItSlot &slot, bool force);
    void drawAsteroidsSlot(int displayIndex, OrbItSlot &slot, bool force);
    void drawCountdownSlot(int displayIndex, OrbItSlot &slot, bool force);
    void drawWeatherSlot(int displayIndex, OrbItSlot &slot, bool force);
    void drawTickerSlot(int displayIndex, OrbItSlot &slot, bool force);
    void drawCustomSlot(int displayIndex, OrbItSlot &slot, bool force);

    void updateWeather(bool force);
    void updateTicker(bool force);
    void updateCountdown(bool force);
    bool fetchWeatherData(WeatherDataModel &model);
    void fetchTickerData(StockDataModel &stock);
    // Mutates m_countdownRuntimes[index]/m_slots[index].countdownConfig per params.action - see the
    // 'countdown' control's validation in parseSlotConfig for the state-dependent preconditions
    // (e.g. 'pause' requires RUNNING) that must already have passed before this is ever called.
    void applyCountdownAction(int index, const String &action, JsonObject params);

    // orbit-api
    void setupApiRoutes();
    void handleGetScreens();
    void handleGetScreen(int index);
    void handlePostScreens();
    void handlePostScreen(int index);
    void handleRefreshScreen(int index);
    // screenIndex is used only by the 'countdown' control, to validate an action (e.g. 'pause')
    // against that specific screen's current live CountdownRuntime state - every other control
    // ignores it. -1 means "no specific screen" (used by loadPersistedLayout()'s restore path,
    // which never reaches the countdown validation branch - see its own countdown special-case).
    bool parseSlotConfig(JsonObject obj, OrbItSlot &outSlot, String &errorMessage, int screenIndex = -1);
    void slotToJson(int index, const OrbItSlot &slot, JsonObject out);
    void sendJson(int code, const JsonDocument &doc);
    void sendError(int code, const String &message);
    // immediateFetch=false is used only when restoring from NVS at construction time, before WiFi
    // is up - skips the live ticker fetch and leaves updatedAt at 0 (meaningless post-reboot millis()
    // values aren't worth persisting/restoring).
    void applySlotConfig(int index, const OrbItSlot &newConfig, JsonObject rawConfig, bool immediateFetch = true);

    // Persistence (NVS via Preferences) - Step 5 of the OrbIt plan. The whole 5-slot layout is
    // stored as one JSON blob under a single key, matching the GET /screens response shape, so
    // save/restore reuses slotToJson()/parseSlotConfig() rather than a separate schema.
    void loadPersistedLayout();
    void persistLayout();

    TimeControl m_timeControl;
    AnalogClockControl m_analogClockControl;
    GaugeControl m_gaugeControl;
    SysMonitorControl m_sysMonitorControl;
    AsteroidsControl m_asteroidsControl;
    CountdownControl m_countdownControl;
    WeatherControl m_weatherControl;
    TickerControl m_tickerControl;

    OrbItSlot m_slots[NUM_SCREENS];

    WeatherDataModel m_weatherModel;
    ScreenMode m_weatherScreenMode = Dark;
    unsigned long m_weatherDelay = 600000; // matches WeatherWidget's refresh rate
    unsigned long m_weatherDelayPrev = 0;

    // One StockDataModel per screen that wants a ticker - simplest v1; no de-duplication if two
    // slots happen to request the same symbol. Each slot polls on its own schedule (OrbItSlot::
    // tickerPollIntervalMs/tickerDelayPrev), not a shared timer.
    StockDataModel m_tickerModels[NUM_SCREENS];

    // One WebDataModel per screen assigned "custom" - same reuse-not-share approach as the ticker
    // models above.
    WebDataModel m_customModels[NUM_SCREENS];

    // Per-screen hand-position memory for ANALOG_CLOCK slots, so AnalogClockControl can erase just
    // the old hands each second instead of a full-screen redraw (see AnalogClockControl.h).
    AnalogClockHands m_analogClockHands[NUM_SCREENS];

    // Per-screen "what did we actually last draw" memory for GAUGE slots, so GaugeControl can
    // update just the fill arc/text instead of a full-screen redraw every value change.
    GaugeState m_gaugeStates[NUM_SCREENS];

    // Per-screen "what did we actually last draw" memory for SYS_MONITOR slots, mirroring
    // m_gaugeStates above - same partial-redraw purpose, just per-quadrant instead of per-arc.
    SysMonitorState m_sysMonitorStates[NUM_SCREENS];

    // Per-screen starfield/planet/rock animation state for ASTEROIDS slots - same "caller owns the
    // per-screen memory" pattern as m_gaugeStates/m_sysMonitorStates above.
    AsteroidsState m_asteroidsStates[NUM_SCREENS];

    // Per-screen live countdown state (running/paused/remaining time) for COUNTDOWN slots -
    // intentionally separate from OrbItSlot::countdownConfig (the persisted template); see
    // CountdownRuntime's own comment for why.
    CountdownRuntime m_countdownRuntimes[NUM_SCREENS];

    WebServer m_server{80};
    bool m_serverStarted = false;

    Preferences m_preferences;
};
#endif // ORBIT_WIDGET_H
