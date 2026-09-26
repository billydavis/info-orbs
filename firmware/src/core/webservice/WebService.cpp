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

    // POST /api/v1/buttons/{left|ok|right}?press={short|medium|long} - acts like a physical button press.
    // With ?redirect=1 the response is a redirect back to "/" (used by the home page's buttons).
    const struct {
        const char *name;
        uint8_t id;
    } buttons[] = {{"left", BUTTON_LEFT}, {"ok", BUTTON_OK}, {"right", BUTTON_RIGHT}};
    for (const auto &button : buttons) {
        String name = button.name;
        uint8_t id = button.id;
        m_server.on("/api/v1/buttons/" + name, HTTP_POST, [this, name, id]() { handleButton(name, id); });
    }

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
void WebService::addPage(Widget *widget, const String &path, std::function<String()> renderBody) {
    m_pages.push_back({widget, path});
    m_server.on(path, HTTP_GET, [this, widget, renderBody]() { sendPage(widget->getName(), renderBody()); });

    // WebServer matches paths exactly, so also accept "/orbit" for "/orbit/" by redirecting to it
    if (path.length() > 1 && path.endsWith("/")) {
        m_server.on(path.substring(0, path.length() - 1), HTTP_GET, [this, path]() {
            m_server.sendHeader("Location", path);
            m_server.send(301);
        });
    }
}

const WebService::WidgetPage *WebService::findPage(Widget *widget) {
    for (const auto &page : m_pages) {
        if (page.widget == widget) {
            return &page;
        }
    }
    return nullptr;
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

void WebService::sendPage(const String &title, const String &body, bool homeLink) {
    String html;
    html.reserve(body.length() + 1024);
    html += F("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>");
    html += htmlEscape(title);
    html += F("</title><style>"
              ":root{color-scheme:light dark}body{font-family:system-ui,sans-serif;max-width:40rem;margin:2rem auto;padding:0 1rem}"
              "table{border-collapse:collapse;width:100%;margin-bottom:1.5rem}th,td{text-align:left;padding:.35rem .5rem;border-bottom:1px solid #8884}"
              "th{width:40%;font-weight:600}code{font-size:.95em}"
              "</style></head><body>");
    if (homeLink) {
        html += F("<p><a href='/'>&larr; Home</a></p>");
    }
    html += "<h1>" + htmlEscape(title) + "</h1>";
    html += body;
    html += F("</body></html>");
    m_server.send(200, "text/html", html);
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

void WebService::handleButton(const String &name, uint8_t buttonId) {
    if (!m_buttonHandler) {
        m_server.send(503, "application/json", "{\"error\":\"buttons are not available\"}");
        return;
    }
    String press = m_server.hasArg("press") ? m_server.arg("press") : "short";
    ButtonState state;
    if (press == "short") {
        state = BTN_SHORT;
    } else if (press == "medium") {
        state = BTN_MEDIUM;
    } else if (press == "long") {
        state = BTN_LONG;
    } else {
        m_server.send(400, "application/json", "{\"error\":\"press must be short, medium or long\"}");
        return;
    }
    m_buttonHandler(buttonId, state);
    if (m_server.hasArg("redirect")) {
        m_server.sendHeader("Location", "/");
        m_server.send(303);
        return;
    }
    JsonDocument doc;
    doc["button"] = name;
    doc["press"] = press;
    if (m_widgetSet != nullptr && m_widgetSet->getCurrent() != nullptr) {
        doc["widget"] = m_widgetSet->getCurrent()->getName();
    }
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
    html.reserve(3072);
    auto row = [&html](const String &label, const String &value) {
        html += "<tr><th>";
        html += label;
        html += "</th><td>";
        html += value;
        html += "</td></tr>";
    };

    html += F("<h2>Device</h2><table>");
    row("Hostname", "<code>" + htmlEscape(info["hostname"].as<String>()) + "</code>");
    row("IP address", info["ip"].as<String>());
    row("MAC address", info["mac"].as<String>());
    row("WiFi", htmlEscape(info["ssid"].as<String>()) + " (" + info["rssi"].as<String>() + " dBm)");
    row("Uptime", uptime);
    row("Free heap", String(info["freeHeap"].as<uint32_t>() / 1024) + " KB (min " + String(info["minFreeHeap"].as<uint32_t>() / 1024) + " KB)");
    row("Firmware built", info["firmwareBuilt"].as<String>());
    html += F("</table><p><a href='/api/v1/system'><code>GET /api/v1/system</code></a> - the above as JSON</p>");

    if (m_widgetSet != nullptr) {
        html += F("<h2>Widgets</h2><table>");
        Widget *current = m_widgetSet->getCurrent();
        for (int8_t i = 0; i < m_widgetSet->getCount(); i++) {
            Widget *widget = m_widgetSet->get(i);
            if (widget == nullptr) {
                continue;
            }
            String name = htmlEscape(widget->getName());
            const WidgetPage *page = findPage(widget);
            if (page != nullptr) {
                name = "<a href='" + page->path + "'>" + name + "</a>";
            }
            row(name, widget == current ? "showing" : "");
        }
        html += F("</table>");
    }

    if (m_buttonHandler) {
        html += F("<h2>Buttons</h2><p>");
        const char *labels[][2] = {{"left", "&larr; Left"}, {"ok", "OK"}, {"right", "Right &rarr;"}};
        for (const auto &label : labels) {
            html += "<form method=post action='/api/v1/buttons/";
            html += label[0];
            html += "?redirect=1' style='display:inline'><button>";
            html += label[1];
            html += "</button></form> ";
        }
        html += F("</p><p>Short press. For medium/long: <code>POST /api/v1/buttons/{left|ok|right}?press=medium</code></p>");
    }

    sendPage("Info-Orbs", html, false);
}
#endif
