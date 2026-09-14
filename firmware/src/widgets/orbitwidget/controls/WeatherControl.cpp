#include "WeatherControl.h"
#include "icons.h"

#include "config_helper.h"

namespace {
void computeColors(ScreenMode screenMode, uint16_t &foreground, uint16_t &background, uint16_t &invertedForeground, uint16_t &invertedBackground) {
    foreground = screenMode == Light ? TFT_BLACK : TFT_WHITE;
    background = screenMode == Light ? TFT_WHITE : TFT_BLACK;

    // NOTE: In Light mode, we draw decorative black chunks and display the high and low on them in white.
    //       It does not make sense to have glaring white chunks in dark mode, so we don't draw them at all,
    //       and display the high and low in white too.
    invertedForeground = screenMode == Light ? background : foreground;
    invertedBackground = screenMode == Light ? foreground : background;
}
} // namespace

WeatherControl::WeatherControl(ScreenManager &manager) : m_manager(manager) {
}

// Write an image to the screen from a hex array.
// scale of the image (1=full size, then multiples of 2 to scale down)
// getting the byte array size is very annoying as it's computed on compile, so you can't do it dynamically.
void WeatherControl::showJPG(int displayIndex, int x, int y, const byte jpgData[], int jpgDataSize, int scale) {
    m_manager.selectScreen(displayIndex);

    TJpgDec.setJpgScale(scale);
    uint16_t w = 0, h = 0;
    TJpgDec.getJpgSize(&w, &h, jpgData, jpgDataSize);
    TJpgDec.drawJpg(x, y, jpgData, jpgDataSize);
}

// Take the text output from the weather API and map it to a icon/byte array, then display it
void WeatherControl::drawIcon(int displayIndex, ScreenMode screenMode, const String &condition, int x, int y, int scale) {
    const byte *iconStart = NULL;
    const byte *iconEnd = NULL;

#if WEATHER_INCLUDE_LIGHT_ICONS
    #define WEATHER_ICON(name) (screenMode == Light ? name##W_start : name##B_start)
    #define WEATHER_ICON_END(name) (screenMode == Light ? name##W_end : name##B_end)
#else
    // Light-mode icons were excluded from the build (WEATHER_INCLUDE_LIGHT_ICONS false) - always use Dark.
    #define WEATHER_ICON(name) (name##B_start)
    #define WEATHER_ICON_END(name) (name##B_end)
#endif

    if (condition == "partly-cloudy-night") {
        iconStart = WEATHER_ICON(moonCloud);
        iconEnd = WEATHER_ICON_END(moonCloud);
    } else if (condition == "partly-cloudy-day") {
        iconStart = WEATHER_ICON(sunClouds);
        iconEnd = WEATHER_ICON_END(sunClouds);
    } else if (condition == "clear-day") {
        iconStart = WEATHER_ICON(sun);
        iconEnd = WEATHER_ICON_END(sun);
    } else if (condition == "clear-night") {
        iconStart = WEATHER_ICON(moon);
        iconEnd = WEATHER_ICON_END(moon);
    } else if (condition == "snow") {
        iconStart = WEATHER_ICON(snow);
        iconEnd = WEATHER_ICON_END(snow);
    } else if (condition == "rain") {
        iconStart = WEATHER_ICON(rain);
        iconEnd = WEATHER_ICON_END(rain);
    } else if (condition == "fog" || condition == "wind" || condition == "cloudy") {
        iconStart = WEATHER_ICON(clouds);
        iconEnd = WEATHER_ICON_END(clouds);
    } else {
        Serial.println("unknown weather icon:" + condition);
    }

    #undef WEATHER_ICON
    #undef WEATHER_ICON_END

    const int size = iconEnd - iconStart;
    if (iconStart != NULL && size > 0) {
        showJPG(displayIndex, x, y, iconStart, size, scale);
    }
}

// Displays the current temperature on a single screen.
// doesn't round deg, just removes all text after the decimal
void WeatherControl::drawTemperature(int displayIndex, ScreenMode screenMode, WeatherDataModel &model) {
    uint16_t foreground, background, invertedForeground, invertedBackground;
    computeColors(screenMode, foreground, background, invertedForeground, invertedBackground);

    m_manager.selectScreen(displayIndex);
    m_manager.fillScreen(background);
    m_manager.drawCentreString(model.getCurrentTemperature(0), centre, 90, 88);

    // No glaring white chunks in Dark mode
    if (screenMode == Light) {
        m_manager.fillRect(0, 150, 240, 90, foreground);
        m_manager.fillRect(centre - 1, 150, 2, 90, background);
    }

    int fontSize = 22;
    m_manager.setFontColor(invertedForeground);
    m_manager.setBackgroundColor(invertedBackground);
    m_manager.drawCentreString("High", 80, 170, fontSize);
    m_manager.drawCentreString("Low", 160, 170, fontSize);
    m_manager.drawCentreString(model.getTodayHigh(0), 80, 210, fontSize);
    m_manager.drawCentreString(model.getTodayLow(0), 160, 210, fontSize);
    m_manager.setFontColor(foreground);
    m_manager.setBackgroundColor(background);
}

// Display the user's current city and the text description of the weather
void WeatherControl::drawCondition(int displayIndex, ScreenMode screenMode, WeatherDataModel &model) {
    uint16_t foreground, background, invertedForeground, invertedBackground;
    computeColors(screenMode, foreground, background, invertedForeground, invertedBackground);

    m_manager.selectScreen(displayIndex);

    //=== TEXT OVERFLOW ============================
    // This takes a given string a and breaks it down in max x character long strings ensuring not to break it only at a space.
    // Given the small width of the screens this will porbablly be needed to this project again so making sure to outline it
    // clearly as this should liekly eventually be turned into a fucntion. Before use the array size should be made to be dynamic.
    // In this case its used for the weather text description

    String message = model.getCurrentText() + " ";
    String messageArr[4];
    int variableRangeS = 0;
    int variableRangeE = 18;
    for (int i = 0; i < 4; i++) {
        while (message.substring(variableRangeE - 1, variableRangeE) != " ") {
            variableRangeE--;
        }
        messageArr[i] = message.substring(variableRangeS, variableRangeE);
        variableRangeS = variableRangeE;
        variableRangeE = variableRangeS + 18;
    }
    //=== OVERFLOW END ==============================

    m_manager.fillScreen(background);
    String cityName = model.getCityName();
    cityName.remove(cityName.indexOf(",", 0));

    m_manager.setFontColor(foreground);
    m_manager.drawFittedString(cityName, centre, 80, 210, 50, Align::MiddleCenter);

    auto y = 125;
    for (auto i = 0; i < 4; i++) {
        m_manager.drawCentreString(messageArr[i], centre, y, 15);
        y += 25;
    }
}
