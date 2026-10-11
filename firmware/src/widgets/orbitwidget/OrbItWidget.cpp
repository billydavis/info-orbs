#include "OrbItWidget.h"

#include "config_helper.h"
#if ORBIT_CONTROL_CUSTOM
    #include <LittleFS.h>
#endif

#ifdef DISABLE_WEB_SERVER
    #error "The OrbIt widget is configured through orbit-api, so it needs the web server - remove DISABLE_WEB_SERVER or ORBIT_WIDGET_ENABLED from config.h"
#endif

namespace {
    const char *API_BASE = "/orbit/api/v1/screens";
    const char *WEATHER_PREVIEW_URI = "/orbit/api/v1/weather/preview";
    const char *CAPABILITIES_URI = "/orbit/api/v1/capabilities";

    const char *NVS_NAMESPACE = "orbit";
    const char *NVS_LAYOUT_KEY = "layout";
    const char *NOT_SAVED_WARNING = "applied, but it could not be saved - it will not survive a reboot";

#if ORBIT_CONTROL_CUSTOM
    // Each custom drawing is saved as its own file on the LittleFS data partition rather than in NVS
    // with the rest of the layout. A drawing can run to 10KB or more, and the 20KB NVS partition -
    // shared with WiFi credentials and everything else - has only about 8KB to spare: measured on a
    // real device, an 8KB drawing left too little room to rewrite even the layout itself. Keeping
    // drawings out of the layout also means one is only written to flash when it actually changes,
    // not on every write to any screen (e.g. a sysMonitor being fed live values).
    String drawingPath(int index) {
        return "/orbit-draw" + String(index) + ".json";
    }

    // Mounts the data partition on first use, formatting it if it has never held a filesystem.
    bool mountDrawingStore() {
        static bool mounted = false;
        if (!mounted) {
            mounted = LittleFS.begin(true);
            if (!mounted) {
                Serial.println("OrbIt: unable to mount LittleFS - custom drawings can't be saved or restored");
            }
        }
        return mounted;
    }
#endif

    // twelvedata's free tier is 8 calls/minute and 800 calls/day (verified against
    // https://twelvedata.com/pricing) - the daily cap, not the per-minute one, is what actually
    // constrains a ticker slot meant to poll continuously: at 800/day, one ticker calling forever can
    // only average one call every ~108s. A 5-minute (300s) floor keeps a single continuously-polling
    // ticker slot well under that (288 calls/day) with headroom for a second slot before the daily cap
    // becomes a real risk - orbit-api deliberately does not try to track/divide the shared daily budget
    // across slots itself (that's a bigger feature); if you configure several screens as tickers, it's
    // on you to keep the combined polling reasonable.
    const unsigned long MIN_TICKER_POLL_INTERVAL_MS = 300000;

    // Plenty for "City, Region, Country" or a pair of coordinates, and keeps the saved layout small.
    const unsigned int MAX_WEATHER_LOCATION_LENGTH = 64;

    // How long a cycling screensaver may stay on each effect: long enough to actually watch one, and
    // short enough that the millisecond count can't overflow.
    const long MIN_SCREENSAVER_CYCLE_SECONDS = 30;
    const long MAX_SCREENSAVER_CYCLE_SECONDS = 86400;

    // Reads one optional color param. The wire format is the RGB565 integer slotToJson() reports, so
    // whatever a GET returns can be sent straight back - which is also what this widget does to itself
    // when it restores the saved layout at boot or a countdown's previous control. A color name is
    // accepted too, as a convenience for hand-written requests. An omitted (or null, or "") param
    // leaves `out` at whatever default the caller initialized it to; anything else is an error rather
    // than a silent fallback, so a typo can't quietly turn a color black.
    bool parseColorParam(JsonVariantConst value, const char *name, uint32_t &out, String &errorMessage) {
        if (value.isNull()) {
            return true;
        }
        if (value.is<uint16_t>()) {
            out = value.as<uint16_t>();
            return true;
        }
        if (value.is<const char *>()) {
            // Older clients sent "" for a color they had nothing to say about - same as omitting it.
            if (value.as<String>().length() == 0) {
                return true;
            }
            uint16_t named;
            if (Utils::tryStringToColor(value.as<String>(), named)) {
                out = named;
                return true;
            }
            errorMessage = "unknown color name '" + value.as<String>() + "' for params." + name;
            return false;
        }
        errorMessage = String("params.") + name + " must be an RGB565 integer (0-65535) or a color name";
        return false;
    }

    // The location a weather slot shows: its own params.location, or WEATHER_LOCATION from config.h.
    // The config value goes into the request as written, as it always has; one given through the API
    // is percent-encoded, since it can hold anything.
    String weatherLocationOf(const OrbItSlot &slot) {
        if (slot.weatherLocation.length() == 0) {
            return WEATHER_LOCATION;
        }
        String encoded;
        for (size_t i = 0; i < slot.weatherLocation.length(); i++) {
            char c = slot.weatherLocation[i];
            if (isalnum((unsigned char) c) || c == '-' || c == '_' || c == '.' || c == '~' || c == ',') {
                encoded += c;
            } else {
                char hex[4];
                snprintf(hex, sizeof(hex), "%%%02X", (unsigned char) c);
                encoded += hex;
            }
        }
        return encoded;
    }

    // Every control, in the order GET /capabilities lists them.
    const OrbItSource ALL_SOURCES[] = {OrbItSource::BLANK, OrbItSource::TIME, OrbItSource::ANALOG_CLOCK, OrbItSource::GAUGE, OrbItSource::SYS_MONITOR, OrbItSource::SCREENSAVER, OrbItSource::COUNTDOWN, OrbItSource::WEATHER, OrbItSource::TICKER, OrbItSource::CUSTOM};

    // Whether this firmware was built with a control - see the ORBIT_CONTROL_* switches in config.h. A
    // control left out can't be assigned to a screen: parseSlotConfig() refuses it, so no slot ever
    // holds one. The #if blocks around its drawing and fetching further down only exist so that code
    // isn't linked into the firmware.
    bool controlBuilt(OrbItSource source) {
        switch (source) {
        case OrbItSource::TIME:
            return ORBIT_CONTROL_TIME;
        case OrbItSource::ANALOG_CLOCK:
            return ORBIT_CONTROL_ANALOG_CLOCK;
        case OrbItSource::GAUGE:
            return ORBIT_CONTROL_GAUGE;
        case OrbItSource::SYS_MONITOR:
            return ORBIT_CONTROL_SYS_MONITOR;
        case OrbItSource::SCREENSAVER:
            return ORBIT_CONTROL_SCREENSAVER;
        case OrbItSource::COUNTDOWN:
            return ORBIT_CONTROL_COUNTDOWN;
        case OrbItSource::WEATHER:
            return ORBIT_CONTROL_WEATHER;
        case OrbItSource::TICKER:
            return ORBIT_CONTROL_TICKER;
        case OrbItSource::CUSTOM:
            return ORBIT_CONTROL_CUSTOM;
        case OrbItSource::BLANK:
        default:
            return true;
        }
    }

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
        case OrbItSource::SCREENSAVER:
            return "screensaver";
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

    // The control a name refers to, whether or not it's built. "asteroids" was a control of its own
    // before it became one of the screensaver's effects.
    bool sourceFromString(const String &name, OrbItSource &out) {
        if (name == "asteroids") {
            out = OrbItSource::SCREENSAVER;
            return true;
        }
        for (OrbItSource source : ALL_SOURCES) {
            if (sourceToString(source) == name) {
                out = source;
                return true;
            }
        }
        return false;
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

OrbItWidget::OrbItWidget(ScreenManager &manager) : Widget(manager), m_timeControl(manager), m_analogClockControl(manager), m_gaugeControl(manager), m_sysMonitorControl(manager), m_screensaverControl(manager), m_countdownControl(manager), m_weatherControl(manager), m_tickerControl(manager), m_server(WebService::getInstance()->server()) {
    // Compile-time starting layout - orbit-api can reassign any of this at runtime. Mixes pieces of
    // Clock/Weather/Stock across screens simultaneously, which is the whole point of this widget.
    m_slots[0].source = OrbItSource::TIME;
    m_slots[0].showDate = true;
    m_slots[0].showDay = true;

    m_slots[1].source = OrbItSource::WEATHER;
    m_slots[1].weatherElement = WeatherElement::TEMPERATURE;

    // Default screen 2 to the decorative Asteroids-style screensaver (starfield + a bouncing
    // planet labeled "Orb-It", ScreensaverConfig's own default effect) instead of a plain text
    // label - orbit-api can still reassign it to anything else, including "custom", at runtime.
    m_slots[2].source = OrbItSource::SCREENSAVER;

    // Default screen 3 to a classic analog clock face.
    m_slots[3].source = OrbItSource::ANALOG_CLOCK;

    m_slots[4].source = OrbItSource::TICKER;
    m_slots[4].tickerSymbol = "BTC/USD";

    // A default this firmware was built without starts out blank instead.
    for (int i = 0; i < NUM_SCREENS; i++) {
        if (!controlBuilt(m_slots[i].source)) {
            m_slots[i].source = OrbItSource::BLANK;
        }
    }

    // Overrides any of the above with whatever was last saved via orbit-api, if anything - NVS
    // access works fully offline, so this is safe to do here before WiFi is even up.
    loadPersistedLayout();

    // Routes can be registered before the core web server is started (once WiFi is up)
    setupApiRoutes();
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
        } else if (m_slots[i].source == OrbItSource::SCREENSAVER) {
            // Throttles the animation to ~20fps regardless of how fast loop() itself spins - a new
            // stamp every 50ms is what actually drives drawScreensaverSlot()'s redraw below.
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

#if ORBIT_CONTROL_WEATHER
    if (anySlotUses(OrbItSource::WEATHER)) {
        updateWeather(force);
    }
#endif
#if ORBIT_CONTROL_TICKER
    if (anySlotUses(OrbItSource::TICKER)) {
        updateTicker(force);
    }
#endif
#if ORBIT_CONTROL_COUNTDOWN
    if (anySlotUses(OrbItSource::COUNTDOWN)) {
        updateCountdown(force);
    }
#endif
}

// Every weather slot refreshes on the one shared timer, with one request per distinct location -
// screens showing different pieces of the same place share a fetch.
void OrbItWidget::updateWeather(bool force) {
    // Between refreshes, only a slot that was just given a location nobody had readings for (see
    // applySlotConfig()) gets fetched.
    bool refreshDue = force || m_weatherDelayPrev == 0 || (millis() - m_weatherDelayPrev) >= m_weatherDelay;
    for (int i = 0; i < NUM_SCREENS; i++) {
        if (m_slots[i].source != OrbItSource::WEATHER || !(refreshDue || m_slots[i].weatherFetchPending)) {
            continue;
        }
        // Cleared whether or not the fetch works - a location that can't be resolved is retried at
        // the next refresh, not on every pass.
        m_slots[i].weatherFetchPending = false;
        String location = weatherLocationOf(m_slots[i]);
        bool alreadyFetched = false;
        for (int j = 0; j < i && !alreadyFetched; j++) {
            alreadyFetched = m_slots[j].source == OrbItSource::WEATHER && weatherLocationOf(m_slots[j]) == location;
        }
        if (!alreadyFetched) {
            fetchWeatherData(location);
        }
    }
    if (refreshDue) {
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
        // the default rather than keeping whatever was configured before. parseSlotConfig() has
        // already rejected an invalid color, so the error message here is never used.
        slot.countdownConfig.color = TFT_CYAN;
        String ignoredError;
        parseColorParam(params["color"], "color", slot.countdownConfig.color, ignoredError);

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
// stay self-contained from the original widgets. Fetches one location (as weatherLocationOf()
// gives it) and hands the result to every weather slot showing that location.
bool OrbItWidget::fetchWeatherData(const String &weatherLocation) {
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
    // What an unknown location gets - the body is a plain-text message, not weather.
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("OrbIt: weather request for '%s' was refused with HTTP %d\n", weatherLocation.c_str(), httpCode);
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

    for (int i = 0; i < NUM_SCREENS; i++) {
        if (m_slots[i].source != OrbItSource::WEATHER || weatherLocationOf(m_slots[i]) != weatherLocation) {
            continue;
        }
        WeatherDataModel &model = m_weatherModels[i];
        model.setCityName(doc["resolvedAddress"].as<String>());
        model.setCurrentTemperature(doc["currentConditions"]["temp"].as<float>());
        model.setCurrentText(doc["days"][0]["description"].as<String>());
        model.setCurrentIcon(doc["currentConditions"]["icon"].as<String>());
        model.setTodayHigh(doc["days"][0]["tempmax"].as<float>());
        model.setTodayLow(doc["days"][0]["tempmin"].as<float>());
    }
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
}

// Each control's case is only compiled in if the control is built (see controlBuilt()), so a control
// left out takes its drawing code with it.
void OrbItWidget::drawSlot(int displayIndex, OrbItSlot &slot, bool force) {
    switch (slot.source) {
#if ORBIT_CONTROL_TIME
    case OrbItSource::TIME:
        drawTimeSlot(displayIndex, slot, force);
        break;
#endif
#if ORBIT_CONTROL_ANALOG_CLOCK
    case OrbItSource::ANALOG_CLOCK:
        drawAnalogClockSlot(displayIndex, slot, force);
        break;
#endif
#if ORBIT_CONTROL_GAUGE
    case OrbItSource::GAUGE:
        drawGaugeSlot(displayIndex, slot, force);
        break;
#endif
#if ORBIT_CONTROL_SYS_MONITOR
    case OrbItSource::SYS_MONITOR:
        drawSysMonitorSlot(displayIndex, slot, force);
        break;
#endif
#if ORBIT_CONTROL_SCREENSAVER
    case OrbItSource::SCREENSAVER:
        drawScreensaverSlot(displayIndex, slot, force);
        break;
#endif
#if ORBIT_CONTROL_COUNTDOWN
    case OrbItSource::COUNTDOWN:
        drawCountdownSlot(displayIndex, slot, force);
        break;
#endif
#if ORBIT_CONTROL_WEATHER
    case OrbItSource::WEATHER:
        drawWeatherSlot(displayIndex, slot, force);
        break;
#endif
#if ORBIT_CONTROL_TICKER
    case OrbItSource::TICKER:
        drawTickerSlot(displayIndex, slot, force);
        break;
#endif
#if ORBIT_CONTROL_CUSTOM
    case OrbItSource::CUSTOM:
        drawCustomSlot(displayIndex, slot, force);
        break;
#endif
    case OrbItSource::BLANK:
    default:
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

void OrbItWidget::drawScreensaverSlot(int displayIndex, OrbItSlot &slot, bool force) {
    if (slot.pendingValue == slot.lastRenderedValue && !force) {
        return;
    }
    m_manager.setFont(DEFAULT_FONT);
    bool fullRedraw = force || !slot.everDrawn;
    m_screensaverControl.draw(displayIndex, slot.screensaverConfig, m_screensaverStates[displayIndex], fullRedraw);
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
    WeatherDataModel &model = m_weatherModels[displayIndex];
    if (!model.isChanged() && !force && slot.everDrawn) {
        return;
    }
    m_manager.setFont(DEFAULT_FONT);
    switch (slot.weatherElement) {
    case WeatherElement::ICON:
        m_weatherControl.drawIcon(displayIndex, m_weatherScreenMode, model.getCurrentIcon(), 0, 0, 1);
        break;
    case WeatherElement::TEMPERATURE:
        m_weatherControl.drawTemperature(displayIndex, m_weatherScreenMode, model, slot.weatherStyle);
        break;
    case WeatherElement::CONDITION:
        m_weatherControl.drawCondition(displayIndex, m_weatherScreenMode, model, slot.weatherStyle);
        break;
    }
    model.setChangedStatus(false);
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
    // everDrawn survives a write only for an overlay frame (see applySlotConfig()) - every other
    // write, and any forced redraw, starts from a screen filled with the drawing's background.
    bool clear = force || !slot.everDrawn;
    if (!model.isChanged() && !clear) {
        return;
    }
    m_manager.selectScreen(displayIndex);
    model.setFullDrawStatus(clear);
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

void OrbItWidget::setupApiRoutes() {
    m_server.on(API_BASE, HTTP_GET, [this]() { handleGetScreens(); });
    m_server.on(API_BASE, HTTP_POST, [this]() { handlePostScreens(); });

    for (int i = 0; i < NUM_SCREENS; i++) {
        String uri = String(API_BASE) + "/" + String(i);
        m_server.on(uri, HTTP_GET, [this, i]() { handleGetScreen(i); });
        m_server.on(uri, HTTP_POST, [this, i]() { handlePostScreen(i); });
#if ORBIT_CONTROL_TICKER
        m_server.on(uri + "/refresh", HTTP_POST, [this, i]() { handleRefreshScreen(i); });
#endif
    }

#if ORBIT_CONTROL_WEATHER
    m_server.on(WEATHER_PREVIEW_URI, HTTP_POST, [this]() { handleWeatherPreview(); });
#endif
    m_server.on(CAPABILITIES_URI, HTTP_GET, [this]() { handleGetCapabilities(); });

    // Only routes for screens 0..NUM_SCREENS-1 are registered above, so an out-of-range index (or
    // any other unmatched path) falls through to the core server's 404, which uses the same
    // {"error": "..."} JSON shape as every other orbit-api error response.

    WebService *web = WebService::getInstance();
    web->addServiceTxt("orbit", API_BASE);
    // OrbIt's own page, linked from the widget list on the home page
    web->addPage(this, "/orbit/", [this]() {
        String html;
        html += F("<h2>Screens</h2><table>");
        for (int i = 0; i < NUM_SCREENS; i++) {
            html += "<tr><th>" + String(i) + "</th><td><a href='" + String(API_BASE) + "/" + String(i) + "'>" + sourceToString(m_slots[i].source) + "</a></td></tr>";
        }
        html += F("</table><h2>Built-in controls</h2><p>");
        bool first = true;
        for (OrbItSource source : ALL_SOURCES) {
            if (controlBuilt(source)) {
                html += (first ? "" : ", ") + sourceToString(source);
                first = false;
            }
        }
        html += F("</p><h2>API</h2><p><a href='");
        html += API_BASE;
        html += F("'><code>GET ");
        html += API_BASE;
        html += F("</code></a> - all screen configs as JSON</p><p><a href='");
        html += CAPABILITIES_URI;
        html += F("'><code>GET ");
        html += CAPABILITIES_URI;
        html += F("</code></a> - what this firmware was built with</p>");
        return html;
    });
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
        // Colors are reported as RGB565 integers, the same form parseColorParam() takes back.
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
    case OrbItSource::SCREENSAVER:
        if (slot.screensaverConfig.cycle) {
            params["effect"] = "cycle";
            params["cycleSeconds"] = slot.screensaverConfig.cycleMs / 1000;
        } else {
            params["effect"] = ScreensaverControl::effectName(slot.screensaverConfig.effect);
        }
        // Left out when the effect is using its own default color, so that's what a round trip
        // through parseSlotConfig() gives back.
        if (slot.screensaverConfig.colorSet) {
            params["color"] = slot.screensaverConfig.color;
        }
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
        // Left out while the screen follows WEATHER_LOCATION, same idea as the colors below.
        if (slot.weatherLocation.length() > 0) {
            params["location"] = slot.weatherLocation;
        }
        // Each color is left out while it's using its default, so that's what a round trip through
        // parseSlotConfig() gives back - same as the screensaver's color.
        for (const auto &color : {std::make_pair("color", slot.weatherStyle.color), std::make_pair("background", slot.weatherStyle.background), std::make_pair("highColor", slot.weatherStyle.highColor), std::make_pair("lowColor", slot.weatherStyle.lowColor), std::make_pair("cityColor", slot.weatherStyle.cityColor)}) {
            if (color.second != WEATHER_COLOR_DEFAULT) {
                params[color.first] = color.second;
            }
        }
        if (slot.weatherElement == WeatherElement::TEMPERATURE) {
            params["showHighLow"] = slot.weatherStyle.showHighLow;
            params["showCity"] = slot.weatherStyle.showCity;
        }
        break;
    case OrbItSource::TICKER:
        params["symbol"] = slot.tickerSymbol;
        params["pollIntervalSeconds"] = slot.tickerPollIntervalMs / 1000;
        break;
    case OrbItSource::CUSTOM: {
        // Exactly the params the drawing was written with - this is also what gets saved to NVS
        // and snapshotted for a countdown's restore-on-stop, so it has to round-trip.
        JsonDocument stored;
        if (slot.customParams.length() > 0 && !deserializeJson(stored, slot.customParams)) {
            params.set(stored.as<JsonObject>());
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
bool OrbItWidget::parseSlotConfig(JsonObject obj, OrbItSlot &outSlot, String &errorMessage, int screenIndex, int *errorStatus) {
    if (errorStatus) {
        *errorStatus = 400;
    }
    if (!obj["control"].is<const char *>()) {
        errorMessage = "missing or invalid 'control'";
        return false;
    }
    String control = obj["control"].as<String>();
    JsonObject params = obj["params"].is<JsonObject>() ? obj["params"].as<JsonObject>() : JsonObject();

    // Reported apart from an unknown control - 501 rather than 400 - so a client can tell "not in
    // this build" from a typo.
    OrbItSource requested;
    if (sourceFromString(control, requested) && !controlBuilt(requested)) {
        errorMessage = "control '" + control + "' isn't built into this firmware - see GET " + CAPABILITIES_URI;
        if (errorStatus) {
            *errorStatus = 501;
        }
        return false;
    }

    if (control == "blank") {
        outSlot.source = OrbItSource::BLANK;
        return true;
    }

    // "asteroids" was a control of its own before it became one of the screensaver's effects - still
    // accepted, both from older clients and from a layout saved by older firmware.
    if (control == "screensaver" || control == "asteroids") {
        ScreensaverConfig screensaver; // defaults
        if (control == "asteroids") {
            screensaver.effect = ScreensaverEffect::ASTEROIDS;
        } else if (!params["effect"].isNull()) {
            String effect = params["effect"].as<String>();
            if (params["effect"].is<const char *>() && effect == "cycle") {
                screensaver.cycle = true;
            } else if (!params["effect"].is<const char *>() || !ScreensaverControl::effectFromName(effect, screensaver.effect)) {
                errorMessage = "unknown screensaver effect '" + effect + "'";
                return false;
            }
        }
        if (!screensaver.cycle && !ScreensaverControl::effectBuilt(screensaver.effect)) {
            errorMessage = String("screensaver effect '") + ScreensaverControl::effectName(screensaver.effect) + "' isn't built into this firmware - see GET " + CAPABILITIES_URI;
            if (errorStatus) {
                *errorStatus = 501;
            }
            return false;
        }
        // Optional, and only meaningful for "cycle" - defaults to ScreensaverConfig's own (5
        // minutes). Clamped rather than rejected, same reasoning as ticker's pollIntervalSeconds.
        if (screensaver.cycle && params["cycleSeconds"].is<int>()) {
            long seconds = constrain(params["cycleSeconds"].as<long>(), MIN_SCREENSAVER_CYCLE_SECONDS, MAX_SCREENSAVER_CYCLE_SECONDS);
            screensaver.cycleMs = (unsigned long) seconds * 1000UL;
        }
        // Optional - omitted means the effect's own default color. parseColorParam() only ever
        // writes a 16-bit value, so this one can't be mistaken for a color it was given.
        const uint32_t NO_COLOR = 0xFFFFFFFF;
        uint32_t color = NO_COLOR;
        if (!parseColorParam(params["color"], "color", color, errorMessage)) {
            return false;
        }
        if (color != NO_COLOR) {
            screensaver.colorSet = true;
            screensaver.color = color;
        }
        outSlot.screensaverConfig = screensaver;
        outSlot.source = OrbItSource::SCREENSAVER;
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
            // Validated here, applied in applyCountdownAction() - this function only reports
            // whether the write is acceptable, it doesn't touch the slot's countdown config.
            uint32_t ignoredColor;
            if (!parseColorParam(params["color"], "color", ignoredColor, errorMessage)) {
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
        // AnalogClockControl.h) if omitted. See parseColorParam() for what a color can be.
        AnalogClockColors colors; // defaults
        if (!parseColorParam(params["background"], "background", colors.background, errorMessage) ||
            !parseColorParam(params["tickColor"], "tickColor", colors.tick, errorMessage) ||
            !parseColorParam(params["hourColor"], "hourColor", colors.hourHand, errorMessage) ||
            !parseColorParam(params["minuteColor"], "minuteColor", colors.minuteHand, errorMessage) ||
            !parseColorParam(params["secondColor"], "secondColor", colors.secondHand, errorMessage)) {
            return false;
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
        if (!parseColorParam(params["color"], "color", gauge.color, errorMessage) ||
            !parseColorParam(params["trackColor"], "trackColor", gauge.trackColor, errorMessage)) {
            return false;
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
        // All optional. A color left out stays "not set", which WeatherControl turns into a default
        // that follows WEATHER_SCREEN_MODE. Ones the chosen element doesn't draw are accepted and
        // kept, so switching element doesn't mean re-sending the colors.
        WeatherStyle style; // defaults
        if (!parseColorParam(params["color"], "color", style.color, errorMessage) ||
            !parseColorParam(params["background"], "background", style.background, errorMessage) ||
            !parseColorParam(params["highColor"], "highColor", style.highColor, errorMessage) ||
            !parseColorParam(params["lowColor"], "lowColor", style.lowColor, errorMessage) ||
            !parseColorParam(params["cityColor"], "cityColor", style.cityColor, errorMessage)) {
            return false;
        }
        if (params["showHighLow"].is<bool>()) {
            style.showHighLow = params["showHighLow"].as<bool>();
        }
        // Optional - omitted (or "") follows WEATHER_LOCATION from config.h.
        String location;
        if (!params["location"].isNull()) {
            if (!params["location"].is<const char *>()) {
                errorMessage = "params.location must be a string";
                return false;
            }
            location = params["location"].as<String>();
            location.trim();
            if (location.length() > MAX_WEATHER_LOCATION_LENGTH) {
                errorMessage = "params.location is too long (max " + String(MAX_WEATHER_LOCATION_LENGTH) + " characters)";
                return false;
            }
        }
        // A temperature for somewhere other than the configured location says where, unless told
        // not to - with several orbs tracking different places, a bare number isn't enough.
        style.showCity = params["showCity"].is<bool>() ? params["showCity"].as<bool>() : location.length() > 0;
        outSlot.weatherLocation = location;
        outSlot.weatherStyle = style;
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
        outSlot.customOverlay = params["overlay"].is<bool>() && params["overlay"].as<bool>();
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
bool OrbItWidget::applySlotConfig(int index, const OrbItSlot &newConfig, JsonObject rawConfig, bool immediateFetch) {
    OrbItSlot &slot = m_slots[index];
    bool wasCustom = (slot.source == OrbItSource::CUSTOM);
    bool wasGauge = (slot.source == OrbItSource::GAUGE);
    bool wasSysMonitor = (slot.source == OrbItSource::SYS_MONITOR);
    // Style-only writes to a weather screen (the common case) keep the readings it already has.
    bool sameWeather = slot.source == OrbItSource::WEATHER && newConfig.source == OrbItSource::WEATHER && slot.weatherLocation == newConfig.weatherLocation;
    // An overlay only has something to go on top of if this screen is already showing a custom
    // drawing - on any other screen it's just an ordinary (clearing) drawing.
    bool overlayFrame = newConfig.source == OrbItSource::CUSTOM && newConfig.customOverlay && wasCustom && slot.everDrawn;

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
    // Whatever this screen had saved that isn't built in has now been replaced. Restoring at boot
    // (immediateFetch=false) never gets here for such a screen - see loadPersistedLayout().
    m_unbuiltSaved[index] = "";
    slot.showDate = newConfig.showDate;
    slot.showDay = newConfig.showDay;
    slot.format24Hour = newConfig.format24Hour;
    slot.weatherElement = newConfig.weatherElement;
    slot.weatherStyle = newConfig.weatherStyle;
    slot.weatherLocation = newConfig.weatherLocation;
    slot.tickerSymbol = newConfig.tickerSymbol;
    slot.tickerPollIntervalMs = newConfig.tickerPollIntervalMs;
    slot.analogColors = newConfig.analogColors;
    slot.gaugeConfig = newConfig.gaugeConfig;
    slot.sysMonitorConfig = newConfig.sysMonitorConfig;
    slot.screensaverConfig = newConfig.screensaverConfig;
    // A restored-at-boot slot hasn't been "written this session" in any meaningful sense - millis()
    // resets every reboot, so a persisted value would be misleading, not just stale.
    slot.updatedAt = immediateFetch ? millis() : 0;

    // Force the next draw() pass to repaint this screen from scratch, regardless of the per-source
    // isChanged()/lastValue guards.
    slot.pendingValue = "";
    slot.lastRenderedValue = "";
    slot.everDrawn = overlayFrame; // ... except an overlay frame, which draws over what's there

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

    if (slot.source == OrbItSource::WEATHER && !sameWeather) {
        // Another screen already showing this location has its readings - no need to ask again.
        // Otherwise ask for a fetch on the next update() rather than leave the screen empty until
        // the shared timer comes round. Not fetched right here: this runs inside the web server's
        // request handler, and a TLS request on top of the bulk handler's stack overflowed the
        // loop task's stack and reset the device.
        m_weatherModels[index] = WeatherDataModel();
        String location = weatherLocationOf(slot);
        bool shared = false;
        for (int i = 0; i < NUM_SCREENS && !shared; i++) {
            if (i != index && m_slots[i].source == OrbItSource::WEATHER && weatherLocationOf(m_slots[i]) == location) {
                m_weatherModels[index] = m_weatherModels[i];
                shared = true;
            }
        }
        slot.weatherFetchPending = !shared;
    }

#if ORBIT_CONTROL_TICKER
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
#endif

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
        // An overlay frame is transient (think: one frame of an animation) - the drawing GET
        // reports, and the one that comes back after a reboot, stays the one it was drawn over.
        if (!overlayFrame) {
            String stored;
            if (!params.isNull()) {
                serializeJson(params, stored);
            }
            // Restoring from NVS (immediateFetch=false) is by definition already saved.
            if (immediateFetch && (!wasCustom || stored != slot.customParams)) {
                slot.customUnsaved = true;
            }
            slot.customParams = stored;
        }
#if ORBIT_CONTROL_CUSTOM
        // A write replaces the whole config, but parseData() only touches the label if one is given.
        m_customModels[index].setLabel("");
        m_customModels[index].parseData(params, TFT_WHITE, TFT_BLACK);
#endif
    } else if (wasCustom) {
        slot.customParams = "";
        slot.customUnsaved = true; // so persistLayout() drops the drawing this screen no longer shows
    }
    return !overlayFrame;
}

// Restores a previously-saved layout from NVS, if any, overriding the compile-time defaults set
// just before this is called in the constructor. Safe to run before WiFi is up - Preferences/NVS
// is local flash storage, no network involved.
void OrbItWidget::loadPersistedLayout() {
    m_preferences.begin(NVS_NAMESPACE, true); // read-only
    String json = m_preferences.getString(NVS_LAYOUT_KEY, "");
    m_preferences.end();
    m_savedLayout = json;

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

        // Exactly as saved, before a custom drawing is added to it below.
        String savedEntry;
        serializeJson(entry, savedEntry);

        // A custom slot's params aren't in the layout - see drawingPath(). No saved drawing (it was
        // empty, couldn't be saved, or the layout predates drawings being saved) restores as a
        // blank screen.
#if ORBIT_CONTROL_CUSTOM
        if (entry["control"].is<const char *>() && String(entry["control"].as<const char *>()) == "custom") {
            JsonDocument drawing;
            if (mountDrawingStore()) {
                File file = LittleFS.open(drawingPath(index), "r");
                if (file) {
                    deserializeJson(drawing, file);
                    file.close();
                }
            }
            entry["params"].set(drawing.as<JsonObject>());
        }
#endif

        String errorMessage;
        OrbItSlot parsed;
        int errorStatus;
        if (!parseSlotConfig(entry, parsed, errorMessage, -1, &errorStatus)) {
            if (errorStatus == 501) {
                // Saved by firmware built with more controls than this one. The screen shows blank,
                // but the entry is kept, so flashing a build with the control again brings it back.
                m_slots[index].source = OrbItSource::BLANK;
                m_unbuiltSaved[index] = savedEntry;
                Serial.println("OrbIt: screen " + String(index) + " shows blank - " + errorMessage);
            } else {
                Serial.println("OrbIt: skipping invalid persisted slot " + String(index) + ": " + errorMessage);
            }
            continue;
        }
        applySlotConfig(index, parsed, entry, /* immediateFetch */ false);
    }
    Serial.println("OrbIt: restored layout from NVS");
}

// Saves the complete current layout as one JSON blob - simpler than tracking which individual
// slot(s) changed. What's saved is which control each screen shows and how it's configured, not
// the live readings a client keeps feeding it: a gauge's value and a sysMonitor's numbers are left
// out (they come back as 0 after a reboot, until the next write), and the blob is only written to
// flash if it differs from what's already there. Together that means a client posting fresh values
// every few seconds, around the clock, never touches flash - each of those writes used to rewrite
// the whole layout, which would wear the NVS partition out within months. countdown slots are
// deliberately skipped entirely: a countdown is a one-off "timebox this task" tool, not a
// permanent screen assignment the way every other control is - it shouldn't outlive a reboot even
// as an idle placeholder. A skipped screen just isn't mentioned in the saved array, so on restore
// it falls back to whatever its compile-time default (or another persisted entry) says instead.
// Custom drawings are the one thing not in that blob - see drawingPath().
bool OrbItWidget::persistLayout() {
    bool saved = true;
    JsonDocument doc;
    JsonArray screens = doc["screens"].to<JsonArray>();
    for (int i = 0; i < NUM_SCREENS; i++) {
        OrbItSlot &slot = m_slots[i];
#if ORBIT_CONTROL_CUSTOM
        if (slot.customUnsaved) {
            slot.customUnsaved = false;
            String path = drawingPath(i);
            if (!mountDrawingStore()) {
                saved = false;
            } else if (slot.source == OrbItSource::CUSTOM && slot.customParams.length() > 0) {
                File file = LittleFS.open(path, "w");
                bool written = file && file.print(slot.customParams) == slot.customParams.length();
                if (file) {
                    file.close();
                }
                if (!written) {
                    Serial.println("OrbIt: unable to save screen " + String(i) + "'s drawing (" + String(slot.customParams.length()) + " bytes)");
                    LittleFS.remove(path); // don't leave half a drawing to be restored
                    saved = false;
                }
            } else if (LittleFS.exists(path)) {
                LittleFS.remove(path);
            }
        }
#endif

        if (slot.source == OrbItSource::COUNTDOWN) {
            continue;
        }
        if (m_unbuiltSaved[i].length() > 0) {
            JsonDocument kept;
            if (!deserializeJson(kept, m_unbuiltSaved[i])) {
                screens.add(kept.as<JsonObject>());
                continue;
            }
        }
        JsonObject entry = screens.add<JsonObject>();
        slotToJson(i, slot, entry);
        entry.remove("updatedAt"); // uptime of the last write - changes every time, means nothing after a reboot
        JsonObject params = entry["params"];
        switch (slot.source) {
        case OrbItSource::CUSTOM:
            entry.remove("params");
            break;
        case OrbItSource::GAUGE:
            params.remove("value");
            break;
        case OrbItSource::SYS_MONITOR:
            for (const char *reading : {"cpu", "cpuTemp", "gpu", "gpuTemp", "ram", "ramTotal", "ssdTemp"}) {
                params.remove(reading);
            }
            break;
        default:
            break;
        }
    }
    String json;
    serializeJson(doc, json);

    if (json != m_savedLayout) {
        m_preferences.begin(NVS_NAMESPACE, false); // read-write
        if (m_preferences.putString(NVS_LAYOUT_KEY, json) == 0) {
            Serial.println("OrbIt: unable to save layout to NVS (" + String(json.length()) + " bytes)");
            saved = false;
        } else {
            Serial.println("OrbIt: saved layout to NVS (" + String(json.length()) + " bytes)");
            m_savedLayout = json;
        }
        m_preferences.end();
    }
    return saved;
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
    int errorStatus;
    if (!parseSlotConfig(doc.as<JsonObject>(), parsed, errorMessage, index, &errorStatus)) {
        sendError(errorStatus, errorMessage);
        return;
    }

    bool needsSave = applySlotConfig(index, parsed, doc.as<JsonObject>());
    consumeCountdownRestore(index);
    bool saved = !needsSave || persistLayout();

    JsonDocument response;
    slotToJson(index, m_slots[index], response.to<JsonObject>());
    if (!saved) {
        response["warning"] = NOT_SAVED_WARNING;
    }
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

// Stands in made-up readings for the fetched ones, to see how a weather screen copes with values
// the real weather won't produce on demand (a three-digit temperature, a negative one). Nothing is
// saved: the readings are live data, and the next fetch replaces them - which this pushes a full
// refresh interval away, so the preview stays up for about ten minutes.
void OrbItWidget::handleWeatherPreview() {
    JsonDocument doc;
    if (!m_server.hasArg("plain") || deserializeJson(doc, m_server.arg("plain")) || !doc.is<JsonObject>()) {
        sendError(400, "body must be a JSON object with any of 'temperature', 'high', 'low'");
        return;
    }
    for (const char *reading : {"temperature", "high", "low"}) {
        if (!doc[reading].isNull() && !doc[reading].is<float>()) {
            sendError(400, String("'") + reading + "' must be a number");
            return;
        }
    }

    // Every weather screen gets them, whatever location it shows.
    for (int i = 0; i < NUM_SCREENS; i++) {
        if (m_slots[i].source != OrbItSource::WEATHER) {
            continue;
        }
        if (doc["temperature"].is<float>()) {
            m_weatherModels[i].setCurrentTemperature(doc["temperature"].as<float>());
        }
        if (doc["high"].is<float>()) {
            m_weatherModels[i].setTodayHigh(doc["high"].as<float>());
        }
        if (doc["low"].is<float>()) {
            m_weatherModels[i].setTodayLow(doc["low"].as<float>());
        }
    }
    m_weatherDelayPrev = millis();

    JsonDocument response;
    for (const char *reading : {"temperature", "high", "low"}) {
        if (doc[reading].is<float>()) {
            response[reading] = doc[reading].as<float>();
        }
    }
    sendJson(200, response);
}

// What this firmware was built with, so a client can offer only what will work - see the
// ORBIT_CONTROL_* and ORBIT_EFFECT_* switches in config.h.
void OrbItWidget::handleGetCapabilities() {
    JsonDocument doc;
    JsonArray controls = doc["controls"].to<JsonArray>();
    for (OrbItSource source : ALL_SOURCES) {
        if (controlBuilt(source)) {
            controls.add(sourceToString(source));
        }
    }
    if (controlBuilt(OrbItSource::SCREENSAVER)) {
        JsonArray effects = doc["screensaverEffects"].to<JsonArray>();
        for (ScreensaverEffect effect : ScreensaverControl::ALL_EFFECTS) {
            if (ScreensaverControl::effectBuilt(effect)) {
                effects.add(ScreensaverControl::effectName(effect));
            }
        }
    }
    sendJson(200, doc);
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
        int errorStatus;
        if (!parseSlotConfig(entry, parsed, errorMessage, screenIndex, &errorStatus)) {
            sendError(errorStatus, "screen " + String(screenIndex) + ": " + errorMessage);
            return;
        }

        indices[count] = screenIndex;
        parsedSlots[count] = parsed;
        rawEntries[count] = entry;
        count++;
    }

    bool needsSave = false;
    for (int i = 0; i < count; i++) {
        needsSave |= applySlotConfig(indices[i], parsedSlots[i], rawEntries[i]);
        consumeCountdownRestore(indices[i]);
    }
    bool saved = !needsSave || persistLayout();

    JsonDocument response;
    if (!saved) {
        response["warning"] = NOT_SAVED_WARNING;
    }
    JsonArray screens = response["screens"].to<JsonArray>();
    for (int i = 0; i < NUM_SCREENS; i++) {
        slotToJson(i, m_slots[i], screens.add<JsonObject>());
    }
    sendJson(200, response);
}
