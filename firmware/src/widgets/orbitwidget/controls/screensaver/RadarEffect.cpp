#include "RadarEffect.h"

#include "ScreensaverCommon.h"
#include "Utils.h"
#include <math.h>

using namespace Screensaver;

namespace {
const int SCOPE_RADIUS = 105;
const int SECTOR_DEGREES = 3;
const int NUM_SECTORS = 360 / SECTOR_DEGREES;
const float SECTORS_PER_SECOND = 20.0f; // one sweep every 6 seconds

// Brightness (of 255) of a sector by the number of steps since the beam was on it: the beam
// itself, then AFTERGLOW_BANDS bands of BAND_SECTORS sectors each, then nothing.
const uint8_t BEAM_BRIGHTNESS = 255;
const uint8_t AFTERGLOW[] = {130, 95, 65, 42, 25};
const int AFTERGLOW_BANDS = sizeof(AFTERGLOW) / sizeof(AFTERGLOW[0]);
const int BAND_SECTORS = 4;
const int AFTERGLOW_SECTORS = AFTERGLOW_BANDS * BAND_SECTORS;

const int RING_RADIUS[] = {35, 70, SCOPE_RADIUS};
const uint8_t OVERLAY_BRIGHTNESS = 90; // rings and crosshairs
const int HUB_RADIUS = 3;

const int BLIP_SIZE = 3; // radius of the dot
const int BLIP_MIN_RADIUS = 20;
const int BLIP_MAX_RADIUS = 98;
// A contact stays fully lit for a moment after the beam passes, then fades in steps and is gone
// well before the beam returns - by which time the afterglow has long left it too, so there's
// only ever black, rings and crosshairs to put back where it was.
const uint8_t BLIP_GONE = 80;
const uint8_t BLIP_FADE_AGE[] = {20, 40, 60, BLIP_GONE};
const uint8_t BLIP_FADE_BRIGHTNESS[] = {255, 170, 110, 60};
// Between sweeps a contact moves by up to this many sectors and pixels - or, one time in
// BLIP_JUMP_ONE_IN, is replaced by a new one somewhere else.
const int BLIP_DRIFT_SECTORS = 2;
const int BLIP_DRIFT_RADIUS = 6;
const int BLIP_JUMP_ONE_IN = 4;

uint8_t sectorBrightness(int age) {
    if (age == 0) {
        return BEAM_BRIGHTNESS;
    }
    return age <= AFTERGLOW_SECTORS ? AFTERGLOW[(age - 1) / BAND_SECTORS] : 0;
}

uint8_t blipBrightness(int age) {
    for (int i = 0; i < 4; i++) {
        if (age < BLIP_FADE_AGE[i]) {
            return BLIP_FADE_BRIGHTNESS[i];
        }
    }
    return 0;
}

// The sector `steps` behind `sector` (ahead of it, if negative).
int sectorBehind(int sector, int steps) {
    return ((sector - steps) % NUM_SECTORS + NUM_SECTORS) % NUM_SECTORS;
}

// How many sectors either side of its own a contact's dot reaches into - more, the nearer the centre.
int blipSpan(int radius) {
    return 2 + 76 / radius;
}
} // namespace

RadarEffect::RadarEffect(ScreenManager &manager) : m_manager(manager) {
}

void RadarEffect::placeBlip(RadarState &state, int blip) {
    state.blipSector[blip] = random(0, NUM_SECTORS);
    state.blipRadius[blip] = random(BLIP_MIN_RADIUS, BLIP_MAX_RADIUS + 1);
}

void RadarEffect::seed(RadarState &state) {
    state.beam = random(0, NUM_SECTORS);
    state.stepsDue = 0;
    for (int blip = 0; blip < RadarState::NUM_BLIPS; blip++) {
        placeBlip(state, blip);
        state.blipAge[blip] = BLIP_GONE;
    }
}

// Angles here are the display library's: 0 at 6 o'clock, increasing clockwise - the way the beam turns.
void RadarEffect::drawSector(int sector, uint32_t color, uint8_t brightness) {
    uint32_t fill = Utils::rgb565dim(color, brightness);
    m_manager.drawArc(CENTRE, CENTRE, SCOPE_RADIUS, 0, sector * SECTOR_DEGREES, (sector + 1) * SECTOR_DEGREES, fill, fill, false);
}

// The parts of the rings and crosshairs that lie in a sector, which repainting it has just covered.
void RadarEffect::drawOverlay(int sector, uint32_t color) {
    uint32_t overlay = Utils::rgb565dim(color, OVERLAY_BRIGHTNESS);
    int start = sector * SECTOR_DEGREES;
    int end = start + SECTOR_DEGREES;
    for (int radius : RING_RADIUS) {
        m_manager.drawArc(CENTRE, CENTRE, radius, radius - 1, start, end, overlay, overlay, false);
    }
    // A crosshair arm lies on the boundary between two sectors, so either one repaints it.
    if (start == 0 || end == 360) {
        m_manager.fillRect(CENTRE, CENTRE, 1, SCOPE_RADIUS + 1, overlay);
    } else if (start == 90 || end == 90) {
        m_manager.fillRect(CENTRE - SCOPE_RADIUS, CENTRE, SCOPE_RADIUS + 1, 1, overlay);
    } else if (start == 180 || end == 180) {
        m_manager.fillRect(CENTRE, CENTRE - SCOPE_RADIUS, 1, SCOPE_RADIUS + 1, overlay);
    } else if (start == 270 || end == 270) {
        m_manager.fillRect(CENTRE, CENTRE, SCOPE_RADIUS + 1, 1, overlay);
    }
}

void RadarEffect::drawBlip(const RadarState &state, int blip, uint32_t color) {
    float angle = (state.blipSector[blip] + 0.5f) * SECTOR_DEGREES * DEG_TO_RAD;
    int x = CENTRE - (int) roundf(state.blipRadius[blip] * sinf(angle));
    int y = CENTRE + (int) roundf(state.blipRadius[blip] * cosf(angle));
    m_manager.fillCircle(x, y, BLIP_SIZE, color);
}

// Advances the beam one sector.
void RadarEffect::step(RadarState &state, uint32_t color) {
    state.beam = sectorBehind(state.beam, -1);

    // The sectors whose brightness changes with this step: the one the beam has moved on to, the
    // one it has just left, the first of each afterglow band, and the one the afterglow has left.
    int repainted[AFTERGLOW_BANDS + 2];
    int numRepainted = 0;
    repainted[numRepainted++] = 0;
    for (int age = 1; age <= AFTERGLOW_SECTORS + 1; age += BAND_SECTORS) {
        repainted[numRepainted++] = age;
    }
    for (int i = 0; i < numRepainted; i++) {
        int sector = sectorBehind(state.beam, repainted[i]);
        drawSector(sector, color, sectorBrightness(repainted[i]));
        drawOverlay(sector, color);
    }

    for (int blip = 0; blip < RadarState::NUM_BLIPS; blip++) {
        bool wasGone = state.blipAge[blip] >= BLIP_GONE;
        int behind = sectorBehind(state.beam, state.blipSector[blip]); // sectors the beam is past it
        if (behind == 0) {
            state.blipAge[blip] = 0;
        } else if (!wasGone) {
            state.blipAge[blip]++;
        }
        if (state.blipAge[blip] >= BLIP_GONE) {
            if (!wasGone) {
                // Faded right out: clear it, mend the rings and crosshairs it sat on, and move it
                // for the next sweep.
                drawBlip(state, blip, TFT_BLACK);
                int span = blipSpan(state.blipRadius[blip]);
                for (int offset = -span; offset <= span; offset++) {
                    drawOverlay(sectorBehind(state.blipSector[blip], offset), color);
                }
                if (random(0, BLIP_JUMP_ONE_IN) == 0) {
                    placeBlip(state, blip);
                } else {
                    state.blipSector[blip] = sectorBehind(state.blipSector[blip], random(-BLIP_DRIFT_SECTORS, BLIP_DRIFT_SECTORS + 1));
                    state.blipRadius[blip] = constrain(state.blipRadius[blip] + random(-BLIP_DRIFT_RADIUS, BLIP_DRIFT_RADIUS + 1), BLIP_MIN_RADIUS, BLIP_MAX_RADIUS);
                }
            }
            continue;
        }

        // Redrawn when it steps down in brightness, and whenever a sector it reaches into was
        // repainted underneath it.
        bool redraw = blipBrightness(state.blipAge[blip]) != blipBrightness(state.blipAge[blip] - 1);
        int span = blipSpan(state.blipRadius[blip]);
        for (int i = 0; i < numRepainted && !redraw; i++) {
            int apart = abs(behind - repainted[i]);
            redraw = min(apart, NUM_SECTORS - apart) <= span;
        }
        if (redraw) {
            drawBlip(state, blip, Utils::rgb565dim(color, blipBrightness(state.blipAge[blip])));
        }
    }

    // Every sector meets at the centre, where repainting them leaves a mess - covered by a hub.
    m_manager.fillCircle(CENTRE, CENTRE, HUB_RADIUS, color);
}

void RadarEffect::draw(RadarState &state, uint32_t color, float dt, bool fullRedraw) {
    if (fullRedraw) {
        seed(state);
        m_manager.fillScreen(TFT_BLACK);
        for (int sector = 0; sector < NUM_SECTORS; sector++) {
            drawOverlay(sector, color);
        }
        m_manager.fillCircle(CENTRE, CENTRE, HUB_RADIUS, color);
        return;
    }

    state.stepsDue += dt * SECTORS_PER_SECOND;
    while (state.stepsDue >= 1.0f) {
        state.stepsDue -= 1.0f;
        step(state, color);
    }
}
