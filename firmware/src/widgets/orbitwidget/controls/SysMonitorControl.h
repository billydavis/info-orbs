#ifndef SYS_MONITOR_CONTROL_H
#define SYS_MONITOR_CONTROL_H

#include "ScreenManager.h"

// One quarter-ring per metric (CPU/GPU/RAM/drive temp) arranged around the bezel, with a center
// readout the caller can pin to any one metric - mocked up as "Halo Cluster" at
// https://claude.ai/artifact/EXGMbSSKAvayjQoMen4rws before any of this was written (see also
// firmware/src/widgets/orbitwidget/docs/orbit-api.md). Quadrant colors are fixed per metric
// (CPU=cyan, GPU=magenta, RAM=orange, drive temp=red) rather than configurable - the color coding
// is the whole point of the layout, not a per-instance style choice.
struct SysMonitorConfig {
    float cpu = 0; // percent, 0-100
    float cpuTemp = 0; // degrees C
    float gpu = 0; // percent, 0-100
    float gpuTemp = 0; // degrees C
    float ram = 0; // percent, 0-100
    float ramTotal = 0; // GB shown alongside RAM's quadrant label; 0 = omit the capacity line
    float ssdTemp = 0; // degrees C, plotted on the same 0-100 scale as the percent metrics
    // Which reading gets the center readout: "cpu", "cpuTemp", "gpu", "gpuTemp", "ram", "ssdTemp",
    // "" (default) to auto-pick whichever of cpu/gpu/ram is currently highest, or "none" to turn
    // the center readout off entirely.
    String center = "";
};

// Tracks what was actually last drawn for one screen, so SysMonitorControl can redraw only the one
// quadrant (arc + label) or the center readout whose value actually changed - the same
// erase-and-redraw discipline GaugeState already established for GaugeControl, just per-quadrant
// instead of per-screen.
struct SysMonitorState {
    bool initialized = false;
    float lastCpu = -1, lastGpu = -1, lastRam = -1, lastSsdTemp = -1;
    String lastCpuLabel, lastGpuLabel, lastRamLabel, lastTempLabel;
    String lastCenterKey, lastCenterValue;
};

class SysMonitorControl {
public:
    explicit SysMonitorControl(ScreenManager &manager);

    // externalForce: this screen might currently be showing something else entirely - always
    // triggers a full repaint regardless of what `state` remembers, matching GaugeControl's own
    // externalForce parameter.
    void draw(int displayIndex, const SysMonitorConfig &config, SysMonitorState &state, bool externalForce);

private:
    ScreenManager &m_manager;
};
#endif // SYS_MONITOR_CONTROL_H
