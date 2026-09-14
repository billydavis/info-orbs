#include "MakapixWidget.h"

#include "Utils.h"
#include "config_helper.h"
#include <WiFi.h>
#include <new>
#include "src/webp/demux.h"

MakapixWidget::MakapixWidget(ScreenManager &manager, const String &playerKey, const String &apiToken,
                              const String &baseUrl, const String &channel, uint32_t cycleMinutes)
    : Widget(manager), m_playerKey(playerKey), m_apiToken(apiToken), m_baseUrl(baseUrl), m_channel(channel) {
    m_cycleDelayMs = (unsigned long)cycleMinutes * 60UL * 1000UL;
}

MakapixWidget::~MakapixWidget() {
    freeImageBuffer();
    delete m_png;
}

void MakapixWidget::setup() {
    // No network calls here - keep setup() fast, matching other widgets. update() drives the fetch.
    m_state = State::NEEDS_BATCH;
}

void MakapixWidget::buttonPressed(uint8_t buttonId, ButtonState state) {
    if (buttonId != BUTTON_OK) {
        return;
    }
    if (state == BTN_MEDIUM) {
        // Force a fresh batch of images now, bypassing the cycle timer.
        Serial.println("Makapix: manual batch refresh requested");
        freeImageBuffer();
        m_hasPendingImage = false;
        m_retryCount = 0;
        m_state = State::NEEDS_BATCH;
    }
}

String MakapixWidget::getName() {
    return "Makapix";
}

unsigned long MakapixWidget::backoffDelayFor(uint8_t retryCount) const {
    uint8_t step = retryCount < MAX_RETRY_BACKOFF_STEPS ? retryCount : MAX_RETRY_BACKOFF_STEPS;
    return 5000UL << step; // 5s, 10s, 20s, ... capped
}

void MakapixWidget::logHeap(const char *tag) {
    Serial.printf("Makapix: heap[%s] free=%u largestBlock=%u\n", tag, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

void MakapixWidget::freeImageBuffer() {
    if (m_imageBuffer != nullptr) {
        free(m_imageBuffer);
        m_imageBuffer = nullptr;
    }
    m_imageLen = 0;
}

void MakapixWidget::update(bool force) {
    unsigned long now = millis();

    switch (m_state) {
    case State::NEEDS_BATCH: {
        if (WiFi.status() != WL_CONNECTED) {
            m_lastAttemptTime = now;
            m_state = State::ERROR_BACKOFF;
            return;
        }
        setBusy(true);
        logHeap("before fetchBatch");
        bool ok = fetchBatch();
        logHeap("after fetchBatch");
        setBusy(false);
        if (ok && m_postCount > 0) {
            m_currentIndex = 0;
            m_retryCount = 0;
            m_state = State::DOWNLOADING_IMAGE;
            m_needsLoadingDisplay = true;
        } else {
            m_retryCount++;
            m_lastAttemptTime = now;
            m_state = State::ERROR_BACKOFF;
        }
        break;
    }

    case State::DOWNLOADING_IMAGE: {
        if (m_currentIndex >= m_postCount) {
            m_state = State::READY;
            m_lastBatchTime = now;
            break;
        }
        setBusy(true);
        Serial.printf("Makapix: downloading image %d/%d\n", m_currentIndex + 1, m_postCount);
        logHeap("before downloadImage");
        bool ok = downloadImage(m_posts[m_currentIndex]);
        logHeap("after downloadImage");
        setBusy(false);
        if (ok) {
            m_pendingDrawIndex = m_currentIndex;
            m_hasPendingImage = true;
        } else {
            Serial.printf("Makapix: failed to download image %d, skipping\n", m_currentIndex);
            m_pendingFailedIndex = m_currentIndex;
        }
        m_currentIndex++;
        if (m_currentIndex >= m_postCount) {
            m_state = State::READY;
            m_lastBatchTime = now;
        }
        break;
    }

    case State::READY:
        if (force || (now - m_lastBatchTime) >= m_cycleDelayMs) {
            m_state = State::NEEDS_BATCH;
        }
        break;

    case State::ERROR_BACKOFF:
        if ((now - m_lastAttemptTime) >= backoffDelayFor(m_retryCount)) {
            m_state = State::NEEDS_BATCH;
        }
        break;
    }
}

void MakapixWidget::draw(bool force) {
    if (m_needsLoadingDisplay) {
        for (uint8_t i = 0; i < m_postCount; i++) {
            showScreenMessage(i, "Makapix", "loading...");
        }
        m_needsLoadingDisplay = false;
    }

    if (m_pendingFailedIndex >= 0) {
        showScreenMessage((uint8_t)m_pendingFailedIndex, "Makapix", "no image");
        m_pendingFailedIndex = -1;
    }

    if (m_hasPendingImage) {
        drawPendingImage();
    }

    if (force && m_state == State::READY) {
        // Screens were cleared (e.g. widget switch) - redraw the current batch without a new fetch.
        m_currentIndex = 0;
        m_state = State::DOWNLOADING_IMAGE;
        m_needsLoadingDisplay = true;
    } else if (force && m_state == State::ERROR_BACKOFF) {
        showMessage("Makapix", "connecting...");
    } else if (force && m_postCount == 0) {
        showMessage("Makapix", "loading...");
    }
}

void MakapixWidget::drawPendingImage() {
    logHeap("before decode");
    bool ok = (m_posts[m_pendingDrawIndex].nativeFormat == "png") ? drawPendingImagePng() : drawPendingImageWebp();
    logHeap("after decode");
    if (!ok) {
        m_pendingFailedIndex = m_pendingDrawIndex;
    }
}

bool MakapixWidget::drawPendingImageWebp() {
    WebPData webpData;
    webpData.bytes = m_imageBuffer;
    webpData.size = m_imageLen;

    // m_imageBuffer only holds a bounded prefix of the file (see downloadImage), not necessarily the
    // whole thing, so this must tolerate an incomplete buffer.
    WebPDemuxState demuxState;
    WebPDemuxer *demux = WebPDemuxPartial(&webpData, &demuxState);
    if (demux == nullptr) {
        Serial.printf("Makapix: WebPDemuxPartial failed, state %d\n", demuxState);
        freeImageBuffer();
        m_hasPendingImage = false;
        return false;
    }

    WebPIterator iter;
    if (!WebPDemuxGetFrame(demux, 1, &iter)) {
        Serial.println("Makapix: WebPDemuxGetFrame(1) failed");
        WebPDemuxDelete(demux);
        freeImageBuffer();
        m_hasPendingImage = false;
        return false;
    }

    if (!iter.complete) {
        // First frame's data extends past our downloaded prefix - skip rather than decode a truncated
        // frame. Should be rare: MAKAPIX_IMAGE_PREFIX_BYTES comfortably covers a single small frame.
        Serial.printf("Makapix: first frame (%dx%d) not fully within the %d-byte prefix, skipping\n",
                      iter.width, iter.height, MAKAPIX_IMAGE_PREFIX_BYTES);
        WebPDemuxReleaseIterator(&iter);
        WebPDemuxDelete(demux);
        freeImageBuffer();
        m_hasPendingImage = false;
        return false;
    }

    if (iter.width <= 0 || iter.height <= 0) {
        Serial.println("Makapix: first frame has invalid dimensions");
        WebPDemuxReleaseIterator(&iter);
        WebPDemuxDelete(demux);
        freeImageBuffer();
        m_hasPendingImage = false;
        return false;
    }

    // No decode-time scaling: fetchBatch() already rejects posts whose source exceeds
    // MAKAPIX_DECODE_MAX_SOURCE_DIM, and libwebp's lossless decoder needs memory proportional to the
    // *source* resolution regardless of any requested output scaling, so scaling wouldn't reduce peak
    // memory here anyway - decode straight at native size.
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) {
        Serial.println("Makapix: WebPInitDecoderConfig failed");
        WebPDemuxReleaseIterator(&iter);
        WebPDemuxDelete(demux);
        freeImageBuffer();
        m_hasPendingImage = false;
        return false;
    }
    config.output.colorspace = MODE_RGB_565;

    Serial.printf("Makapix: decoding WebP frame %dx%d\n", iter.width, iter.height);
    VP8StatusCode status = WebPDecode(iter.fragment.bytes, iter.fragment.size, &config);
    logHeap("after WebPDecode (peak)");

    // The demuxer (and the fragment it points into m_imageBuffer) are no longer needed once decoded.
    WebPDemuxReleaseIterator(&iter);
    WebPDemuxDelete(demux);

    if (status != VP8_STATUS_OK) {
        Serial.printf("Makapix: WebPDecode failed, status %d\n", status);
        freeImageBuffer();
        m_hasPendingImage = false;
        return false;
    }

    int w = config.output.width;
    int h = config.output.height;
    int stride = config.output.u.RGBA.stride;
    uint16_t *rgb565 = (uint16_t *)config.output.u.RGBA.rgba;

    int16_t offsetX = w < SCREEN_SIZE ? (SCREEN_SIZE - w) / 2 : -(w - SCREEN_SIZE) / 2;
    int16_t offsetY = h < SCREEN_SIZE ? (SCREEN_SIZE - h) / 2 : -(h - SCREEN_SIZE) / 2;

    m_manager.selectScreen(m_pendingDrawIndex);
    m_manager.fillScreen(TFT_BLACK); // wipe any leftover "loading..."/"no image" text before pushing
    if (stride == w * (int)sizeof(uint16_t)) {
        m_manager.pushImage(offsetX, offsetY, w, h, rgb565);
    } else {
        // Row stride has padding - push one row at a time rather than assuming a tightly packed buffer.
        for (int row = 0; row < h; row++) {
            uint16_t *rowPtr = (uint16_t *)(config.output.u.RGBA.rgba + row * stride);
            m_manager.pushImage(offsetX, offsetY + row, w, 1, rowPtr);
        }
    }

    WebPFreeDecBuffer(&config.output);
    freeImageBuffer();
    m_hasPendingImage = false;
    return true;
}

int MakapixWidget::pngDrawCallback(PNGDRAW *pDraw) {
    MakapixWidget *self = (MakapixWidget *)pDraw->pUser;
    uint16_t lineBuf[SCREEN_SIZE];
    // BIG_ENDIAN matches the byte order this codebase's other image paths (TJpg_Decoder, and our own
    // WebP conversion) already rely on, given TJpgDec.setSwapBytes(true) is configured globally.
    self->m_png->getLineAsRGB565(pDraw, lineBuf, PNG_RGB565_BIG_ENDIAN, 0 /* black background under transparency */);

    int w = pDraw->iWidth;
    int h = self->m_png->getHeight();
    int16_t offsetX = w < SCREEN_SIZE ? (SCREEN_SIZE - w) / 2 : -(w - SCREEN_SIZE) / 2;
    int16_t offsetY = h < SCREEN_SIZE ? (SCREEN_SIZE - h) / 2 : -(h - SCREEN_SIZE) / 2;

    self->m_manager.pushImage(offsetX, offsetY + pDraw->y, w, 1, lineBuf);
    return 1; // continue decoding
}

bool MakapixWidget::drawPendingImagePng() {
    // Allocated on demand (see m_png's declaration for why) and freed before returning, every path.
    m_png = new (std::nothrow) PNG();
    if (m_png == nullptr) {
        Serial.println("Makapix: out of memory allocating PNG decoder");
        freeImageBuffer();
        m_hasPendingImage = false;
        return false;
    }

    int rc = m_png->openRAM(m_imageBuffer, m_imageLen, pngDrawCallback);
    if (rc != PNG_SUCCESS) {
        Serial.printf("Makapix: PNG openRAM failed, rc=%d\n", rc);
        delete m_png;
        m_png = nullptr;
        freeImageBuffer();
        m_hasPendingImage = false;
        return false;
    }

    Serial.printf("Makapix: decoding PNG frame %dx%d\n", m_png->getWidth(), m_png->getHeight());

    m_manager.selectScreen(m_pendingDrawIndex);
    m_manager.fillScreen(TFT_BLACK); // wipe any leftover "loading..."/"no image" text before pushing

    rc = m_png->decode(this, 0);
    m_png->close();
    logHeap("after PNG decode (peak)");

    delete m_png;
    m_png = nullptr;
    freeImageBuffer();
    m_hasPendingImage = false;

    if (rc != PNG_SUCCESS) {
        Serial.printf("Makapix: PNG decode failed, rc=%d\n", rc);
        return false;
    }
    return true;
}

void MakapixWidget::showMessage(const String &line1, const String &line2) {
    for (uint8_t i = 0; i < MAKAPIX_BATCH_SIZE; i++) {
        showScreenMessage(i, line1, line2);
    }
}

void MakapixWidget::showScreenMessage(uint8_t screenIndex, const String &line1, const String &line2) {
    m_manager.selectScreen(screenIndex);
    m_manager.fillScreen(TFT_BLACK);
    m_manager.setFontColor(TFT_WHITE, TFT_BLACK);
    m_manager.drawCentreString(line1, ScreenCenterX, ScreenCenterY - 12, 18);
    if (line2.length() > 0) {
        m_manager.drawCentreString(line2, ScreenCenterX, ScreenCenterY + 12, 14);
    }
}

bool MakapixWidget::fetchBatch() {
    JsonDocument reqDoc;
    reqDoc["request_type"] = "query_posts";
    reqDoc["channel"] = m_channel;
    reqDoc["limit"] = MAKAPIX_QUERY_LIMIT;
    JsonArray fields = reqDoc["include_fields"].to<JsonArray>();
    fields.add("owner_handle");
    fields.add("width");
    fields.add("height");
    fields.add("frame_count");
    String body;
    serializeJson(reqDoc, body);

    HTTPClient http;
    http.setConnectTimeout(MAKAPIX_HTTP_TIMEOUT_MS);
    http.setTimeout(MAKAPIX_HTTP_TIMEOUT_MS);
    http.begin(m_baseUrl + "/player/rpc");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + m_apiToken);

    int httpCode = http.POST(body);

    if (httpCode == 401) {
        http.end();
        if (!rotateToken()) {
            return false;
        }
        http.begin(m_baseUrl + "/player/rpc");
        http.setConnectTimeout(MAKAPIX_HTTP_TIMEOUT_MS);
        http.setTimeout(MAKAPIX_HTTP_TIMEOUT_MS);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", "Bearer " + m_apiToken);
        httpCode = http.POST(body);
    }

    if (httpCode != 200) {
        Serial.printf("Makapix query_posts failed, HTTP %d\n", httpCode);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.println("Makapix: deserializeJson() failed for query_posts response");
        return false;
    }

    if (!doc["success"].as<bool>()) {
        Serial.println("Makapix: query_posts response reported failure");
        return false;
    }

    m_postCount = 0;
    JsonArray posts = doc["posts"].as<JsonArray>();
    for (JsonObject post : posts) {
        if (m_postCount >= MAKAPIX_BATCH_SIZE) {
            break;
        }
        String kind = post["kind"].as<String>();
        if (kind != "artwork") {
            // Non-image posts (e.g. playlists) have no art_url.
            continue;
        }
        String artUrl = post["art_url"].as<String>();
        if (artUrl.length() == 0) {
            continue;
        }
        // native_format is mandatory on every artwork post per the player API spec, and is not limited
        // to WebP (the spec's own example post is a PNG) - only attempt formats our decoder supports.
        String nativeFormat = post["native_format"].as<String>();
        nativeFormat.toLowerCase();
        if (nativeFormat != "webp" && nativeFormat != "png") {
            Serial.printf("Makapix: skipping post with unsupported native_format \"%s\"\n", nativeFormat.c_str());
            continue;
        }
        int width = post["width"] | 0;
        int height = post["height"] | 0;
        if (nativeFormat == "webp" && (width > MAKAPIX_DECODE_MAX_SOURCE_DIM || height > MAKAPIX_DECODE_MAX_SOURCE_DIM)) {
            // libwebp's lossless decoder needs working memory proportional to the source resolution
            // regardless of any output scaling requested (see MAKAPIX_DECODE_MAX_SOURCE_DIM) - skip
            // rather than risk an out-of-memory decode failure on this no-PSRAM device. PNGdec decodes a
            // scanline at a time so this constraint doesn't apply to PNG posts.
            Serial.printf("Makapix: skipping post %dx%d, exceeds decode source size cap\n", width, height);
            continue;
        }
        MakapixPost &out = m_posts[m_postCount];
        out.artUrl = artUrl;
        out.nativeFormat = nativeFormat;
        out.width = width;
        out.height = height;
        out.frameCount = post["frame_count"] | 1;
        out.valid = true;
        m_postCount++;
    }

    return true;
}

bool MakapixWidget::rotateToken() {
    HTTPClient http;
    http.setConnectTimeout(MAKAPIX_HTTP_TIMEOUT_MS);
    http.setTimeout(MAKAPIX_HTTP_TIMEOUT_MS);
    http.begin(m_baseUrl + "/player/" + m_playerKey + "/token/rotate");
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST("{}");

    if (httpCode != 200) {
        Serial.printf("Makapix: token rotate failed, HTTP %d\n", httpCode);
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getString());
    http.end();
    if (error || !doc["api_token"].is<const char *>()) {
        Serial.println("Makapix: token rotate response parse failed");
        return false;
    }

    m_apiToken = doc["api_token"].as<String>();
    Serial.println("Makapix: api_token rotated (in-memory only, not persisted across reboot)");
    return true;
}

String MakapixWidget::resolveArtUrl(const String &artUrl) const {
    if (artUrl.startsWith("http")) {
        return artUrl;
    }
    // art_url is root-relative (e.g. "/api/vault/..."), resolve against the API host root.
    int schemeEnd = m_baseUrl.indexOf("://");
    if (schemeEnd < 0) {
        return m_baseUrl + artUrl;
    }
    int hostStart = schemeEnd + 3;
    int pathStart = m_baseUrl.indexOf('/', hostStart);
    String hostRoot = pathStart < 0 ? m_baseUrl : m_baseUrl.substring(0, pathStart);
    return hostRoot + artUrl;
}

bool MakapixWidget::downloadImage(const MakapixPost &post) {
    freeImageBuffer();
    String url = resolveArtUrl(post.artUrl);
    bool isPng = post.nativeFormat == "png";

    HTTPClient http;
    http.setConnectTimeout(MAKAPIX_HTTP_TIMEOUT_MS);
    http.setTimeout(MAKAPIX_HTTP_TIMEOUT_MS);
    http.begin(url);
    if (!isPng) {
        // WebP: only the first animation frame is needed (see drawPendingImageWebp), so fetch a bounded
        // prefix instead of the whole (possibly much larger, many-frame) file.
        http.addHeader("Range", "bytes=0-" + String(MAKAPIX_IMAGE_PREFIX_BYTES - 1));
    }
    int httpCode = http.GET();
    if (httpCode != 200 && httpCode != 206) {
        Serial.printf("Makapix: image download failed, HTTP %d\n", httpCode);
        http.end();
        return false;
    }

    int len = http.getSize();
    if (len <= 0) {
        Serial.printf("Makapix: image size %d invalid\n", len);
        http.end();
        return false;
    }
    int prefixCap = isPng ? MAKAPIX_PNG_MAX_BYTES : MAKAPIX_IMAGE_PREFIX_BYTES;
    if (len > prefixCap) {
        if (isPng) {
            // Unlike WebP's frame-based demux, a truncated PNG can't be decoded at all - reject rather
            // than download a partial file we can't use.
            Serial.printf("Makapix: PNG size %d exceeds %d byte cap, skipping\n", len, prefixCap);
            http.end();
            return false;
        }
        // Defensive: if the CDN ignored the Range header and sent the whole (possibly large) file,
        // still only read/keep our bounded prefix rather than the full advertised length.
        len = prefixCap;
    }

    logHeap("after TLS connect, before image malloc");
    m_imageBuffer = (uint8_t *)malloc(len);
    if (m_imageBuffer == nullptr) {
        Serial.println("Makapix: out of memory allocating image buffer");
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    size_t got = 0;
    unsigned long start = millis();
    while (got < (size_t)len && (millis() - start) < MAKAPIX_HTTP_TIMEOUT_MS) {
        if (stream->available()) {
            int n = stream->readBytes(m_imageBuffer + got, len - got);
            got += n;
        }
    }
    http.end();

    if (got != (size_t)len) {
        Serial.println("Makapix: incomplete image download");
        freeImageBuffer();
        return false;
    }
    m_imageLen = len;
    return true;
}
