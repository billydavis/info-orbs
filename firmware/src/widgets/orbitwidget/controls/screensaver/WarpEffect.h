#ifndef WARP_EFFECT_H
#define WARP_EFFECT_H

#include "ScreenManager.h"

// A flight down a wormhole: stars streak outward, speeding up as they go, between rings that
// expand the same way. The tunnel keeps taking random turns. Its far end swings off to one side
// while what's about to pass stays centred on the viewer, so the rings in between trail off in a
// curve - and since each ring and star keeps the turn the tunnel had when it appeared, a new turn
// arrives from the distance, the way a real bend would.
//
// No default member initializers: this lives in ScreensaverState's union with the other effects'
// states, and seed() sets every field before anything reads it.
struct WarpState {
    static const int NUM_STARS = 40;
    static const int NUM_RINGS = 4;

    // How far, in pixels, the tunnel's far end currently sits from the screen's centre, the turn
    // it's easing towards, and the seconds left until it picks another.
    float bendX, bendY;
    float bendTargetX, bendTargetY;
    float turnTimer;

    // A star is a streak along a fixed direction from the tunnel's axis. Kept as small integers
    // rather than an angle and float positions: erasing a streak then recomputes exactly the
    // pixels drawing it did, and it's a quarter of the memory.
    int8_t starDirX[NUM_STARS], starDirY[NUM_STARS]; // unit direction, scaled by 127
    int8_t starBendX[NUM_STARS], starBendY[NUM_STARS]; // the tunnel's bend when it appeared
    float starRadius[NUM_STARS]; // distance of the streak's leading end from the axis
    uint8_t starInner[NUM_STARS], starOuter[NUM_STARS]; // the streak currently on screen, as distances

    float ringRadius[NUM_RINGS];
    int8_t ringBendX[NUM_RINGS], ringBendY[NUM_RINGS]; // the tunnel's bend when it appeared
    int16_t ringDrawn[NUM_RINGS]; // radius of the ring currently on screen
};

class WarpEffect {
public:
    static const uint32_t DEFAULT_COLOR = TFT_SKYBLUE;

    explicit WarpEffect(ScreenManager &manager);

    // Draws to whichever screen is currently selected; `color` is the rings'. fullRedraw starts a
    // fresh tunnel and paints everything from scratch; otherwise this advances `dt` seconds,
    // erasing each streak and ring that moved and then redrawing them all.
    void draw(WarpState &state, uint32_t color, float dt, bool fullRedraw);

private:
    void seed(WarpState &state);
    void newTurn(WarpState &state);
    void spawnStar(WarpState &state, int i, int minRadius, int maxRadius);
    void drawStreak(const WarpState &state, int i, uint32_t color);
    void drawRing(const WarpState &state, int ring, uint32_t color);
    void drawAll(WarpState &state, uint32_t color);

    ScreenManager &m_manager;
};
#endif // WARP_EFFECT_H
