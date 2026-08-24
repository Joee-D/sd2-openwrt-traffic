#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <SD2Common.h>

#include "OpenWrtClient.h"
#include "config.h"

// 界面直接用 TFT_eSPI 绘制, 不依赖 LVGL, 也不打包任何自定义字体:
//  - 速率数字: 库自带 Font2 放大 4 倍(64px)
//  - 单位/峰值: Font2(16px)
//  - CPU/内存: 标签 Font2(16px) + 百分比 Font2 2x(32px)
//  - 上下行箭头用三角形绘制
// 流程与姊妹工程 sd2-deepseek-balance 保持一致:
// 启动页 -> WiFi -> NTP 校时 -> 周期拉取, 休眠时段按本地时间判断。
TFT_eSPI tft = TFT_eSPI();
OpenWrtClient router(ROUTER_HOST, ROUTER_PORT);

// 公共基础组件（sd2-common）
static sd2::Wifi wifi;
static sd2::SleepScheduler sleepSched(SLEEP_START_HOUR, SLEEP_END_HOUR);
// SD2 固定硬件：背光 GPIO5(D1)，反相 PWM（与 deepseek 工程一致）
static sd2::Backlight backlight(5, sd2::Backlight::PWM_INVERTED);

// ---------- 颜色（与 sd2-deepseek-balance 保持一致）----------
#define C_BG      tft.color565(0x0E, 0x11, 0x20)
#define C_CARD    tft.color565(0x1B, 0x22, 0x3C)
#define C_BORDER  tft.color565(0x2E, 0x38, 0x58)
#define C_LABEL   tft.color565(0x9A, 0xA5, 0xC8)
#define C_WHITE   tft.color565(0xFF, 0xFF, 0xFF)
#define C_GREEN   tft.color565(0x34, 0xD3, 0x99)
#define C_RED     tft.color565(0xFF, 0x6B, 0x6B)
#define C_ACCENT  tft.color565(0x4D, 0x6B, 0xFE)
// 高温告警色(deepseek 无此语义, 保留橙色做区分)
#define C_TEMP_HOT tft.color565(0xFF, 0x5D, 0x18)

// ---------- 状态 ----------
static bool bootDone = false;
static bool ntpDone = false;
static bool wifiFailShown = false;
static uint32_t lastPoll = 0;
static uint32_t lastOkTime = 0;
static uint32_t lastSleepCheck = 0;
static uint32_t lastHeapPrint = 0;
static uint32_t lastStatsLog = 0;
static uint32_t bootStart = 0;

// ---------- 数据 ----------
static double rxMbps = 0, txMbps = 0;
static int rxHist[CHART_POINTS];
static int txHist[CHART_POINTS];
static int cpuPct = 0;
static int memPct = 0;
static int tempC = -1; // CPU 温度(摄氏度, 无传感器为 -1)

// ---------- 启动页 ----------
void drawBootPage(bool fail) {
    tft.fillScreen(C_BG);
    tft.setTextFont(2);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    const char *title = "OP MONITOR";
    tft.drawString(title, (240 - tft.textWidth(title)) / 2, 84);
    const char *hint = fail ? "WiFi failed, retrying..." : "Connecting WiFi...";
    tft.setTextSize(1);
    tft.setTextColor(fail ? C_RED : C_LABEL);
    tft.drawString(hint, (240 - tft.textWidth(hint)) / 2, 132);
}

// ---------- 数据拉取 ----------
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
    tempC = doc["temp"] | -1;

    for (int i = 0; i < CHART_POINTS - 1; i++)
    {
        rxHist[i] = rxHist[i + 1];
        txHist[i] = txHist[i + 1];
    }
    rxHist[CHART_POINTS - 1] = (int)(rxMbps * 10);
    txHist[CHART_POINTS - 1] = (int)(txMbps * 10);
    return true;
}

// ---------- 主页面元素 ----------
// 一列速度显示: 箭头 + 大数字 + 单位(只刷新本列区域, 避免整屏闪烁)
static void drawSpeedCol(int colX, bool down, double mbps, uint16_t arrowColor)
{
    char big[16], unit[8];
    sd2::formatRate(mbps, big, sizeof(big), unit, sizeof(unit));

    // 清本列区域(列高 84, 去掉 Pk 峰值给底部表盘腾空间)
    tft.fillRect(colX, 0, 112, 84, C_BG);

    // 大数字 Font2 3x(48px), 居中
    tft.setTextFont(2);
    tft.setTextSize(3);
    tft.setTextColor(C_WHITE);
    tft.drawString(big, colX + (112 - tft.textWidth(big)) / 2, 2);

    // 箭头 + 单位, 数字下方居中
    tft.setTextFont(2);
    tft.setTextSize(1);
    int groupW = 12 + 4 + tft.textWidth(unit);
    int gx = colX + (112 - groupW) / 2;
    if (down)
        tft.fillTriangle(gx, 54, gx + 12, 54, gx + 6, 66, arrowColor);
    else
        tft.fillTriangle(gx, 66, gx + 12, 66, gx + 6, 54, arrowColor);
    tft.setTextColor(C_LABEL);
    tft.drawString(unit, gx + 16, 52);
}

// 折线图(透明背景, 无网格)
static void drawChart()
{
    // 卡片背景(与 deepseek 表格一致)
    tft.fillRoundRect(4, 84, 232, 54, 8, C_CARD);
    tft.drawRoundRect(4, 84, 232, 54, 8, C_BORDER);

    const int cx = 10, cy = 90, cw = 220, chh = 42;
    // 清空曲线区(用卡片底色), 避免折线顶点残留
    tft.fillRect(cx - 3, cy - 3, cw + 6, chh + 6, C_CARD);

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
    tft.fillRect(x, y, w, 8, C_CARD);
    if (pct > 0)
        tft.fillRect(x, y, (long)w * pct / 100, 8, C_ACCENT);
}

// 底部一行: 标签(小, 左对齐) + 百分比(2x, 右对齐) + 进度条
static void drawStatRow(int x, int colW, const char *label, int pct, int yPct, int yBar)
{
    // 标签左对齐
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(C_LABEL);
    tft.drawString(label, x, yPct + 8);

    // 百分比右对齐
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.drawString(buf, x + colW - tft.textWidth(buf), yPct);
    drawBar(x, yBar, colW, pct);
}

// 右下: 温度圆圈(完整圆环 + 圆弧进度 + 中央数值)
static void drawTempGauge()
{
    const int cx = 176, cy = 186, r = 42, ir = 37;
    tft.fillRect(126, 142, 100, 94, C_BG);

    // 完整圆环(表盘底色)
    tft.drawArc(cx, cy, r, ir, 0, 360, C_BORDER, C_BG);

    if (tempC > 0)
    {
        // 0~100°C 映射为圆弧进度(起始于左下 135°, 顺时针)
        int end = 135 + 270 * tempC / 100;
        if (end > 360) end = 360;
        tft.drawArc(cx, cy, r, ir, 135, end,
                    tempC > 70 ? C_TEMP_HOT : C_GREEN, C_BORDER);
    }

    // 中央温度数值
    char buf[16];
    snprintf(buf, sizeof(buf), tempC > 0 ? "%dC" : "--", tempC);
    tft.setTextFont(2);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE);
    tft.drawString(buf, cx - tft.textWidth(buf) / 2, cy - 15);
}

static void draw()
{
    tft.startWrite();

    // 顶部: 左=下行(红), 右=上行(绿)
    drawSpeedCol(6, true, rxMbps, C_RED);
    drawSpeedCol(124, false, txMbps, C_GREEN);

    // 曲线
    drawChart();

    // 底部左: CPU / Memory 两行(标签+百分比+进度条)
    tft.fillRect(2, 140, 118, 92, C_BG);
    drawStatRow(6, 108, "CPU", cpuPct, 142, 176);
    drawStatRow(6, 108, "Memory", memPct, 188, 222);

    // 底部右: 温度圆圈
    drawTempGauge();

    tft.endWrite();
}

// 主页面完整重绘（启动页/休眠唤醒等整屏切换时调用，先清屏避免文字残留）
static void drawMainPage()
{
    tft.fillScreen(C_BG);
    draw();
}

// ---------- 定时休眠（NTP 本地时间）----------
void updateSleep()
{
    bool changed = sleepSched.update(sd2::localHour());
    if (changed && sleepSched.sleeping())
    {
        tft.fillScreen(C_BG);
        backlight.off();
        Serial.println("[sleep] display off, fetch paused");
    }
    else if (changed && !sleepSched.sleeping())
    {
        backlight.on();
        for (int i = 0; i < CHART_POINTS; i++)
            rxHist[i] = txHist[i] = 0; // 清掉休眠期间的曲线
        lastPoll = millis() - POLL_INTERVAL_MS; // 醒来立即刷新
        if (bootDone) drawMainPage();
        Serial.println("[sleep] wake up");
    }
}

// ---------- 网络 ----------
void handleWiFi()
{
    wifi.loop();
    if (wifi.justConnected())
    {
        Serial.print("WiFi connected, IP: ");
        Serial.println(wifi.ip().c_str());
        bootDone = true;
        if (!sleepSched.sleeping()) drawMainPage();
        sd2::timeBegin(TZ_OFFSET_SEC, NTP_SERVER);
        lastPoll = millis() - POLL_INTERVAL_MS; // 立即拉取
    }
    if (wifi.justDisconnected())
    {
        Serial.println("WiFi disconnected, retrying...");
    }

    if (!wifi.connected() && !bootDone && !wifiFailShown && millis() - bootStart > 30000)
    {
        wifiFailShown = true;
        drawBootPage(true);
    }
}

void handleFetch()
{
    if (!wifi.connected() || sleepSched.sleeping()) return;
    if (millis() - lastPoll < POLL_INTERVAL_MS) return;

    // 休眠时段按本地时间判断, 先等 NTP 校时
    if (!ntpDone)
    {
        if (sd2::timeSynced())
        {
            ntpDone = true;
            Serial.printf("NTP time synced: %lu\n", (unsigned long)time(nullptr));
        }
        return;
    }
    if (sleepSched.sleeping()) return; // 休眠时段不拉取数据

    lastPoll = millis();
    if (fetchData())
    {
        lastOkTime = millis();
        draw();
    }
    else if (millis() - lastOkTime > 60000)
    {
        // 长时间取不到数据: 自动重启自愈
        Serial.println("[watchdog] no data for 60s, restarting");
        ESP.restart();
    }

    if (millis() - lastStatsLog > 10000)
    {
        lastStatsLog = millis();
        Serial.printf("[rate] rx %.1f tx %.1f Mbps, temp %dC\n", rxMbps, txMbps, tempC);
    }
}

// ---------- 主程序 ----------
void setup()
{
    Serial.begin(SERIAL_BAUD);
    delay(200);
    Serial.println();
    Serial.println("SD2 OpenWrt speed monitor (TFT_eSPI native)");

    tft.begin();
    tft.setRotation(0);
    tft.setTextDatum(TL_DATUM);

    backlight.begin();
    backlight.setBrightness(BRIGHTNESS);
    Serial.printf("[bl] backlight pin=%d mode=PWM_INVERTED brightness=%d\n", 5, BRIGHTNESS);

    bootStart = millis();
    drawBootPage(false);

    wifi.begin(WIFI_SSID, WIFI_PASS, /*persistent=*/false);
}

void loop()
{
    handleWiFi();
    handleFetch();

    if (millis() - lastSleepCheck >= 1000)
    {
        lastSleepCheck = millis();
        updateSleep();
    }
    if (millis() - lastHeapPrint >= 10000)
    {
        lastHeapPrint = millis();
        Serial.printf("Free heap: %u B\n", ESP.getFreeHeap());
    }

    delay(20);
}
