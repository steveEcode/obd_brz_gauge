# App 对接协议

[English](APP_PROTOCOL.en.md) | 简体中文

设备在 OTA 模式下暴露的 WiFi HTTP API（App 用的 `/ota/*`）、网页 USB 管理协议，
以及三连表之间的 ESP-NOW 帧格式。用户侧的烧录与升级操作见 [FLASH.md](FLASH.md)。

## OTA 模式下设备暴露什么

仪表 **版本页 → OTA 按钮** 进入 OTA 模式后：

- WiFi SoftAP：SSID `OBD-Gauge-OTA-XXXX`（MAC 后两字节）、密码 `88888888`、
  IP `192.168.4.1`、端口 80、信道 1，同时只接受 1 个客户端
- 每次会话生成随机 16 位 hex token，用于 HTTP 鉴权
- 浏览器打开 `http://192.168.4.1/` 是设备页（车型、主题 sidecar、SD / `/config` 文件、
  coredump、导入日志），走下文的 `/api/v1`。设备页只在 OTA 模式下存在；
  不带 `text/html` 的 `GET /`（手机联网探测）返回 204
- 10 分钟内没有上传、设备页或 `/api/v1` 请求，服务自动关闭

进入 OTA 模式会彻底释放蓝牙（视频与主题资源也一并释放给 SoftAP），设备不提供任何
BLE OTA / 清单服务；离开 OTA 模式即重启。

## 设备清单（`GET /ota/info`）

App 刷写前用它做硬件匹配校验：

- 免鉴权，OTA 模式下可用
- 载荷：UTF-8 JSON（上限 512 字节，只返回 App 实际使用的字段）

```json
{
  "device": {
    "board": "Waveshare ESP32-S3-Touch-LCD-1.85",
    "variant": "obd_brz_gauge",
    "lcd": "ST77916",
    "screen": { "w": 360, "h": 360, "bpp": 16 },
    "flash_mb": 16,
    "psram_mb": 8,
    "ota_slots": 2,
    "bootmedia_slots": 1,
    "bootmedia_format": 1
  },
  "firmware": {
    "build_tag": "main-124-abcdef123456",
    "branch": "main",
    "count": 124
  }
}
```

`build_tag` 格式为 `<分支>-<提交数>-<短哈希>`，编译时由 `main/CMakeLists.txt` 从 git
注入。App 用 `firmware.count` 与 `latest.json` 比较判断新旧。
硬件字段与所选 release 不一致时 App 应拒绝刷写。

网页与 App 统一读取 `PUBLIC_BASE_URL/firmware/esp32/latest.json`（可通过 `VITE_RELEASE_BASE_URL` 配置目录）。兼容发布端 `device/files` 与旧 `hardware/firmware` 清单；下载和缓存校验 SHA256 与字节数。网页完整刷写先校验全部四个文件，再写入；App 上传前重新读取当前设备硬件。发布清单对应源码提交，后续产物提交不改变设备 build tag。

## WiFi OTA HTTP API

App 连上设备 SoftAP 后走 HTTP（所有路径都应答 OPTIONS 预检，CORS 头给 `/ota/*` 及下述车型社区安装白名单）。
先调免鉴权的 `GET /ota/discover` 取本次会话的 token：

```json
{"ssid":"OBD-Gauge-OTA-ABCD","password":"88888888","ip":"192.168.4.1","token":"a1b2c3d4e5f6a7b8","port":80}
```

**公共请求头**：`X-OTA-Token: <GET /ota/discover 返回的 16 位 hex>`（`/ota/discover`、`/ota/info` 免鉴权）

| 端点 | 方法 | 说明 |
|------|------|------|
| `/ota/discover` | GET | 局域网发现，返回 SoftAP 信息与 token |
| `/ota/info` | GET | 设备清单 JSON（格式见上）|
| `/ota/status` | GET | `{"state":"ready\|receiving\|done\|error","received":N,"expected":M}` |
| `/ota/firmware` | POST | 固件分块上传 |
| `/ota/bootmedia/prepare` | POST | 开机动画预检（头 `X-OTA-Size`、`X-OTA-Manifest-Size`），返回 `{"ok":true,"storage":"sd","free":N,"psramFree":N,"largestBlock":N}`，不擦除 |
| `/ota/bootmedia` | POST | 开机动画分块上传 |
| `/ota/bootmedia/upload` | POST | 旧版网页串口上传路径别名；长度沿用本会话最近一次成功的 prepare |
| `/ota/theme/prepare` | POST | 主题预检（头 `X-OTA-Size`），返回 `{"ok":true,"storage":"sd","partitionSize":N,"psramFree":N,"largestBlock":N}`，不擦除 |
| `/ota/theme` | POST | 主题分块上传 |
| `/ota/theme/erase` | POST | 删除当前选中主题的 `/OBD/THEMES/<key>/` 目录并清除选择；返回 `{"ok":true,"deleted":true|false}`，下次开机使用内置默认主题。需要 SD 卡挂载；其他主题、公共开机动画不受影响 |
| `/ota/vehicles/clear` | POST | 清空所有已安装车型的 JSON/PID 配置与 SD 镜像，返回 `{"ok":true,"cleared":true,"pending":"obd2.generic"}`；需要 SD 卡挂载，保留内置通用车型、模板、主题和动画 |
| `/ota/coredump` | GET | 下载已保存的 coredump 镜像（`application/octet-stream`，`coredump.bin`）；没有 coredump 时返回 204 No Content、空 Body |
| `/ota/coredump/erase` | POST | 擦除 coredump 分区，返回 `{"ok":true}` |

**分块上传**（`/ota/firmware`、`/ota/theme`、`/ota/bootmedia`）：每个请求都带
`X-OTA-SHA256`（整个负载的 64 位 hex）、`X-OTA-Size`（负载总字节数）、
`X-Offset`（本块起始偏移，缺省 0）、`X-Last`（`1`=最后一块）；`/ota/bootmedia`
另加 `X-OTA-Manifest-Size`。Body 为本块数据（`Content-Type` 含 `base64` 时按 base64 文本解码）。
旧版网页串口上传器使用 `/ota/bootmedia/upload`，并只在 prepare 时发送长度；设备从同一会话
最近一次成功的 `/ota/bootmedia/prepare` 读取长度，其余分块、SHA 校验和 SD 安装行为相同。
`X-OTA-Manifest-Size: 0` 表示完整 OBV/OBV2 文件；该值也可由兼容路径沿用 prepare。

App 的主题、开机动画、车型安装/启用和主题删除每次操作都重新发现 Wi-Fi 会话，优先 Wi-Fi；Android 失败后释放网络绑定并回退原生 USB，iOS 仅用 Wi-Fi。USB `hello` 的 `ota_active` 为 `true` 时，先发送 `ota.stop` 停止 HTTP 服务并清理未完成上传，再开始 USB 操作。客户端必须校验 JSON 对象、`ok === true` 和中间块的准确 `nextOffset`；协议错误立即停止，不按网络故障重试。主题/动画安装成功后由固件自动重启，App 不再发送第二次重启。主题删除与车型启用在业务成功后单独请求重启；重启失败只提示手动重启，不重放业务操作。

- `X-Offset: 0` 开始一次新上传；中间块返回 `{"ok":true,"nextOffset":N}`
- 偏移、总长或 SHA 与进行中的上传对不上：`unexpected chunk offset`
- 某块网络接收失败（`receive failed` / `no memory`）：设备回滚到该块起点，原样重发该块即可；SD 写入、同步、回退定位或摘要处理失败时终止本次会话，排查错误后从偏移 0 重新上传
- 收齐后校验，失败返回 `size mismatch` / `sha mismatch`，需从偏移 0 重传
- 固件收齐校验后写入非活动 OTA 槽；主题/动画在接收时流式写 SD 临时文件，校验后安装。
  最后一块等安装完成后回复 `{"ok":true,"message":"<类型> received, installing"}`，约 1 s 后重启。
- 安装失败返回 `install failed`，可从偏移 0 重试。SD 传输阶段不替换旧文件；单文件替换先将旧文件改名为 `<目标>.obd-bak`，新文件改名失败时尝试恢复。若恢复也失败，备份保留且不会被覆盖，需检查 SD 卡并手动恢复。FAT 元数据操作不承诺掉电原子性。

各上传端点的 Body：

- `/ota/firmware`：固件二进制，不超过 OTA 槽大小（3 MB）。
- `/ota/theme`：与 USB 命令 `5` 相同的 `OBDT` 主题流（客户端解包 ZIP，包含 `theme.bin`、`project.json` 和资源），受 SD 可用空间与格式限制；临时目录验证通过后通过恢复日志替换并保存 NVS 选择。上传或验证失败保留当前主题，不提前删除。兼容旧客户端直接上传 `theme.bin`，按文件头分流；无效的 `OBDT` 包不会回退到旧版导入。
- `/ota/bootmedia`：`[manifest][bin]`，`X-OTA-Size` 为总长，manifest ≤4 KB；
  SD 文件 `/OBD/BOOT/bootmedia.raw` 中 manifest 补 `0xFF` 到 4 KB，SHA 不包含填充。
  manifest 长度为 `0` 时为完整 OBV/OBV2，写入 `boot.obv.part`，同步并验证后才提交为 `boot.obv`。新动画提交成功后清理其他旧格式；清理失败记录警告，保留新文件并返回安装成功。

上传总长度、manifest 长度和偏移头部使用十进制无符号 32 位整数；显式空值、符号、非数字字符、溢出或超长值返回 400。`X-Offset` 缺省为 0；`/api/v1` 的 `X-OTA-Size` 缺省为请求体长度。

**错误响应**：`/ota/*` 出错为 HTTP 400 + `{"error":"<原因>"}`，如 `unauthorized`、
`missing headers`、`invalid headers`、`SD card required`、`not enough SD space`、`install failed`。

**主题库占用**：选择任务运行时，`/ota/theme/prepare`、`/ota/theme/erase` 及
`/ota/theme`、`/ota/bootmedia` 首块返回 `theme library busy`。

**媒体必须用 SD**：prepare 与上传都要求已挂载 SD，空闲空间至少为文件大小（动画含填充）
加 64 KB 余量。`storage` 固定为 `"sd"`；主题兼容字段 `partitionSize` 和动画 `free`
表示预检时一次采样的 SD 空闲空间减 64 KB（不足时为 0，截到 `UINT32_MAX`），不预留空间，也不表示 Flash 容量；`psramFree` /
`largestBlock` 仅供诊断，不要求整个媒体包能装入 PSRAM。动画上传不改变主题；只有新动画提交成功后才清理公共目录中的其他旧格式。

## USB 网页管理、主题与开机动画

网页对仪表的操作统一使用 ESP32-S3 原生 USB：固件刷写使用 ROM 下载器，主题/开机动画使用以下媒体事务，车型安装、启用、主题删除和重启使用管理 RPC。无需输入设备地址或 token、连接热点或开启 OTA；请退出仪表 OTA 模式后操作。社区浏览、登录、下载与发布仍通过网络。App WiFi API 保持兼容。

管理 RPC 使用命令 `6` BEGIN_RPC：40 字节负载为 JSON 长度 `u32`（1–16448）、保留 `u32=0`、JSON SHA-256 32 字节；后续复用 DATA、END、ABORT。请求在内存接收，不持有 SD 接收锁，业务函数自行管理存储锁。END 回复在状态 `u8`、接收长度 `u32` 后附加 UTF-8 JSON，CRC 覆盖整个负载；普通媒体回复为 5 字节，OBV2 验证失败可附错误文本。传输成功但业务失败时，传输状态仍为 `0`，JSON 为 `{"ok":false,"error":"..."}`，可附 `issues`。`theme.erase` 因开机主题准备、显示锁或存储锁暂时忙碌而失败时，附带 `retryable:true`；无卡和其他删除错误不可重试。新固件的失败回复附带 `stage`（`prepare` 或 `delete`）和 `code`（`esp_err_to_name` 错误码），无卡会明确报告 `SD card is not mounted`。网页对明确可重试的业务失败发起新 RPC，每隔 500 毫秒重试，清理等待预算为 2 分钟，并显示等待开机加载提示；兼容未提供 `retryable` 字段且错误恰为 `theme delete failed` 或 `display busy` 的旧固件。旧版笼统错误持续失败时到期停止并提示升级固件诊断；明确 `retryable:false` 或其他业务错误立即停止。不将通信失败作为新删除操作重试。相同 END 的传输重试返回缓存 JSON，不重复执行操作。旧固件拒绝命令 `6` 时返回状态 `1`，网页提示升级。

| 请求 JSON | 成功回复 | 生效 |
|----------|----------|------|
| `{"op":"hello"}` | `{"ok":true,"usb_management":1,"ota_active":false}` | 确认协议与 OTA 状态，不修改设备 |
| `{"op":"ota.stop"}` | `{"ok":true}` | 停止 HTTP OTA 并清理未完成上传，允许后续 USB 接管；不重启。启动/停止并发时返回 `ok:false`、`retryable:true`，不等待 LVGL 调用方 |
| `{"op":"vehicle.install","profile":{...}}` | `{"ok":true,"id":"...","warnings":0,"issues":[]}` | 校验并安装，不启用、不重启 |
| `{"op":"vehicle.activate","id":"..."}` | `{"ok":true,"pending":"..."}` | 下次启动启用；支持 `obd2.generic` |
| `{"op":"vehicle.clear","confirm":true}` | `{"ok":true,"cleared":true,"pending":"obd2.generic"}` | 需显式确认；清空已安装车型 JSON 与 SD `/OBD/CFG/vehicles` 镜像（含当前车型），需要已挂载 SD；保存下次启动使用内置车型，保留本次运行的编译态指针；不自动重启。删除失败返回 `ok:false`，可能已有部分文件删除；CATALOG / IMPORT / EXPORT 模板及主题/动画保留 |
| `{"op":"theme.erase"}` | `{"ok":true,"deleted":true}` | 释放主题页面，删除当前 SD 主题目录和选择；`false` 表示未找到主题包 |
| `{"op":"reboot"}` | `{"ok":true}` | 先回复，再延时重启 |

点击“删除当前主题”后网页单独请求重启，恢复内置默认主题；安装本地或社区主题时，网页先校验新主题包，再用 `theme.erase` 检测并清理当前主题，没有主题时直接继续，收到成功确认后上传并启用新主题，期间不单独重启。其他主题和公共开机动画保留。网页需要 Chrome/Edge 的 Web Serial 和 HTTPS 或 localhost；以下 USB 应用态协议要求 ESP32-S3 原生 USB，CH340/CP210x 只用于固件刷写。

USB 刷写页保留“连接设备 → 选择开机动画 → 上传”流程。选择动画后，网页先下载 `[manifest][bin]`，
再将设备从 ROM 下载模式复位到应用态，通过 **ESP32-S3 原生 USB Serial/JTAG** 分帧发送；固件沿用
`upload_sink` 做 SHA-256 校验，并将文件暂存、安装到 SD `/OBD/BOOT/bootmedia.raw`，成功后重启。
需要已挂载且空间足够的 SD 卡。此串口协议不支持 CH340、CP210x 等外置 USB-UART 桥；它们仍可用于
原有固件刷写，但不能执行此动画上传。WiFi `/ota/bootmedia` 上传路径保持兼容。

帧格式为小端：`OBDS`、版本 `1`、命令、序号 `u16`、负载长度 `u32`、负载 CRC-32 `u32`、负载。
命令 `1` BEGIN 负载为总长度 `u32`、清单长度 `u32`、包 SHA-256 32 字节；命令 `2` DATA 负载以偏移
`u32` 开头，后接最多 4096 字节；命令 `3` END 完成校验并安装；命令 `4` ABORT 清理临时文件。
回应使用命令值加 `0x80`，负载为状态码 `u8` 和已接收长度 `u32`。客户端按序号等待回应，超时重发相同帧；
固件对最近一帧做幂等应答。状态码 0 表示成功，非零表示拒绝或校验失败。

主题 ZIP 由网页解包（支持 STORE / DEFLATE，不由固件解压）。命令 `5` BEGIN_THEME 使用同样的
40 字节 BEGIN 负载，清单长度必须为 `0`，SHA-256 覆盖完整主题流；后续复用 DATA、END、ABORT。
旧固件对命令 `5` 返回状态 `1`，网页提示先升级固件。状态 `2` 表示参数、SD 或空间不满足要求，
`6` 表示验证或安装失败，`8` 表示帧 CRC 错误可重发，`9` 表示 OTA 或 SD 存储任务正忙。闲置上传会话 30 秒后清理。
回应 CRC 覆盖已填入接收长度的完整 5 字节负载。

OBV2 开机动画使用命令 `7` BEGIN_OBV：同样的 40 字节负载，总长度为完整 `.obv` 长度（≤256 MiB），
清单长度必须为 `0`，SHA-256 覆盖整个文件，后续复用 DATA、END、ABORT。先写
`/OBD/BOOT/boot.obv.part`；END 先校验 SHA，再执行 `fflush` / `fsync`，然后重新打开文件，校验头/索引/时间轴、全部帧头和数据 CRC，并完整解码首秒帧，
成功后才提交为 `boot.obv`，删除旧的 `bootmedia.raw` 和 `boot.jpg`，再重启；
命令 `1` 上传 legacy raw 时则在新文件提交后删除旧的 `boot.obv` 和 `boot.jpg`。
两种网页 USB 上传都会保留新文件并清理其他旧格式；传输中断、文件同步失败或校验失败时保留已有动画。SD 同步失败返回状态 `6` 且跳过文件验证；旧文件清理失败返回状态 `6`，保留已提交的新文件。状态 `10` 表示 OBV2 文件验证失败，
在状态 `u8`、接收长度 `u32` 后附 UTF-8 错误文本；回应 CRC 覆盖完整负载。END 等待上限为 120 秒。
旧固件不支持命令 `7` 时返回状态 `1`，网页提示升级。网页 H.264 MP4 在浏览器本地转换为 Q90 JPEG/OBV2，
ESP32 不负责转码；格式和播放预算见 [THEMES.md](THEMES.md)。

USB 接收从 BEGIN 到校验、提交期间持有存储任务锁，后台导入扫描不能删除正在写入的 `.part` 文件。
DATA 写入发生不可恢复的存储或摘要错误时返回状态 `5`，并中止会话、释放存储锁；客户端排查错误后需重新 BEGIN。
中止、接收超时或失败时关闭临时文件并释放锁；主题流提交后先释放存储锁，再进入主题安装器，
遵循 LVGL → storage 的锁顺序。媒体 BEGIN（命令 `1`、`5`、`7`）获取存储锁最多等待 1 秒；
网页在上传开始时收到状态 `9`，每隔 500 毫秒重试，等待预算为 30 秒，以兼容开机主题读取与后台 SD 任务。
忙碌回应已明确拒绝本次 BEGIN，因此重试使用新序号，避免命中固件缓存的忙碌回应；
响应超时或 CRC 错误的传输重发仍使用原序号。预算耗尽后报告存储忙，不发送资源数据；请确认已退出 OTA 模式。

主题流为小端：`OBDT`、版本 `u8=1`、保留 `u8=0`、文件数 `u16`，随后每个文件依次为
名称长度 `u16`、文件长度 `u32`、ASCII 文件名、文件内容。最多 128 个文件，上传流最多 256 MiB；
必须含 `theme.bin` 和 `project.json`，其余仅允许 `<key>.obv`，拒绝重复名称和路径穿越。
网页将 gaugeproj 命名为 `project.json` 传输；固件流式提取规则并存储为 `sidecar.json`，核对主题 ID。
全部写入并验证后，释放旧主题页面，记录目录备份日志，替换主题目录，保存 NVS 选择，移除日志后回应 END 并重启。
错误会尝试回滚；启动时在加载主题前恢复未提交的日志。主题资源保存在 `/OBD/THEMES/<theme-key>/`，不写媒体 Flash 分区。
需要至少 `2 × 总长度 + 1 MiB` 可用 SD 空间，用于接收流、解出的文件与余量；旧主题在提交前保留。

**安全性**：token 每会话随机；SoftAP WPA2-PSK；`/ota/*` 负载全部 SHA256 校验。
主题打包格式见 [THEMES.md](THEMES.md)。

## 设备 API（`/api/v1`）

此 HTTP API 为旧网页和兼容客户端保留。当前网页管理操作使用 USB RPC；App 仍使用 `/ota/*`。

设备页使用的 API，与 `/ota/*` 共用 SoftAP 和 token：每个实际 API 请求都要带 `X-OTA-Token`。仅 `POST /api/v1/vehicles`、
`POST /api/v1/vehicles/activate`、`POST /api/v1/reboot` 向浏览器开放 CORS（含错误响应）；
复用全局 OPTIONS 处理器，只允许 `POST, OPTIONS` 与 `Content-Type, X-OTA-Token` 请求头，
不允许跨源凭据。其余 `/api/v1` 端点不开放 CORS，鉴权不变。

网页车型社区通过 USB 安装、查看固件校验报告，再独立确认启用和重启，安装不会自动切换车型。
旧 HTTP 客户端仍可能受混合内容/局域网策略限制。固件也会在开机或插卡后自动处理 SD `/OBD/IMPORT/` 中的文件。社区配置不是实车认证，
请在停车时核对车型、请求与解码；同 ID 安装可能替换旧配置，建议先备份。

成功返回 `{"ok":true,...}`；失败统一为 `{"ok":false,"error":"<code>","detail":"<说明>"}`——
token 缺失或错误 401 `unauthorized`，路径未知 404 `not_found`，方法不对 405 `method_not_allowed`，
其余按原因映射为 400 `invalid_arg`、404 `not_found`、409 `invalid_state` / `not_supported`、
413 `too_large`、500 `no_memory` / `failed`。

| 端点 | 方法 | 请求 | 返回 / 错误 |
|------|------|------|-------------|
| `/api/v1/info` | GET | – | `firmware`、`board`、`partitions`、`sd`、`config`、`vehicle`、`theme`、`coredump`、`uptime_s`、`heap`、`ssid` |
| `/api/v1/vehicles` | GET | – | `{ok,active,state,safe_mode,last_error,vehicles[{id,name,rev,builtin,active,pending,state}]}` |
| `/api/v1/vehicles` | POST | Body：车型 JSON（≤16 KB）| 只安装不激活：`{ok,id,errors,warnings,issues}`；400 `empty_body`、413 `too_large`、422 `invalid_profile`（附 `issues`）|
| `/api/v1/vehicles/<id>` | GET | – | 存储的车型 JSON 原文；404 `not_found` |
| `/api/v1/vehicles/activate` | POST | `{"id":"..."}` | `{"ok":true,"pending":"<id>","reboot_required":true}`；400 `bad_id` |
| `/api/v1/vehicles/reset` | POST | – | 同上，`pending` 为 `obd2.generic`；同时取消首启迁移待激活的车型 |
| `/api/v1/vehicles/rollback` | POST | – | 同上，`pending` 为上一个车型 |
| `/api/v1/theme/sidecar` | POST | 分块上传 sidecar 或完整 `.gaugeproj.json`（≤32 MB）| `{ok,size,report}`；保存需要 SD：409 `sd_unavailable`、507 `sd_full`；422 `invalid_sidecar` |
| `/api/v1/theme/sidecar` | DELETE | `?theme=<id>`（≤64 字符，省略 = 当前主题）| `{"ok":true}`；无 SD 返回 409 `sd_unavailable` |
| `/api/v1/fs/put` | PUT / POST | `?path=`，分块 Body | `{ok,path,size}`；400 `bad_path`、409 `sd_unavailable` / `config_unavailable`、507 `sd_full` / `config_full` |
| `/api/v1/fs/list` | GET | `?path=`（默认 `/sdcard/OBD`）| `{ok,path,entries[{name,dir,size}],truncated}`，最多 200 项；404 `not_found` |
| `/api/v1/logs` | GET | `?bytes=`（默认 8192，最大 32768）| `IMPORT.LOG` 末尾（`text/plain`）；无 SD 时 409 `sd_unavailable` |
| `/api/v1/sd` | GET | – | `{ok,state,total,free,freq_khz}`，`state` 为 `absent\|mounted\|error\|ejected\|disabled` |
| `/api/v1/time` | POST | `{"epoch":<unix 秒>}` | `{ok,epoch}`；早于 2020-09-13 或晚于 2100 年返回 400 `bad_request` |
| `/api/v1/reboot` | POST | – | `{"ok":true}`，约 0.5 s 后重启 |

- 车型 id 最长 31 字符，只允许 `A-Z a-z 0-9 . _ -`；激活 / 重置 / 回滚都在下次开机生效，设备不会自动重启
- `fs/put` / `fs/list` 的 `path` 只能在 `/sdcard/OBD/...` 或 `/config/...` 下（相对路径按
  `/sdcard/OBD` 解析），禁止 `..`、`//`、`\`；`/config` 单文件 ≤256 KB

`/api/v1/info` 的 `partitions.resource_storage` 为 `"sd"`；旧可写媒体字段
`theme_size`、`bootmedia_size`、`bootmedia_free` 为 0，表示 Flash 媒体缓存退役，
不是物理分区大小为 0。`bootmedia_video` 表示当前主题或公共 SD 动画文件存在，
不代表内容已校验可播放。物理布局仍见 [FLASH.md](FLASH.md)。

**分块上传**（`theme/sidecar`、`fs/put`）：头 `X-OTA-Size`（总长，缺省取 Content-Length）、
`X-Offset`、`X-Last`，`X-OTA-SHA256` 可选（给了就必须是 64 位 hex，并在末块校验）。

- `X-Offset: 0` 开始新上传（丢弃未完成的旧上传）；中间块返回 `{"ok":true,"nextOffset":N}`
- 偏移不连续返回 409 `bad_offset`，`detail` 给出期望的偏移
- 某块接收失败会回滚到该块起点，原样重发即可；末块校验失败返回 400 `sha_mismatch` / `size_mismatch`
- 同一时刻只有一个 `/api/v1` 上传；开始 `/ota/*` 上传会丢弃它

## 开机动画编辑规则（App 端）

- 画布 360×360，比例锁 1:1；建议圆形预览遮罩（圆屏四角裁切）
- 手机端先裁切起止时间，再编码上传
- 动画包必须有 SD 卡承载，空间需包含 4 KB 清单填充和 64 KB 余量；不受旧媒体分区容量限制。
- App 可保存多个草稿；上传的 legacy 动画替换 SD `/OBD/BOOT/bootmedia.raw`。
- VIDEO 模式只读取公共 SD 开机目录
  `/OBD/BOOT/boot.obv` → `/OBD/BOOT/boot.jpg` → legacy `/OBD/BOOT/bootmedia.raw`。
  开机动画与当前主题无关；网页 / App 旧上传路径保持不变，仍写入公共 `/OBD/BOOT/`。

## 三连表 ESP-NOW 帧

主表与从表在 WiFi 信道 1 上用 ESP-NOW 广播通信。所有多字节字段小端，所有帧以 magic `0x4F42` +
版本字节开头。v5 是旧格式，v6 是当前格式，两者并存，新旧固件可以混用、逐台升级。

| 帧 | 方向 | 长度 | 内容 |
|----|------|------|------|
| v5 presence | 从 → 主 | 4 B | magic、版本 `5`、位置 1..3；每 500 ms |
| v6 HELLO | 从 → 主 | 8 / 10 / 15 B（扩展需求） | 见下表；每 500 ms，与 v5 presence 一起发 |
| v5 控制 | 双向 | 6 B | magic、版本 `5`、cmd（`1` 联动测试开始 / `2` 同步转速告警阈值）、u16 参数 |
| v5 DATA | 主 → 从 | 47 B | 旧格式固定结构，见下 |
| v6 DATA | 主 → 从 | 38 + 6n + 2 B（≤250）| 见下表 |
| v6 TPMS | 主 → 从 | 142 B | type `3`；有订阅时每 500 ms |

主表每 100 ms（联动测试期间 20 ms）广播一次快照。只有至少一台从表在线、且所有在线从表在最近
2 s 内都发过 caps bit0 置位的 HELLO 时才发 v6，否则发 v5；切换时日志打印 `Broadcast format -> vN`。
旧固件按长度（4 / 6 / 47）分派、再要求版本 = 5，因此会丢弃 v6 帧，v6 的任何长度都不会与之冲突。
控制帧固定用 v5，混合固件下阈值同步和联动测试照常工作。从表绑定主表 MAC 后只接收该 MAC 的帧。

**v6 DATA**：

| 偏移 | 长度 | 字段 |
|------|------|------|
| 0 | 2 | magic `0x4F42` |
| 2 | 1 | version `6` |
| 3 | 1 | type `1`（DATA）|
| 4 | 1 | flags：bit0 主表 `sys.obd_link == 2`，bit1 联动测试中，bit2 主表 `rpm_warn_linked_en` |
| 5 | 1 | frag = (index << 4) \| count，count 1..15 |
| 6 | 1 | n_rec（≤35）|
| 7 | 1 | gear，int8：-1 = R、0 = N、1..8，127 = 无效 |
| 8 | 4 | seq（同一快照的各分片相同）|
| 12 | 4 | profile_hash |
| 16 | 4 | std_valid：bit i = num_id `0x0001+i` 在主表有效 |
| 20 | 4 | veh_valid：bit i = num_id `0x0100+i` 在主表有效 |
| 24 | 1 | sweep_step |
| 25 | 1 | intro_step |
| 26 | 12 | name，NUL 填充 |
| 38 | 6n | records `{u16 num_id, i32 value}`，内部单位 |
| 末尾 | 2 | CRC-16/CCITT-FALSE，覆盖之前所有字节 |

- 每个分片都重复完整帧头，从表逐片独立应用，不需要重组
- 只发有效值；有效位为 0 的槽位在从表置无效
- num_id 与内部单位以 `contracts/signals.json` 为准；`sys.*`（`0x0080`–`0x00BF`）不传输、收到也忽略；
  未知 id 忽略；挡位只走 gear 字节
- `veh.*` 按车型声明顺序编号，只有 profile_hash 与从表自己当前车型一致时才应用；
  profile_hash = FNV-1a 32（车型 id 字符串 + `0x00` + `rev` u16 LE）

**v6 HELLO**：

| 偏移 | 长度 | 字段 |
|------|------|------|
| 0 | 2 | magic `0x4F42` |
| 2 | 1 | version `6` |
| 3 | 1 | type `2` |
| 4 | 1 | 位置 1..3 |
| 5 | 1 | caps：bit0 = 能解码 v6 DATA；bit1 = 附加本地 OBD 需求；bit2 = 扩展数据源订阅及 TPMS 接收 |
| 6 | 2 | 保留，填 0（接收方接受长度 ≥ 8）|
| 8 | 2 | caps bit1 置位时的本地 OBD 需求掩码；bit i 对应 `SIG_OBD_*` |
| 10 | 4 | caps bit2 置位时的 `veh.*` 需求掩码，bit i 对应车型第 i 个扩展槽位 |
| 14 | 1 | caps bit2 置位时的胎压轮位需求，bit0..3 = FL / FR / RL / RR，高四位为 0 |

从表把当前页面绑定、数值视频和规则需要的所有可共享数据源放进需求；切页完整替换，不累积旧页面需求。主表在 2 s 需求窗口内合并所有从表掩码，再一次性向串行 OBD 轮询器请求所需标准信号。`veh.*` 保留全部有效值广播与 profile_hash 检查，不改变车型采样策略；`sys.*` 不注册或转发（`sys.obd_link` 仍按 DATA flags 表示主表 OBD 链路）。caps bit2 要求 bit1 且长度至少 15 B；缺失扩展字段的帧拒绝，旧 HELLO 的未声明需求按 0。旧主表忽略附加字段，旧从表继续使用 8 / 10 B HELLO，但不能接收胎压扩展。

**v6 TPMS（type 3，142 B）**：与 DATA 独立协商，只要最近 2 秒内有扩展 HELLO 订阅轮位，主表每 500 ms 发送一次所有已订阅轮位的完整快照；没有订阅时不发送，不受其他旧从表导致的 v5 DATA 回退影响。旧固件忽略此帧。主表未停留在胎压页也会转发本机接收到的数据；不增加 ELM 请求。

| 偏移 | 长度 | 字段 |
|------|------|------|
| 0 | 2 | magic `0x4F42` |
| 2 | 1 | version `6` |
| 3 | 1 | type `3` |
| 4 | 4 | seq，取同轮主表 DATA 快照序号 |
| 8 | 1 | 轮位掩码，bit0..3 = FL / FR / RL / RR；1..15 |
| 9 | 3 | 保留，填 0 |
| 12 | 128 | 四个 32 B 轮位记录，固定 FL / FR / RL / RR 顺序 |
| 140 | 2 | CRC-16/CCITT-FALSE，覆盖之前所有字节 |

每个轮位记录的偏移相对其 32 B 起点：

| 偏移 | 长度 | 字段 |
|------|------|------|
| 0 | 1 | flags：bit0 bound，bit1 received；received 必须同时 bound |
| 1 | 3 | 传感器六位 HEX 编号的 u24 LE 值；包括前导零 |
| 4 | 4 | pressure_pa，u32 |
| 8 | 2 | temperature_c，i16 |
| 10 | 2 | voltage_mv，u16；估算电量在接收端从电压计算 |
| 12 | 1 | state |
| 13 | 1 | version |
| 14 | 1 | ble_version_code |
| 15 | 4 | RSSI，i32 |
| 19 | 6 | MAC，仅诊断，不用于胎压绑定 |
| 25 | 1 | addr_type |
| 26 | 4 | age_ms，主表距原始传感器广播的时间，u32 饱和 |
| 30 | 2 | 保留，填 0 |

未订阅或未绑定轮位的记录全部填 0；bound 但未 received 时只写编号与 flags，其余填 0。从表由维护任务统一应用，接收回调只入队，并重新检查绑定 MAC 和队列报文的 2 秒寿命。只接受递增 seq（支持 u32 回绕）；连续 2 秒未应用新快照后允许主表重启的新序号。从表依据接收时间继续增加 age_ms，年龄达到 120000 ms 时测量值无效；转发心跳不刷新传感器年龄。连续 2 秒没有新 TPMS 快照、换绑或停止链路会清空从表胎压视图，不回退到本机旧绑定。

**v5 DATA**（47 B，旧单位）：偏移 0 magic、2 版本、3 flags（位定义同 v6）、4 seq u32、8 rpm u16、
10 车速 u8、11 sweep_step、12 intro_step、13 水温、15 进气温、17 油温、19 油压、21 增压、
23 **退役槽位**、25 负载、27 节气门、29 电压 u32、33 空燃比、35 name 12 B（13–33 除电压外均为 i16）。
偏移 23 原为刹车温度，现在固定写 -1000、解码不读，**永不复用**。

从表 2 s 收不到主表数据即判定断链，`obd.*` / `veh.*` 全部置无效。
v5 帧不带挡位和 `veh.*`，所以只有主表和所有从表都是新固件时，从表才显示挡位。
