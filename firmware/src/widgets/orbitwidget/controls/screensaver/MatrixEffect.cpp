#include "MatrixEffect.h"

#include "ScreensaverCommon.h"
#include "Utils.h"

using namespace Screensaver;

namespace {
const int GLYPH_COLS = 5;
const int GLYPH_ROWS = 7;
const int GLYPH_SCALE = 2; // screen pixels per glyph pixel
const int CELL_WIDTH = SCREEN_SIZE / MatrixState::COLS;
const int CELL_HEIGHT = SCREEN_SIZE / MatrixState::ROWS;

const uint32_t HEAD_COLOR = TFT_WHITE;
// Brightness (of 255) of the last two cells of a drop's tail, nearest the end last.
const uint8_t TAIL_FADE[] = {150, 70};
const int MIN_LENGTH = 5;
const int MAX_LENGTH = 13;
const int MIN_STEP_MS = 60;
const int MAX_STEP_MS = 170;
const int MAX_DROP_DELAY_MS = 2500;
// A glyph somewhere in a tail changes about this often, per frame, as in the film.
const int MUTATE_ONE_IN = 3;

// Half-width katakana and digits, one row per byte, top row first, high bit on the left. They're
// drawn mirrored - as the film's are - which here just means reading each row from the low bit.
const uint8_t GLYPHS[][GLYPH_ROWS] = {
    {0b11111, 0b00001, 0b00101, 0b00110, 0b00100, 0b00100, 0b01000}, // a
    {0b00001, 0b00010, 0b00100, 0b01100, 0b10100, 0b00100, 0b00100}, // i
    {0b00100, 0b11111, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000}, // u
    {0b00000, 0b11111, 0b00100, 0b00100, 0b00100, 0b11111, 0b00000}, // e
    {0b00010, 0b11111, 0b00010, 0b00110, 0b01010, 0b10010, 0b00010}, // o
    {0b01000, 0b11111, 0b01001, 0b01001, 0b01001, 0b01001, 0b10010}, // ka
    {0b00100, 0b11111, 0b00100, 0b11111, 0b00100, 0b00100, 0b00100}, // ki
    {0b01111, 0b01001, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000}, // ku
    {0b01000, 0b01111, 0b10010, 0b00010, 0b00010, 0b00010, 0b00100}, // ke
    {0b00000, 0b11111, 0b00001, 0b00001, 0b00001, 0b11111, 0b00000}, // ko
    {0b01010, 0b11111, 0b01010, 0b01010, 0b00010, 0b00100, 0b01000}, // sa
    {0b00000, 0b11001, 0b00001, 0b11001, 0b00001, 0b00010, 0b11100}, // shi
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01010, 0b10001, 0b00000}, // su
    {0b01000, 0b11111, 0b01001, 0b01010, 0b01000, 0b01000, 0b00111}, // se
    {0b10001, 0b10001, 0b01001, 0b00001, 0b00010, 0b00100, 0b01000}, // so
    {0b01111, 0b01001, 0b10101, 0b00011, 0b00010, 0b00100, 0b01000}, // ta
    {0b00010, 0b11100, 0b00100, 0b11111, 0b00100, 0b00100, 0b01000}, // chi
    {0b10101, 0b10101, 0b10101, 0b00001, 0b00010, 0b00100, 0b01000}, // tsu
    {0b01110, 0b00000, 0b11111, 0b00100, 0b00100, 0b00100, 0b01000}, // te
    {0b01000, 0b01000, 0b01000, 0b01100, 0b01010, 0b01000, 0b01000}, // to
    {0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b01000, 0b10000}, // na
    {0b00000, 0b01110, 0b00000, 0b00000, 0b00000, 0b11111, 0b00000}, // ni
    {0b11111, 0b00001, 0b01010, 0b00100, 0b01010, 0b10000, 0b00000}, // nu
    {0b00100, 0b11111, 0b00010, 0b00100, 0b01110, 0b10101, 0b00100}, // ne
    {0b00010, 0b00010, 0b00010, 0b00100, 0b00100, 0b01000, 0b10000}, // no
    {0b00000, 0b01010, 0b01001, 0b01001, 0b10001, 0b10001, 0b00000}, // ha
    {0b10000, 0b10000, 0b11111, 0b10000, 0b10000, 0b10000, 0b01111}, // hi
    {0b11111, 0b00001, 0b00001, 0b00001, 0b00010, 0b00100, 0b01000}, // fu
    {0b00000, 0b01000, 0b10100, 0b00010, 0b00001, 0b00000, 0b00000}, // he
    {0b00100, 0b11111, 0b00100, 0b10101, 0b10101, 0b00100, 0b00100}, // ho
    {0b11111, 0b00001, 0b00001, 0b01010, 0b00100, 0b00010, 0b00000}, // ma
    {0b01110, 0b00001, 0b01110, 0b00001, 0b11110, 0b00001, 0b00000}, // mi
    {0b00100, 0b00100, 0b01000, 0b01000, 0b10010, 0b11111, 0b00001}, // mu
    {0b00001, 0b00001, 0b01010, 0b00100, 0b01010, 0b10000, 0b00000}, // me
    {0b01110, 0b00100, 0b11111, 0b00100, 0b00100, 0b00100, 0b00011}, // mo
    {0b01000, 0b11111, 0b01001, 0b01010, 0b01000, 0b01000, 0b01000}, // ya
    {0b00000, 0b01110, 0b00010, 0b00010, 0b00010, 0b11111, 0b00000}, // yu
    {0b11111, 0b00001, 0b11111, 0b00001, 0b00001, 0b11111, 0b00000}, // yo
    {0b01110, 0b00000, 0b11111, 0b00001, 0b00001, 0b00010, 0b01100}, // ra
    {0b10001, 0b10001, 0b10001, 0b00001, 0b00001, 0b00010, 0b00100}, // ri
    {0b11111, 0b10001, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000}, // wa
    {0b11111, 0b00001, 0b11111, 0b00001, 0b00010, 0b00100, 0b01000}, // wo
    {0b00000, 0b11001, 0b00001, 0b00001, 0b00010, 0b00100, 0b11000}, // n
    {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}, // 0
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, // 1
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}, // 2
    {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}, // 3
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}, // 4
    {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}, // 5
    {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}, // 6
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}, // 7
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}, // 8
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}, // 9
};
const int NUM_GLYPHS = sizeof(GLYPHS) / sizeof(GLYPHS[0]);

bool onScreen(int row) {
    return row >= 0 && row < MatrixState::ROWS;
}

// False for the cells in the corners that the round bezel hides completely - not worth drawing.
bool cellVisible(int col, int row) {
    int left = col * CELL_WIDTH;
    int top = row * CELL_HEIGHT;
    int dx = max(max(left - CENTRE, CENTRE - (left + CELL_WIDTH)), 0);
    int dy = max(max(top - CENTRE, CENTRE - (top + CELL_HEIGHT)), 0);
    return dx * dx + dy * dy <= CENTRE * CENTRE;
}
} // namespace

MatrixEffect::MatrixEffect(ScreenManager &manager) : m_manager(manager) {
}

void MatrixEffect::scheduleDrop(MatrixState &state, int col, unsigned long now, int maxDelayMs) {
    state.head[col] = -1;
    state.length[col] = random(MIN_LENGTH, MAX_LENGTH + 1);
    state.stepMs[col] = random(MIN_STEP_MS, MAX_STEP_MS + 1);
    state.nextStepMs[col] = now + random(0, maxDelayMs);
}

void MatrixEffect::seed(MatrixState &state, unsigned long now) {
    for (int col = 0; col < MatrixState::COLS; col++) {
        // The first drops start closer together than later ones, so the screen isn't left empty.
        scheduleDrop(state, col, now, MAX_DROP_DELAY_MS / 2);
    }
}

void MatrixEffect::drawGlyph(const MatrixState &state, int col, int row, uint32_t color) {
    if (!cellVisible(col, row)) {
        return;
    }
    const uint8_t *rows = GLYPHS[state.glyph[col][row]];
    int left = col * CELL_WIDTH + (CELL_WIDTH - GLYPH_COLS * GLYPH_SCALE) / 2;
    int top = row * CELL_HEIGHT + (CELL_HEIGHT - GLYPH_ROWS * GLYPH_SCALE) / 2;
    for (int y = 0; y < GLYPH_ROWS; y++) {
        // One fillRect per run of lit pixels rather than one per pixel.
        int x = 0;
        while (x < GLYPH_COLS) {
            if (!(rows[y] & (1 << x))) {
                x++;
                continue;
            }
            int runStart = x;
            while (x < GLYPH_COLS && (rows[y] & (1 << x))) {
                x++;
            }
            m_manager.fillRect(left + runStart * GLYPH_SCALE, top + y * GLYPH_SCALE, (x - runStart) * GLYPH_SCALE, GLYPH_SCALE, color);
        }
    }
}

void MatrixEffect::clearCell(int col, int row) {
    if (cellVisible(col, row)) {
        m_manager.fillRect(col * CELL_WIDTH, row * CELL_HEIGHT, CELL_WIDTH, CELL_HEIGHT, TFT_BLACK);
    }
}

// Moves a column's drop down one row. Only the cells whose look changes are touched: the new head,
// the old head (now tail), the two cells fading out at the tail's end, and the one that just left
// it. Each is a redraw of the same glyph in a new color, so nothing needs erasing first - a column
// only gets its next drop once the last one has cleared it completely.
void MatrixEffect::stepDrop(MatrixState &state, int col, uint32_t color, unsigned long now) {
    int head = ++state.head[col];
    int length = state.length[col];

    if (onScreen(head)) {
        state.glyph[col][head] = random(0, NUM_GLYPHS);
        drawGlyph(state, col, head, HEAD_COLOR);
    }
    if (onScreen(head - 1)) {
        drawGlyph(state, col, head - 1, color);
    }
    for (int i = 0; i < 2; i++) {
        int row = head - length + 2 - i;
        if (onScreen(row)) {
            drawGlyph(state, col, row, Utils::rgb565dim(color, TAIL_FADE[i]));
        }
    }
    int cleared = head - length;
    if (onScreen(cleared)) {
        clearCell(col, cleared);
    }

    if (cleared >= MatrixState::ROWS - 1) {
        scheduleDrop(state, col, now, MAX_DROP_DELAY_MS);
    } else {
        state.nextStepMs[col] = now + state.stepMs[col];
    }
}

void MatrixEffect::draw(MatrixState &state, uint32_t color, unsigned long now, bool fullRedraw) {
    if (fullRedraw) {
        seed(state, now);
        m_manager.fillScreen(TFT_BLACK);
        return;
    }

    for (int col = 0; col < MatrixState::COLS; col++) {
        if ((long) (now - state.nextStepMs[col]) >= 0) {
            stepDrop(state, col, color, now);
        }
    }

    if (random(0, MUTATE_ONE_IN) == 0) {
        int col = random(0, MatrixState::COLS);
        // Only between the head and the fading end, where a cell is plainly the rain's color.
        int first = max(state.head[col] - state.length[col] + 3, 0);
        int last = min(state.head[col] - 2, MatrixState::ROWS - 1);
        if (state.head[col] >= 0 && first <= last) {
            int row = random(first, last + 1);
            state.glyph[col][row] = random(0, NUM_GLYPHS);
            clearCell(col, row);
            drawGlyph(state, col, row, color);
        }
    }
}
