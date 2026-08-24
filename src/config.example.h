#pragma once

// ============================================================
//  SD2 OpenWrt 网速监视器 - 配置
//  复制本文件为 src/config.h 后填写(config.h 不入库)
// ============================================================

// ---- 1. WiFi 配置 ----
// SD2 需要加入一个能访问到 OpenWrt(192.168.1.1) 的无线网络。
// 如果 OpenWrt 本身不带 WiFi(比如 x86 软路由), 就填它后面挂的无线路由/AP 的账号。
// ⚠️ 请填入你自己的 WiFi 名称/密码; 提交到公开仓库前不要写真实凭据。
const char *WIFI_SSID = "your_wifi_ssid";
const char *WIFI_PASS = "your_wifi_password";

// ---- 2. OpenWrt 路由器 ----
const char *ROUTER_HOST = "192.168.1.1";
const uint16_t ROUTER_PORT = 80;

// ---- 2.1 路由器端配套脚本(必须已安装) ----
// 把 router/traffic 放到路由器 /www/cgi-bin/traffic 并 chmod +x。
// 固件每 POLL_INTERVAL_MS 毫秒 GET 一次 http://192.168.1.1/cgi-bin/traffic,
// 路由器返回一行小 JSON: 速率(字节/秒)已在路由器算好, 固件只负责显示。

// ---- 3. 轮询与显示 ----
// 轮询间隔(毫秒), 1 秒一个点
const uint32_t POLL_INTERVAL_MS = 1000;
// 图表点数(1 秒 1 个点, 60 = 显示最近 1 分钟)
const uint16_t CHART_POINTS = 60;

// ---- 4. 显示内容 ----
// 固件完全通用: 路由器端 openwrt/traffic 脚本返回什么(rx/tx/cpu/mem)就显示什么。
// 要监控哪个接口(pppoe-wan / br-lan / eth0...), 改脚本里的 IFACE 即可, 固件不用动。

// ---- 5. NTP 时间同步 ----
// ESP8266 无 RTC, 休眠时段按本地时间判断, 需要联网校时
#define NTP_SERVER "ntp.aliyun.com"
#define TZ_OFFSET_SEC (8UL * 3600UL)

// ---- 6. 休眠时段(24 小时制小时, 支持跨午夜) ----
// 该时段关闭屏幕背光并停止刷新, NTP 校时后按本地时间到点自动唤醒。
// 两个都设为 -1 表示不启用休眠。
const int SLEEP_START_HOUR = 0;   // 默认 0 点(晚上12:00)
const int SLEEP_END_HOUR   = 7;   // 默认到早晨 7 点

// ---- 7. 屏幕背光 ----
// 背光引脚(GPIO5/D1)与反相 PWM 驱动为 SD2 固定硬件, 已固化在 main.cpp,
// 与 deepseek 工程一致; 这里只需保留亮度(0~1023, 数值越大越亮, 默认 800)
#define BRIGHTNESS 800

// ---- 8. 串口 ----
#define SERIAL_BAUD 921600
