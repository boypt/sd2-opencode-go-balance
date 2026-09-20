#pragma once

// OpenCode Go 用量查询客户端
// 接口: GET https://opencode.ai/zen/go/v1/usage
// Authorization: Bearer <API Key>（Anthropic 兼容 Key）
// User-Agent 保持 cc-switch/1.0，与 CC Switch 预设一致。
// 底层 HTTPS/HTTP 读取解析由 sd2-common 的 sd2::Https 提供。

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <Esp.h>
#include <SD2Common.h>

#include "config.h"
#include "cert.h"

struct OpenCodeGoWindow {
    bool present = false;  // usage.<key> 是否存在
    bool valid = true;     // status != "invalid"
    int percent = 0;       // 已用百分比
    String status;
    String resetsAt;       // ISO8601 时间

    // 与 cc-switch 提取器一致：剩余 = max(100 - percent, 0)
    int remaining() const {
        int r = 100 - percent;
        return r > 0 ? r : 0;
    }
};

struct OpenCodeGoUsage {
    bool ok = false;       // 请求 + 解析是否成功
    int http_code = 0;
    OpenCodeGoWindow rolling;  // 5 小时
    OpenCodeGoWindow weekly;   // 周额度
    OpenCodeGoWindow monthly;  // 月额度
    String error;              // 中文错误描述
};

static sd2::Https https(GTS_ROOT_R4_PEM, VERIFY_TLS_CERT);

static String openCodeSessionId() {
    static String id;
    if (id.length() == 0) {
        id = "sd2-opencode-go-balance-" + String(ESP.getChipId(), HEX);
    }
    return id;
}

static void parseWindow(const JsonObject &obj, OpenCodeGoWindow &out) {
    if (obj.isNull())
        return;
    out.present = true;
    out.status = obj["status"] | "";
    out.percent = obj["percent"] | 0;
    out.resetsAt = obj["resetsAt"] | "";
    out.valid = out.status != "invalid";
}

static bool isRetryableCode(int c) {
    // 401 = Key 无效；403 = Key 有效但没有 Go 订阅，重试无意义
    if (c == 401 || c == 403) return false;
    // 其余均可重试：含 0（建连失败/响应不完整）/429/5xx/解析失败
    return true;
}

static bool fetchOpenCodeGoUsageOnce(OpenCodeGoUsage &out, uint32_t timeout_ms) {
    sd2::HttpResponse resp;
    String error;
    String sessionHeader = "x-opencode-session: " + openCodeSessionId();
    if (!https.get(OPENCODE_API_HOST, OPENCODE_API_PATH,
                   OPENCODE_GO_API_KEY, "cc-switch/1.0",
                   resp, error, timeout_ms, sessionHeader.c_str())) {
        // 底层错误串可能偏长：只保留短 ASCII（截断至约 18 字符），不拼接长 body
        if (error.length() == 0) error = "Network error";
        if (error.length() > 18) error = error.substring(0, 18);
        out.error = error;
        return false;
    }
    out.http_code = resp.httpCode;
    Serial.printf("HTTP %d, body %u B\n",
                  out.http_code, (unsigned)resp.body.length());

    // http_code==0：建连失败/响应不完整，视为可重试网络错误
    if (out.http_code == 0) {
        out.error = "Network error";
        return false;
    }
    if (resp.body.length() == 0) {
        out.error = "Empty response";
        return false;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, resp.body);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        out.error = "Response error";
        return false;
    }

    if (out.http_code == 200) {
        JsonObject usage = doc["usage"].as<JsonObject>();
        if (!usage.isNull()) {
            parseWindow(usage["rolling"].as<JsonObject>(), out.rolling);
            parseWindow(usage["weekly"].as<JsonObject>(), out.weekly);
            parseWindow(usage["monthly"].as<JsonObject>(), out.monthly);
        }

        if (!out.rolling.present && !out.weekly.present && !out.monthly.present) {
            out.error = "Response error";
            return false;
        }
        out.ok = true;
        return true;
    }

    String apiErr = doc["error"]["message"] | "";
    if (apiErr.length() > 0)
        Serial.printf("API error: %s\n", apiErr.c_str());

    // 401 = Key 无效；403 = Key 有效但没有 Go 订阅
    if (out.http_code == 401) {
        out.error = "Bad API key";
    } else if (out.http_code == 403) {
        out.error = "No Go plan";
    } else if (out.http_code == 429) {
        out.error = "HTTP 429";
    } else if (out.http_code >= 500) {
        out.error = "Server error";
    } else {
        out.error = "HTTP " + String(out.http_code);
    }
    return false;
}

// 带重试的获取入口：指数退避（等待 = retry_interval_ms * attempt，上限 30000ms
// + random(0,1000) 抖动，attempt 从 1 计），直到成功或用完 max_attempts 次。
// 401/403 立即返回；http_code==0 视为可重试网络错误（保留 "Network error"）。
static bool fetchOpenCodeGoUsage(OpenCodeGoUsage &out,
                                 uint32_t timeout_ms = 15000,
                                 uint32_t retry_interval_ms = 10000,
                                 int max_attempts = FETCH_MAX_ATTEMPTS) {
    bool ok = false;
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        out = OpenCodeGoUsage();
        // 每次 attempt 开始前检查 WiFi，掉线则尝试重连
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
            delay(500);
            if (WiFi.status() != WL_CONNECTED) {
                out.error = "WiFi lost";
                Serial.printf("Fetch attempt %d/%d failed: %s (HTTP %d)\n",
                              attempt, max_attempts, out.error.c_str(), out.http_code);
                if (attempt < max_attempts) {
                    uint32_t wait_ms = retry_interval_ms * (uint32_t)attempt;
                    if (wait_ms > 30000) wait_ms = 30000;
                    wait_ms += random(0, 1000);
                    Serial.printf("Retry in %lu ms\n", (unsigned long)wait_ms);
                    for (uint32_t waited = 0; waited < wait_ms; waited += 200) {
                        yield();
                        delay(200);
                        if (WiFi.status() != WL_CONNECTED) break;
                    }
                }
                continue;
            }
        }
        ok = fetchOpenCodeGoUsageOnce(out, timeout_ms);
        if (ok) {
            if (attempt > 1)
                Serial.printf("Fetch recovered on attempt %d\n", attempt);
            return true;
        }
        Serial.printf("Fetch attempt %d/%d failed: %s (HTTP %d)\n",
                      attempt, max_attempts, out.error.c_str(), out.http_code);
        // 鉴权/订阅类错误重试无意义，直接返回
        if (!isRetryableCode(out.http_code)) return false;
        if (attempt < max_attempts) {
            // 指数退避：等待 = retry_interval_ms * attempt，上限 30000ms + 抖动
            uint32_t wait_ms = retry_interval_ms * (uint32_t)attempt;
            if (wait_ms > 30000) wait_ms = 30000;
            wait_ms += random(0, 1000);
            Serial.printf("Retry in %lu ms\n", (unsigned long)wait_ms);
            // 分片等待：每 200ms 一片 yield()，片间检查 WiFi，掉线提前跳出
            for (uint32_t waited = 0; waited < wait_ms; waited += 200) {
                yield();
                delay(200);
                if (WiFi.status() != WL_CONNECTED) break;
            }
        }
    }
    return ok;
}
