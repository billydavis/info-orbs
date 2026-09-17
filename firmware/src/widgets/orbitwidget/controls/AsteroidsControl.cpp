#include "AsteroidsControl.h"

#include "config_helper.h"
#include <math.h>

namespace {
const int CENTRE = SCREEN_SIZE / 2;
// Keeps every moving object's centre within this radius of CENTRE so nothing drifts under the
// round bezel - same margin reasoning as GaugeControl's OUTER_RADIUS.
const int PLAYFIELD_RADIUS = 100;
const int PLANET_RADIUS = 24;
const uint32_t PLANET_COLOR = 0xFD20; // orange (RGB565)
const uint32_t PLANET_RING_COLOR = TFT_SILVER;
const uint32_t LABEL_COLOR = TFT_BLACK;
const int LABEL_FONT_SIZE = 13;
// The "Orb-It" label routinely renders wider than the planet's own diameter - erasing only
// PLANET_RADIUS worth of square left the label's anti-aliased edge pixels (black blended into the
// orange fill) behind as a trailing streak whenever the planet moved. This pads the erase box well
// past the circle itself so the whole label is always inside it.
const int PLANET_ERASE_MARGIN = 20;
const uint32_t ROCK_COLOR = TFT_LIGHTGREY;
const float MAX_FRAME_DT = 0.2f; // clamp so a stall (e.g. first frame, widget switch) doesn't fling anything across the screen

// Bounces a circular object of `radius` off the play field's boundary, reflecting velocity about
// the radial normal (elastic bounce off the inside of a circle).
void stepAndBounce(float &x, float &y, float &vx, float &vy, float radius, float dt) {
    x += vx * dt;
    y += vy * dt;

    float dx = x - CENTRE;
    float dy = y - CENTRE;
    float dist = sqrtf(dx * dx + dy * dy);
    float limit = PLAYFIELD_RADIUS - radius;
    if (dist > limit && dist > 0.001f) {
        float nx = dx / dist;
        float ny = dy / dist;
        float dot = vx * nx + vy * ny;
        vx -= 2.0f * dot * nx;
        vy -= 2.0f * dot * ny;
        float overlap = dist - limit;
        x -= nx * overlap;
        y -= ny * overlap;
    }
}
} // namespace

AsteroidsControl::AsteroidsControl(ScreenManager &manager) : m_manager(manager) {
}

void AsteroidsControl::seed(AsteroidsState &state) {
    for (int i = 0; i < AsteroidsState::NUM_STARS; i++) {
        // Random point anywhere in the square framebuffer is fine for stars - unlike the planet/
        // rocks they're single pixels, so a few landing outside the visible circle just get
        // clipped by the physical bezel like everything else near the corners already does.
        state.starX[i] = random(0, SCREEN_SIZE);
        state.starY[i] = random(0, SCREEN_SIZE);
        int brightness = random(0, 3);
        state.starColor[i] = brightness == 0 ? TFT_WHITE : (brightness == 1 ? TFT_LIGHTGREY : TFT_DARKGREY);
    }

    float planetAngle = random(0, 360) * DEG_TO_RAD;
    float planetSpeed = 14.0f;
    float planetStartRadius = 30.0f;
    state.planetX = CENTRE + planetStartRadius * cosf(planetAngle);
    state.planetY = CENTRE + planetStartRadius * sinf(planetAngle);
    float planetDir = random(0, 360) * DEG_TO_RAD;
    state.planetVX = planetSpeed * cosf(planetDir);
    state.planetVY = planetSpeed * sinf(planetDir);
    state.lastPlanetX = (int) state.planetX;
    state.lastPlanetY = (int) state.planetY;

    for (int r = 0; r < AsteroidsState::NUM_ROCKS; r++) {
        float angle = random(0, 360) * DEG_TO_RAD;
        float radiusFromCentre = random(30, PLAYFIELD_RADIUS - 10);
        state.rockX[r] = CENTRE + radiusFromCentre * cosf(angle);
        state.rockY[r] = CENTRE + radiusFromCentre * sinf(angle);
        float dir = random(0, 360) * DEG_TO_RAD;
        float speed = random(18, 34);
        state.rockVX[r] = speed * cosf(dir);
        state.rockVY[r] = speed * sinf(dir);

        // A fixed, once-only jittered hexagon so each rock keeps a consistent "irregular rock"
        // silhouette as it drifts, rather than a perfect hexagon - it never rotates, only translates.
        int baseRadius = random(6, 10);
        int maxOffset = 0;
        for (int p = 0; p < AsteroidsState::ROCK_POINTS; p++) {
            float pointAngle = (360.0f / AsteroidsState::ROCK_POINTS) * p * DEG_TO_RAD;
            int jitter = random(-3, 4);
            int pointRadius = baseRadius + jitter;
            int ox = (int) roundf(pointRadius * cosf(pointAngle));
            int oy = (int) roundf(pointRadius * sinf(pointAngle));
            state.rockOffsetX[r][p] = (int8_t) ox;
            state.rockOffsetY[r][p] = (int8_t) oy;
            maxOffset = max(maxOffset, max(abs(ox), abs(oy)));
        }
        state.rockBoundRadius[r] = maxOffset + 1;
        state.lastRockX[r] = (int) state.rockX[r];
        state.lastRockY[r] = (int) state.rockY[r];
    }

    state.lastFrameMs = millis();
}

void AsteroidsControl::drawStars(AsteroidsState &state) {
    for (int i = 0; i < AsteroidsState::NUM_STARS; i++) {
        m_manager.fillRect(state.starX[i], state.starY[i], 1, 1, state.starColor[i]);
    }
}

void AsteroidsControl::eraseAndRestoreStars(AsteroidsState &state, int x, int y, int radius) {
    int left = x - radius;
    int top = y - radius;
    int size = radius * 2;
    m_manager.fillRect(left, top, size, size, TFT_BLACK);
    for (int i = 0; i < AsteroidsState::NUM_STARS; i++) {
        if (state.starX[i] >= left && state.starX[i] < left + size && state.starY[i] >= top && state.starY[i] < top + size) {
            m_manager.fillRect(state.starX[i], state.starY[i], 1, 1, state.starColor[i]);
        }
    }
}

void AsteroidsControl::drawPlanet(int x, int y) {
    m_manager.fillCircle(x, y, PLANET_RADIUS, PLANET_COLOR);
    m_manager.drawCircle(x, y, PLANET_RADIUS - 7, PLANET_RING_COLOR);
    m_manager.drawString("Orb-It", x, y, LABEL_FONT_SIZE, Align::MiddleCenter, LABEL_COLOR, PLANET_COLOR);
}

void AsteroidsControl::drawRock(const AsteroidsState &state, int rockIndex, int x, int y) {
    for (int p = 0; p < AsteroidsState::ROCK_POINTS; p++) {
        int nextP = (p + 1) % AsteroidsState::ROCK_POINTS;
        int x1 = x + state.rockOffsetX[rockIndex][p];
        int y1 = y + state.rockOffsetY[rockIndex][p];
        int x2 = x + state.rockOffsetX[rockIndex][nextP];
        int y2 = y + state.rockOffsetY[rockIndex][nextP];
        m_manager.drawLine(x1, y1, x2, y2, ROCK_COLOR);
    }
}

void AsteroidsControl::draw(int displayIndex, AsteroidsState &state, bool fullRedraw) {
    m_manager.selectScreen(displayIndex);

    if (fullRedraw || !state.initialized) {
        seed(state);
        m_manager.fillScreen(TFT_BLACK);
        drawStars(state);
        drawPlanet((int) state.planetX, (int) state.planetY);
        for (int r = 0; r < AsteroidsState::NUM_ROCKS; r++) {
            drawRock(state, r, (int) state.rockX[r], (int) state.rockY[r]);
        }
        state.initialized = true;
        return;
    }

    unsigned long now = millis();
    float dt = (now - state.lastFrameMs) / 1000.0f;
    if (dt > MAX_FRAME_DT) {
        dt = MAX_FRAME_DT;
    }
    state.lastFrameMs = now;

    stepAndBounce(state.planetX, state.planetY, state.planetVX, state.planetVY, PLANET_RADIUS, dt);
    eraseAndRestoreStars(state, state.lastPlanetX, state.lastPlanetY, PLANET_RADIUS + PLANET_ERASE_MARGIN);
    drawPlanet((int) state.planetX, (int) state.planetY);
    state.lastPlanetX = (int) state.planetX;
    state.lastPlanetY = (int) state.planetY;

    for (int r = 0; r < AsteroidsState::NUM_ROCKS; r++) {
        stepAndBounce(state.rockX[r], state.rockY[r], state.rockVX[r], state.rockVY[r], (float) state.rockBoundRadius[r], dt);
        eraseAndRestoreStars(state, state.lastRockX[r], state.lastRockY[r], state.rockBoundRadius[r] + 1);
        drawRock(state, r, (int) state.rockX[r], (int) state.rockY[r]);
        state.lastRockX[r] = (int) state.rockX[r];
        state.lastRockY[r] = (int) state.rockY[r];
    }
}
