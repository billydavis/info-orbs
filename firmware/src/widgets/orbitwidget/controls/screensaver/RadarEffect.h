#ifndef RADAR_EFFECT_H
#define RADAR_EFFECT_H

#include "ScreenManager.h"

// A radar scope: a beam sweeping round the screen over range rings and crosshairs, leaving a
// fading afterglow behind it. Contacts light up as the beam passes them and fade until it comes
// round again, each having moved a little by then.
//
// The scope is divided into narrow sectors and the beam advances one at a time. A sector's
// brightness depends only on how long ago the beam passed, falling off in a few bands, so each
// step repaints just the handful of sectors that moved from one band to the next.
//
// No default member initializers: this lives in ScreensaverState's union with the other effects'
// states, and seed() sets every field before anything reads it.
struct RadarState {
    static const int NUM_BLIPS = 6;

    uint8_t beam; // the sector the beam is on
    float stepsDue; // fractional sectors the beam has still to advance

    uint8_t blipSector[NUM_BLIPS];
    uint8_t blipRadius[NUM_BLIPS]; // distance from the centre
    uint8_t blipAge[NUM_BLIPS]; // beam steps since it was lit - BLIP_GONE once it has faded out
};

class RadarEffect {
public:
    static const uint32_t DEFAULT_COLOR = TFT_GREEN;

    explicit RadarEffect(ScreenManager &manager);

    // Draws to whichever screen is currently selected; `color` is the scope's. fullRedraw paints an
    // empty scope with the beam at a random angle; otherwise this advances the beam by `dt` seconds.
    void draw(RadarState &state, uint32_t color, float dt, bool fullRedraw);

private:
    void seed(RadarState &state);
    void placeBlip(RadarState &state, int blip);
    void step(RadarState &state, uint32_t color);
    void drawSector(int sector, uint32_t color, uint8_t brightness);
    void drawOverlay(int sector, uint32_t color);
    void drawBlip(const RadarState &state, int blip, uint32_t color);

    ScreenManager &m_manager;
};
#endif // RADAR_EFFECT_H
