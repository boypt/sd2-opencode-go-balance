#pragma once

// ============================================================
//  SD2 小电视 OpenCode Go 额度显示器 - 用户配置模板
//  使用方法：复制本文件为 src/config.h 后填写你的配置
//    cp src/config.example.h src/config.h
//  config.h 已在 .gitignore 中，不会被提交到仓库
// ============================================================

// ---- WiFi ----
const char WIFI_SSID[] = "your-wifi-ssid";
const char WIFI_PASSWORD[] = "your-wifi-password";

#define OPENCODE_API_HOST "opencode.ai"
#define OPENCODE_API_PATH "/zen/go/v1/usage"
// ---- OpenCode Go API Key ----
// 在 https://opencode.ai 的 Zen / Go 套餐页创建（Anthropic 兼容 API Key）
// 固件请求：GET https://opencode.ai/zen/go/v1/usage
const char OPENCODE_GO_API_KEY[] = "sk-your-opencode-go-api-key";

// ---- 刷新间隔（毫秒）----
const uint32_t POLL_INTERVAL_MS = 5UL * 60UL * 1000UL; // 默认 5 分钟

// ---- 获取失败重试 ----
// 指数退避（等待 = FETCH_RETRY_INTERVAL_MS * attempt，上限 30s + 抖动），最多
// FETCH_MAX_ATTEMPTS 次；无数据时 30s 快速重试，有数据才等满轮询周期
#define FETCH_RETRY_INTERVAL_MS 10000UL
#define FETCH_MAX_ATTEMPTS 3
#define FAST_RETRY_MS 30000UL

// ---- HTTPS 证书校验 ----
// 1 = 校验证书（默认，内置 Google GTS Root R4，见 cert.h）
// 0 = 跳过证书校验（部分网络环境证书链异常时应急用，不推荐）
#define VERIFY_TLS_CERT 1

// ---- NTP 时间同步 ----
// ESP8266 无 RTC，校验证书前必须先同步系统时间（时区按东八区）
#define NTP_SERVER "ntp.aliyun.com"
#define TZ_OFFSET_SEC (8UL * 3600UL)

// ---- 定时休眠 ----
// 默认 00:00-07:00 关闭显示并停止获取数据（WiFi 保持连接，醒来立即恢复）
#define SLEEP_START_HOUR 0   // 开始休眠（本地时间，小时）
#define SLEEP_END_HOUR 7     // 结束休眠（本地时间，小时）

// ---- 屏幕背光 ----
// 背光引脚(GPIO5/D1)与反相 PWM 为 SD2 固定硬件，已固化在 main.cpp，
// 这里只需保留亮度（0~1023，数值越大越亮，默认 800）
#define BRIGHTNESS 800

// ---- 串口 ----
#define SERIAL_BAUD 921600
