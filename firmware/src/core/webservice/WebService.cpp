#include "WebService.h"
#include <ESPmDNS.h>
#include <WiFi.h>

WebService *WebService::m_instance = nullptr;

WebService *WebService::getInstance() {
    if (m_instance == nullptr) {
        m_instance = new WebService();
    }
    return m_instance;
}

const String &WebService::getHostname() {
    if (m_hostname.isEmpty()) {
#ifdef MDNS_HOSTNAME
        m_hostname = MDNS_HOSTNAME;
#else
        m_hostname = "info-orbs-" + WiFi.macAddress().substring(15);
        m_hostname.toLowerCase();
#endif
    }
    return m_hostname;
}

void WebService::begin() {
    if (m_started) {
        return;
    }
    m_started = true;

#ifndef DISABLE_WEB_SERVER
    m_server.on("/", HTTP_GET, [this]() { handleRoot(); });
    m_server.on("/api/v1/system", HTTP_GET, [this]() { handleSystem(); });
    m_server.onNotFound([this]() {
        String uri = m_server.uri();
        uri.replace("\\", "\\\\");
        uri.replace("\"", "\\\"");
        m_server.send(404, "application/json", "{\"error\":\"not found: " + uri + "\"}");
    });
    m_server.begin();
    Serial.println("Web server listening on port 80");
#endif

    startMdns();
}

// mDNS is independent of the web server (which stays reachable by IP), so a failure here is
// retried from loop() instead of leaving <hostname>.local dead until the next reboot.
void WebService::startMdns() {
    m_lastMdnsAttempt = millis();
    if (!MDNS.begin(getHostname().c_str())) {
        // begin() can fail after mdns_init() succeeded (e.g. setting the hostname), and a
        // second mdns_init() would then fail forever, so tear down before the next attempt
        MDNS.end();
        Serial.printf("mDNS responder failed to start, retrying in %lus\n", MDNS_RETRY_INTERVAL / 1000);
        return;
    }
    m_mdnsStarted = true;
    Serial.println("mDNS responder started: " + m_hostname + ".local");

#ifndef DISABLE_WEB_SERVER
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "path", "/");
    for (const auto &txt : m_serviceTxt) {
        MDNS.addServiceTxt("http", "tcp", txt.first.c_str(), txt.second.c_str());
    }
#endif
}

void WebService::loop() {
    if (!m_started) {
        return;
    }
    if (!m_mdnsStarted && millis() - m_lastMdnsAttempt >= MDNS_RETRY_INTERVAL) {
        startMdns();
    }
#ifndef DISABLE_WEB_SERVER
    m_server.handleClient();
#endif
}

#ifndef DISABLE_WEB_SERVER
void WebService::addStatusSection(std::function<void(String &html)> section) {
    m_statusSections.push_back(section);
}

void WebService::addServiceTxt(const String &key, const String &value) {
    m_serviceTxt.push_back({key, value});
    if (m_mdnsStarted) {
        MDNS.addServiceTxt("http", "tcp", key.c_str(), value.c_str());
    }
}

String WebService::htmlEscape(const String &in) {
    String out;
    out.reserve(in.length());
    for (char c : in) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

void WebService::fillSystemInfo(JsonDocument &doc) {
    doc["hostname"] = m_hostname + ".local";
    doc["ip"] = WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = WiFi.RSSI();
    doc["uptimeSeconds"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["minFreeHeap"] = ESP.getMinFreeHeap();
    doc["firmwareBuilt"] = String(__DATE__) + " " + __TIME__;
}

void WebService::handleSystem() {
    JsonDocument doc;
    fillSystemInfo(doc);
    String body;
    serializeJson(doc, body);
    m_server.send(200, "application/json", body);
}

void WebService::handleRoot() {
    JsonDocument info;
    fillSystemInfo(info);

    unsigned long secs = info["uptimeSeconds"];
    char uptime[32];
    snprintf(uptime, sizeof(uptime), "%lud %02luh %02lum %02lus", secs / 86400, (secs / 3600) % 24, (secs / 60) % 60, secs % 60);

    String html;
    html.reserve(2048);
    html += F("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
              "<title>Info-Orbs</title><style>"
              ":root{color-scheme:light dark}body{font-family:system-ui,sans-serif;max-width:40rem;margin:2rem auto;padding:0 1rem}"
              "table{border-collapse:collapse;width:100%;margin-bottom:1.5rem}th,td{text-align:left;padding:.35rem .5rem;border-bottom:1px solid #8884}"
              "th{width:40%;font-weight:600}code{font-size:.95em}"
              "</style></head><body><h1>Info-Orbs</h1><h2>Device</h2><table>");

    auto row = [&html](const char *label, const String &value) {
        html += "<tr><th>";
        html += label;
        html += "</th><td>";
        html += value;
        html += "</td></tr>";
    };
    row("Hostname", "<code>" + htmlEscape(info["hostname"].as<String>()) + "</code>");
    row("IP address", info["ip"].as<String>());
    row("MAC address", info["mac"].as<String>());
    row("WiFi", htmlEscape(info["ssid"].as<String>()) + " (" + info["rssi"].as<String>() + " dBm)");
    row("Uptime", uptime);
    row("Free heap", String(info["freeHeap"].as<uint32_t>() / 1024) + " KB (min " + String(info["minFreeHeap"].as<uint32_t>() / 1024) + " KB)");
    row("Firmware built", info["firmwareBuilt"].as<String>());
    html += F("</table><p><a href='/api/v1/system'><code>GET /api/v1/system</code></a> - the above as JSON</p>");

    for (const auto &section : m_statusSections) {
        section(html);
    }

    html += F("</body></html>");
    m_server.send(200, "text/html", html);
}
#endif
