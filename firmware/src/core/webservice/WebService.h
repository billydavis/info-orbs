#ifndef WEBSERVICE_H
#define WEBSERVICE_H

#include "Button.h"
#include "Widget.h"
#include "WidgetSet.h"
#include "config_helper.h"
#include <Arduino.h>
#include <functional>
#include <vector>

#ifndef DISABLE_WEB_SERVER
    #include <ArduinoJson.h>
    #include <WebServer.h>
#endif

// Owns the device's network presence: hostname, mDNS (http://<hostname>.local) and, unless
// DISABLE_WEB_SERVER is defined, an HTTP server on port 80.
//
// The home page at "/" belongs to core: device info, the widget list and virtual buttons. Widgets
// don't add to it - a widget that wants a web UI registers its own page with addPage(), and the
// home page links to it from the widget list. Widgets can also register API routes directly via
// server().on(...). Requests are handled synchronously from loop(), so handlers run on the main
// task and may touch widget state directly.
//
// Trust boundary: the local network. There is no authentication and no TLS, so every route is
// reachable by any LAN client, including cross-site form POSTs from a browser on the LAN. Keep
// routes free of secrets (API keys, WiFi password) and of anything destructive.
class WebService {
public:
    static WebService *getInstance();

    // Hostname from MDNS_HOSTNAME, or "info-orbs-XX" (XX = last 2 hex digits of the MAC).
    // Valid once WiFi.mode() has been set, so the MAC is available.
    const String &getHostname();

    // Starts the HTTP server and mDNS. Call after WiFi is connected (in station mode), so the
    // server doesn't collide with WiFiManager's config portal on port 80. Safe to call repeatedly.
    // If mDNS fails to start, loop() retries it every MDNS_RETRY_INTERVAL ms.
    void begin();
    void loop();
    bool isStarted() { return m_started; }

#ifndef DISABLE_WEB_SERVER
    WebServer &server() { return m_server; }

    // Widgets listed on the home page
    void setWidgetSet(WidgetSet *widgetSet) { m_widgetSet = widgetSet; }

    // Enables POST /api/v1/buttons/{left|ok|right}; the handler performs the press as if it came
    // from the physical button
    void setButtonHandler(std::function<void(uint8_t buttonId, ButtonState state)> handler) { m_buttonHandler = handler; }

    // Registers a page for a widget at path (e.g. "/orbit/"), linked from the widget list on the
    // home page. renderBody returns the page's inner HTML; the page title, styling and a link
    // back home are added by sendPage(). A path ending in "/" is also reachable without it (redirect).
    // Can be called before begin().
    void addPage(Widget *widget, const String &path, std::function<String()> renderBody);

    // Sends a complete HTML page with the shared styling. Use for any custom HTML response.
    void sendPage(const String &title, const String &body, bool homeLink = true);

    // Adds a TXT record to the _http._tcp mDNS service (applied at begin() if not yet started)
    void addServiceTxt(const String &key, const String &value);

    static String htmlEscape(const String &in);
#endif

private:
    WebService() = default;

    static constexpr unsigned long MDNS_RETRY_INTERVAL = 30000;

    static WebService *m_instance;

    void startMdns();

    String m_hostname{""};
    bool m_started{false};
    bool m_mdnsStarted{false};
    unsigned long m_lastMdnsAttempt{0};

#ifndef DISABLE_WEB_SERVER
    struct WidgetPage {
        Widget *widget;
        String path;
    };

    void fillSystemInfo(JsonDocument &doc);
    void handleRoot();
    void handleSystem();
    void handleButton(const String &name, uint8_t buttonId);
    const WidgetPage *findPage(Widget *widget);

    WebServer m_server{80};
    WidgetSet *m_widgetSet{nullptr};
    std::function<void(uint8_t, ButtonState)> m_buttonHandler;
    std::vector<WidgetPage> m_pages;
    std::vector<std::pair<String, String>> m_serviceTxt;
#endif
};

#endif // WEBSERVICE_H
