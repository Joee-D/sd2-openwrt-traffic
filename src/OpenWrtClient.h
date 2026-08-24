#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <SD2Common.h>

// 轻量客户端: 每秒 GET 一次路由器上的 /cgi-bin/traffic,
// 拿到一行小 JSON(速率已在路由器算好), 无登录无会话。
// 底层 HTTP 由 sd2-common 的 sd2::httpGet 提供。
class OpenWrtClient
{
public:
    OpenWrtClient(const char *host, uint16_t port);

    bool fetchTraffic(String &bodyOut);

private:
    String host_;
    uint16_t port_;
};

OpenWrtClient::OpenWrtClient(const char *host, uint16_t port)
    : host_(host), port_(port)
{
}

bool OpenWrtClient::fetchTraffic(String &bodyOut)
{
    sd2::HttpResponse resp;
    if (!sd2::httpGet(host_.c_str(), port_, "/cgi-bin/traffic", resp, 4000))
    {
        Serial.println(F("[owrt] tcp connect failed"));
        return false;
    }
    bodyOut = resp.body;
    return bodyOut.length() > 0;
}
