#include "OrbItWidget.h"

#include "config_helper.h"

namespace {
const char *API_BASE = "/orbit/api/v1/screens";

// twelvedata's free tier is 8 calls/minute and 800 calls/day (verified against
// https://twelvedata.com/pricing) - the daily cap, not the per-minute one, is what actually
// constrains a ticker slot meant to poll continuously: at 800/day, one ticker calling forever can
// only average one call every ~108s. A 5-minute (300s) floor keeps a single continuously-polling
// ticker slot well under that (288 calls/day) with headroom for a second slot before the daily cap
// becomes a real risk - orbit-api deliberately does not try to track/divide the shared daily budget
// across slots itself (that's a bigger feature); if you configure several screens as tickers, it's
// on you to keep the combined polling reasonable.
const unsigned long MIN_TICKER_POLL_INTERVAL_MS = 300000;

String sourceToString(OrbItSource source) {
    switch (source) {
    case OrbItSource::TIME:
        return "time";
    case OrbItSource::ANALOG_CLOCK:
        return "analogClock";
    case OrbItSource::GAUGE:
        return "gauge";
    case OrbItSource::SYS_MONITOR:
        return "sysMonitor";
    case OrbItSource::ASTEROIDS:
        return "asteroids";
    case OrbItSource::COUNTDOWN:
        return "countdown";
    case OrbItSource::WEATHER:
        return "weather";
    case OrbItSource::TICKER:
        return "ticker";
    case OrbItSource::CUSTOM:
        return "custom";
    case OrbItSource::BLANK:
    default:
        return "blank";
    }
}

String weatherElementToString(WeatherElement element) {
    switch (element) {
    case WeatherElement::ICON:
        return "icon";
    case WeatherElement::TEMPERATURE:
        return "temperature";
    case WeatherElement::CONDITION:
        return "condition";
    }
    return "";
}

String gaugeStyleToString(GaugeStyle style) {
    switch (style) {
    case GaugeStyle::RING:
        return "ring";
    case GaugeStyle::SPEEDOMETER:
        return "speedometer";
    case GaugeStyle::INSTRUMENT:
        return "instrument";
    }
    return "";
}

String countdownStateToString(CountdownRunState state) {
    switch (state) {
    case CountdownRunState::RUNNING:
        return "running";
    case CountdownRunState::PAUSED:
        return "paused";
    case CountdownRunState::COMPLETED:
        return "completed";
    case CountdownRunState::IDLE:
    default:
        return "idle";
    }
}
} // namespace

OrbItWidget::OrbItWidget(ScreenManager &manager) : Widget(manager), m_timeControl(manager), m_analogClockControl(manager), m_gaugeControl(manager), m_sysMonitorControl(manager), m_asteroidsControl(manager), m_countdownControl(manager), m_weatherControl(manager), m_tickerControl(manager) {
    // Compile-time starting layout - orbit-api can reassign any of this at runtime. Mixes pieces of
    // Clock/Weather/Stock across screens simultaneously, which is the whole point of this widget.
    m_slots[0].source = OrbItSource::TIME;
    m_slots[0].showDate = true;
    m_slots[0].showDay = true;

    m_slots[1].source = OrbItSource::WEATHER;
    m_slots[1].weatherElement = WeatherElement::TEMPERATURE;

    // Default screen 2 to the decorative Asteroids-style screensaver (starfield + a bouncing
    // planet labeled "Orb-It") instead of a plain text label - orbit-api can still reassign it to
    // anything else, including "custom", at runtime.
    m_slots[2].source = OrbItSource::ASTEROIDS;

    // Default screen 3 to a classic analog clock face.
    m_slots[3].source = OrbItSource::ANALOG_CLOCK;

    m_slots[4].source = OrbItSource::TICKER;
    m_slots[4].tickerSymbol = "BTC/USD";

    // Overrides any of the above with whatever was last saved via orbit-api, if anything - NVS
    // access works fully offline, so this is safe to do here before WiFi is even up.
    loadPersistedLayout();
}

void OrbItWidget::setup() {
#ifdef WEATHER_SCREEN_MODE
    m_weatherScreenMode = WEATHER_SCREEN_MODE;
#endif
    for (int i = 0; i < NUM_SCREENS; i++) {
        m_tickerModels[i].setSymbol(m_slots[i].tickerSymbol);
    }
}

bool OrbItWidget::anySlotUses(OrbItSource source) {
    for (int i = 0; i < NUM_SCREENS; i++) {
        if (m_slots[i].source == source) {
            return true;
        }
    }
    return false;
}

void OrbItWidget::update(bool force) {
    GlobalTime *time = GlobalTime::getInstance();

    // hour*60+minute is the same "only redraw when the displayed value actually changes" stamp
    // WeatherWidget's own clock screen uses (WeatherWidget::getClockStamp()) - showDate/showDay
    // don't need their own change tracking since they're static per-slot config, not per-tick data.
    String clockStamp = String(time->getHour() * 60 + time->getMinute());
    // The analog clock's second hand needs per-second granularity, not per-minute.
    String secondStamp = String(time->getHour() * 3600 + time->getMinute() * 60 + time->getSecond());
    for (int i = 0; i < NUM_SCREENS; i++) {
        if (m_slots[i].source == OrbItSource::TIME) {
            m_slots[i].pendingValue = clockStamp;
        } else if (m_slots[i].source == OrbItSource::ANALOG_CLOCK) {
            m_slots[i].pendingValue = secondStamp;
        } else if (m_slots[i].source == OrbItSource::GAUGE) {
            // Gauge config only ever changes via an orbit-api write (applySlotConfig already
            // resets lastRenderedValue to force a redraw then) - this just needs to be *some*
            // stable stamp of the full config so an unrelated redraw pass doesn't skip it forever.
            const GaugeConfig &g = m_slots[i].gaugeConfig;
            m_slots[i].pendingValue = String(g.value) + "|" + g.label + "|" + String(g.color) + "|" + String(g.trackColor) + "|" + String((int) g.style);
        } else if (m_slots[i].source == OrbItSource::SYS_MONITOR) {
            // Same "stable stamp of the full config" reasoning as GAUGE above - values only ever
            // change via an orbit-api write, which already forces a redraw itself.
            const SysMonitorConfig &s = m_slots[i].sysMonitorConfig;
            m_slots[i].pendingValue = String(s.cpu) + "|" + String(s.cpuTemp) + "|" + String(s.gpu) + "|" + String(s.gpuTemp) + "|" + String(s.ram) + "|" + String(s.ramTotal) + "|" + String(s.ssdTemp) + "|" + s.center;
        } else if (m_slots[i].source == OrbItSource::ASTEROIDS) {
            // Throttles the animation to ~20fps regardless of how fast loop() itself spins - a new
            // stamp every 50ms is what actually drives drawAsteroidsSlot()'s redraw below.
            m_slots[i].pendingValue = String(millis() / 50);
        } else if (m_slots[i].source == OrbItSource::COUNTDOWN) {
            const CountdownRuntime &runtime = m_countdownRuntimes[i];
            switch (runtime.state) {
            case CountdownRunState::RUNNING:
                // Once-a-second granularity is all the ring/MM:SS display needs.
                m_slots[i].pendingValue = "running:" + String(runtime.remainingMs(millis()) / 1000);
                break;
            case CountdownRunState::COMPLETED:
                // Twice a second, to drive CountdownControl's flash animation.
                m_slots[i].pendingValue = "completed:" + String(millis() / 500);
                break;
            case CountdownRunState::PAUSED:
                m_slots[i].pendingValue = "paused:" + String(runtime.remainingMsAtPause);
                break;
            case CountdownRunState::IDLE:
            default:
                m_slots[i].pendingValue = "idle";
                break;
            }
        }
    }

    if (anySlotUses(OrbItSource::WEATHER)) {
        updateWeather(force);
    }
    if (anySlotUses(OrbItSource::TICKER)) {
        updateTicker(force);
    }
    if (anySlotUses(OrbItSource::COUNTDOWN)) {
        updateCountdown(force);
    }
}

void OrbItWidget::updateWeather(bool force) {
    if (force || m_weatherDelayPrev == 0 || (millis() - m_weatherDelayPrev) >= m_weatherDelay) {
        fetchWeatherData(m_weatherModel);
        m_weatherDelayPrev = millis();
    }
}

// Each TICKER slot polls on its own schedule (slot.tickerPollIntervalMs/tickerDelayPrev), not one
// shared timer for every ticker slot - orbit-api can give screens tracking different symbols
// different cadences. `force` still forces every ticker slot to refetch together (used for the
// one-time boot fetch via WidgetSet::initializeAllWidgetsData()), independent of each slot's own
// due time.
void OrbItWidget::updateTicker(bool force) {
    for (int i = 0; i < NUM_SCREENS; i++) {
        OrbItSlot &slot = m_slots[i];
        if (slot.source != OrbItSource::TICKER) {
            continue;
        }
        if (force || slot.tickerDelayPrev == 0 || (millis() - slot.tickerDelayPrev) >= slot.tickerPollIntervalMs) {
            fetchTickerData(m_tickerModels[i]);
            slot.tickerDelayPrev = millis();
        }
    }
}

// The only thing that needs to happen here is noticing a RUNNING countdown has reached zero and
// flipping it to COMPLETED (recording completedAtMs so CountdownControl knows the flash animation's
// phase) - everything else about a countdown's state only ever changes via an explicit orbit-api
// action (applyCountdownAction()), not from the passage of time on its own.
void OrbItWidget::updateCountdown(bool force) {
    unsigned long now = millis();
    for (int i = 0; i < NUM_SCREENS; i++) {
        if (m_slots[i].source != OrbItSource::COUNTDOWN) {
            continue;
        }
        CountdownRuntime &runtime = m_countdownRuntimes[i];
        if (runtime.state == CountdownRunState::RUNNING && runtime.remainingMs(now) == 0) {
            runtime.state = CountdownRunState::COMPLETED;
            runtime.completedAtMs = now;
        }
    }
}

// Mutates m_countdownRuntimes[index] (and, for 'set', slot.countdownConfig) per the given action.
// Called only from applySlotConfig() after parseSlotConfig() has already validated the action
// against this slot's current state (e.g. 'pause' requires RUNNING) - this doesn't re-check.
void OrbItWidget::applyCountdownAction(int index, const String &action, JsonObject params) {
    OrbItSlot &slot = m_slots[index];
    CountdownRuntime &runtime = m_countdownRuntimes[index];
    unsigned long now = millis();

    if (action == "set") {
        slot.countdownConfig.durationSeconds = (unsigned long) params["durationSeconds"].as<int>();
        slot.countdownConfig.label = params["label"].is<const char *>() ? params["label"].as<String>() : "";
        // Full replace, like every other control's params - a 'set' without a color falls back to
        // the default rather than keeping whatever was configured before.
        slot.countdownConfig.color = params["color"].is<const char *>() ? Utils::stringToColor(params["color"].as<String>()) : TFT_CYAN;

        runtime.totalMs = slot.countdownConfig.durationSeconds * 1000UL;
        runtime.runStartedAtMs = now;
        runtime.remainingMsAtPause = 0;
        runtime.completedAtMs = 0;
        runtime.state = CountdownRunState::RUNNING;
    } else if (action == "restart") {
        // Reuses the existing label/color/duration template - only the timing resets.
        runtime.totalMs = slot.countdownConfig.durationSeconds * 1000UL;
        runtime.runStartedAtMs = now;
        runtime.remainingMsAtPause = 0;
        runtime.completedAtMs = 0;
        runtime.state = CountdownRunState::RUNNING;
    } else if (action == "pause") {
        unsigned long elapsed = now - runtime.runStartedAtMs;
        runtime.remainingMsAtPause = elapsed >= runtime.totalMs ? 0 : runtime.totalMs - elapsed;
        runtime.state = CountdownRunState::PAUSED;
    } else if (action == "resume") {
        // Recompute a virtual start time so remainingMs() keeps counting down seamlessly from
        // exactly where 'pause' left off, without needing a separate "paused duration" field.
        runtime.runStartedAtMs = now - (runtime.totalMs - runtime.remainingMsAtPause);
        runtime.state = CountdownRunState::RUNNING;
    } else if (action == "stop") {
        runtime.state = CountdownRunState::IDLE;
        runtime.remainingMsAtPause = 0;
        runtime.runStartedAtMs = 0;
        runtime.completedAtMs = 0;
        // Flagged rather than restored right here - this runs nested inside applySlotConfig() for
        // this same index, and restoring now would mean re-entering applySlotConfig() mid-call
        // (corrupting whatever it does next, e.g. CUSTOM's own params handling). The actual restore
        // happens in consumeCountdownRestore(), called by the API handlers once this outer
        // applySlotConfig() call has fully returned.
        if (m_countdownPreviousConfig[index].length() > 0) {
            m_countdownRestorePending[index] = true;
        }
    }
}

// Restores whatever was snapshotted on this screen right before a countdown's 'set' first replaced
// it (see applySlotConfig()'s own snapshot-capture comment), if a 'stop' action just flagged one
// pending. Must be called as a separate, sequential top-level call after applySlotConfig() has
// fully returned for this index - never from inside it - see applyCountdownAction()'s 'stop' case.
void OrbItWidget::consumeCountdownRestore(int index) {
    if (!m_countdownRestorePending[index]) {
        return;
    }
    m_countdownRestorePending[index] = false;

    String snapshotJson = m_countdownPreviousConfig[index];
    m_countdownPreviousConfig[index] = "";
    if (snapshotJson.length() == 0) {
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, snapshotJson)) {
        return; // shouldn't happen - this is exactly what slotToJson()/serializeJson() wrote
    }
    OrbItSlot parsed;
    String errorMessage;
    if (!parseSlotConfig(doc.as<JsonObject>(), parsed, errorMessage, index)) {
        return; // shouldn't happen either, for the same reason
    }
    applySlotConfig(index, parsed, doc.as<JsonObject>());
}

// Mirrors WeatherWidget::getWeatherData() - duplicated rather than shared since OrbIt is meant to
// stay self-contained from the original widgets.
bool OrbItWidget::fetchWeatherData(WeatherDataModel &model) {
    const String weatherLocation = WEATHER_LOCATION;
#ifdef WEATHER_UNITS_METRIC
    const String weatherUnits = "metric";
#else
    const String weatherUnits = "us";
#endif
    const String weatherApiKey = WEATHER_API_KEY;
    const String httpRequestAddress = "https://weather.visualcrossing.com/VisualCrossingWebServices/rest/services/timeline/" +
                                       weatherLocation + "/next3days?key=" + weatherApiKey + "&unitGroup=" + weatherUnits +
                                       "&include=days,current&iconSet=icons1&lang=" + LOC_LANG;

    HTTPClient http;
    http.begin(httpRequestAddress);
    int httpCode = http.GET();
    if (httpCode <= 0) {
        Serial.printf("OrbIt: weather HTTP request failed, error: %s\n", http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getString());
    http.end();
    if (error) {
        Serial.println("OrbIt: weather deserializeJson() failed");
        return false;
    }

    model.setCityName(doc["resolvedAddress"].as<String>());
    model.setCurrentTemperature(doc["currentConditions"]["temp"].as<float>());
    model.setCurrentText(doc["days"][0]["description"].as<String>());
    model.setCurrentIcon(doc["currentConditions"]["icon"].as<String>());
    model.setTodayHigh(doc["days"][0]["tempmax"].as<float>());
    model.setTodayLow(doc["days"][0]["tempmin"].as<float>());
    return true;
}

// Mirrors StockWidget::getStockData() - duplicated for the same self-containment reason.
void OrbItWidget::fetchTickerData(StockDataModel &stock) {
    String httpRequestAddress = "https://api.twelvedata.com/quote?apikey=e03fc53524454ab8b65d91b23c669cc5&symbol=" + stock.getSymbol();

    HTTPClient http;
    http.begin(httpRequestAddress);
    int httpCode = http.GET();

    if (httpCode > 0) {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            float currentPrice = doc["close"].as<float>();
            if (currentPrice > 0.0) {
                stock.setCurrentPrice(doc["close"].as<float>());
                stock.setPercentChange(doc["percent_change"].as<float>() / 100);
                stock.setPriceChange(doc["change"].as<float>());
                stock.setHighPrice(doc["fifty_two_week"]["high"].as<float>());
                stock.setLowPrice(doc["fifty_two_week"]["low"].as<float>());
                stock.setCompany(doc["name"].as<String>());
                stock.setTicker(doc["symbol"].as<String>());
                stock.setCurrencySymbol(doc["currency"].as<String>());
            } else {
                Serial.println("OrbIt: skipping invalid ticker data for: " + stock.getSymbol());
            }
        } else {
            Serial.println("OrbIt: ticker deserializeJson() failed");
        }
    } else {
        Serial.printf("OrbIt: ticker HTTP request failed, error: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
}

void OrbItWidget::draw(bool force) {
    for (int i = 0; i < NUM_SCREENS; i++) {
        drawSlot(i, m_slots[i], force);
    }

    // One isChanged() flag can drive multiple weather slots in the same pass (e.g. icon on one
    // screen, temperature on another) - reset it once after all of them have had a chance to see it,
    // not inside drawWeatherSlot() itself.
    if (m_weatherModel.isChanged()) {
        m_weatherModel.setChangedStatus(false);
    }
}

void OrbItWidget::drawSlot(int displayIndex, OrbItSlot &slot, bool force) {
    switch (slot.source) {
    case OrbItSource::TIME:
        drawTimeSlot(displayIndex, slot, force);
        break;
    case OrbItSource::ANALOG_CLOCK:
        drawAnalogClockSlot(displayIndex, slot, force);
        break;
    case OrbItSource::GAUGE:
        drawGaugeSlot(displayIndex, slot, force);
        break;
    case OrbItSource::SYS_MONITOR:
        drawSysMonitorSlot(displayIndex, slot, force);
        break;
    case OrbItSource::ASTEROIDS:
        drawAsteroidsSlot(displayIndex, slot, force);
        break;
    case OrbItSource::COUNTDOWN:
        drawCountdownSlot(displayIndex, slot, force);
        break;
    case OrbItSource::WEATHER:
        drawWeatherSlot(displayIndex, slot, force);
        break;
    case OrbItSource::TICKER:
        drawTickerSlot(displayIndex, slot, force);
        break;
    case OrbItSource::CUSTOM:
        drawCustomSlot(displayIndex, slot, force);
        break;
    case OrbItSource::BLANK:
        if (force || !slot.everDrawn) {
            m_manager.selectScreen(displayIndex);
            m_manager.fillScreen(TFT_BLACK);
            slot.everDrawn = true;
        }
        break;
    }
}

void OrbItWidget::drawTimeSlot(int displayIndex, OrbItSlot &slot, bool force) {
    if (slot.pendingValue == slot.lastRenderedValue && !force) {
        return;
    }
    m_manager.setFont(DEFAULT_FONT);
    m_timeControl.drawFullTime(displayIndex, GlobalTime::getInstance(), slot.showDate, slot.showDay, slot.format24Hour, TFT_WHITE, TFT_BLACK);
    slot.lastRenderedValue = slot.pendingValue;
    slot.everDrawn = true;
}

void OrbItWidget::drawAnalogClockSlot(int displayIndex, OrbItSlot &slot, bool force) {
    if (slot.pendingValue == slot.lastRenderedValue && !force) {
        return;
    }
    bool fullRedraw = force || !slot.everDrawn;
    m_analogClockControl.draw(displayIndex, GlobalTime::getInstance(), slot.analogColors, m_analogClockHands[displayIndex], fullRedraw);
    slot.lastRenderedValue = slot.pendingValue;
    slot.everDrawn = true;
}

void OrbItWidget::drawGaugeSlot(int displayIndex, OrbItSlot &slot, bool force) {
    if (slot.pendingValue == slot.lastRenderedValue && !force) {
        return;
    }
    m_manager.setFont(DEFAULT_FONT);
    // `force` only (not slot.everDrawn) - GaugeControl decides full-vs-incremental redraw itself
    // from its own GaugeState (style/color changed?), not from this widget's generic per-slot
    // bookkeeping, since most gauge updates are pure value changes that shouldn't force a full
    // repaint the way switching sources into this slot does.
    m_gaugeControl.draw(displayIndex, slot.gaugeConfig, TFT_BLACK, m_gaugeStates[displayIndex], force);
    slot.lastRenderedValue = slot.pendingValue;
    slot.everDrawn = true;
}

void OrbItWidget::drawSysMonitorSlot(int displayIndex, OrbItSlot &slot, bool force) {
    if (slot.pendingValue == slot.lastRenderedValue && !force) {
        return;
    }
    m_manager.setFont(DEFAULT_FONT);
    // `force` only (not slot.everDrawn) - same reasoning as drawGaugeSlot(): SysMonitorControl
    // decides full-vs-per-quadrant redraw itself from its own SysMonitorState.
    m_sysMonitorControl.draw(displayIndex, slot.sysMonitorConfig, m_sysMonitorStates[displayIndex], force);
    slot.lastRenderedValue = slot.pendingValue;
    slot.everDrawn = true;
}

void OrbItWidget::drawAsteroidsSlot(int displayIndex, OrbItSlot &slot, bool force) {
    if (slot.pendingValue == slot.lastRenderedValue && !force) {
        return;
    }
    m_manager.setFont(DEFAULT_FONT);
    bool fullRedraw = force || !slot.everDrawn;
    m_asteroidsControl.draw(displayIndex, m_asteroidsStates[displayIndex], fullRedraw);
    slot.lastRenderedValue = slot.pendingValue;
    slot.everDrawn = true;
}

void OrbItWidget::drawCountdownSlot(int displayIndex, OrbItSlot &slot, bool force) {
    if (slot.pendingValue == slot.lastRenderedValue && !force) {
        return;
    }
    bool fullRedraw = force || !slot.everDrawn;
    m_countdownControl.draw(displayIndex, slot.countdownConfig, m_countdownRuntimes[displayIndex], fullRedraw);
    slot.lastRenderedValue = slot.pendingValue;
    slot.everDrawn = true;
}

void OrbItWidget::drawWeatherSlot(int displayIndex, OrbItSlot &slot, bool force) {
    if (!m_weatherModel.isChanged() && !force && slot.everDrawn) {
        return;
    }
    m_manager.setFont(DEFAULT_FONT);
    switch (slot.weatherElement) {
    case WeatherElement::ICON:
        m_weatherControl.drawIcon(displayIndex, m_weatherScreenMode, m_weatherModel.getCurrentIcon(), 0, 0, 1);
        break;
    case WeatherElement::TEMPERATURE:
        m_weatherControl.drawTemperature(displayIndex, m_weatherScreenMode, m_weatherModel);
        break;
    case WeatherElement::CONDITION:
        m_weatherControl.drawCondition(displayIndex, m_weatherScreenMode, m_weatherModel);
        break;
    }
    slot.everDrawn = true;
}

void OrbItWidget::drawTickerSlot(int displayIndex, OrbItSlot &slot, bool force) {
    StockDataModel &stock = m_tickerModels[displayIndex];
    if (!stock.isChanged() && !force && slot.everDrawn) {
        return;
    }
    m_manager.setFont(DEFAULT_FONT);
    m_tickerControl.draw(displayIndex, stock, TFT_WHITE, TFT_BLACK);
    stock.setChangedStatus(false);
    slot.everDrawn = true;
}

void OrbItWidget::drawCustomSlot(int displayIndex, OrbItSlot &slot, bool force) {
    WebDataModel &model = m_customModels[displayIndex];
    if (!model.isChanged() && !force && slot.everDrawn) {
        return;
    }
    m_manager.selectScreen(displayIndex);
    model.draw(m_manager);
    model.setChangedStatus(false);
    slot.everDrawn = true;
}

void OrbItWidget::buttonPressed(uint8_t buttonId, ButtonState state) {
    // Navigation is via orbit-api, not buttons - intentionally no-op.
}

String OrbItWidget::getName() {
    return "OrbIt";
}

// ===================== orbit-api =====================

void OrbItWidget::serviceApi() {
    if (!m_serverStarted) {
        setupApiRoutes();
        m_server.begin();
        m_serverStarted = true;
        Serial.println("OrbIt: orbit-api listening on port 80");
    }
    m_server.handleClient();
}

void OrbItWidget::setupApiRoutes() {
    m_server.on(API_BASE, HTTP_GET, [this]() { handleGetScreens(); });
    m_server.on(API_BASE, HTTP_POST, [this]() { handlePostScreens(); });

    for (int i = 0; i < NUM_SCREENS; i++) {
        String uri = String(API_BASE) + "/" + String(i);
        m_server.on(uri, HTTP_GET, [this, i]() { handleGetScreen(i); });
        m_server.on(uri, HTTP_POST, [this, i]() { handlePostScreen(i); });
        m_server.on(uri + "/refresh", HTTP_POST, [this, i]() { handleRefreshScreen(i); });
    }

    // Only routes for screens 0..NUM_SCREENS-1 are registered above, so an out-of-range index (or
    // any other unmatched path) falls through to here rather than WebServer's default plain-text
    // 404 - keeps every orbit-api error response in the same {"error": "..."} JSON shape.
    m_server.onNotFound([this]() { sendError(404, "not found: " + m_server.uri()); });
}

void OrbItWidget::sendJson(int code, const JsonDocument &doc) {
    String body;
    serializeJson(doc, body);
    m_server.send(code, "application/json", body);
}

void OrbItWidget::sendError(int code, const String &message) {
    JsonDocument doc;
    doc["error"] = message;
    sendJson(code, doc);
}

void OrbItWidget::slotToJson(int index, const OrbItSlot &slot, JsonObject out) {
    out["screen"] = index;
    out["control"] = sourceToString(slot.source);
    out["updatedAt"] = slot.updatedAt;

    JsonObject params = out["params"].to<JsonObject>();
    switch (slot.source) {
    case OrbItSource::TIME:
        params["showDate"] = slot.showDate;
        params["showDay"] = slot.showDay;
        params["format24Hour"] = slot.format24Hour;
        break;
    case OrbItSource::ANALOG_CLOCK:
        // Reported as raw numeric RGB565 values, not color names - Utils::stringToColor() has no
        // reverse (color->name) lookup, same lossy-read-back tradeoff already accepted for a rich
        // "custom" slot's element array.
        params["background"] = slot.analogColors.background;
        params["tickColor"] = slot.analogColors.tick;
        params["hourColor"] = slot.analogColors.hourHand;
        params["minuteColor"] = slot.analogColors.minuteHand;
        params["secondColor"] = slot.analogColors.secondHand;
        break;
    case OrbItSource::GAUGE:
        params["label"] = slot.gaugeConfig.label;
        params["value"] = slot.gaugeConfig.value;
        params["min"] = slot.gaugeConfig.min;
        params["max"] = slot.gaugeConfig.max;
        // Same raw-numeric-RGB565 read-back tradeoff as analogClock above.
        params["color"] = slot.gaugeConfig.color;
        params["trackColor"] = slot.gaugeConfig.trackColor;
        params["style"] = gaugeStyleToString(slot.gaugeConfig.style);
        break;
    case OrbItSource::SYS_MONITOR:
        params["cpu"] = slot.sysMonitorConfig.cpu;
        params["cpuTemp"] = slot.sysMonitorConfig.cpuTemp;
        params["gpu"] = slot.sysMonitorConfig.gpu;
        params["gpuTemp"] = slot.sysMonitorConfig.gpuTemp;
        params["ram"] = slot.sysMonitorConfig.ram;
        params["ramTotal"] = slot.sysMonitorConfig.ramTotal;
        params["ssdTemp"] = slot.sysMonitorConfig.ssdTemp;
        params["center"] = slot.sysMonitorConfig.center;
        break;
    case OrbItSource::ASTEROIDS:
        break;
    case OrbItSource::COUNTDOWN: {
        params["label"] = slot.countdownConfig.label;
        params["durationSeconds"] = slot.countdownConfig.durationSeconds;
        params["color"] = slot.countdownConfig.color;
        // Status fields, not part of what a write accepts - GET is the only place these matter.
        const CountdownRuntime &runtime = m_countdownRuntimes[index];
        params["state"] = countdownStateToString(runtime.state);
        params["remainingSeconds"] = runtime.remainingMs(millis()) / 1000;
        break;
    }
    case OrbItSource::WEATHER:
        params["element"] = weatherElementToString(slot.weatherElement);
        break;
    case OrbItSource::TICKER:
        params["symbol"] = slot.tickerSymbol;
        params["pollIntervalSeconds"] = slot.tickerPollIntervalMs / 1000;
        break;
    case OrbItSource::CUSTOM: {
        // Best-effort, lossy read-back: WebDataModel doesn't expose a way to reconstruct the exact
        // element array it was given, so a rich (element-array) custom slot reports a count rather
        // than the original primitives. A plain-text custom slot round-trips exactly.
        WebDataModel &model = m_customModels[index];
        params["label"] = model.getLabel();
        if (model.getElementsCount() > 0) {
            params["elementCount"] = model.getElementsCount();
        } else {
            params["data"] = model.getData();
        }
        break;
    }
    case OrbItSource::BLANK:
        break;
    }
}

// Parses {"control": "...", "params": {...}} into outSlot. Does not touch anything but outSlot -
// callers decide when/whether it's safe to apply. Returns false + a human-readable errorMessage on
// any validation failure (unknown control, missing/malformed params) - never partially applies.
bool OrbItWidget::parseSlotConfig(JsonObject obj, OrbItSlot &outSlot, String &errorMessage, int screenIndex) {
    if (!obj["control"].is<const char *>()) {
        errorMessage = "missing or invalid 'control'";
        return false;
    }
    String control = obj["control"].as<String>();
    JsonObject params = obj["params"].is<JsonObject>() ? obj["params"].as<JsonObject>() : JsonObject();

    if (control == "blank") {
        outSlot.source = OrbItSource::BLANK;
        return true;
    }

    if (control == "asteroids") {
        outSlot.source = OrbItSource::ASTEROIDS;
        return true;
    }

    if (control == "countdown") {
        if (!params["action"].is<const char *>()) {
            errorMessage = "control 'countdown' requires params.action ('set'|'pause'|'resume'|'stop'|'restart')";
            return false;
        }
        String action = params["action"].as<String>();
        // screenIndex < 0 means "no specific screen to validate state against" - not currently
        // reachable (every live caller passes a real index), kept only as a safe fallback.
        bool isCurrentlyCountdown = screenIndex >= 0 && m_slots[screenIndex].source == OrbItSource::COUNTDOWN;
        CountdownRunState currentState = screenIndex >= 0 ? m_countdownRuntimes[screenIndex].state : CountdownRunState::IDLE;

        if (action == "set") {
            // The only action allowed to turn a non-countdown slot into a countdown for the first
            // time - every other action below requires the slot to already be one.
            if (!params["durationSeconds"].is<int>() || params["durationSeconds"].as<int>() <= 0) {
                errorMessage = "countdown action 'set' requires a positive integer params.durationSeconds";
                return false;
            }
        } else if (action == "pause") {
            if (!isCurrentlyCountdown || currentState != CountdownRunState::RUNNING) {
                errorMessage = "countdown action 'pause' requires the countdown to currently be running";
                return false;
            }
        } else if (action == "resume") {
            if (!isCurrentlyCountdown || currentState != CountdownRunState::PAUSED) {
                errorMessage = "countdown action 'resume' requires the countdown to currently be paused";
                return false;
            }
        } else if (action == "restart") {
            if (!isCurrentlyCountdown || m_slots[screenIndex].countdownConfig.durationSeconds == 0) {
                errorMessage = "countdown action 'restart' requires the screen to already be a countdown with a duration set (use action 'set' first)";
                return false;
            }
        } else if (action == "stop") {
            if (!isCurrentlyCountdown) {
                errorMessage = "countdown action 'stop' requires the screen to currently be a countdown";
                return false;
            }
        } else {
            errorMessage = "unknown countdown action '" + action + "'";
            return false;
        }

        outSlot.source = OrbItSource::COUNTDOWN;
        return true;
    }

    if (control == "time") {
        // Both optional, default false - just HH:MM unless asked for more.
        outSlot.showDate = params["showDate"].is<bool>() ? params["showDate"].as<bool>() : false;
        outSlot.showDay = params["showDay"].is<bool>() ? params["showDay"].as<bool>() : false;
        outSlot.format24Hour = params["format24Hour"].is<bool>() ? params["format24Hour"].as<bool>() : false;
        outSlot.source = OrbItSource::TIME;
        return true;
    }

    if (control == "analogClock") {
        // All optional - each falls back to AnalogClockColors' own default (see
        // AnalogClockControl.h) if omitted. Color names go through the same Utils::stringToColor()
        // parser the rest of the codebase already uses for color strings (WebDataModel etc.).
        AnalogClockColors colors; // defaults
        if (params["background"].is<const char *>()) {
            colors.background = Utils::stringToColor(params["background"].as<String>());
        }
        if (params["tickColor"].is<const char *>()) {
            colors.tick = Utils::stringToColor(params["tickColor"].as<String>());
        }
        if (params["hourColor"].is<const char *>()) {
            colors.hourHand = Utils::stringToColor(params["hourColor"].as<String>());
        }
        if (params["minuteColor"].is<const char *>()) {
            colors.minuteHand = Utils::stringToColor(params["minuteColor"].as<String>());
        }
        if (params["secondColor"].is<const char *>()) {
            colors.secondHand = Utils::stringToColor(params["secondColor"].as<String>());
        }
        outSlot.analogColors = colors;
        outSlot.source = OrbItSource::ANALOG_CLOCK;
        return true;
    }

    if (control == "gauge") {
        // All optional - each falls back to GaugeConfig's own default (see GaugeControl.h) if
        // omitted, matching the mockup's anticipated shape (references/orbit-api.md).
        GaugeConfig gauge;
        if (params["label"].is<const char *>()) {
            gauge.label = params["label"].as<String>();
        }
        if (params["value"].is<float>()) {
            gauge.value = params["value"].as<float>();
        }
        if (params["min"].is<float>()) {
            gauge.min = params["min"].as<float>();
        }
        if (params["max"].is<float>()) {
            gauge.max = params["max"].as<float>();
        }
        if (params["color"].is<const char *>()) {
            gauge.color = Utils::stringToColor(params["color"].as<String>());
        }
        if (params["trackColor"].is<const char *>()) {
            gauge.trackColor = Utils::stringToColor(params["trackColor"].as<String>());
        }
        if (params["style"].is<const char *>()) {
            String style = params["style"].as<String>();
            if (style == "ring") {
                gauge.style = GaugeStyle::RING;
            } else if (style == "speedometer") {
                gauge.style = GaugeStyle::SPEEDOMETER;
            } else if (style == "instrument") {
                gauge.style = GaugeStyle::INSTRUMENT;
            } else {
                errorMessage = "unknown gauge style '" + style + "'";
                return false;
            }
        }
        outSlot.gaugeConfig = gauge;
        outSlot.source = OrbItSource::GAUGE;
        return true;
    }

    if (control == "sysMonitor") {
        // All optional - each falls back to SysMonitorConfig's own default (0, or "" for center) if
        // omitted, same convention as "gauge" above.
        SysMonitorConfig sys;
        if (params["cpu"].is<float>()) {
            sys.cpu = params["cpu"].as<float>();
        }
        if (params["cpuTemp"].is<float>()) {
            sys.cpuTemp = params["cpuTemp"].as<float>();
        }
        if (params["gpu"].is<float>()) {
            sys.gpu = params["gpu"].as<float>();
        }
        if (params["gpuTemp"].is<float>()) {
            sys.gpuTemp = params["gpuTemp"].as<float>();
        }
        if (params["ram"].is<float>()) {
            sys.ram = params["ram"].as<float>();
        }
        if (params["ramTotal"].is<float>()) {
            sys.ramTotal = params["ramTotal"].as<float>();
        }
        if (params["ssdTemp"].is<float>()) {
            sys.ssdTemp = params["ssdTemp"].as<float>();
        }
        if (params["center"].is<const char *>()) {
            String center = params["center"].as<String>();
            if (center == "none" || center == "cpu" || center == "cpuTemp" || center == "gpu" || center == "gpuTemp" || center == "ram" || center == "ssdTemp") {
                sys.center = center;
            } else {
                errorMessage = "unknown sysMonitor center '" + center + "'";
                return false;
            }
        }
        outSlot.sysMonitorConfig = sys;
        outSlot.source = OrbItSource::SYS_MONITOR;
        return true;
    }

    if (control == "weather") {
        if (!params["element"].is<const char *>()) {
            errorMessage = "control 'weather' requires params.element";
            return false;
        }
        String element = params["element"].as<String>();
        if (element == "icon") {
            outSlot.weatherElement = WeatherElement::ICON;
        } else if (element == "temperature") {
            outSlot.weatherElement = WeatherElement::TEMPERATURE;
        } else if (element == "condition") {
            outSlot.weatherElement = WeatherElement::CONDITION;
        } else {
            errorMessage = "unknown weather element '" + element + "'";
            return false;
        }
        outSlot.source = OrbItSource::WEATHER;
        return true;
    }

    if (control == "ticker") {
        if (!params["symbol"].is<const char *>() || params["symbol"].as<String>().length() == 0) {
            errorMessage = "control 'ticker' requires a non-empty params.symbol";
            return false;
        }
        outSlot.tickerSymbol = params["symbol"].as<String>();
        // Optional - defaults to OrbItSlot's own default (15 minutes, matching StockWidget).
        // Silently clamped up to MIN_TICKER_POLL_INTERVAL_MS rather than rejected: a client asking
        // for "as fast as possible" should just get the fastest safe rate, not an error to retry
        // with a magic number it has to already know.
        if (params["pollIntervalSeconds"].is<int>()) {
            int requestedSeconds = params["pollIntervalSeconds"].as<int>();
            unsigned long requestedMs = requestedSeconds > 0 ? (unsigned long) requestedSeconds * 1000UL : 0;
            outSlot.tickerPollIntervalMs = max(requestedMs, MIN_TICKER_POLL_INTERVAL_MS);
        }
        outSlot.source = OrbItSource::TICKER;
        return true;
    }

    if (control == "custom") {
        // No further validation here - params is the exact shape WebDataModel::parseData() already
        // parses defensively (missing/malformed fields fall back to sane defaults, matching how
        // WebDataWidget's own remote JSON is handled). Actually applying it happens in
        // applySlotConfig(), which needs the raw JsonObject, not just this typed OrbItSlot.
        outSlot.source = OrbItSource::CUSTOM;
        return true;
    }

    errorMessage = "unknown control '" + control + "'";
    return false;
}

// Applies a validated slot config and resets its render-diffing state so the change is guaranteed
// to actually redraw next time this widget's draw() runs - which WidgetSet only calls while OrbIt
// is the currently displayed widget, so it's always safe to touch m_slots[]/data models here
// without needing to know whether OrbIt is on-screen right now. rawConfig is the original
// {"control":..., "params":...} object - only used for "custom", which needs to hand its params
// straight to WebDataModel::parseData() rather than going through typed OrbItSlot fields.
void OrbItWidget::applySlotConfig(int index, const OrbItSlot &newConfig, JsonObject rawConfig, bool immediateFetch) {
    OrbItSlot &slot = m_slots[index];
    bool wasGauge = (slot.source == OrbItSource::GAUGE);
    bool wasSysMonitor = (slot.source == OrbItSource::SYS_MONITOR);

    // A countdown's 'set' action turning a non-countdown slot into a countdown for the first time
    // snapshots whatever was here before, so 'stop' can hand the screen back later (consumed by
    // consumeCountdownRestore()). Must happen here, before slot.* below gets overwritten by
    // newConfig - slotToJson(index, slot, ...) needs the OLD (pre-overwrite) slot.
    if (newConfig.source == OrbItSource::COUNTDOWN && slot.source != OrbItSource::COUNTDOWN) {
        JsonObject peekParams = rawConfig["params"].is<JsonObject>() ? rawConfig["params"].as<JsonObject>() : JsonObject();
        bool isSetAction = peekParams["action"].is<const char *>() && String(peekParams["action"].as<const char *>()) == "set";
        if (isSetAction) {
            JsonDocument snapshotDoc;
            slotToJson(index, slot, snapshotDoc.to<JsonObject>());
            serializeJson(snapshotDoc, m_countdownPreviousConfig[index]);
        }
    }

    slot.source = newConfig.source;
    slot.showDate = newConfig.showDate;
    slot.showDay = newConfig.showDay;
    slot.format24Hour = newConfig.format24Hour;
    slot.weatherElement = newConfig.weatherElement;
    slot.tickerSymbol = newConfig.tickerSymbol;
    slot.tickerPollIntervalMs = newConfig.tickerPollIntervalMs;
    slot.analogColors = newConfig.analogColors;
    slot.gaugeConfig = newConfig.gaugeConfig;
    slot.sysMonitorConfig = newConfig.sysMonitorConfig;
    // A restored-at-boot slot hasn't been "written this session" in any meaningful sense - millis()
    // resets every reboot, so a persisted value would be misleading, not just stale.
    slot.updatedAt = immediateFetch ? millis() : 0;

    // Force the next draw() pass to repaint this screen from scratch, regardless of the per-source
    // isChanged()/lastValue guards.
    slot.pendingValue = "";
    slot.lastRenderedValue = "";
    slot.everDrawn = false;

    // Only reset GaugeState when this slot is newly becoming a gauge (from some other source, or
    // for the first time) - repeated gauge-to-gauge reconfiguration (the common case: a client
    // just POSTing an updated value) should NOT reset it, or every update would force a full
    // repaint and bring back the exact flicker this state tracking exists to avoid.
    if (slot.source == OrbItSource::GAUGE && !wasGauge) {
        m_gaugeStates[index] = GaugeState();
    }

    // Same reasoning as GaugeState above: only reset on newly becoming sysMonitor, not on every
    // value update, or a plain value POST would force a full repaint every time.
    if (slot.source == OrbItSource::SYS_MONITOR && !wasSysMonitor) {
        m_sysMonitorStates[index] = SysMonitorState();
    }

    if (slot.source == OrbItSource::TICKER) {
        m_tickerModels[index] = StockDataModel();
        m_tickerModels[index].setSymbol(slot.tickerSymbol);
        if (immediateFetch) {
            fetchTickerData(m_tickerModels[index]); // don't make the caller wait out the normal poll delay
            slot.tickerDelayPrev = millis(); // ... and don't let the very next update() tick immediately refetch it again
        } else {
            // WiFi isn't up yet (restoring at construction time) - the normal update() polling loop
            // will fetch once it is.
            slot.tickerDelayPrev = 0;
        }
    }

    if (slot.source == OrbItSource::COUNTDOWN) {
        // Like CUSTOM above, this needs the raw params object directly (params.action isn't a typed
        // OrbItSlot field - it's a one-shot command, not persisted state) rather than anything off
        // newConfig. parseSlotConfig() has already validated action against this slot's current
        // CountdownRuntime state by the time this runs.
        JsonObject params = rawConfig["params"].is<JsonObject>() ? rawConfig["params"].as<JsonObject>() : JsonObject();
        applyCountdownAction(index, params["action"].as<String>(), params);
    }

    if (slot.source == OrbItSource::CUSTOM) {
        // Reuse the existing instance and let parseData() (which internally does the proper
        // delete[]-then-new[] dance in setElementsCount()/initElements()) replace its contents -
        // NOT `m_customModels[index] = WebDataModel()`, which would shallow-copy over the old
        // m_elements pointer and leak whatever it previously pointed to.
        JsonObject params = rawConfig["params"].is<JsonObject>() ? rawConfig["params"].as<JsonObject>() : JsonObject();
        m_customModels[index].parseData(params, TFT_WHITE, TFT_BLACK);
    }
}

// Restores a previously-saved layout from NVS, if any, overriding the compile-time defaults set
// just before this is called in the constructor. Safe to run before WiFi is up - Preferences/NVS
// is local flash storage, no network involved.
void OrbItWidget::loadPersistedLayout() {
    m_preferences.begin("orbit", true); // read-only
    String json = m_preferences.getString("layout", "");
    m_preferences.end();

    if (json.length() == 0) {
        return; // nothing saved yet - keep the compile-time defaults
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);
    if (error || !doc["screens"].is<JsonArray>()) {
        Serial.println("OrbIt: ignoring corrupt persisted layout in NVS");
        return;
    }

    for (JsonObject entry : doc["screens"].as<JsonArray>()) {
        if (!entry["screen"].is<int>()) {
            continue;
        }
        int index = entry["screen"].as<int>();
        if (index < 0 || index >= NUM_SCREENS) {
            continue;
        }

        // countdown is never persisted (persistLayout() skips it entirely - see its own comment) so a
        // live layout will never actually contain one of these entries. This only guards against a
        // leftover entry from an older firmware version that did persist countdown slots - drop it
        // silently rather than restore anything from it; that screen just falls back to whatever its
        // compile-time default (or another persisted entry) says instead.
        if (entry["control"].is<const char *>() && String(entry["control"].as<const char *>()) == "countdown") {
            continue;
        }

        String errorMessage;
        OrbItSlot parsed;
        if (!parseSlotConfig(entry, parsed, errorMessage)) {
            Serial.println("OrbIt: skipping invalid persisted slot " + String(index) + ": " + errorMessage);
            continue;
        }
        applySlotConfig(index, parsed, entry, /* immediateFetch */ false);
    }
    Serial.println("OrbIt: restored layout from NVS");
}

// Saves the complete current layout as one JSON blob - simpler and (for 5 small slots) not
// meaningfully more flash wear than tracking which individual slot(s) changed. countdown slots are
// deliberately skipped entirely: a countdown is a one-off "timebox this task" tool, not a
// permanent screen assignment the way every other control is - it shouldn't outlive a reboot even
// as an idle placeholder. A skipped screen just isn't mentioned in the saved array, so on restore
// it falls back to whatever its compile-time default (or another persisted entry) says instead.
void OrbItWidget::persistLayout() {
    JsonDocument doc;
    JsonArray screens = doc["screens"].to<JsonArray>();
    for (int i = 0; i < NUM_SCREENS; i++) {
        if (m_slots[i].source == OrbItSource::COUNTDOWN) {
            continue;
        }
        slotToJson(i, m_slots[i], screens.add<JsonObject>());
    }
    String json;
    serializeJson(doc, json);

    m_preferences.begin("orbit", false); // read-write
    m_preferences.putString("layout", json);
    m_preferences.end();
}

void OrbItWidget::handleGetScreens() {
    JsonDocument doc;
    JsonArray screens = doc["screens"].to<JsonArray>();
    for (int i = 0; i < NUM_SCREENS; i++) {
        slotToJson(i, m_slots[i], screens.add<JsonObject>());
    }
    sendJson(200, doc);
}

void OrbItWidget::handleGetScreen(int index) {
    JsonDocument doc;
    slotToJson(index, m_slots[index], doc.to<JsonObject>());
    sendJson(200, doc);
}

void OrbItWidget::handlePostScreen(int index) {
    if (!m_server.hasArg("plain")) {
        sendError(400, "missing JSON body");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, m_server.arg("plain"));
    if (error) {
        sendError(400, "malformed JSON body");
        return;
    }

    OrbItSlot parsed;
    String errorMessage;
    if (!parseSlotConfig(doc.as<JsonObject>(), parsed, errorMessage, index)) {
        sendError(400, errorMessage);
        return;
    }

    applySlotConfig(index, parsed, doc.as<JsonObject>());
    consumeCountdownRestore(index);
    persistLayout();

    JsonDocument response;
    slotToJson(index, m_slots[index], response.to<JsonObject>());
    sendJson(200, response);
}

// On-demand refetch, bypassing the slot's own pollIntervalSeconds/MIN_TICKER_POLL_INTERVAL_MS
// floor entirely - that floor exists to keep *automatic* polling within twelvedata's free-tier
// budget (see MIN_TICKER_POLL_INTERVAL_MS above), not to stop a person from deliberately asking for
// the latest price right now. Blowing through the daily call budget via repeated manual refreshes
// is on the caller, same as it would be for StockWidget's own middle-button refresh.
void OrbItWidget::handleRefreshScreen(int index) {
    OrbItSlot &slot = m_slots[index];
    if (slot.source != OrbItSource::TICKER) {
        sendError(400, "screen " + String(index) + " is not a 'ticker' slot - nothing to refresh");
        return;
    }

    fetchTickerData(m_tickerModels[index]);
    slot.tickerDelayPrev = millis(); // don't let the very next automatic poll immediately re-fetch too

    JsonDocument response;
    slotToJson(index, slot, response.to<JsonObject>());
    sendJson(200, response);
}

void OrbItWidget::handlePostScreens() {
    if (!m_server.hasArg("plain")) {
        sendError(400, "missing JSON body");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, m_server.arg("plain"));
    if (error) {
        sendError(400, "malformed JSON body");
        return;
    }

    if (!doc["screens"].is<JsonArray>()) {
        sendError(400, "body must be {\"screens\": [...]}");
        return;
    }
    JsonArray screensArray = doc["screens"].as<JsonArray>();

    // Validate every entry before applying any of them, so a bad entry can't leave a partially
    // applied batch behind.
    int indices[NUM_SCREENS];
    OrbItSlot parsedSlots[NUM_SCREENS];
    JsonObject rawEntries[NUM_SCREENS];
    int count = 0;

    for (JsonObject entry : screensArray) {
        if (count >= NUM_SCREENS) {
            sendError(400, "too many entries in 'screens' (max " + String(NUM_SCREENS) + ")");
            return;
        }
        if (!entry["screen"].is<int>()) {
            sendError(400, "each entry requires an integer 'screen'");
            return;
        }
        int screenIndex = entry["screen"].as<int>();
        if (screenIndex < 0 || screenIndex >= NUM_SCREENS) {
            sendError(400, "'screen' must be between 0 and " + String(NUM_SCREENS - 1));
            return;
        }

        String errorMessage;
        OrbItSlot parsed;
        if (!parseSlotConfig(entry, parsed, errorMessage, screenIndex)) {
            sendError(400, "screen " + String(screenIndex) + ": " + errorMessage);
            return;
        }

        indices[count] = screenIndex;
        parsedSlots[count] = parsed;
        rawEntries[count] = entry;
        count++;
    }

    for (int i = 0; i < count; i++) {
        applySlotConfig(indices[i], parsedSlots[i], rawEntries[i]);
        consumeCountdownRestore(indices[i]);
    }
    if (count > 0) {
        persistLayout();
    }

    JsonDocument response;
    JsonArray screens = response["screens"].to<JsonArray>();
    for (int i = 0; i < NUM_SCREENS; i++) {
        slotToJson(i, m_slots[i], screens.add<JsonObject>());
    }
    sendJson(200, response);
}
