#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>

// 解 HTTP chunked 传输编码(备用, 一般 CGI 响应不会分块)
static String owrtDechunk(const String &s)
{
    String out;
    size_t i = 0;
    while (i < s.length())
    {
        int nl = s.indexOf("\r\n", i);
        if (nl < 0)
            break;
        String szStr = s.substring(i, nl);
        szStr.trim();
        long sz = strtol(szStr.c_str(), NULL, 16);
        if (sz <= 0 || (size_t)(nl + 2 + sz) > s.length())
            break;
        out += s.substring(nl + 2, nl + 2 + sz);
        i = nl + 2 + sz + 2;
    }
    return out;
}

// 轻量客户端: 每秒 GET 一次路由器上的 /cgi-bin/traffic,
// 拿到一行小 JSON(速率已在路由器算好), 无登录无会话。
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
    WiFiClient client;
    if (!client.connect(host_.c_str(), port_))
    {
        Serial.println(F("[owrt] tcp connect failed"));
        return false;
    }
    client.setTimeout(3000);
    client.setNoDelay(true);

    String req = String("GET /cgi-bin/traffic HTTP/1.1\r\n") +
                 "Host: " + host_ + ":" + String(port_) + "\r\n" +
                 "Connection: close\r\n\r\n";
    client.print(req);

    String resp;
    uint8_t buf[128];
    uint32_t t0 = millis();
    while (millis() - t0 < 4000)
    {
        while (client.available())
        {
            int n = client.read(buf, sizeof(buf));
            if (n <= 0)
                break;
            for (int i = 0; i < n; i++)
                resp += (char)buf[i];
        }
        if (!client.connected() && client.available() == 0)
            break;
        delay(1);
    }
    client.stop();

    int idx = resp.indexOf("\r\n\r\n");
    if (idx < 0)
        return false;

    String head = resp.substring(0, idx);
    String body = resp.substring(idx + 4);
    if (head.indexOf("chunked") >= 0)
        body = owrtDechunk(body);

    bodyOut = body;
    return bodyOut.length() > 0;
}
