# SD2 小电视 · OpenWrt 网速监视器

在 [SD2 小电视](https://oshwhub.com/Q21182889/esp-xiao-dian-shi)（ESP8266 + 1.3 寸 ST7789 240×240 屏幕）上直接显示 OpenWrt 路由器网速的小固件。

路由器上放一个轻量 CGI 脚本（`router/traffic`），每次请求实时读接口计数器并差分出当前速率，连同 CPU / 内存 / 当前小时返回一行小 JSON；小电视每秒 GET 一次直接显示。不需要额外客户端，固件里也没有路由器登录凭据。

## 屏幕效果

![屏幕显示效果](images/screenshot.png)

## 显示内容

- 上/下行实时速率：64px 大数字（<10 Mbps 保留一位小数，10 以上显示整数，Gbps 显示 1.2）
- 每个方向带小箭头、单位（Mbps/Kbps/Gbps）与峰值（Pk）
- 最近 60 秒速率曲线（红=下行，绿=上行），透明背景无网格
- 底部：CPU / 内存占用率（带进度条）
- 休眠时段（默认 0:00–7:00）：关闭背光并停止刷新，每 30 秒探测一次时间以便准点唤醒

界面直接用 TFT_eSPI 绘制（内置 Font2 缩放 + 三角形箭头），不引入 LVGL，不打包自定义字体。

## 硬件

| 项目 | 说明 |
|------|------|
| 主控 | ESP8266（ESP-12E/F），PlatformIO `board = nodemcuv2` |
| 屏幕 | ST7789，240×240，SPI，**无 CS**，MISO 未接 |
| 烧录 | Micro USB + 板载 CH340 |

引脚（已写入 `platformio.ini` 的 TFT_eSPI 编译宏）：

| 功能 | NodeMCU | GPIO |
|------|---------|------|
| TFT DC | D3 | GPIO0 |
| TFT RST | D4 | GPIO2 |
| TFT SCLK | D5 | GPIO14 |
| TFT MOSI | D7 | GPIO13 |
| 背光 | D1 | GPIO5 |

购买成品时请和卖家确认带 CH340 芯片、支持自行烧录。

## 快速开始

1. 用 VS Code 打开本工程（已安装 PlatformIO 插件）。
2. 复制 [`src/config.example.h`](src/config.example.h) 为 `src/config.h`，然后编辑：
   ```bash
   cp src/config.example.h src/config.h
   ```
   - `WIFI_SSID` / `WIFI_PASS`：你的 WiFi
   - `ROUTER_HOST` / `ROUTER_PORT`：OpenWrt 地址（默认 192.168.1.1:80）
   - `SLEEP_START_HOUR` / `SLEEP_END_HOUR`：休眠时段（`-1` 禁用）
3. 路由器上安装数据接口（一次性）：
   ```bash
   scp router/traffic root@192.168.1.1:/www/cgi-bin/traffic
   ssh root@192.168.1.1 "chmod +x /www/cgi-bin/traffic"
   ```
   想监控哪个接口，改 `router/traffic` 里的 `IFACE`（如 `pppoe-wan` / `br-lan` / `eth0`）。
4. USB 连接小电视，点击 VS Code 底部 PlatformIO 工具栏的 **Upload**（或终端 `pio run -t upload`）。
5. 点击 **Serial Monitor**（波特率 921600）可查看日志。

## 目录结构

```
src/
├── main.cpp          主程序：TFT_eSPI 界面、WiFi、轮询、休眠
├── OpenWrtClient.h   轻量 HTTP GET 客户端（解析一行小 JSON）
├── config.example.h  配置模板（复制为 config.h 后填写，config.h 不入库）
└── config.h          本地配置（.gitignore 忽略，不会提交）
router/
└── traffic           OpenWrt CGI 数据接口（安装到 /www/cgi-bin/traffic）
images/               屏幕效果图
```

## 工作原理

1. 开机连接 WiFi。
2. 每秒 GET 一次 `http://192.168.1.1/cgi-bin/traffic`。
3. 路由器脚本读取 `/proc/net/dev` 计数器，与上次请求差分出字节/秒速率，连同 CPU（`/proc/stat` 差分）、内存占用、当前小时返回。
4. 固件换算成 Mbps 并绘制：大数字、单位、峰值、60 秒曲线、CPU/内存条。
5. 休眠时段内关闭背光、停止刷新，只每 30 秒探测一次时间，到点自动唤醒。

## 常见问题

**1. 一直显示黑屏 / 无数据**

先在电脑上 `curl http://192.168.1.1/cgi-bin/traffic`，确认脚本已安装且有 JSON 输出；再看串口日志中是否有 `[owrt] tcp connect failed`。

**2. 屏幕花屏 / 无显示**

- 确认是 ST7789 240×240（SD2 标准配置）；若为其他驱动，修改 `platformio.ini` 的驱动宏和引脚。
- 背光：`platformio.ini` 中 `-DTFT_BL=5`（GPIO5/D1），不同批次 SD2 可能不同。

**3. 烧录失败**

- 确认数据线是数据线（不是纯充电线）。
- 上传波特率默认 921600，失败可改为 `upload_speed = 115200`。

**4. 想监控局域网总流量**

把 `router/traffic` 里的 `IFACE` 改为 `br-lan` 后重装脚本即可，固件不用改。

## 参考

- 硬件开源：https://oshwhub.com/Q21182889/esp-xiao-dian-shi
- 固件参考：https://github.com/Jason6111/sd2
- 姊妹项目：https://github.com/Joee-D/sd2-deepseek-balance
