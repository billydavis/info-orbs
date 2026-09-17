#ifndef WEB_DATA_WIDGET_H
#define WEB_DATA_WIDGET_H

#include "Widget.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>

#include "Utils.h"
#include "WebDataModel.h"

class WebDataWidget : public Widget {
public:
    WebDataWidget(ScreenManager &manager, String url);
    ~WebDataWidget() override;
    void setup() override;
    void update(bool force = false) override;
    void draw(bool force = false) override;
    void buttonPressed(uint8_t buttonId, ButtonState state) override;
    String getName() override;

private:
    void drawConnectionError(int screen);
    void drawConnectionIndicator(int screen, int32_t backgroundColor);

    unsigned long m_lastUpdate = 0;
    unsigned long m_updateDelay = 1000;
    String httpRequestAddress;
    WebDataModel m_obj[5];
    int32_t m_defaultColor = TFT_WHITE;
    int32_t m_defaultBackground = TFT_BLACK;
    // Set on the most recent update() attempt - lets draw() tell a screen that's never had
    // real data apart from one that's simply between polls.
    bool m_lastFetchFailed = false;
    // True for exactly one draw() pass after m_lastFetchFailed flips, so the small indicator
    // gets toggled on already-populated screens even though their content itself didn't change.
    bool m_connectionStatusChanged = false;
};
#endif // WEB_DATA_WIDGET_H
