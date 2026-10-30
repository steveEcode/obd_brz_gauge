# 车型适配

铃木 Swift / 雨燕 KWP 专用初始化兼容：显式协议 4/5 的配置可在 `bus.init` 中使用 `8111F1`、`8101F1`、`8001F1` 三种 `ATSH` 请求头，并以 `2100` 检查连接和读取数据块。此为引擎兼容能力，未新增内置/目录车型，未验证具体年份；标准 OBD2、CAN 车型及旧配置行为不变，见 [PID_GUIDE.md](PID_GUIDE.md)。

[English](VEHICLES.en.md) | 简体中文

固件只内置 **`OBD2 Generic`**（`obd2.generic`，标准 SAE J1979）。其余车型是
**JSON 车型档案**（schema `skygauge.vehicle/1`），通过网页或配套 App（经
`/api/v1/vehicles` 接口）安装/激活，不编译进固件，新增
车型不需要改固件代码。

```
默认行为 = 固件内置 obd2.generic（标准 OBD2，无需安装任何东西）
其余车型需通过网页或 App 安装档案，再激活，重启后生效
```

网页 `/vehicles/editor` 可从标准 OBD2、本地 JSON 或社区协议开始，列出采集数据与 PID，并修改或新增数据。
新建标准配置包含 10 项内置标准数据，每项默认 1 秒一次，间隔可修改；导入的配置保留原频率。
补入标准数据不覆盖已有自定义或关闭项，宝马、Jeep 等厂商原始请求与回退字段保持完整。字段与公式说明见 [PID_GUIDE.md](PID_GUIDE.md)。

## 车型档案目录

当前车型目录仓库共 **18 个档案**（[vehicles/catalog/](../vehicles/catalog/)），
固件只内置其中的 `obd2.generic`，其余 17 个是待安装的 JSON 文件：

| id | 名称 | 协议 | 寻址 | 挡位来源 | 增压 | 油压(OBD) | 备注 |
|---|---|---|---|---|---|---|---|
| `obd2.generic` | `OBD2 Generic` | 自动 | 功能 7DF（协议 7/9 使用 `18DB33F1`） | 比值估算 | ✓ | — | **固件内置**，通用 SAE J1979 |
| `subaru.zc6.can` | `ZN/C6 CAN` | 6 锁定 | 物理 7E0 | 比值估算 | — | — | 唯一用 CAN 广播监听（`ATMA`，`monitor` 块）的档案；RPM 仍走 OBD |
| `subaru.zc6.pid` | `ZN/C6 PID` | 6 锁定 | 物理 7E0 | 比值估算 | — | — | 不支持 ATMA 的适配器用这个（纯 OBD 回退） |
| `subaru.zd8.obd` | `ZD8 OBD` | 6 锁定 | 物理 7E0 | 比值估算 | — | — | 与 `subaru.zd8.pid` 轮询策略相同；两个档案都保留以让老 NVS 序号 1:1 映射 |
| `subaru.zd8.pid` | `ZD8` | 6 锁定 | 物理 7E0 | 比值估算 | — | — | 同上 |
| `mazda.mx5nd` | `MX-5 ND` | 自动 | 物理 7E0 | 比值估算 | — | — | 40ms 短超时（CAN 响应快） |
| `bmw.fg` | `BMW F/G` | 6 锁定 | 功能 7DF | DID 直读 + 比值回退 | ✓ | — | G20/G21/G22；挡位 DID 应答异常时回退比值估算（`fail_after 1`） |
| `toyota.supra_a90` | `Supra A90` | 6 锁定 | 功能 7DF | 比值估算 | ✓ | ✓ | B58/B48；唯一另一个带 OBD 油压信号的档案 |
| `bmw.g.obd` | `BMW G OBD` | **7 锁定** | 功能 7DF | 比值估算 | ✓ | — | G 系自动检测不稳，锁定协议 7 |
| `bmw.e` | `BMW E` | 6 锁定 | 功能 7DF | DID 直读 | — | ✓ | E9x/E46/E39 |
| `mini.jcw_f56` | `JCW F56` | 自动 | 物理 7E0 | 比值估算 | ✓ | — | B48 前驱 |
| `mini.r55` | `MINI R55` | 6 锁定 | 功能 7DF | 比值估算 | ✓ | — | N14/N18/N16；UDS `22 58 22` 固定走物理 7E0（见下方行为变化） |
| `porsche.997_2` | `POS 997.2` | 6 锁定 | 物理 7E0 | 比值估算（7 速） | — | — | 987.2/997.2 DFI |
| `porsche.997_1` | `POS 997.1` | 6 锁定 | 物理 7E0 | 比值估算 | — | — | 987.1/997.1 M96/M97 |
| `porsche.987_1` | `POS 987.1` | 3 锁定 | ISO 9141-2 默认寻址 | 比值估算 | — | — | 987.1 实测为 ELM protocol 3；挡位比初始沿用 997.1，需按变速箱校准 |
| `alfa.giulia_20t` | `GIULIA 2.0T` | 7 锁定 | 29 位功能 `18DB33F1` | 比值估算 | ✓ | — | Giorgio 平台；50ms 轮询间隔（UDS 响应慢） |
| `jeep.generic` | `jeep` | 7 锁定 | 29 位功能 `18DB33F1` | 比值估算 | — | — | 通用占位，齿比/轮胎待按实际车型修正 |
| `honda.integra` | `Honda Integra` | 7 锁定 | 29 位功能 `18DB33F1` | 禁用（CVT） | ✓ | — | 11 代思域平台 L15C7；非 Type R 可能无物理油温传感器 |

`subaru.zc6.pid` 的油温配置仍是 Mode 21 请求 `2101`。`multiframe: true` 表示响应可能多帧；
固件用 `ATCAF0` 手动处理响应时，会把请求封装成 ISO-TP 单帧 `02 21 01`（ELM 命令显示为
`022101`）。JSON 不应添加这个长度字节。
如需让 ELM 自动处理 ISO-TP，可在该源增加 `"multiframe_mode": "elm"`；请求保持
`2101`，响应仍按完整多帧拼接后解析，且不切换到 `CAF0/CFC0`。

数字以 [`vehicles/catalog/*.vehicle.json`](../vehicles/catalog/) 为准，字段定义见
[`contracts/vehicle-rpn.md`](../contracts/vehicle-rpn.md)（`skygauge.vehicle/1` schema、
RPN 公式操作码、服务/寻址白名单、静态校验规则）。

## 可编辑导入包

[vehicles/import/](../vehicles/import/README.md) 提供 **19 份完整可导入模板**：
现有 18 份档案的独立 `custom.*` 副本，加一份 `custom.bmw.g.gear`。
原始车型目录为 18 份。模板展开请求头、应答前缀和轮询间隔，适合复制后修改；
直接把选中的 JSON 放进 SD 卡 `OBD/IMPORT/` 即可安装，再选择车型并重启。

`custom.bmw.g.gear` 复用 `bmw.fg` 的协议 6、EGS 原始请求
`18 03 22 DA 2E`（`hdr: 6F1`、`rx: 618`、`expect: 62DA2E`），
直接读取 0..8 的挡位值，失败回退齿比估算。原协议 7 的 `custom.bmw.g.obd`
仍单独提供；两者按实际车辆选择，本次生成没有新增实车验证。

请求信号使用 `rate: "round"` 和 `period_ms`：转速 50 ms；车速 100 ms；挡位/负载/
节气门/增压 200 ms；空燃比/油压 500 ms；温度/电瓶电压 1000 ms。
这些是最小间隔，实际轮询仍串行执行。纯 CAN 监听与比值推导沿用原配置。
请求格式、按位解码、调整方法及完整文件表见导入包 README 和
[PID_GUIDE.md](PID_GUIDE.md)。

## 默认继承标准 OBD2

现在只需声明车型特有的信号；根字段 `defaults` 缺省为 `"obd2"`，
未声明的标准信号自动继承内置 Generic OBD2 的请求、公式、单位、过滤与轮询策略。
最小可导入配置如下，`bus` 和 `signals` 都可以省略：

```json
{
  "schema": "skygauge.vehicle/1",
  "id": "custom.mycar",
  "rev": 1,
  "name": "My Car"
}
```

补齐以下 10 个标准信号：

| 信号 / Signal | PID |
|---|---|
| `obd.rpm` | `010C` |
| `obd.intake_temp` | `010F` |
| `obd.speed` | `010D` |
| `obd.coolant_temp` | `0105` |
| `obd.load` | `0104` |
| `obd.throttle` | `0111` |
| `obd.oil_temp` | `015C` |
| `obd.battery_voltage` | `0142` |
| `obd.afr` | `0144` |
| `obd.boost` | `0170`，回退 `010B` |

显式声明的信号**整体替换**同 id 的默认定义，不做字段合并，也不自动追加标准 PID
作为失败回退。例如自定义 `obd.oil_temp` 后，只有你在 `src` 中写出的油温路径会被请求；
只写 `id` 或只改 `period_ms` 不够，仍需完整的 `src` / `derive` 定义。
省略信号时用默认值，显式禁用时在 `signals` 数组里写：

```json
{"id": "obd.oil_temp", "disabled": true}
```

禁用项只允许 `id`、`disabled`、`notes`，不进入轮询。需要完全自定义请求或纯 CAN
监听时，设置根字段 `"defaults": "none"`，此时 `signals` 必须为非空数组。
当 `bus.proto` 为 0 或省略时，优先使用用户手动选择的协议；用户选择 `0 - Automatic` 时，自动探测结果仅在当前会话缓存，不覆盖 NVS 中的自动设置。持续无有效数据会重新探测；显式 `bus.proto` 仍锁定协议。旧固件已保存的非零协议无法区分来源，升级后需要在协议页重新选择一次 `0 - Automatic`。没有 `bus` 时其余默认值为请求头 `7E0`、`ATST=0x19`、请求间隔 30 ms；
继承的标准请求使用车型的 `bus.hdr`，需要特殊头的请求仍在各自 `src.hdr` 指定。
在 J1850 / ISO 9141-2 / ISO KWP2000（协议 1–5）上，普通 OBD 请求保留 ELM327 的协议原生寻址；即使导入档案带有 CAN 格式的 `bus.hdr` / `src.hdr`（如 `7E0`），也不会发送 `ATSH`。引擎使用 `ATH0` 隐藏响应头。协议 7/9 将标准 OBD 请求头 `7DF`、`7E0`–`7E7` 映射到对应的 29 位功能/物理地址，已写成 29 位的头保持原值。CAN 响应支持 `ATH1 + ATS0` 无空格格式以及 29 位头按字节分隔的格式。
导入初始化中的 `ATSP0` 不会覆盖已经选定的具体协议；使用适配器自动模式 `ATSP0` / `ATSPAh` 后，通过 `ATDPN` 确认实际协议再开始轮询。协议选择命令被拒绝时，不会把旧协议上的应答误认成当前候选成功。协议 7/9 的标准接收过滤 `7E8`–`7EF` 同步映射为 29 位地址；原生 29 位物理请求也支持 Mode 01 功能寻址回退。普通请求/初始化超时同样先中断并等到 `>`，BLE 写入失败则重连，防止后续命令拼到未完成的命令上。
传动比和 `obd.gear` 的比值推导不会继承；挡位需显式直读或提供本车的 `drivetrain`。
自动补齐不代表 ECU 支持该 PID，读不到仍按现有失效规则显示 `--`。
资源上限、轮询频率与信号依赖均在补齐后校验。

[精简示例](../vehicles/examples/README.md) 提供标准 OBD2 最小配置及仅声明 BMW G
直读挡位的配置；安装方式相同，需要包含此默认继承功能的固件。

## 安装与激活

有三种方式把档案装进设备，效果相同：

### 1. SD 卡 drop 文件夹（`/OBD/IMPORT`）

把以下文件放进 SD 卡 `OBD/IMPORT/` 目录，开机或插卡时触发扫描：

| 文件 | 作用 |
|---|---|
| `<id>.vehicle.json` | 安装一个车型档案（校验+编译，不激活） |
| `ACTIVATE.TXT` | 第一行写 profile id，激活它（下次开机生效） |
| `RESET.TXT` | 回退到 `obd2.generic` |

处理顺序（`sd_import.c`）：先装 `*.vehicle.json`，再处理 `ACTIVATE.TXT`，
**`RESET.TXT` 总是最后处理，会覆盖同一批次里的 `ACTIVATE.TXT`**。
结果写入 `OBD/IMPORT/IMPORT.LOG`（`<时间> OK|ERR <文件>: <详情>`），
成功的文件重命名为 `.ok`，失败的重命名为 `.err` 并生成同名 `.err.txt`
列出校验器给出的具体问题。

### 2. 网页与配套 App

车型配置的安装、激活、回退和删除通过网页或配套 App 完成。仪表不提供车型管理页面；
网页或 App 通过以下设备 API 操作，激活后仍需重启才生效。

### 3. `/api/v1/vehicles` API（配套 App）

OTA 热点下提供这组 HTTP 接口（需要 `X-OTA-Token`），供网页或配套 App 管理车型：

| 方法 | 路径 | 作用 |
|---|---|---|
| `GET` | `/api/v1/vehicles` | 列出已装档案 + 当前激活状态/安全模式/最近错误 |
| `POST` | `/api/v1/vehicles` | 上传并安装档案（校验+编译+存储，**不激活**） |
| `POST` | `/api/v1/vehicles/activate` | `{"id":"..."}`，激活（返回 `reboot_required:true`） |
| `POST` | `/api/v1/vehicles/reset` | 回退到 `obd2.generic` |
| `POST` | `/api/v1/vehicles/rollback` | 回到上一个档案 |
| `POST` | `/api/v1/vehicles/remove` | `{"id":"..."}`，删除已装档案 |
| `GET` | `/api/v1/vehicles/<id>` | 读取某档案的原始 JSON |
| `DELETE` | `/api/v1/vehicles/<id>` | 删除某档案 |

**无论哪种方式，激活都要等下次重启才生效**，当前这次开机继续用旧档案。

存储位置：安装的档案存在配置分区 `/config/vehicles/<id>.json`，并镜像一份到
SD 卡 `/OBD/CFG/vehicles`；激活中的档案另外复制一份到 `nvs_app` 的
`veh/blob`（≤8KB），这样清配置分区也不会丢当前车型。

## 生命周期与安全机制

状态机（`main/vehicle/vp_manager.c`）：

```
install        → INSTALLED
activate(id)   → PENDING（下次开机生效）
下次开机验证通过 → ACTIVE
3 次开机验证都不通过 → 回退到上一个档案，再不行回退到 obd2.generic
```

**验证规则**：开机后等待 20 秒，要求电压 ≥13.0V，且至少
`min(3, 该档案的信号数)` 个不同信号槛位有应答（转速如果档案里有就必须算在内）。

**安全模式**：连续 3 次崩溃（panic / 中断看门狗 / 任务看门狗复位）后进入安全模式，
强制用 `obd2.generic` 且不再处理 SD 导入里的 `*.vehicle.json` / `ACTIVATE.TXT`
（`RESET.TXT` 仍会处理，用于手动脱困）。运行满 60 秒无崩溃后清除崩溃计数。

## 老固件车型序号迁移

老固件用 NVS 里的车型序号（`vehicle_profile_idx`），升级后首次开机按下表
映射成新的档案 id 并自动安装+激活（`main/app_obd_dsp/first_boot_migration.c`
的 `vp_manager_legacy_id()`）：

| 老序号 | 新 id |
|---|---|
| 0 | `obd2.generic` |
| 1 | `subaru.zc6.can` |
| 2 | `subaru.zc6.pid` |
| 3 | `subaru.zd8.obd` |
| 4 | `subaru.zd8.pid` |
| 5 | `mazda.mx5nd` |
| 6 | `bmw.fg` |
| 7 | `toyota.supra_a90` |
| 8 | `bmw.g.obd` |
| 9 | `bmw.e` |
| 10 | `mini.jcw_f56` |
| 11 | `mini.r55` |
| 12 | `porsche.997_2` |
| 13 | `porsche.997_1` |
| 14 | `alfa.giulia_20t` |
| 15 | `jeep.generic` |
| 16 | `honda.integra` |

迁移会先找 SD 卡 `OBD/CATALOG/<id>.vehicle.json`（或 `<id>.json`）安装+激活；
找不到时把目标 id 记入 NVS，等 SD 卡插上或联网后台重试。
**用户在这期间手动重置或激活了别的车型，迁移就不再覆盖用户的选择**
（`first_boot_migration_drop_vehicle()`，由内部 `RESET.TXT`、网页或 App 的
`/api/v1/vehicles/reset`、以及任意一次手动 `activate` 触发）。

## 相比老固件的行为变化

以下差异来自 `test/host/vehicle/fixtures/golden/*.json` 的新旧对比测试，
已在主机单元测试里锁定：

- **`obd.boost`（涡轮车，7 个档案）**：老固件把 `(MAP-100)/10` 截断存成 0.1 bar
  精度；新档案存 0.01 bar 精度，显示更细
- **`mini.r55` 的 `22 58 22`（油温查询）**：老固件走功能寻址头 7DF 发送；
  新引擎禁止 service `22` 用功能寻址头，固定改走物理头 7E0
- **`bmw.fg` 的 `obd.gear`**：老固件只要 DID 应答一次超范围就回退比值挡位；
  新引擎用 `fail_after 1` + `fallback: "ratio"`，行为更明确但效果一致
- **`subaru.zc6.can` 的 CAN 水温/油温**：`monitor.stale_ms`（15000ms）内无帧时，
  老固件把水温写成 -40、油温置为无效；新引擎把两者都标记为无效（不再假装 -40℃）
- **`mazda.mx5nd` / `mini.jcw_f56` / `obd2.generic` 的 `ATSP0`**：
  `bus.proto` 为 0 时保留用户的手动/自动选择；自动结果仅作会话缓存，持续无有效数据时重新探测，不再把检测结果写成 NVS 固定协议

## ADS1115 油压传感器（V1 硬件）回退

V1 硬件板（`CONFIG_OBD_HW_VERSION_V1_WAVESHARE`）上接了 ADS1115 模拟油压传感器。
只有当**当前激活档案没有 `obd.oil_pressure` 信号**时，固件才会启用 ADS1115
采集（`main/app_main.c`）：

```c
#if CONFIG_OBD_HW_VERSION_V1_WAVESHARE
    if (!vp_manager_has_signal(SIG_OBD_OIL_PRESSURE)) {
        signal_store_set_local(SIG_OBD_OIL_PRESSURE, true);
        oil_pressure_start();
    }
#endif
```

目前 `toyota.supra_a90`（DID `4436`）和 `bmw.e`（DID `586F`）两个档案自带 OBD
油压信号，激活这两个档案时 ADS1115 不会启动；其余档案（包括 `obd2.generic`）
都走 ADS1115。

## 新增/修改车型

不需要改固件代码，只需要写一个符合 `skygauge.vehicle/1` schema 的 JSON：

1. 按 [`contracts/vehicle-rpn.md`](../contracts/vehicle-rpn.md) 写档案
   （信号、来源、RPN 公式、CAN 规则、服务/AT 白名单都有详细说明和 3 个完整例子），
   跟着抄字段改的教程版见 [PID_GUIDE.md](PID_GUIDE.md)
2. 参考 [`vehicles/catalog/`](../vehicles/catalog/) 里任意一个现成档案起草
3. 通过以上三种方式之一安装 + 激活，看 `IMPORT.LOG` 或
   `GET /api/v1/vehicles` 的 `last_error` 排查校验失败（`E_SIZE`/`E_SCHEMA`/
   `E_LIMIT`/`E_SIGNAL`/`E_SERVICE`/`E_AT`/`E_RPN_*` 等错误码见 schema 文档）
4. 限制：档案文本 ≤16384 字节，信号 ≤48 个（其中 `veh.*` 自定义信号 ≤32 个）
5. 找未知车型的私有 PID 可以用 [tools 里的伪 ELM327 挖掘流程](DEVELOPMENT.md#工具脚本)，不需要上车

目录里新增/替换档案 JSON 时，同步更新本文件最上面的目录表格。

## 清空已安装车型配置

网页 USB 刷写页支持批量“清空已安装车型配置”：显式勾选确认后，删除 config 区 `/vehicles/*.json` 及 SD `/OBD/CFG/vehicles/*.json`（含当前车型），安排重启使用内置 `obd2.generic`。需挂载 SD，任何删除错误都会停止并报错；当前编译态车型一直保留到重启，避免轮询访问已释放内存。CATALOG / IMPORT / EXPORT 模板、主题与开机动画保留。此操作需要支持 `vehicle.clear` 的新版固件。

## 增压与高温读数

内置 Generic 和省略 `obd.boost` 的配置优先请求标准 `0170`，读取 10 字节多帧响应中 Boost Pressure Sensor A 的 D/E 字节，按 `(D*256+E)/32` 解出绝对压力，再减固定 `100 kPa` 转为相对压力；无效或不支持时回退到 `010B` 的 `MAP - 100 kPa` 估算。两条路径都保留负压，单位为 `kPa_rel`，内置显示按 0.1 bar 精度舍入；指针和曲线下限为 -1.0 bar。车型表的增压列列出显式定义。实车需支持相应 PID，缺失或过期仍显示 `--`。固定 100 kPa 参考受海拔和天气影响；准确表压应使用实车大气压力作差。

水温、油温的信号上限和内置仪表刻度上限改为 255°C，油温防抖和 ESP-NOW v5 接收范围同步放宽；大幅温度跳变仍需要连续确认。标准 `0105` / `015C` 的单字节 `A-40` 最大只能表达 215°C；255°C 需要原始公式支持，不能通过改上限生成。目录配置更新到 rev 3，导入模板同步更新；已安装配置中显式的旧 `valid` 或负压截断不会被固件覆盖，需要重新导入新版，定制公式也应检查自己的范围。

## 转速、车速与油门相关数据采集调度

固件优先安排当前页面需要的转速、车速、涡轮压力和节气门请求，再穿插后台参数。未指定 `period_ms` 时，转速／车速的最小请求间隔为 50 / 100 ms，压力／节气门每批优先读取；显式 `period_ms` 保持有效，压力／节气门的显式 `rate: "slow"` 仍沿用慢速策略。新导入模板使用转速 50 ms、车速 100 ms、压力／节气门 200 ms，已安装配置不会被自动改写。

调度按实际发送时间判断是否到期。同一批内共用 PID 的信号合并请求；较长批次在完成至少两个请求、仍有至少两个待处理请求时，最多补一次到期转速采样，然后继续处理余下参数。后台参数仍有轮询机会，不打断请求前后命令、多帧事务或提示符等待。连续失败的请求按 1 / 2 / 4 / 8 / 16 秒退避，成功和重新连接后恢复。仅请求当前页面或挡位估算需要的数据；转速保持常开。

速度缓存保留真实采样值，内置数字页、车速页与指针页的转速、车速、压力、节气门、负载直接显示最新样本，不再额外逐步追赶；温度等慢参数保留显示平滑。以上周期均为最小间隔，实际频率由适配器、协议、超时、请求数量及车型配置决定，不保证 20 / 10 Hz。更多优先信号同时显示时会共享串行带宽；模拟调度对比见 [PERFORMANCE.md](PERFORMANCE.md#油门相关数据优先调度验证)。

## 尚酷 2.0T 参数库核对

核对用户提供的 Car Scanner `vehicle-profiles.json`（1304 份档案），未找到 `Scirocco` / `Sirocco` / `尚酷` 命名的专用档案。相关 `872`（`PQ25, PQ35, PQ46`）和 `873`（`PQ25, PQ35, PQ46 + UDS AT/DSG DQ500`）包含大众参数，但其 ECM 压力请求为 `VWTP:01:2173`，当前引擎不支持该传输。Dashboard 的 `222211` 是百分比，不能作为 bar 直接导入。`742` 的 EA888 Gen.3 / MQB 参数也不能只凭 2.0T 排量判为兼容。需要年份、发动机和 ECU 信息验证；本次不新增未经验证的尚酷专用配置，通用标准 PID 路径仍可单独使用。

油温迟滞滤波现在能确认 16～19°C 的跳变，并正确取整负温度。
