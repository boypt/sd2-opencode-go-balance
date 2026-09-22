# AGENTS.md — sd2-opencode-go-balance

ESP8266 (nodemcuv2) + ST7789 240x240 固件，PlatformIO 构建。显示 OpenCode Go 5H/WEEK/MONTH 额度。

## 构建

- 编译验证：`~/.platformio/penv/bin/pio run`，8s 左右 `[SUCCESS]` 即过。
- 上传：`~/.platformio/penv/bin/pio run -t upload`；串口监视：`~/.platformio/penv/bin/pio device monitor`。波特率 `921600`，上传失败降 `upload_speed=115200`。

## 配置与密钥

- `src/config.example.h` 是模板；真实配置 `src/config.h` 被 `.gitignore` 忽略，**永不提交**（内有 WiFi 密码 + `sk-` Key）。
- 当前 `config.h` 用代理 host（`OPENCODE_API_HOST/PATH`），与模板默认值不同，改请求地址只动 `config.h`。
- `cert.h` 是 GTS Root R4；`VERIFY_TLS_CERT=0` 仅应急。

## 目录与边界

- `src/main.cpp`：界面 + `handleFetch` 轮询（`POLL_INTERVAL_MS` 5min，`FAST_RETRY_MS` 30s 仅无数据时）。
- `src/OpenCodeGoClient.h`：用量请求 + 重试。`DynamicJsonDocument(2048)`，`http_code==0`/空 body 视为可重试；仅 401/403 直返；退避 `min(10s*attempt,30s)+rand(0-1s)`，200ms 分片 `yield()`。
- `lib/sd2-common` 是 git submodule，地址为自有 fork（`git@github.com:boypt/sd2-common.git`），含 WiFi/NTP/休眠/背光/HTTPS/TFT 引脚（`platformio/tft_setup.h` 引脚已固化，无 CS）。本工程问题优先改 `src/`；确需改公共库时进子模块改、另行 push，再回本工程 bump gitlink。
- `platformio.ini` 用 `extra_configs = lib/sd2-common/platformio/sd2-st7789.ini` 合并公共 env，本工程只加字体宏和 `lib_deps`。

## 已知坑

- 黑屏先查构建：build flags 禁用 `-I${PROJECT_DIR}/...`（Windows 下反斜杠被 SCons 当转义吃掉，`tft_setup.h` 静默不加载，TFT_eSPI 回退默认 User_Setup.h）。子模块已改为相对路径 `-Ilib/sd2-common/platformio`。改 `tft_setup.h` 后必须 `run -t clean` 全量重编（SCons 不会因 `__has_include` 依赖重编 TFT_eSPI.cpp）；`run -v` 日志见 `LOAD_FONT7 redefined` 警告即仍在回退默认配置。
- SD2 1.54" 240x240 方屏驱动用 `ST7789_2_DRIVER`（精简初始化含 INVON），`ST7789_DRIVER` 全量初始化不适配该面板会黑屏。
- 找不到 `/dev/ttyUSB0`：Windows 侧用 `usbipd` 绑定并 `attach --wsl` 串口设备，WSL2 内 `sudo modprobe usbserial ch341`，再 `ls /dev/ttyUSB*` 确认。
- 单台 WiFi 连不上但扫描正常（目标 AP 可见、assoc 卡 status=7、偶发连接中复位）：多为旧 GeekMagic 固件残留的 PHY 校准/SDK config 区损坏，`run -t erase` 全片擦除后重刷即恢复。erase 后首次 upload 常报 "Invalid head of packet (0x46)"，重试即可；monitor 占着 COM 口时 upload 报 PermissionError，先关 monitor。
- `lib/sd2-common` 换行符已用其目录下 `.gitattributes`（`* text=auto`，入库 LF）根治。若仍见全文件 dirty，先用 `git -C lib/sd2-common diff -w --stat` 确认是否为空（空即纯噪声）；提交主工程时只加本工程文件，别顺手收子模块噪声。
- `readHttpResponse` 依赖 `Connection: close` 收包；chunked 截断会变 JSON 解析失败走重试，不要调小 `timeout_ms`（默认 15000）。
- `fetchOpenCodeGoUsage` 阻塞最长约 65s，属预期；`loop` 阻塞期间不断 WiFi 状态机，重试间隔已用分片等待。
- 无测试/CI；`pio run` 编译通过即为验证。RAM ~38% / Flash ~44%，加库注意余量。

## 提交

- 信息风格：`feat:` / `fix:` / `docs:` / `refactor:` + 中文简述。勿 `push`（除非明确要求）。
