#include "OrbItWidget.h"

#include "config_helper.h"

namespace {
const char *API_BASE = "/orbit/api/v1/screens";

String sourceToString(OrbItSource source) {
    switch (source) {
    case OrbItSource::TIME:
        return "time";
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
} // namespace

OrbItWidget::OrbItWidget(ScreenManager &manager) : Widget(manager), m_timeControl(manager), m_weatherControl(manager), m_tickerControl(manager) {
    // Compile-time starting layout - orbit-api can reassign any of this at runtime. Mixes pieces of
    // Clock/Weather/Stock across screens simultaneously, which is the whole point of this widget.
    m_slots[0].source = OrbItSource::TIME;
    m_slots[0].showDate = true;
    m_slots[0].showDay = true;

    m_slots[1].source = OrbItSource::WEATHER;
    m_slots[1].weatherElement = WeatherElement::TEMPERATURE;

    m_slots[2].source = OrbItSource::CUSTOM;
    // Go through parseData() (not WebDataModel's raw setData()) so background/color get properly
    // defaulted to TFT_BLACK/TFT_WHITE - calling setData() alone leaves them at WebDataModel's own
    // unset default (-1, which renders as white - the exact WebDataWidget footgun documented in
    // firmware/src/widgets/orbitwidget/docs/orbit-api.md).
    JsonDocument defaultLabelDoc;
    defaultLabelDoc["data"] = "OrbIt Widget";
    m_customModels[2].parseData(defaultLabelDoc.as<JsonObject>(), TFT_WHITE, TFT_BLACK);

    m_slots[3].source = OrbItSource::WEATHER;
    m_slots[3].weatherElement = WeatherElement::ICON;

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
    for (int i = 0; i < NUM_SCREENS; i++) {
        if (m_slots[i].source == OrbItSource::TIME) {
            m_slots[i].pendingValue = clockStamp;
        }
    }

    if (anySlotUses(OrbItSource::WEATHER)) {
        updateWeather(force);
    }
    if (anySlotUses(OrbItSource::TICKER)) {
        updateTicker(force);
    }
}

void OrbItWidget::updateWeather(bool force) {
    if (force || m_weatherDelayPrev == 0 || (millis() - m_weatherDelayPrev) >= m_weatherDelay) {
        fetchWeatherData(m_weatherModel);
        m_weatherDelayPrev = millis();
    }
}

void OrbItWidget::updateTicker(bool force) {
    if (force || m_tickerDelayPrev == 0 || (millis() - m_tickerDelayPrev) >= m_tickerDelay) {
        for (int i = 0; i < NUM_SCREENS; i++) {
            if (m_slots[i].source == OrbItSource::TICKER) {
                fetchTickerData(m_tickerModels[i]);
            }
        }
        m_tickerDelayPrev = millis();
    }
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
    case OrbItSource::WEATHER:
        params["element"] = weatherElementToString(slot.weatherElement);
        break;
    case OrbItSource::TICKER:
        params["symbol"] = slot.tickerSymbol;
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
bool OrbItWidget::parseSlotConfig(JsonObject obj, OrbItSlot &outSlot, String &errorMessage) {
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

    if (control == "time") {
        // Both optional, default false - just HH:MM unless asked for more.
        outSlot.showDate = params["showDate"].is<bool>() ? params["showDate"].as<bool>() : false;
        outSlot.showDay = params["showDay"].is<bool>() ? params["showDay"].as<bool>() : false;
        outSlot.format24Hour = params["format24Hour"].is<bool>() ? params["format24Hour"].as<bool>() : false;
        outSlot.source = OrbItSource::TIME;
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
    slot.source = newConfig.source;
    slot.showDate = newConfig.showDate;
    slot.showDay = newConfig.showDay;
    slot.format24Hour = newConfig.format24Hour;
    slot.weatherElement = newConfig.weatherElement;
    slot.tickerSymbol = newConfig.tickerSymbol;
    // A restored-at-boot slot hasn't been "written this session" in any meaningful sense - millis()
    // resets every reboot, so a persisted value would be misleading, not just stale.
    slot.updatedAt = immediateFetch ? millis() : 0;

    // Force the next draw() pass to repaint this screen from scratch, regardless of the per-source
    // isChanged()/lastValue guards.
    slot.pendingValue = "";
    slot.lastRenderedValue = "";
    slot.everDrawn = false;

    if (slot.source == OrbItSource::TICKER) {
        m_tickerModels[index] = StockDataModel();
        m_tickerModels[index].setSymbol(slot.tickerSymbol);
        if (immediateFetch) {
            fetchTickerData(m_tickerModels[index]); // don't make the caller wait out the normal poll delay
        } // else: WiFi isn't up yet (restoring at construction time) - the normal update() polling
          // loop will fetch once it is.
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
// meaningfully more flash wear than tracking which individual slot(s) changed.
void OrbItWidget::persistLayout() {
    JsonDocument doc;
    JsonArray screens = doc["screens"].to<JsonArray>();
    for (int i = 0; i < NUM_SCREENS; i++) {
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
    if (!parseSlotConfig(doc.as<JsonObject>(), parsed, errorMessage)) {
        sendError(400, errorMessage);
        return;
    }

    applySlotConfig(index, parsed, doc.as<JsonObject>());
    persistLayout();

    JsonDocument response;
    slotToJson(index, m_slots[index], response.to<JsonObject>());
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
        if (!parseSlotConfig(entry, parsed, errorMessage)) {
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
