
#include "WebDataWidget.h"

WebDataWidget::WebDataWidget(ScreenManager &manager, String url) : Widget(manager) {
    httpRequestAddress = url;

    m_lastUpdate = 0;
    for (int i = 0; i < 5; i++) {
        m_obj[i] = WebDataModel();
    }
}

WebDataWidget::~WebDataWidget() {
}

void WebDataWidget::setup() {
}

void WebDataWidget::buttonPressed(uint8_t buttonId, ButtonState state) {
}

void WebDataWidget::draw(bool force) {
    // A connection status flip (up->down or down->up) needs the small indicator toggled on
    // every already-populated screen even though its content itself didn't change - handled by
    // the refreshIndicator branch below, separately from the normal isChanged()/force repaint.
    bool refreshIndicator = m_connectionStatusChanged;
    for (int i = 0; i < 5; i++) {
        WebDataModel *data = &m_obj[i];
        if (force) {
            data->setInitializedStatus(false);
        }
        bool hasEverHadData = data->getLabel().length() > 0 || data->getData().length() > 0 || data->getElementsCount() > 0;
        if (data->isChanged() || force) {
            m_manager.selectScreen(i);
            if (m_lastFetchFailed && !hasEverHadData) {
                drawConnectionError(i);
            } else {
                data->draw(m_manager);
                if (hasEverHadData) {
                    drawConnectionIndicator(i, data->getBackgroundColor());
                }
            }
            data->setChangedStatus(false);
        } else if (refreshIndicator && hasEverHadData) {
            m_manager.selectScreen(i);
            drawConnectionIndicator(i, data->getBackgroundColor());
        }
    }
    m_connectionStatusChanged = false;
}

// Screen i has no data of its own to fall back to (never parsed a successful response) and the
// last fetch failed - tell the user why it's blank rather than leaving it an unexplained black
// square. A screen that already has real data from an earlier successful poll keeps showing it
// instead (see the hasEverHadData check in draw()/drawConnectionIndicator()), since that's more
// useful than wiping it on a transient failure.
void WebDataWidget::drawConnectionError(int screen) {
    m_manager.fillScreen(TFT_BLACK);
    m_manager.setLegacyTextColor(TFT_RED, TFT_BLACK);
    m_manager.setLegacyTextSize(2);
    m_manager.setLegacyTextDatum(MC_DATUM);
    m_manager.drawLegacyString("No Connection", 120, 100, 2);
    m_manager.setLegacyTextSize(1);
    m_manager.drawLegacyString(httpRequestAddress, 120, 130, 1);
}

// Thin ring right at the bezel edge - same technique/geometry StockWidget already uses for its
// up/down indicator (StockWidget.cpp's displayStock(), radius 120/118 at center 120,120), so this
// reads as the established "status ring" look rather than a one-off. Redrawn in the screen's own
// background color to erase it once the connection recovers, without disturbing the stale content
// it's drawn on top of.
void WebDataWidget::drawConnectionIndicator(int screen, int32_t backgroundColor) {
    uint32_t color = m_lastFetchFailed ? TFT_RED : backgroundColor;
    m_manager.drawArc(120, 120, 120, 118, 0, 360, color, color);
}

void WebDataWidget::update(bool force) {
    if (force || m_lastUpdate == 0 || (millis() - m_lastUpdate) >= m_updateDelay) {
        HTTPClient http;
        http.begin(httpRequestAddress);
        int httpCode = http.GET();

        bool failed;
        if (httpCode > 0) { // Check for the returning code
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, http.getString());
            if (!error) {
                if (doc["interval"].is<int>()) {
                    m_updateDelay = doc["interval"];
                }
                JsonVariant array;
                if (doc["displays"].is<JsonArray>()) {
                    array = doc["displays"].as<JsonArray>();
                } else {
                    // Handle legacy response that doesn't have response level data
                    array = doc.as<JsonArray>();
                }
                for (int i = 0; i < array.size(); i++) {
                    m_obj[i].parseData(array[i].as<JsonObject>(), m_defaultColor, m_defaultBackground);
                }
                m_lastUpdate = millis();
                failed = false;
            } else {
                // Handle JSON deserialization error
                Serial.println("deserializeJson() failed");
                failed = true;
            }
        } else {
            // Handle HTTP request error
            Serial.printf("HTTP request failed, error: %s\n", http.errorToString(httpCode).c_str());
            failed = true;
        }
        http.end();

        if (failed != m_lastFetchFailed) {
            m_lastFetchFailed = failed;
            m_connectionStatusChanged = true;
        }
    }
}

String WebDataWidget::getName() {
    return "WebData";
}