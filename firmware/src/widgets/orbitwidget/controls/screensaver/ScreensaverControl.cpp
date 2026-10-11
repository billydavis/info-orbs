#include "ScreensaverControl.h"

#include "ScreensaverCommon.h"

namespace {
    const int NUM_EFFECTS = (int) ScreensaverEffect::RADAR + 1;

    // The next built effect after `effect`, wrapping round.
    ScreensaverEffect nextBuiltEffect(ScreensaverEffect effect) {
        for (int step = 1; step <= NUM_EFFECTS; step++) {
            ScreensaverEffect next = (ScreensaverEffect) (((int) effect + step) % NUM_EFFECTS);
            if (ScreensaverControl::effectBuilt(next)) {
                return next;
            }
        }
        return effect;
    }

    uint32_t defaultColor(ScreensaverEffect effect) {
        switch (effect) {
        case ScreensaverEffect::MATRIX:
            return MatrixEffect::DEFAULT_COLOR;
        case ScreensaverEffect::WARP:
            return WarpEffect::DEFAULT_COLOR;
        case ScreensaverEffect::ORRERY:
            return OrreryEffect::DEFAULT_COLOR;
        case ScreensaverEffect::RADAR:
            return RadarEffect::DEFAULT_COLOR;
        case ScreensaverEffect::ASTEROIDS:
        default:
            return AsteroidsEffect::DEFAULT_COLOR;
        }
    }
} // namespace

ScreensaverControl::ScreensaverControl(ScreenManager &manager) : m_manager(manager), m_asteroids(manager), m_matrix(manager), m_warp(manager), m_orrery(manager), m_radar(manager) {
}

const ScreensaverEffect ScreensaverControl::ALL_EFFECTS[] = {ScreensaverEffect::ASTEROIDS, ScreensaverEffect::MATRIX, ScreensaverEffect::WARP, ScreensaverEffect::ORRERY, ScreensaverEffect::RADAR};

bool ScreensaverControl::effectBuilt(ScreensaverEffect effect) {
    switch (effect) {
    case ScreensaverEffect::ASTEROIDS:
        return ORBIT_EFFECT_ASTEROIDS;
    case ScreensaverEffect::MATRIX:
        return ORBIT_EFFECT_MATRIX;
    case ScreensaverEffect::WARP:
        return ORBIT_EFFECT_WARP;
    case ScreensaverEffect::ORRERY:
        return ORBIT_EFFECT_ORRERY;
    case ScreensaverEffect::RADAR:
        return ORBIT_EFFECT_RADAR;
    }
    return false;
}

const char *ScreensaverControl::effectName(ScreensaverEffect effect) {
    switch (effect) {
    case ScreensaverEffect::MATRIX:
        return "matrix";
    case ScreensaverEffect::WARP:
        return "warp";
    case ScreensaverEffect::ORRERY:
        return "orrery";
    case ScreensaverEffect::RADAR:
        return "radar";
    case ScreensaverEffect::ASTEROIDS:
    default:
        return "asteroids";
    }
}

bool ScreensaverControl::effectFromName(const String &name, ScreensaverEffect &out) {
    if (name == "asteroids") {
        out = ScreensaverEffect::ASTEROIDS;
        return true;
    }
    if (name == "matrix") {
        out = ScreensaverEffect::MATRIX;
        return true;
    }
    if (name == "warp") {
        out = ScreensaverEffect::WARP;
        return true;
    }
    if (name == "orrery") {
        out = ScreensaverEffect::ORRERY;
        return true;
    }
    if (name == "radar") {
        out = ScreensaverEffect::RADAR;
        return true;
    }
    return false;
}

void ScreensaverControl::draw(int displayIndex, const ScreensaverConfig &config, ScreensaverState &state, bool fullRedraw) {
    m_manager.selectScreen(displayIndex);

    unsigned long now = millis();
    ScreensaverEffect effect = config.effect;
    if (config.cycle) {
        if (!state.initialized || !state.cycling) {
            // This screen has only just been set cycling - start from the first effect.
            effect = ORBIT_FIRST_EFFECT;
            state.effectStartedMs = now;
        } else if (now - state.effectStartedMs >= config.cycleMs) {
            effect = nextBuiltEffect(state.showing);
            state.effectStartedMs = now;
        } else {
            effect = state.showing;
        }
    }
    state.cycling = config.cycle;

    // The union only holds the state of the effect that last drew, so a different one starts afresh.
    if (!state.initialized || state.showing != effect) {
        fullRedraw = true;
    }
    float dt = 0;
    if (fullRedraw) {
        state.showing = effect;
        state.initialized = true;
    } else {
        dt = (now - state.lastFrameMs) / 1000.0f;
        if (dt > Screensaver::MAX_FRAME_DT) {
            dt = Screensaver::MAX_FRAME_DT;
        }
    }
    state.lastFrameMs = now;

    uint32_t color = config.colorSet ? config.color : defaultColor(state.showing);
    // An effect that isn't built has no case here, so its code isn't linked in. A config can't ask
    // for one - OrbItWidget refuses it.
    switch (state.showing) {
#if ORBIT_EFFECT_ASTEROIDS
    case ScreensaverEffect::ASTEROIDS:
        m_asteroids.draw(state.asteroids, color, dt, now, fullRedraw);
        break;
#endif
#if ORBIT_EFFECT_MATRIX
    case ScreensaverEffect::MATRIX:
        m_matrix.draw(state.matrix, color, now, fullRedraw);
        break;
#endif
#if ORBIT_EFFECT_WARP
    case ScreensaverEffect::WARP:
        m_warp.draw(state.warp, color, dt, fullRedraw);
        break;
#endif
#if ORBIT_EFFECT_ORRERY
    case ScreensaverEffect::ORRERY:
        m_orrery.draw(state.orrery, color, dt, fullRedraw);
        break;
#endif
#if ORBIT_EFFECT_RADAR
    case ScreensaverEffect::RADAR:
        m_radar.draw(state.radar, color, dt, fullRedraw);
        break;
#endif
    default:
        break;
    }
}
