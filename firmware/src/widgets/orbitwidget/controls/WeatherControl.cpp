#include "WeatherControl.h"
#include "icons.h"

#include "config_helper.h"

namespace {
uint32_t colorOr(uint32_t color, uint32_t fallback) {
    return color == WEATHER_COLOR_DEFAULT ? fallback : color;
}

void resolveColors(ScreenMode screenMode, const WeatherStyle &style, uint32_t &foreground, uint32_t &background) {
    foreground = colorOr(style.color, screenMode == Light ? TFT_BLACK : TFT_WHITE);
    background = colorOr(style.background, screenMode == Light ? TFT_WHITE : TFT_BLACK);
}

// The resolved address comes back as e.g. "Victoria, BC, Canada" - just the first part.
String shortCityName(WeatherDataModel &model) {
    String cityName = model.getCityName();
    cityName.remove(cityName.indexOf(",", 0));
    return cityName;
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
void WeatherControl::drawTemperature(int displayIndex, ScreenMode screenMode, WeatherDataModel &model, const WeatherStyle &style) {
    uint32_t foreground, background;
    resolveColors(screenMode, style, foreground, background);

    m_manager.selectScreen(displayIndex);
    m_manager.fillScreen(background);

    // Has to be set here, not assumed: whichever control drew text last (a gauge, a sysMonitor, a
    // ticker...) left its own color behind, and the temperature used to come out in that.
    m_manager.setFontColor(foreground, background);
    if (style.showCity) {
        // Up in the narrow top of the round screen, clear of the temperature in either layout.
        m_manager.setFontColor(colorOr(style.cityColor, foreground), background);
        m_manager.drawFittedString(shortCityName(model), centre, 36, 150, 24, Align::MiddleCenter);
        m_manager.setFontColor(foreground, background);
    }

    String currentTemp = model.getCurrentTemperature(0);
    if (!style.showHighLow) {
        // On its own the temperature gets the whole screen: as large as fits a box that stays
        // inside the round bezel, however many digits it has.
        // A little smaller and lower under a city name, to leave a gap below it.
        if (style.showCity) {
            m_manager.drawFittedString(currentTemp, centre, centre + 6, 200, 108, Align::MiddleCenter);
        } else {
            m_manager.drawFittedString(currentTemp, centre, centre, 200, 120, Align::MiddleCenter);
        }
        return;
    }
    // ° is a 2-byte UTF-8 sequence, so a 3-digit temperature (e.g. "104°") is 5 bytes
    // vs. 4 for a 2-digit one (e.g. "72°") - scale down so it still fits on screen.
    int tempFontSize = currentTemp.length() > 4 ? 70 : 88;
    int tempY = 90;
    if (style.showCity) {
        // Squeezed between the city name and the high/low - a size down, and nudged lower, so it
        // doesn't touch the name.
        tempFontSize = currentTemp.length() > 4 ? 62 : 74;
        tempY = 96;
    }
    m_manager.drawCentreString(currentTemp, centre, tempY, tempFontSize);

    // NOTE: In Light mode, we draw a decorative chunk in the text color and display the high and low
    //       on it in the background color. It does not make sense to have a glaring chunk in dark
    //       mode, so we don't draw it at all, and display the high and low in the text color too.
    uint32_t bandForeground = foreground;
    uint32_t bandBackground = background;
    if (screenMode == Light) {
        m_manager.fillRect(0, 150, 240, 90, foreground);
        m_manager.fillRect(centre - 1, 150, 2, 90, background);
        bandForeground = background;
        bandBackground = foreground;
    }

    int fontSize = 22;
    m_manager.setFontColor(colorOr(style.highColor, bandForeground), bandBackground);
    m_manager.drawCentreString("High", 80, 170, fontSize);
    m_manager.drawCentreString(model.getTodayHigh(0), 80, 210, fontSize);
    m_manager.setFontColor(colorOr(style.lowColor, bandForeground), bandBackground);
    m_manager.drawCentreString("Low", 160, 170, fontSize);
    m_manager.drawCentreString(model.getTodayLow(0), 160, 210, fontSize);
    m_manager.setFontColor(foreground, background);
}

// Display the user's current city and the text description of the weather
void WeatherControl::drawCondition(int displayIndex, ScreenMode screenMode, WeatherDataModel &model, const WeatherStyle &style) {
    uint32_t foreground, background;
    resolveColors(screenMode, style, foreground, background);

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
    String cityName = shortCityName(model);

    m_manager.setFontColor(colorOr(style.cityColor, foreground), background);
    m_manager.drawFittedString(cityName, centre, 80, 210, 50, Align::MiddleCenter);

    m_manager.setFontColor(foreground, background);
    auto y = 125;
    for (auto i = 0; i < 4; i++) {
        m_manager.drawCentreString(messageArr[i], centre, y, 15);
        y += 25;
    }
}
