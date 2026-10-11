#ifndef SCREENSAVER_CONTROL_H
#define SCREENSAVER_CONTROL_H

#include "AsteroidsEffect.h"
#include "MatrixEffect.h"
#include "OrreryEffect.h"
#include "RadarEffect.h"
#include "ScreenManager.h"
#include "WarpEffect.h"

enum class ScreensaverEffect {
    ASTEROIDS,
    MATRIX,
    WARP,
    ORRERY,
    RADAR,
};

// The first effect this firmware was built with (see the ORBIT_EFFECT_* switches in config.h) -
// what a screensaver shows when it isn't told which, and where a cycle starts.
#if ORBIT_EFFECT_ASTEROIDS
    #define ORBIT_FIRST_EFFECT ScreensaverEffect::ASTEROIDS
#elif ORBIT_EFFECT_MATRIX
    #define ORBIT_FIRST_EFFECT ScreensaverEffect::MATRIX
#elif ORBIT_EFFECT_WARP
    #define ORBIT_FIRST_EFFECT ScreensaverEffect::WARP
#elif ORBIT_EFFECT_ORRERY
    #define ORBIT_FIRST_EFFECT ScreensaverEffect::ORRERY
#else
    #define ORBIT_FIRST_EFFECT ScreensaverEffect::RADAR
#endif

struct ScreensaverConfig {
    ScreensaverEffect effect = ORBIT_FIRST_EFFECT;
    // true = ignore `effect` and show each effect in turn, cycleMs at a time.
    bool cycle = false;
    unsigned long cycleMs = 300000;
    // false = each effect's own default color. Kept apart from `color` because every RGB565 value,
    // black included, is a color someone can ask for.
    bool colorSet = false;
    uint32_t color = 0;
};

// One screen's animation state. A screen only ever runs one effect at a time, so the effects'
// states share the same memory rather than each costing its own array of NUM_SCREENS.
struct ScreensaverState {
    bool initialized = false;
    ScreensaverEffect showing = ScreensaverEffect::ASTEROIDS;
    unsigned long lastFrameMs = 0;
    bool cycling = false; // the last frame was drawn from a cycling config
    unsigned long effectStartedMs = 0; // when a cycling screen moved on to `showing`
    union {
        AsteroidsState asteroids;
        MatrixState matrix;
        WarpState warp;
        OrreryState orrery;
        RadarState radar;
    };
};

// Purely decorative control - no data source. Draws whichever effect the config picks, or each in
// turn; each effect is its own class in this directory, and adding one means a ScreensaverEffect
// value, a member in ScreensaverState's union, and a case in each switch in ScreensaverControl.cpp.
class ScreensaverControl {
public:
    explicit ScreensaverControl(ScreenManager &manager);

    // fullRedraw starts the effect afresh and paints everything from scratch (first time this
    // screen shows the control, a config change, or a force redraw); otherwise this steps one
    // animation frame and the effect only repaints what moved.
    void draw(int displayIndex, const ScreensaverConfig &config, ScreensaverState &state, bool fullRedraw);

    // The names orbit-api uses for params.effect. effectFromName() knows every effect, built or not.
    static const char *effectName(ScreensaverEffect effect);
    static bool effectFromName(const String &name, ScreensaverEffect &out);

    // Every effect, in cycle order.
    static const ScreensaverEffect ALL_EFFECTS[(int) ScreensaverEffect::RADAR + 1];
    // Whether this firmware was built with an effect - see the ORBIT_EFFECT_* switches in config.h.
    static bool effectBuilt(ScreensaverEffect effect);

private:
    ScreenManager &m_manager;
    AsteroidsEffect m_asteroids;
    MatrixEffect m_matrix;
    WarpEffect m_warp;
    OrreryEffect m_orrery;
    RadarEffect m_radar;
};
#endif // SCREENSAVER_CONTROL_H
