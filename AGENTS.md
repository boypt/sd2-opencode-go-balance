# AGENTS.md — sd2-opencode-go-balance

ESP8266 (nodemcuv2) + ST7789 240x240 固件，PlatformIO 构建。显示 OpenCode Go 5H/WEEK/MONTH 额度。

## 构建（WSL2 下跑 opencode 时）

- WSL 内无原生 `pio`，调 Windows 侧 PIO 6.2.0。必须先转 Windows 路径：
  `WIN_DIR=$(wslpath -w "$PWD"); "/mnt/c/Users/Ben/.platformio/penv/Scripts/pio.exe" run -d "$WIN_DIR"`
- 已有便捷 alias：`wsl-pio`（同上）。验证只跑 `run`，8s 左右 `[SUCCESS]` 即过。
- `upload` / `monitor` 回 Windows PowerShell / VSCode 做（WSL2 串口需 usbipd，直连最省事）。波特率 `921600`，上传失败降 `upload_speed=115200`。

## 配置与密钥

- `src/config.example.h` 是模板；真实配置 `src/config.h` 被 `.gitignore` 忽略，**永不提交**（内有 WiFi 密码 + `sk-` Key）。
- 当前 `config.h` 用代理 host（`OPENCODE_API_HOST/PATH`），与模板默认值不同，改请求地址只动 `config.h`。
- `cert.h` 是 GTS Root R4；`VERIFY_TLS_CERT=0` 仅应急。

## 目录与边界

- `src/main.cpp`：界面 + `handleFetch` 轮询（`POLL_INTERVAL_MS` 5min，`FAST_RETRY_MS` 30s 仅无数据时）。
- `src/OpenCodeGoClient.h`：用量请求 + 重试。`DynamicJsonDocument(2048)`，`http_code==0`/空 body 视为可重试；仅 401/403 直返；退避 `min(10s*attempt,30s)+rand(0-1s)`，200ms 分片 `yield()`。
- `lib/sd2-common` 是 git submodule（`git clone --recursive`），含 WiFi/NTP/休眠/背光/HTTPS/TFT 引脚（`platformio/tft_setup.h` 引脚已固化，无 CS）。**禁止修改**，有问题只改本工程 `src/`。
- `platformio.ini` 用 `extra_configs = lib/sd2-common/platformio/sd2-st7789.ini` 合并公共 env，本工程只加字体宏和 `lib_deps`。

## 已知坑

- `lib/sd2-common` 在 WSL 下 `git status` 常显全文件 dirty，实为换行符噪声，提交时排除：只 `git add src/OpenCodeGoClient.h src/main.cpp src/config.example.h`。
- `readHttpResponse` 依赖 `Connection: close` 收包；chunked 截断会变 JSON 解析失败走重试，不要调小 `timeout_ms`（默认 15000）。
- `fetchOpenCodeGoUsage` 阻塞最长约 65s，属预期；`loop` 阻塞期间不断 WiFi 状态机，重试间隔已用分片等待。
- 无测试/CI；`wsl-pio` 编译通过即为验证。RAM ~38% / Flash ~44%，加库注意余量。

## 提交

- 信息风格：`feat:` / `fix:` / `docs:` / `refactor:` + 中文简述。勿 `push`（除非明确要求）。
