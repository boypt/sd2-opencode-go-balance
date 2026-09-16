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
//  显示三行额度: 5H / WEEK / MONTH，剩余百分比 + 重置时间
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
uint16_t barColorFor(int remaining) {
    if (remaining >= 50) return C_GREEN;
    if (remaining >= 20) return C_YELLOW;
    return C_RED;
}

// 一行额度：标题/百分比在上，进度条居中，重置时间用小字放底部
void drawQuotaRow(int y, const char *label, const OpenCodeGoWindow &w) {
    tft.fillRect(18, y, 204, 64, C_BG);
    drawText(20, y + 3, label, C_LABEL, &FreeSans9pt7b);

    String pct = "--";
    uint16_t pctColor = C_RED;
    int remaining = 0;
    if (w.present && w.valid) {
        remaining = w.remaining();
        pct = String(remaining) + "%";
        pctColor = C_WHITE;
    }
    int pw = textWidth(pct, &FreeSans12pt7b);
    drawText(220 - pw, y + 2, pct, pctColor, &FreeSans12pt7b);

    tft.fillRect(20, y + 30, 204, 12, C_BORDER);
    if (w.present && w.valid && remaining > 0) {
        tft.fillRect(20, y + 30, 204L * remaining / 100, 12, barColorFor(remaining));
    }

    if (w.present && !w.valid) {
        drawMiniText(220 - miniTextWidth("INVALID"), y + 48, "INVALID", C_RED);
    } else {
        String reset = formatReset(w.resetsAt);
        if (reset.length() > 0)
            drawMiniText(220 - miniTextWidth(reset), y + 48, reset, C_SUB);
    }
}

void drawMainPage() {
    tft.startWrite();
    tft.fillScreen(C_BG);

    tft.pushImage(10, 1, OC_LOGO_W, OC_LOGO_H, oc_logo, 0x0000);
    tft.drawFastHLine(16, 40, 208, C_BORDER);

    drawQuotaRow(46, "5H", lastData.rolling);
    drawQuotaRow(110, "WEEK", lastData.weekly);
    drawQuotaRow(174, "MONTH", lastData.monthly);
    tft.drawFastHLine(20, 110, 204, C_BORDER);
    tft.drawFastHLine(20, 174, 204, C_BORDER);

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

        Serial.printf("Quota: 5h %d%% / week %d%% / month %d%%\n",
                      d.rolling.percent, d.weekly.percent, d.monthly.percent);

        tft.startWrite(); // 单事务批量重绘，避免逐块清空闪动
        tft.fillRect(0, 226, 240, 14, C_BG); // 清除上次的错误提示
        drawQuotaRow(46, "5H", d.rolling);
        drawQuotaRow(110, "WEEK", d.weekly);
        drawQuotaRow(174, "MONTH", d.monthly);
        tft.drawFastHLine(20, 110, 204, C_BORDER);
        tft.drawFastHLine(20, 174, 204, C_BORDER);
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
        drawMiniText(20, 228, "ERR " + d.error, C_RED);
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
