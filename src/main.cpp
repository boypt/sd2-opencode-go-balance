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
#include <Esp.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <Sd2Common.h>
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

// 持久化错误状态（ASCII短串，供 UPDATE 行显示；成功时清空）
static String lastError;
static int lastHttpCode = 0;

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
// 新布局（240x240）：下半部分左右分栏，左时钟（日期 + HH/MM）、右额度
//   ┌─ logo(居中) ──────────────────────┐ y=1
//   ├─ 横线 y=40 ───────────────────────┤
//   │ 日期 MM-DD WKD │ 状态行 UPDATE 22:00 │ y=44
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
// 日期 y50..57、HH 大字 y 70..117、分隔线 y=134（秒摆动）、MM 大字 y 150..197
static const int CLOCK_X = 0;
static const int CLOCK_Y = 44;    // 向上扩大到覆盖日期行
static const int CLOCK_W = 120;
static const int CLOCK_H = 160;   // 44..203

// 分隔线（秒摆）几何：线体 SEP_W 宽，围绕左栏中心 cx=60 左右摆动 ±SEP_SWING
static const int SEP_Y = 134;
static const int SEP_W = 56;
static const int SEP_SWING = 10;
// 秒重绘条带：覆盖分隔线全部摆动位置 + 余量；y 夹在 HH 底(117) 与 MM 顶(150) 之间
static const int SEP_STRIP_X = 18;
static const int SEP_STRIP_Y = 131;
static const int SEP_STRIP_W = 84;
static const int SEP_STRIP_H = 6;

// 上次绘制的本地分钟（hour*60+min）/ 秒；-1 = 尚未绘制。
// 分钟用"小时+分钟"组合比较，避免整点(如 08:59 -> 09:00)被只看分钟时误判
static int lastClockMinute = -1;
static int lastClockSecond = -1;

// 当前本地分钟（hour*60+min）/ 秒；NTP 未同步返回 -1
static int currentLocalMinute() {
    if (!sd2::timeSynced())
        return -1;
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    return tmv.tm_hour * 60 + tmv.tm_min;
}
static int currentLocalSecond() {
    if (!sd2::timeSynced())
        return -1;
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    return tmv.tm_sec;
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

// 秒摆动：按当前秒做 4 步循环 左-中-右-中（sec&3），偏移 ±SEP_SWING
static int sepOffsetForSecond(int sec) {
    static const int kOffsets[4] = {-SEP_SWING, 0, SEP_SWING, 0};
    return kOffsets[sec & 3];
}

// 短分隔线：已同步时按当前秒左右摆动，未同步时静态居中
static void drawSeparator() {
    int x = 60 - SEP_W / 2; // 居中基准
    if (sd2::timeSynced())
        x += sepOffsetForSecond(currentLocalSecond());
    tft.drawFastHLine(x, SEP_Y, SEP_W, C_ACCENT);
}

// 左栏顶部日期星期：MM-DD WKD（星期为英文三字母，字体仅 ASCII）；
// 未同步占位 "-- --"，同步后随分钟全盒重绘自动补上
static void drawDateLine() {
    String s;
    if (sd2::timeSynced()) {
        time_t now = time(nullptr);
        struct tm tmv;
        localtime_r(&now, &tmv);
        static const char *const kWeekday[7] = {"SUN", "MON", "TUE", "WED",
                                                "THU", "FRI", "SAT"};
        s = sd2::formatLocalTime(now, "%m-%d");
        s += ' ';
        s += kWeekday[tmv.tm_wday]; // tm_wday: 0=Sun .. 6=Sat
    } else {
        s = "-- --";
    }
    drawMiniText(4, 50, s, C_SUB); // 紧贴左沿
}

// 左栏时钟：日期在顶、HH 大字在中上、MM 大字在下，中间一条摆动分隔线
void drawClock() {
    const int cx = 60; // 左栏水平中心

    drawDateLine(); // 日期只在零点变化，由分钟/整盒重绘天然覆盖

    int minuteOfDay = currentLocalMinute();
    String hh = "--", mm = "--";
    if (minuteOfDay >= 0) {
        hh = sd2::formatLocalTime(time(nullptr), "%H");
        mm = sd2::formatLocalTime(time(nullptr), "%M");
    }

    drawSegText(cx, 70, hh, C_WHITE); // 字高 48 -> 占 y 70..117

    drawSeparator(); // 秒摆动分隔线（未同步时居中）

    drawSegText(cx, 150, mm, C_WHITE); // 占 y 150..197

    // 记录本次绘制的分钟/秒，供 tick 去重（未同步时不记录，保持 --/-- 可继续尝试）
    if (minuteOfDay >= 0) {
        lastClockMinute = minuteOfDay;
        lastClockSecond = currentLocalSecond();
    }
}

// 常驻 tick（loop ~20ms 进一次，仅变化时碰屏）：
//  - 分钟变化：整块时钟盒重绘（数字 + 分隔线）
//  - 秒变化：只清/重画分隔线小条带，不碰 HH/MM 数字
// 休眠跳过；未同步保持静态居中、不摆动
static void tickClock() {
    if (sleepSched.sleeping())     // 休眠中不刷新（背光已关）
        return;

    int m = currentLocalMinute();
    if (m < 0)                     // 未同步：分隔线保持静态居中
        return;

    if (m != lastClockMinute) {    // 分钟变化 -> 全时钟盒重绘
        tft.startWrite();
        tft.fillRect(CLOCK_X, CLOCK_Y, CLOCK_W, CLOCK_H, C_BG);
        drawClock(); // 一并更新 lastClockMinute / lastClockSecond
        tft.endWrite();
        return;
    }

    int s = currentLocalSecond();  // 秒变化 -> 只重画分隔线条带
    if (s == lastClockSecond)
        return;
    lastClockSecond = s;

    tft.startWrite();
    tft.fillRect(SEP_STRIP_X, SEP_STRIP_Y, SEP_STRIP_W, SEP_STRIP_H, C_BG);
    drawSeparator();
    tft.endWrite();
}

// 右栏顶部 slim 状态行：最后成功更新时间（小字右对齐，只显示时分）
// 注意：内置字体只有 ASCII 字形，中文无法显示，故用 UPDATE / NO UPDATE 表达。
// 四态：
//   无成功无错误 -> "NO UPDATE"
//   无成功有错误 -> "ERR <lastError>"（截断至 mini 字体 112px 内）
//   有成功无错误 -> "UPDATE HH:MM"
//   有成功有错误 -> "UPDATE HH:MM E:<short>"（short 为简写，截断保 112px 内）
// 错误态颜色用 C_RED（黑底可读），正常态 C_SUB。
static String shortUpdateError() {
    if (lastError.startsWith("Network")) return "Network";
    if (lastError.startsWith("WiFi")) return "WiFi";
    if (lastError.startsWith("HTTP")) return lastError; // "HTTP 429" 等已短
    if (lastError.startsWith("Server")) return "Server";
    if (lastError.startsWith("Response") || lastError.startsWith("Empty") ||
        lastError.startsWith("Resp"))
        return "Resp";
    if (lastError.startsWith("Bad")) return "BadKey";
    if (lastError.startsWith("No Go") || lastError.startsWith("NoPlan")) return "NoPlan";
    if (lastError.startsWith("No Key") || lastError.startsWith("NoKey")) return "NoKey";
    if (lastError.length() <= 8) return lastError;
    return lastError.substring(0, 8);
}

void drawUpdateRow() {
    tft.fillRect(122, STATUS_Y, 112, STATUS_H, C_BG);
    String upd;
    uint16_t color = C_SUB;
    bool hasErr = lastError.length() > 0;
    if (lastSuccessTime == 0 && !hasErr) {
        upd = "NO UPDATE";
    } else if (lastSuccessTime == 0 && hasErr) {
        upd = "ERR " + lastError;
        while (upd.length() > 4 && miniTextWidth(upd) > 112)
            upd.remove(upd.length() - 1);
        color = C_RED;
    } else if (!hasErr) {
        upd = "UPDATE " + sd2::formatLocalTime(lastSuccessTime, "%H:%M");
    } else {
        String base = "UPDATE " + sd2::formatLocalTime(lastSuccessTime, "%H:%M") + " E:";
        String sh = shortUpdateError();
        upd = base + sh;
        while (sh.length() > 0 && miniTextWidth(upd) > 112) {
            sh.remove(sh.length() - 1);
            upd = base + sh;
        }
        color = C_RED;
    }
    drawMiniText(QUOTA_X1 - miniTextWidth(upd), STATUS_Y + 4, upd, color);
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
        lastError = "No Key";
        lastHttpCode = 0;
        yield();
        tft.startWrite();
        drawUpdateRow();
        tft.endWrite();
        yield();
        return;
    }

    OpenCodeGoUsage d;
    // 失败时内部指数退避自动重试，最多 FETCH_MAX_ATTEMPTS 次
    Serial.printf("Fetch begin heap=%u maxblock=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxFreeBlockSize());
    Serial.flush();
    bool ok = fetchOpenCodeGoUsage(d, 15000, FETCH_RETRY_INTERVAL_MS,
                                   FETCH_MAX_ATTEMPTS);
    Serial.printf("Fetch end ok=%d heap=%u maxblock=%u\n", ok,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxFreeBlockSize());
    Serial.flush();
    lastFetchMs = millis();

    if (ok) {
        hasData = true;
        lastData = d;
        lastSuccessTime = time(nullptr);
        lastError = "";
        lastHttpCode = 0;

        Serial.printf("Quota: 5h %d%% / week %d%% / month %d%%\n",
                      d.rolling.percent, d.weekly.percent, d.monthly.percent);

        // 局部重绘：清空正文 + 底部错误条，再重画整块正文（含时钟），
        // 与 drawMainPage 的 drawBody 完全一致，避免残影、避免整屏闪动
        tft.startWrite();
        tft.fillRect(0, 44, 240, 240 - 44, C_BG); // 清除上次内容与错误提示
        drawBody();
        tft.endWrite();
    } else {
        yield();
        Serial.printf("Fetch failed after retries: %s (HTTP %d)\n",
                      d.error.c_str(), d.http_code);
        // 无数据时 30s 快速重试，有数据才等满轮询周期；无新增阻塞 delay
        if (!hasData) {
            lastFetchMs = millis() - POLL_INTERVAL_MS + FAST_RETRY_MS;
        }
        // 持久化短错误串（截断至约 18 字符防溢出），供 UPDATE 行显示
        String e = d.error;
        if (e.length() == 0) e = "Fetch fail";
        if (e.length() > 18) e = e.substring(0, 18);
        lastError = e;
        lastHttpCode = d.http_code;
        // 屏幕上提示错误，下次成功会自动重绘覆盖；
        // startWrite/endWrite 严格配对，中间无 early return，前后各一次 yield()
        tft.startWrite();
        drawUpdateRow();
        tft.fillRect(0, 226, 240, 14, C_BG);
        String err = "ERR " + lastError;
        drawMiniText((240 - miniTextWidth(err)) / 2, 229, err, C_RED);
        tft.endWrite();
        yield();
    }
}

// ---------- 主程序 ----------
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(200);
    Serial.println();
    Serial.printf("Reset: %s | free=%u maxblock=%u\n",
                  ESP.getResetReason().c_str(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxFreeBlockSize());
    Serial.printf("%s\n", ESP.getResetInfo().c_str());
    Serial.flush();
    Serial.println("SD2 OpenCode Go quota monitor starting...");

    app.setHooks(drawBootPage, handleFetch, onConnected, nullptr, nullptr, onWake);
    app.begin(WIFI_SSID, WIFI_PASSWORD, BRIGHTNESS, TZ_OFFSET_SEC, NTP_SERVER);
}

void loop() {
    app.loop();
}
