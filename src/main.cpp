#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <TFT_eSPI.h>

#include "OpenWrtClient.h"
#include "config.h"

// 界面直接用 TFT_eSPI 绘制, 不依赖 LVGL, 也不打包任何自定义字体:
//  - 速率数字: 库自带 Font2 放大 4 倍(64px)
//  - 单位/峰值: Font2(16px)
//  - CPU/内存: 标签 Font2(16px) + 百分比 Font2 2x(32px)
//  - 上下行箭头用三角形绘制
TFT_eSPI tft = TFT_eSPI();
OpenWrtClient router(ROUTER_HOST, ROUTER_PORT);

// 背景: 纯黑
#define C_BG     0x0000
#define C_GRAY   0x8410
#define C_WHITE  0xFFFF
#define C_RED    0xF800
#define C_GREEN  0x07E0
#define C_BAR_BG 0x18E3
#define C_BAR_FG 0x4DBF

// 数据
static double rxMbps = 0, txMbps = 0;
static double rxPeak = 0, txPeak = 0;
static int rxHist[CHART_POINTS];
static int txHist[CHART_POINTS];
static int cpuPct = 0;
static int memPct = 0;
static int currentHour = -1;
static uint32_t lastOkTime = 0;

static uint32_t lastWifiTry = 0;
static uint32_t lastPoll = 0;
static uint32_t lastHeapLog = 0;
static uint32_t lastStatsLog = 0;

// 背光控制(关闭=真正熄屏省电)
static void setBacklight(bool on)
{
    if (on)
    {
        pinMode(TFT_BL, INPUT);
        analogWrite(TFT_BL, 180);
        pinMode(TFT_BL, OUTPUT);
    }
    else
    {
        // 本板背光是低电平点亮(active-low): 拉高引脚即关闭
        pinMode(TFT_BL, OUTPUT);
        digitalWrite(TFT_BL, HIGH);
    }
}

// 是否处于休眠时段(支持跨午夜)
static bool inSleepWindow(int hour)
{
    if (SLEEP_START_HOUR < 0 || SLEEP_END_HOUR < 0)
        return false;
    if (SLEEP_START_HOUR == SLEEP_END_HOUR)
        return false;
    if (SLEEP_START_HOUR < SLEEP_END_HOUR)
        return hour >= SLEEP_START_HOUR && hour < SLEEP_END_HOUR;
    return hour >= SLEEP_START_HOUR || hour < SLEEP_END_HOUR;
}

// 大数字格式: 最多 3 个字符(64px 字体宽度有限)
static void formatBig(double mbps, char *val, size_t valLen)
{
    double bps = mbps * 1000000.0;
    if (bps >= 1e9)
        snprintf(val, valLen, "%.1f", bps / 1e9);
    else if (bps >= 1e7)
        snprintf(val, valLen, "%.0f", bps / 1e6);
    else if (bps >= 1e6)
        snprintf(val, valLen, "%.1f", bps / 1e6);
    else if (bps >= 1e3)
        snprintf(val, valLen, "%.0f", bps / 1e3);
    else
        snprintf(val, valLen, "%d", (int)bps);
}

// 速率单位
static void formatUnit(double mbps, char *unit, size_t unitLen)
{
    double bps = mbps * 1000000.0;
    if (bps >= 1e9)
        snprintf(unit, unitLen, "Gbps");
    else if (bps >= 1e6)
        snprintf(unit, unitLen, "Mbps");
    else if (bps >= 1e3)
        snprintf(unit, unitLen, "Kbps");
    else
        snprintf(unit, unitLen, "bps");
}

// 拉取路由器数据(脚本返回什么就显示什么)
static bool fetchData()
{
    String body;
    if (!router.fetchTraffic(body))
        return false;

    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, body);
    if (err)
    {
        Serial.printf("[owrt] parse error: %s, body=%.120s\n", err.c_str(), body.c_str());
        return false;
    }

    // 路由器已按请求差分好速率, 这里直接使用(字节/秒)
    rxMbps = (doc["rx"] | 0.0) * 8.0 / 1e6;
    txMbps = (doc["tx"] | 0.0) * 8.0 / 1e6;
    cpuPct = doc["cpu"] | 0;
    memPct = doc["mem"] | 0;
    currentHour = doc["hour"] | -1;

    if (rxMbps > rxPeak)
        rxPeak = rxMbps;
    if (txMbps > txPeak)
        txPeak = txMbps;

    for (int i = 0; i < CHART_POINTS - 1; i++)
    {
        rxHist[i] = rxHist[i + 1];
        txHist[i] = txHist[i + 1];
    }
    rxHist[CHART_POINTS - 1] = (int)(rxMbps * 10);
    txHist[CHART_POINTS - 1] = (int)(txMbps * 10);
    return true;
}

// 一列速度显示: 箭头 + 大数字 + 单位 + 峰值(只刷新本列区域, 避免整屏闪烁)
static void drawSpeedCol(int colX, bool down, double mbps, double peak, uint16_t arrowColor)
{
    char big[16], unit[8];
    char pkv[16], pku[8], pk[32];
    formatBig(mbps, big, sizeof(big));
    formatUnit(mbps, unit, sizeof(unit));
    formatBig(peak, pkv, sizeof(pkv));
    formatUnit(peak, pku, sizeof(pku));

    // 清本列区域
    tft.fillRect(colX, 0, 112, 112, C_BG);

    // 大数字 Font2 4x(64px), 居中
    tft.setTextFont(2);
    tft.setTextSize(4);
    tft.setTextColor(C_WHITE);
    tft.drawString(big, colX + (112 - tft.textWidth(big)) / 2, 4);

    // 箭头 + 单位, 数字下方居中
    tft.setTextFont(2);
    tft.setTextSize(1);
    int groupW = 12 + 4 + tft.textWidth(unit);
    int gx = colX + (112 - groupW) / 2;
    if (down)
        tft.fillTriangle(gx, 74, gx + 12, 74, gx + 6, 86, arrowColor);
    else
        tft.fillTriangle(gx, 86, gx + 12, 86, gx + 6, 74, arrowColor);
    tft.setTextColor(C_GRAY);
    tft.drawString(unit, gx + 16, 72);

    // 峰值 Font2(16px), 居中
    tft.setTextFont(2);
    tft.setTextSize(1);
    snprintf(pk, sizeof(pk), "Pk %s %s", pkv, pku);
    tft.drawString(pk, colX + (112 - tft.textWidth(pk)) / 2, 94);
}

// 折线图(透明背景, 无网格)
static void drawChart()
{
    const int cx = 5, cy = 116, cw = 230, chh = 56;
    // 清空区上下左右各多清 3px, 避免折线顶点残留
    tft.fillRect(cx - 3, cy - 3, cw + 6, chh + 6, C_BG);

    // 最近 60 秒内的最大值
    int maxv = 16;
    for (int i = 0; i < CHART_POINTS; i++)
    {
        if (rxHist[i] > maxv)
            maxv = rxHist[i];
        if (txHist[i] > maxv)
            maxv = txHist[i];
    }


    int yb = cy + chh - 2;
    for (int i = 1; i < CHART_POINTS; i++)
    {
        int x0 = cx + (i - 1) * cw / (CHART_POINTS - 1);
        int x1 = cx + i * cw / (CHART_POINTS - 1);
        int y0r = yb - rxHist[i - 1] * chh / maxv;
        int y1r = yb - rxHist[i] * chh / maxv;
        int y0t = yb - txHist[i - 1] * chh / maxv;
        int y1t = yb - txHist[i] * chh / maxv;
        if (y0r < cy) y0r = cy;
        if (y1r < cy) y1r = cy;
        if (y0t < cy) y0t = cy;
        if (y1t < cy) y1t = cy;
        tft.drawLine(x0, y0r, x1, y1r, C_RED);
        tft.drawLine(x0, y0t, x1, y1t, C_GREEN);
    }
}

static void drawBar(int x, int y, int w, int pct)
{
    tft.fillRect(x, y, w, 8, C_BAR_BG);
    if (pct > 0)
        tft.fillRect(x, y, (long)w * pct / 100, 8, C_BAR_FG);
}

// 底部标签 + 大号百分比, 同一行
static void drawLabelPct(int x, const char *label, int pct)
{
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(C_GRAY);
    int smallW = tft.textWidth(label);

    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    int bigW = tft.textWidth(buf);
    int gx = x + (108 - (smallW + 4 + bigW)) / 2;

    tft.setTextSize(1);
    tft.setTextColor(C_GRAY);
    tft.drawString(label, gx, 188);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.drawString(buf, gx + smallW + 4, 180);
}

static void draw()
{
    tft.startWrite();

    // 顶部: 左=下行(红), 右=上行(绿)
    drawSpeedCol(6, true, rxMbps, rxPeak, C_RED);
    drawSpeedCol(124, false, txMbps, txPeak, C_GREEN);

    // 曲线
    drawChart();

    // 底部: 左=CPU, 右=内存
    tft.fillRect(6, 174, 108, 56, C_BG);
    tft.fillRect(126, 174, 108, 56, C_BG);
    drawLabelPct(6, "CPU", cpuPct);
    drawLabelPct(126, "Mem", memPct);
    drawBar(6, 218, 108, cpuPct);
    drawBar(126, 218, 108, memPct);

    tft.endWrite();
}

void setup()
{
    Serial.begin(921600);
    Serial.println(F("\nSD2 OpenWrt speed monitor (TFT_eSPI native)"));

    tft.begin();
    tft.setRotation(0);
    tft.setTextDatum(TL_DATUM);
    tft.fillScreen(C_BG);
    setBacklight(true);
    tft.setTextFont(2);
    tft.setTextColor(C_WHITE);
    tft.drawString("Booting...", 8, 8);

    WiFi.mode(WIFI_STA);
    WiFi.persistent(false); // 不把 WiFi 配置写回 flash
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    lastWifiTry = millis();
}

void loop()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - lastWifiTry > 5000)
        {
            WiFi.mode(WIFI_STA);
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            lastWifiTry = millis();
        }
        return;
    }

    static bool netReported = false;
    if (!netReported)
    {
        netReported = true;
        Serial.printf("[net] connected to \"%s\", ip %s\n",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }

    static bool sleeping = false;
    uint32_t interval = sleeping ? 30000UL : POLL_INTERVAL_MS;

    if (millis() - lastPoll >= interval)
    {
        lastPoll = millis();
        if (fetchData())
        {
            bool wantSleep = inSleepWindow(currentHour);
            if (wantSleep && !sleeping)
            {
                sleeping = true;
                tft.fillScreen(C_BG);
                setBacklight(false);
                Serial.println(F("[sleep] enter sleep window"));
            }
            else if (!wantSleep && sleeping)
            {
                sleeping = false;
                setBacklight(true);
                for (int i = 0; i < CHART_POINTS; i++)
                    rxHist[i] = txHist[i] = 0; // 清掉休眠期间的曲线
                Serial.println(F("[sleep] wake up"));
            }

            if (sleeping)
                return; // 休眠中: 不刷新界面, 只低频探测时间

            lastOkTime = millis();
            draw();
        }
        else if (!sleeping && millis() - lastOkTime > 60000)
        {
            // 长时间取不到数据: 自动重启自愈
            Serial.println(F("[watchdog] no data for 60s, restarting"));
            ESP.restart();
        }

        if (millis() - lastStatsLog > 10000)
        {
            lastStatsLog = millis();
            Serial.printf("[rate] rx %.1f tx %.1f Mbps\n", rxMbps, txMbps);
        }
        if (millis() - lastHeapLog > 15000)
        {
            lastHeapLog = millis();
            Serial.printf("[mem] heap=%u\n", ESP.getFreeHeap());
        }
    }
}
