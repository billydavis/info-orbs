#ifndef MAKAPIX_WIDGET_H
#define MAKAPIX_WIDGET_H

#include "Widget.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PNGdec.h>

#ifndef MAKAPIX_BASE_URL
    #define MAKAPIX_BASE_URL "https://makapix.club/api"
#endif
#ifndef MAKAPIX_CHANNEL
    #define MAKAPIX_CHANNEL "all"
#endif
#ifndef MAKAPIX_CYCLE_MINUTES
    #define MAKAPIX_CYCLE_MINUTES 5
#endif

#define MAKAPIX_BATCH_SIZE 5 // one image per physical orb screen
// The "all" channel skews toward canvases larger than MAKAPIX_DECODE_MAX_SOURCE_DIM (see below), which
// get filtered out before download. Asking for more candidate posts than we need, and keeping only the
// first MAKAPIX_BATCH_SIZE that pass the size filter, means a batch is far more likely to actually fill
// all 5 screens instead of leaving most of them blank. 20 is comfortably within the API's 1-50 limit.
#define MAKAPIX_QUERY_LIMIT 20
// We only need the *first* animation frame, but Makapix serves the whole (possibly many-frame, 100KB+)
// WebP file. Fetching the whole thing is wasteful and was overflowing heap on a no-PSRAM ESP32, so we
// fetch a bounded prefix via an HTTP Range request instead and demux the first frame out of that (the
// CDN confirmed Accept-Ranges: bytes support). A frame's own compressed data is typically only a few KB
// for these small pixel-art images, so this comfortably covers it even when the full file is much larger.
#define MAKAPIX_IMAGE_PREFIX_BYTES (20 * 1024)
#define MAKAPIX_HTTP_TIMEOUT_MS 8000

// Makapix posts are served as either animated WebP or static PNG (native_format on the post tells us
// which - see fetchBatch()). WebP: v1 decodes only the first frame, via libwebp's demux + advanced
// decode API (vendored in firmware/lib/webp - decode+demux only, no encoder/mux), decoding straight to
// RGB565. PNG: decoded a scanline at a time via PNGdec's callback API (same streaming pattern already
// used for JPEG elsewhere in this codebase) and pushed to the screen row-by-row - no full-frame buffer
// needed at all.
//
// libwebp's lossless (VP8L) decoder - what these small flat-color pixel-art WebPs are almost always
// encoded as - needs internal working memory proportional to the *source* resolution, not the requested
// output resolution: asking it to decode-scale a 256x256 source down to 128x128 still needs enough
// contiguous heap to decode the full 256x256 source internally first. So unlike a typical JPEG/PNG
// decoder, requesting a smaller output does not meaningfully shrink peak memory here - only a smaller
// *source* does. We therefore reject oversized WebP posts by their metadata dimensions before ever
// downloading them, rather than trying to decode-scale them down. PNGdec's scanline decoding doesn't
// have this problem, so this cap only applies to WebP posts.
#define MAKAPIX_DECODE_MAX_SOURCE_DIM 128
// PNG can't be usefully partial-downloaded the way WebP's first frame can (see MAKAPIX_IMAGE_PREFIX_BYTES
// below) - zlib-compressed IDAT data must be read as a whole to decode any of it. So PNG posts are
// downloaded in full, up to this size cap.
#define MAKAPIX_PNG_MAX_BYTES (40 * 1024)

class MakapixWidget : public Widget {
public:
    MakapixWidget(ScreenManager &manager, const String &playerKey, const String &apiToken,
                  const String &baseUrl = MAKAPIX_BASE_URL, const String &channel = MAKAPIX_CHANNEL,
                  uint32_t cycleMinutes = MAKAPIX_CYCLE_MINUTES);
    ~MakapixWidget() override;

    void setup() override;
    void update(bool force = false) override;
    void draw(bool force = false) override;
    void buttonPressed(uint8_t buttonId, ButtonState state) override;
    String getName() override;

private:
    enum class State {
        NEEDS_BATCH, // must query_posts for a fresh set of MAKAPIX_BATCH_SIZE images
        DOWNLOADING_IMAGE, // downloading/drawing image at m_currentIndex
        READY, // all screens populated, waiting for the cycle timer
        ERROR_BACKOFF // last operation failed, waiting before retry
    };

    struct MakapixPost {
        String artUrl;
        String nativeFormat; // "webp" or "png"
        int width = 0;
        int height = 0;
        int frameCount = 1;
        bool valid = false;
    };

    // --- API calls ---
    bool fetchBatch();
    bool rotateToken();
    bool downloadImage(const MakapixPost &post);
    String resolveArtUrl(const String &artUrl) const;

    // --- Display ---
    void drawPendingImage(); // decodes m_imageBuffer (format per m_posts[m_pendingDrawIndex]) and draws it
    bool drawPendingImageWebp();
    bool drawPendingImagePng();
    static int pngDrawCallback(PNGDRAW *pDraw);
    void showMessage(const String &line1, const String &line2 = "");
    void showScreenMessage(uint8_t screenIndex, const String &line1, const String &line2 = "");

    // --- State/timers ---
    void freeImageBuffer();
    unsigned long backoffDelayFor(uint8_t retryCount) const;
    static void logHeap(const char *tag);

    String m_playerKey;
    String m_apiToken;
    String m_baseUrl;
    String m_channel;
    unsigned long m_cycleDelayMs;

    State m_state = State::NEEDS_BATCH;

    MakapixPost m_posts[MAKAPIX_BATCH_SIZE];
    uint8_t m_postCount = 0;
    uint8_t m_currentIndex = 0;

    uint8_t *m_imageBuffer = nullptr; // compressed WebP bytes, downloaded but not yet decoded
    size_t m_imageLen = 0;
    bool m_hasPendingImage = false;
    uint8_t m_pendingDrawIndex = 0;

    bool m_needsLoadingDisplay = false; // set once per new batch, consumed by draw() to show "loading..."
    int8_t m_pendingFailedIndex = -1; // screen that just failed to populate; -1 = none pending

    // Allocated on the heap only for the duration of a PNG decode, not held as a plain member: PNGdec's
    // PNGIMAGE struct embeds a fixed ~39KB buffer (zlib window + palette + line buffers) internally to
    // avoid its own malloc/free calls, so holding one permanently would reserve that ~39KB for the whole
    // session even when no PNG post is ever encountered.
    PNG *m_png = nullptr;

    unsigned long m_lastBatchTime = 0;
    unsigned long m_lastAttemptTime = 0;
    uint8_t m_retryCount = 0;
    static const uint8_t MAX_RETRY_BACKOFF_STEPS = 5;
};

#endif // MAKAPIX_WIDGET_H
