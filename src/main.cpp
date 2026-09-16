// ============================================================
//  SD2 小电视 OpenCode Go 额度显示器
//
//  硬件: ESP8266(ESP-12F) + 1.3" ST7789 240x240 (无CS)
//  参考: https://github.com/Jason6111/sd2
//  接口: GET https://opencode.ai/zen/go/v1/usage
//        （OpenCode Zen / Go 套餐官方用量接口，Bearer 鉴权）
//
//  公共基础功能来自 sd2-common（WiFi/NTP/休眠/背光/HTTP/格式化）
//  使用前先修改 src/config.h（WiFi / API Key）
//  显示：左侧当前时间 + 最后更新时间；右侧三行额度
//        5H / WEEK / MONTH，剩余百分比 + 重置时间
// ============================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <SD2Common.h>
#include <Sd2App.h>

#include "config.h"
#include "OpenCodeGoClient.h"
#include "OpenCodeLogo.h"

// ---------- 公共运行骨架（sd2-common）----------
static sd2::App app(POLL_INTERVAL_MS, SLEEP_START_HOUR, SLEEP_END_HOUR);
TFT_eSPI &tft = app.tft;
static sd2::Wifi &wifi = app.wifi;
static sd2::SleepScheduler &sleepSched = app.sleep;
static sd2::Backlight &backlight = app.backlight;
static bool &bootDone = app.bootDone;
static uint32_t &lastFetchMs = app.lastFetchMs;

// ---------- 数据状态 ----------
static bool hasData = false;

static OpenCodeGoUsage lastData;

// 最后一次成功获取数据的本地时间（time(nullptr)）；
// 0 表示从未成功，用于与"当前时钟"区分
static time_t lastSuccessTime = 0;

// ---------- 界面工具 ----------
void drawText(int x, int y, const String &s, uint16_t color, const GFXfont *font) {
    tft.setFreeFont(font);
    tft.setTextColor(color);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(s, x, y);
}

int textWidth(const String &s, const GFXfont *font) {
    tft.setFreeFont(font);
    return tft.textWidth(s);
}

void drawMiniText(int x, int y, const String &s, uint16_t color) {
    tft.setTextFont(1);
    tft.setTextColor(color);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(s, x, y);
}

int miniTextWidth(const String &s) {
    tft.setTextFont(1);
    return tft.textWidth(s);
}

// ---------- ISO 时间解析 ----------
// OpenCode 接口返回 UTC 时间（如 "2026-08-28T15:42:25.791Z"）。
// 用 mktime 交给系统时区处理，再补回 UTC -> 北京的偏移。
static bool parseIsoTime(const String &iso, time_t &out) {
    if (iso.length() < 19)
        return false;

    struct tm tmv = {};
    tmv.tm_year = iso.substring(0, 4).toInt() - 1900;
    tmv.tm_mon = iso.substring(5, 7).toInt() - 1;
    tmv.tm_mday = iso.substring(8, 10).toInt();
    tmv.tm_hour = iso.substring(11, 13).toInt();
    tmv.tm_min = iso.substring(14, 16).toInt();
    tmv.tm_sec = iso.substring(17, 19).toInt();

    time_t t = mktime(&tmv) + TZ_OFFSET_SEC;
    if (t <= 0)
        return false;
    out = t;
    return true;
}

String formatReset(const String &iso) {
    if (iso.length() == 0)
        return "";
    time_t t;
    if (!parseIsoTime(iso, t))
        return "R --";
    return "R " + sd2::formatLocalTime(t, "%m-%d %H:%M");
}

// ---------- 启动页 ----------
void drawBootPage(bool fail) {
    tft.fillScreen(C_BG);
    tft.pushImage((240 - OC_LOGO_W) / 2, 90, OC_LOGO_W, OC_LOGO_H, oc_logo, 0x0000);
    const char *hint = fail ? "WiFi failed, retrying..." : "Connecting WiFi...";
    int hw = textWidth(hint, &FreeSans9pt7b);
    drawText((240 - hw) / 2, 140, hint, fail ? C_RED : C_LABEL, &FreeSans9pt7b);
}

// ---------- 主页面元素 ----------
// 新布局（240x240）：下半部分左右分栏，左时钟（HH/MM 两行）、右额度
//   ┌─ logo(居中) ──────────────────────┐ y=1
//   ├─ 横线 y=40 ───────────────────────┤
//   │ 时钟栏 x0..120 │ 状态行 UPDATE 22:00 │ y=44
//   │  HH  (F7 48px) │ 5H    行 y=58     │
//   │  ───────       │ WEEK  行 y=114    │
//   │  MM  (F7 48px) │ MONTH 行 y=170    │
//   └─ 错误条 y=226..240 ────────────────┘
uint16_t barColorFor(int remaining) {
    if (remaining >= 50) return C_GREEN;
    if (remaining >= 20) return C_YELLOW;
    return C_RED;
}

// 额度栏几何：整体右移收窄到右侧一列，进度条缩短但保持可读
static const int QUOTA_X0 = 124;   // 内容左边界
static const int QUOTA_X1 = 232;   // 右对齐基准
static const int ROW_H = 56;       // 三行额度行高（让出顶部状态行后压缩，进度条仍可读）
static const int ROW_TOP = 58;     // 第一行额度顶部 y（其上是状态行）
static const int STATUS_Y = 44;    // 右栏顶部状态行（最后更新时间）顶部 y
static const int STATUS_H = 14;    // 状态行高度（slim）

// 一行额度：标题/百分比在上，进度条居中，重置时间用小字放底部
void drawQuotaRow(int y, const char *label, const OpenCodeGoWindow &w) {
    tft.fillRect(122, y, 112, ROW_H, C_BG);
    drawText(QUOTA_X0, y + 3, label, C_LABEL, &FreeSans9pt7b);

    String pct = "--";
    uint16_t pctColor = C_RED;
    int remaining = 0;
    if (w.present && w.valid) {
        remaining = w.remaining();
        pct = String(remaining) + "%";
        pctColor = C_WHITE;
    }
    int pw = textWidth(pct, &FreeSans12pt7b);
    drawText(QUOTA_X1 - pw, y + 2, pct, pctColor, &FreeSans12pt7b);

    tft.fillRect(QUOTA_X0, y + 28, QUOTA_X1 - QUOTA_X0, 12, C_BORDER);
    if (w.present && w.valid && remaining > 0) {
        tft.fillRect(QUOTA_X0, y + 28,
                     (int32_t)(QUOTA_X1 - QUOTA_X0) * remaining / 100, 12,
                     barColorFor(remaining));
    }

    if (w.present && !w.valid) {
        drawMiniText(QUOTA_X1 - miniTextWidth("INVALID"), y + 44, "INVALID", C_RED);
    } else {
        String reset = formatReset(w.resetsAt);
        if (reset.length() > 0)
            drawMiniText(QUOTA_X1 - miniTextWidth(reset), y + 44, reset, C_SUB);
    }
}

// 左栏时钟区 bounding box（只覆盖左栏，不含分隔线/右栏/logo）：
// HH 大字 y 70..117、短分隔线 y=134、MM 大字 y 150..197（Font 7 字高 48）
static const int CLOCK_X = 0;
static const int CLOCK_Y = 64;
static const int CLOCK_W = 120;
static const int CLOCK_H = 140;

// 上次绘制的本地分钟（hour*60+min）；-1 = 尚未绘制。
// 用"小时+分钟"组合比较，避免整点(如 08:59 -> 09:00)被只看分钟时误判
static int lastClockMinute = -1;

// 当前本地分钟（hour*60+min）；NTP 未同步返回 -1
static int currentLocalMinute() {
    if (!sd2::timeSynced())
        return -1;
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    return tmv.tm_hour * 60 + tmv.tm_min;
}

// TFT_eSPI 内置 Font 7（七段数码管）：字高 48（chr_hgt_f7s），
// 数字 0-9 与 '-' 字宽均为 32（满格），仅含 [space] 0-9 : . - ；
// platformio.ini 已 -DLOAD_FONT7，不占额外 Flash。
// 测宽必须与绘制同一字体：先 setTextFont(7) 再 textWidth，不与 FreeSans helper 混用。
static void drawSegText(int cx, int y, const String &s, uint16_t color) {
    tft.setTextFont(7);
    int w = tft.textWidth(s); // 同字体测宽后水平居中
    tft.setTextColor(color);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(s, cx - w / 2, y);
}

// 左栏时钟：HH 大字在上、MM 大字在下（均 Font 7，字高 48），中间一条短分隔线
void drawClock() {
    const int cx = 60; // 左栏水平中心

    int minuteOfDay = currentLocalMinute();
    String hh = "--", mm = "--";
    if (minuteOfDay >= 0) {
        hh = sd2::formatLocalTime(time(nullptr), "%H");
        mm = sd2::formatLocalTime(time(nullptr), "%M");
    }

    drawSegText(cx, 70, hh, C_WHITE); // 字高 48 -> 占 y 70..117

    // 短分隔线：分隔时与分
    tft.drawFastHLine(cx - 28, 134, 56, C_ACCENT);

    drawSegText(cx, 150, mm, C_WHITE); // 占 y 150..197

    // 记录本次绘制的分钟，供 tick 去重（未同步时不记录，保持 --/-- 可继续尝试）
    if (minuteOfDay >= 0)
        lastClockMinute = minuteOfDay;
}

// 常驻分钟 tick：仅当本地分钟变化时局部重绘时钟区，
// 避免每 20ms 的 loop 全量刷新造成闪烁
static void tickClock() {
    if (sleepSched.sleeping())     // 休眠中不刷新（背光已关）
        return;
    int m = currentLocalMinute();
    if (m < 0 || m == lastClockMinute) // 未同步 / 分钟未变则不重绘
        return;

    tft.startWrite();
    tft.fillRect(CLOCK_X, CLOCK_Y, CLOCK_W, CLOCK_H, C_BG);
    drawClock(); // HH / MM 两行大字 + 短分隔线一并重画
    tft.endWrite();
}

// 右栏顶部 slim 状态行：最后成功更新时间（小字右对齐，只显示时分）
// 注意：内置字体只有 ASCII 字形，中文无法显示，故用 UPDATE / NO UPDATE 表达。
void drawUpdateRow() {
    tft.fillRect(122, STATUS_Y, 112, STATUS_H, C_BG);
    String upd;
    if (lastSuccessTime == 0)
        upd = "NO UPDATE";
    else
        upd = "UPDATE " + sd2::formatLocalTime(lastSuccessTime, "%H:%M");
    drawMiniText(QUOTA_X1 - miniTextWidth(upd), STATUS_Y + 4, upd, C_SUB);
}

// 正文区域（时钟栏 + 状态行 + 额度栏 + 分隔线）；调用前须保证该区域已清底
void drawBody() {
    drawUpdateRow();
    drawQuotaRow(ROW_TOP, "5H", lastData.rolling);
    drawQuotaRow(ROW_TOP + ROW_H, "WK.", lastData.weekly);
    drawQuotaRow(ROW_TOP + ROW_H * 2, "MO.", lastData.monthly);
    drawClock();
    tft.drawFastVLine(120, STATUS_Y, STATUS_H + ROW_H * 3, C_BORDER);
}

void drawMainPage() {
    tft.startWrite();
    tft.fillScreen(C_BG);

    tft.pushImage((240 - OC_LOGO_W) / 2, 1, OC_LOGO_W, OC_LOGO_H, oc_logo, 0x0000);
    tft.drawFastHLine(16, 40, 208, C_BORDER);

    drawBody();

    tft.endWrite();
}

// ---------- 公共骨架回调 ----------
static void onConnected() {
    drawMainPage();
}

static void onWake() {
    if (bootDone) drawMainPage();
}

void handleFetch() {
    // 常驻循环里的分钟时钟 tick：与网络/额度获取无关，放在最前，
    // WiFi 掉线时也照样走时；内部自行跳过休眠与未同步
    tickClock();

    if (!wifi.connected() || sleepSched.sleeping()) return;
    if (millis() - lastFetchMs < POLL_INTERVAL_MS) return;

    // 未填 Key 时直接提示
    if (strlen(OPENCODE_GO_API_KEY) < 20) {
        lastFetchMs = millis();
        return;
    }

    OpenCodeGoUsage d;
    // 失败时内部指数退避自动重试，最多 FETCH_MAX_ATTEMPTS 次
    bool ok = fetchOpenCodeGoUsage(d, 15000, FETCH_RETRY_INTERVAL_MS,
                                   FETCH_MAX_ATTEMPTS);
    lastFetchMs = millis();

    if (ok) {
        hasData = true;
        lastData = d;
        lastSuccessTime = time(nullptr);

        Serial.printf("Quota: 5h %d%% / week %d%% / month %d%%\n",
                      d.rolling.percent, d.weekly.percent, d.monthly.percent);

        // 局部重绘：清空正文 + 底部错误条，再重画整块正文（含时钟），
        // 与 drawMainPage 的 drawBody 完全一致，避免残影、避免整屏闪动
        tft.startWrite();
        tft.fillRect(0, 44, 240, 240 - 44, C_BG); // 清除上次内容与错误提示
        drawBody();
        tft.endWrite();
    } else {
        Serial.printf("Fetch failed after retries: %s (HTTP %d)\n",
                      d.error.c_str(), d.http_code);
        // 无数据时 30s 快速重试，有数据才等满轮询周期；无新增阻塞 delay
        if (!hasData) {
            lastFetchMs = millis() - POLL_INTERVAL_MS + FAST_RETRY_MS;
        }
        // 屏幕上提示错误，下次成功会自动重绘覆盖
        tft.startWrite();
        tft.fillRect(0, 226, 240, 14, C_BG);
        String err = "ERR " + d.error;
        drawMiniText((240 - miniTextWidth(err)) / 2, 229, err, C_RED);
        tft.endWrite();
    }
}

// ---------- 主程序 ----------
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(200);
    Serial.println();
    Serial.println("SD2 OpenCode Go quota monitor starting...");

    app.setHooks(drawBootPage, handleFetch, onConnected, nullptr, nullptr, onWake);
    app.begin(WIFI_SSID, WIFI_PASSWORD, BRIGHTNESS, TZ_OFFSET_SEC, NTP_SERVER);
}

void loop() {
    app.loop();
}
