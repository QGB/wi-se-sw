//
// Created by depau on 1/26/21.
//

#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>

#include "config.h"
#include "html.h"
#include "qgb_server.h"
#include "debug.h"

String toString(const IPAddress &address) {
    return String() + address[0] + "." + address[1] + "." + address[2] + "." + address[3];
}

void WiSeServer::begin() {
    DefaultHeaders::Instance().addHeader("Server", serverHeader);
    DefaultHeaders::Instance().addHeader("X-Ttyd-Implementation", "Wi-Se/C++");
#ifdef CORS_ALLOW_ORIGIN
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", CORS_ALLOW_ORIGIN);
#endif

    // Handle regular HTTP requests
    httpd->on("/", HTTP_GET, handleIndex);
    httpd->on("/index.html", HTTP_GET, handleIndex);
    httpd->on("/token", HTTP_GET,
              std::bind(&WiSeServer::handleToken, this, std::placeholders::_1));
    httpd->on("/stty", HTTP_GET | HTTP_POST,
              std::bind(&WiSeServer::handleSttyRequest, this, std::placeholders::_1),
              nullptr,
              std::bind(&WiSeServer::handleSttyBody, this, std::placeholders::_1, std::placeholders::_2,
                        std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
    httpd->on("/stats", HTTP_GET, std::bind(&WiSeServer::handleStatsRequest, this, std::placeholders::_1));
    httpd->on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!checkHttpBasicAuth(request)) return;
        request->send(200, "text/plain", String(ESP.getFreeHeap()));
    });
    httpd->on("/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!checkHttpBasicAuth(request)) return;
        request->send(200, "text/plain", "");
        ESP.reset();
    });
    httpd->on("/whoami", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!checkHttpBasicAuth(request)) return;
        AsyncResponseStream *response = request->beginResponseStream("application/json");

        DynamicJsonDocument doc(2000);
        doc["board"] = BOARD_NAME;
        doc["pretty_name"] = DEVICE_PRETTY_NAME;
        doc["hostname"] = WIFI_HOSTNAME;

        JsonObject software = doc.createNestedObject("software");
        software["implementation"] = F("Wi-Se C++");
        software["version"] = VERSION;

        JsonObject soc = doc.createNestedObject("soc");
#ifdef ESP8266
        soc["type"] = F("ESP8266");
#else
        soc["type"] = F("ESP32");
#endif
        soc["chipId"] = ESP.getChipId();
        soc["sdk"] = ESP.getFullVersion();
        soc["mhz"] = ESP.getCpuFreqMHz();

        JsonObject health = doc.createNestedObject("health");
        health["vccVoltage"] = ESP.getVcc() / 1000.0;
        health["heapFree"] = ESP.getFreeHeap();
        health["heapFrag"] = ESP.getHeapFragmentation();

        JsonObject net = doc.createNestedObject("net");
        net["wifiMode"] = WIFI_MODE == WIFI_STA ? "sta" : "softap";
        net["ssid"] = WiFi.SSID();
        net["bssid"] = WiFi.BSSIDstr();
        net["macAddr"] = WiFi.macAddress();
        net["ip"] = toString(WiFi.localIP());
        net["netmask"] = toString(WiFi.subnetMask());
        net["gateway"] = toString(WiFi.gatewayIP());
        net["rssi"] = WiFi.RSSI();

        serializeJson(doc, *response);
        request->send(response);
    });


    // Handle CORS preflight
    httpd->onNotFound([](AsyncWebServerRequest *request) {
        if (request->method() == HTTP_OPTIONS) {
            request->send(200);
        } else {
            request->send(404);
        }
    });

    // Handle WebSocket connections
    websocket->onEvent(std::bind(&WiSeServer::onWebSocketEvent, this, std::placeholders::_1, std::placeholders::_2,
                                 std::placeholders::_3, std::placeholders::_4, std::placeholders::_5,
                                 std::placeholders::_6));
    httpd->addHandler(websocket);
    debugf("Web app server is up\r\n");
}


void WiSeServer::end() const {
    websocket->enable(false);
    websocket->closeAll();
}

bool WiSeServer::checkHttpBasicAuth(AsyncWebServerRequest *request) {
    if (!HTTP_AUTH_ENABLE) {
        return true;
    }
    if (!request->authenticate(HTTP_AUTH_USER, HTTP_AUTH_PASS)) {
        request->requestAuthentication(DEVICE_PRETTY_NAME);
        return false;
    }
    return true;
}

void WiSeServer::handleIndex(AsyncWebServerRequest *request) {
    if (!checkHttpBasicAuth(request)) return;
    debugf("GET /\r\n");
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html, index_html_len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}


void WiSeServer::handleToken(AsyncWebServerRequest *request) const {
    if (!checkHttpBasicAuth(request)) return;
    debugf("GET /token\r\n");
    char response[100] = R"({"token": ")";
    strcat(response, this->token);
    strcat(response, "\"}");
    request->send(200, "application/json;charset=utf-8", response);
}


void WiSeServer::sttySendResponse(AsyncWebServerRequest *request) const {
    AsyncResponseStream *response = request->beginResponseStream("application/json");

    DynamicJsonDocument doc(200);

    doc["baudrate"] = ttyd->getUartBaudRate();

    uint8_t uartConfig = ttyd->getUartConfig();
    switch (uartConfig & MASK_UART_BITS) {
        case UART_NB_BIT_5:
            doc["bits"] = 5;
            break;
        case UART_NB_BIT_6:
            doc["bits"] = 6;
            break;
        case UART_NB_BIT_7:
            doc["bits"] = 7;
            break;
        case UART_NB_BIT_8:
            doc["bits"] = 8;
            break;
    }

    switch (uartConfig & MASK_UART_PARITY) {
        case UART_PARITY_NONE:
            doc["parity"] = nullptr;
            break;
        case UART_PARITY_EVEN:
            doc["parity"] = 0;
            break;
        case UART_PARITY_ODD:
            doc["parity"] = 1;
    }

    switch (uartConfig & MASK_UART_STOP) {
        case UART_NB_STOP_BIT_0:
            doc["stop"] = 0;
            break;
        case UART_NB_STOP_BIT_1:
            doc["stop"] = 1;
            break;
        case UART_NB_STOP_BIT_15:
            doc["stop"] = 15;
            break;
        case UART_NB_STOP_BIT_2:
            doc["stop"] = 2;
            break;
    }

    serializeJson(doc, *response);
    request->send(response);
}

void invalidJsonBadRequest(AsyncWebServerRequest *request, const char *message) {
    AsyncResponseStream *response = request->beginResponseStream("text/plain");
    response->printf("Invalid input in JSON: %s", message);
    response->setCode(400);
    request->send(response);
}


void WiSeServer::handleStatsRequest(AsyncWebServerRequest *request) const {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(200);
    doc["tx"] = ttyd->getTotalTx();
    doc["rx"] = ttyd->getTotalRx();
    doc["txRateBps"] = ttyd->getTxRate();
    doc["rxRateBps"] = ttyd->getRxRate();
    serializeJson(doc, *response);
    request->send(response);
}

void WiSeServer::handleSttyRequest(AsyncWebServerRequest *request) const {
    if (!checkHttpBasicAuth(request)) return;

    if (request->method() == HTTP_GET) {
        debugf("GET /stty\r\n");
        sttySendResponse(request);
    }
}

void
WiSeServer::handleSttyBody(
        AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) const {
    if (!checkHttpBasicAuth(request)) return;

    if (request->method() == HTTP_POST) {
        debugf("POST /stty\r\n");
        DynamicJsonDocument doc(200);
        deserializeJson(doc, data, len);

        uint32_t baudrate = ttyd->getUartBaudRate();
        uint8_t uartConfig = ttyd->getUartConfig();

        if (doc.isNull()) {
            return invalidJsonBadRequest(request, "JSON is invalid");
        }

        if (doc.containsKey("baudrate")) {
            if (!doc["baudrate"].is<unsigned int>()) {
                return invalidJsonBadRequest(request, "\"baudrate\" must be a positive number");
            }
            baudrate = doc["baudrate"];
        }

        if (doc.containsKey("bits")) {
            if (!doc["bits"].is<unsigned int>()) {
                return invalidJsonBadRequest(request, "\"bits\" must be a positive number, one of 5, 6, 7, 8");
            }
            switch ((uint8_t) doc["bits"]) {
                case 5:
                    uartConfig = (uartConfig & ~MASK_UART_BITS) | UART_NB_BIT_5;
                    break;
                case 6:
                    uartConfig = (uartConfig & ~MASK_UART_BITS) | UART_NB_BIT_6;
                    break;
                case 7:
                    uartConfig = (uartConfig & ~MASK_UART_BITS) | UART_NB_BIT_7;
                    break;
                case 8:
                    uartConfig = (uartConfig & ~MASK_UART_BITS) | UART_NB_BIT_8;
                    break;
                default:
                    return invalidJsonBadRequest(request, "\"bits\" must be a positive number, one of 5, 6, 7, 8");
            }
        }

        if (doc.containsKey("parity")) {
            if (doc["parity"].isNull()) {
                uartConfig = (uartConfig & ~MASK_UART_PARITY) | UART_PARITY_NONE;
            } else {
                if (!doc["parity"].is<unsigned int>()) {
                    return invalidJsonBadRequest(
                            request, "\"parity\" must be a number or null, null (none), 0 (even), 1 (odd)");
                }

                switch ((signed int) doc["parity"]) {
                    case 0:
                        uartConfig = (uartConfig & ~MASK_UART_PARITY) | UART_PARITY_EVEN;
                        break;
                    case 1:
                        uartConfig = (uartConfig & ~MASK_UART_PARITY) | UART_PARITY_ODD;
                        break;
                    default:
                        return invalidJsonBadRequest(
                                request, "\"parity\" must be a number or null, null (none), 0 (even), 1 (odd)");
                }
            }
        }

        if (doc.containsKey("stop")) {
            if (!doc["stop"].is<unsigned int>()) {
                return invalidJsonBadRequest(request, "\"stop\" must be a positive number, one of 0, 1, 15, 2");
            }
            switch ((uint8_t) doc["stop"]) {
                case 0:
                    uartConfig = (uartConfig & ~MASK_UART_STOP) | UART_NB_STOP_BIT_0;
                    break;
                case 1:
                    uartConfig = (uartConfig & ~MASK_UART_STOP) | UART_NB_STOP_BIT_1;
                    break;
                case 15:
                    uartConfig = (uartConfig & ~MASK_UART_STOP) | UART_NB_STOP_BIT_15;
                    break;
                case 2:
                    uartConfig = (uartConfig & ~MASK_UART_STOP) | UART_NB_STOP_BIT_2;
                    break;
                default:
                    return invalidJsonBadRequest(request, "\"stop\" must be a positive number, one of 0, 1, 15, 2");
            }
        }

        ttyd->stty(baudrate, uartConfig);
        sttySendResponse(request);
    }
}

void WiSeServer::onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg,
                                  uint8_t *data, size_t len) {
    AwsFrameInfo *info = nullptr;
    char cachedCommand;

    if (ttyd->isClientBlocked(client->id())) {
        debugf("WS blocked client %d sent data!\r\n", client->id());
        return;
    }

    switch (type) {
        case WS_EVT_CONNECT:
            debugf("WS new client %d\r\n", client->id());
            if (!ttyd->onNewWebSocketClient(client->id())) {
                client->close(WS_CLOSE_TOO_BIG);
                websocket->cleanupClients(WS_MAX_CLIENTS);
            }
            break;
        case WS_EVT_DISCONNECT:
            debugf("WS client disconnected %d\r\n", client->id());
            ttyd->removeClient(client->id());
            websocket->cleanupClients(WS_MAX_CLIENTS);
            debugf("DEALLOC CASE DISCONNECT client ID %d\r\n", client->id());
            deleteCachedCommand(client->id());
            break;
        case WS_EVT_ERROR:
            debugf("WS client error [%u] error(%u): %s\r\n", client->id(), *((uint16_t *) arg), (char *) data);
            client->printf(R"({"error": "%u: %s"})", *((uint16_t *) arg), (char *) data);
//            ttyd->removeClient(client->id());
//            client->close(WS_CLOSE_BAD_CONDITION);
//            websocket->cleanupClients(WS_MAX_CLIENTS);
//            deallocClientDataBuffer(client->id());
            break;
        case WS_EVT_PONG:
            debugf("WS client pong %d\r\n", client->id());
            ttyd->handleWebSocketPong(client->id());
            break;
        case WS_EVT_DATA:
            if (client->status() != WS_CONNECTED) {
                debugf("WS received data from not-connected client %d\r\n", client->id());
                return;
            }

            info = (AwsFrameInfo *) arg;

            if (info->final && info->index == 0 && info->len == len) {
                // Entire message in one frame
                debugf("\r\nWS client data FINAL INDEX 0 final %d, len %d\r\n", client->id(), len);
                ttyd->handleWebSocketMessage(client->id(), data, len);
            } else {
                // Message is split into multiple frames or frame is fragmented
                debugf("\r\nWS client data FRAGMENTED %d, index %llu len %d\r\n", client->id(), info->index, len);

                if (info->index == 0) {
                    // Cache command
                    cachedCommand = 0;
                    storeCommandCache(client->id(), data[0]);
                } else {
                    cachedCommand = getCachedCommand(client->id());
                }
                ttyd->handleWebSocketMessage(client->id(), data, len, cachedCommand);

                if (info->index + len >= info->len) {
                    deleteCachedCommand(client->id());
                }
            }
            break;
    }
}
