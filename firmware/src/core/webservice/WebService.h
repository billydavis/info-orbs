#ifndef WEBSERVICE_H
#define WEBSERVICE_H

#include "config_helper.h"
#include <Arduino.h>
#include <functional>
#include <vector>

#ifndef DISABLE_WEB_SERVER
    #include <ArduinoJson.h>
    #include <WebServer.h>
#endif

// Owns the device's network presence: hostname, mDNS (http://<hostname>.local) and, unless
// DISABLE_WEB_SERVER is defined, an HTTP server on port 80 with a device info page at "/".
//
// Widgets that want HTTP endpoints register them in setup() via server().on(...) and can add a
// block to the info page via addStatusSection(). Requests are handled synchronously from loop(),
// so handlers run on the main task and may touch widget state directly.
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

    // Appends HTML (typically an <h2> and a <table>) to the info page at "/"
    void addStatusSection(std::function<void(String &html)> section);

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
    void fillSystemInfo(JsonDocument &doc);
    void handleRoot();
    void handleSystem();

    WebServer m_server{80};
    std::vector<std::function<void(String &html)>> m_statusSections;
    std::vector<std::pair<String, String>> m_serviceTxt;
#endif
};

#endif // WEBSERVICE_H
