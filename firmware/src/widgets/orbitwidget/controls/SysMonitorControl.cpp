#include "SysMonitorControl.h"

#include "config_helper.h"
#include <math.h>

namespace {
const int CENTRE = SCREEN_SIZE / 2;

// Same quadrant text positions as the ring-based layout (r=70/120 along each 45deg diagonal from
// center) - only the ring itself is gone, replaced by a small horizontal bar under the label so
// the layout otherwise reads the same.
const int LABEL_W = 84, LABEL_H = 48;
const int BAR_W = 60, BAR_H = 6;
// Quadrant chip boxes vertically span 47-95 (top pair) and 146-194 (bottom pair) around center
// 120,120 - keeping CENTER_H within that 95-146 gap (with a little margin) is what stops the
// center readout's own erase-and-redraw from clipping into the bottom two chips.
const int CENTER_W = 150, CENTER_H = 44;

struct QuadrantSpec {
    uint32_t color;
    int chipX, chipY; // block center
};

// CPU/GPU on top, RAM/drive temp on bottom - processors above, capacity/thermal below.
const QuadrantSpec CPU_Q = {TFT_CYAN, 71, 71}; // top-left
const QuadrantSpec GPU_Q = {TFT_MAGENTA, 170, 71}; // top-right
const QuadrantSpec RAM_Q = {TFT_ORANGE, 71, 170}; // bottom-left
const QuadrantSpec TEMP_Q = {TFT_RED, 170, 170}; // bottom-right

float clampPercent(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 100.0f) {
        return 100.0f;
    }
    return value;
}

// Redraws one quadrant's value/label text and its small fill bar, and nothing else on screen -
// this is the unit of "only redraw the data values that changed" for this control. The whole block
// is erased first (one fillRect) since a shorter new bar/text wouldn't otherwise cover what the
// previous, longer one left behind.
void drawQuadrant(ScreenManager &manager, const QuadrantSpec &q, float percent, const String &valueText, const String &labelText) {
    manager.fillRect(q.chipX - LABEL_W / 2, q.chipY - LABEL_H / 2, LABEL_W, LABEL_H, TFT_BLACK);

    manager.setFontColor(q.color, TFT_BLACK);
    manager.drawCentreString(valueText, q.chipX, q.chipY - 15, 19);
    manager.setFontColor(TFT_WHITE, TFT_BLACK);
    manager.drawCentreString(labelText, q.chipX, q.chipY + 2, 10);

    int barX = q.chipX - BAR_W / 2;
    int barY = q.chipY + 12;
    manager.fillRect(barX, barY, BAR_W, BAR_H, TFT_DARKGREY);
    int fillW = (int) roundf(BAR_W * (clampPercent(percent) / 100.0f));
    if (fillW > 0) {
        manager.fillRect(barX, barY, fillW, BAR_H, q.color);
    }
}

// Resolves which reading owns the center readout: config.center pins one explicitly (by name), or
// an empty value falls back to auto-picking whichever of cpu/gpu/ram is currently highest - the
// "peak load" behavior from the first pass, now opt-in rather than the only mode. "none" turns the
// center readout off entirely (outKey="none", caller skips drawing it) - the center is optional,
// not every layout needs a fifth reading crammed into the middle. No "FOCUS"/"PEAK LOAD" eyebrow -
// the pinned vs. auto distinction isn't worth the vertical space it costs in an already-tight hub;
// the value itself (labeled by metric name) carries enough meaning.
void resolveCenter(const SysMonitorConfig &c, String &outKey, String &outValue, uint32_t &outColor) {
    if (c.center == "none") {
        outKey = "none";
    } else if (c.center == "cpu") {
        outKey = "cpu";
        outValue = "CPU " + String((int) roundf(c.cpu)) + "%";
        outColor = TFT_CYAN;
    } else if (c.center == "cpuTemp") {
        outKey = "cpuTemp";
        outValue = "CPU " + String((int) roundf(c.cpuTemp)) + "\xC2\xB0" "C";
        outColor = TFT_CYAN;
    } else if (c.center == "gpu") {
        outKey = "gpu";
        outValue = "GPU " + String((int) roundf(c.gpu)) + "%";
        outColor = TFT_MAGENTA;
    } else if (c.center == "gpuTemp") {
        outKey = "gpuTemp";
        outValue = "GPU " + String((int) roundf(c.gpuTemp)) + "\xC2\xB0" "C";
        outColor = TFT_MAGENTA;
    } else if (c.center == "ram") {
        outKey = "ram";
        outValue = "RAM " + String((int) roundf(c.ram)) + "%";
        outColor = TFT_ORANGE;
    } else if (c.center == "ssdTemp") {
        outKey = "ssdTemp";
        outValue = "SSD " + String((int) roundf(c.ssdTemp)) + "\xC2\xB0" "C";
        outColor = TFT_RED;
    } else {
        if (c.cpu >= c.gpu && c.cpu >= c.ram) {
            outKey = "cpu";
            outValue = "CPU " + String((int) roundf(c.cpu)) + "%";
            outColor = TFT_CYAN;
        } else if (c.gpu >= c.ram) {
            outKey = "gpu";
            outValue = "GPU " + String((int) roundf(c.gpu)) + "%";
            outColor = TFT_MAGENTA;
        } else {
            outKey = "ram";
            outValue = "RAM " + String((int) roundf(c.ram)) + "%";
            outColor = TFT_ORANGE;
        }
    }
}

// Fixed size rather than drawFittedString's auto-fit - letting the font size track content length
// meant it visibly jumped every time the digit count changed (e.g. a value crossing from single-
// to double-digit), which read as more distracting than the value change itself. Set slightly
// above the quadrants' own value font size (19) so the hero reading in the middle doesn't read
// smaller than the numbers around it, while still comfortably fitting the longest realistic string
// ("SSD 100°C") within CENTER_W.
const int CENTER_FONT_SIZE = 20;

void drawCenter(ScreenManager &manager, const String &value, uint32_t color) {
    manager.fillRect(CENTRE - CENTER_W / 2, CENTRE - CENTER_H / 2, CENTER_W, CENTER_H, TFT_BLACK);
    manager.setFontColor(color, TFT_BLACK);
    manager.drawCentreString(value, CENTRE, CENTRE, CENTER_FONT_SIZE);
}
} // namespace

SysMonitorControl::SysMonitorControl(ScreenManager &manager) : m_manager(manager) {
}

void SysMonitorControl::draw(int displayIndex, const SysMonitorConfig &config, SysMonitorState &state, bool externalForce) {
    m_manager.selectScreen(displayIndex);
    bool fullRedraw = externalForce || !state.initialized;

    String cpuLabel = "CPU \xC2\xB7 " + String((int) roundf(config.cpuTemp)) + "\xC2\xB0";
    String gpuLabel = "GPU \xC2\xB7 " + String((int) roundf(config.gpuTemp)) + "\xC2\xB0";
    String ramLabel = config.ramTotal > 0 ? ("RAM \xC2\xB7 " + String(config.ramTotal, 1) + "G") : String("RAM");
    String tempLabel = "SSD \xC2\xB7 TEMP";

    String cpuValue = String((int) roundf(config.cpu)) + "%";
    String gpuValue = String((int) roundf(config.gpu)) + "%";
    String ramValue = String((int) roundf(config.ram)) + "%";
    String tempValue = String((int) roundf(config.ssdTemp)) + "\xC2\xB0";

    String centerKey, centerValue;
    uint32_t centerColor;
    resolveCenter(config, centerKey, centerValue, centerColor);

    if (fullRedraw) {
        m_manager.fillScreen(TFT_BLACK);
        drawQuadrant(m_manager, CPU_Q, config.cpu, cpuValue, cpuLabel);
        drawQuadrant(m_manager, GPU_Q, config.gpu, gpuValue, gpuLabel);
        drawQuadrant(m_manager, RAM_Q, config.ram, ramValue, ramLabel);
        drawQuadrant(m_manager, TEMP_Q, config.ssdTemp, tempValue, tempLabel);
        if (centerKey != "none") {
            drawCenter(m_manager, centerValue, centerColor);
        }
    } else {
        if (config.cpu != state.lastCpu || cpuLabel != state.lastCpuLabel) {
            drawQuadrant(m_manager, CPU_Q, config.cpu, cpuValue, cpuLabel);
        }
        if (config.gpu != state.lastGpu || gpuLabel != state.lastGpuLabel) {
            drawQuadrant(m_manager, GPU_Q, config.gpu, gpuValue, gpuLabel);
        }
        if (config.ram != state.lastRam || ramLabel != state.lastRamLabel) {
            drawQuadrant(m_manager, RAM_Q, config.ram, ramValue, ramLabel);
        }
        if (config.ssdTemp != state.lastSsdTemp || tempLabel != state.lastTempLabel) {
            drawQuadrant(m_manager, TEMP_Q, config.ssdTemp, tempValue, tempLabel);
        }
        if (centerKey == "none") {
            if (state.lastCenterKey != "none") {
                // Just switched off - erase whatever was there, don't redraw anything in its place.
                m_manager.fillRect(CENTRE - CENTER_W / 2, CENTRE - CENTER_H / 2, CENTER_W, CENTER_H, TFT_BLACK);
            }
        } else if (centerKey != state.lastCenterKey || centerValue != state.lastCenterValue) {
            drawCenter(m_manager, centerValue, centerColor);
        }
    }

    state.initialized = true;
    state.lastCpu = config.cpu;
    state.lastGpu = config.gpu;
    state.lastRam = config.ram;
    state.lastSsdTemp = config.ssdTemp;
    state.lastCpuLabel = cpuLabel;
    state.lastGpuLabel = gpuLabel;
    state.lastRamLabel = ramLabel;
    state.lastTempLabel = tempLabel;
    state.lastCenterKey = centerKey;
    state.lastCenterValue = centerValue;
}
