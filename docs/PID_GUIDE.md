# PID / 信号定义指南

铃木 KWP 专用配置仅在显式声明 `bus.proto: 4` 或 `5` 时允许 `bus.init` 中的 `ATSH8111F1`、`ATSH8101F1`、`ATSH8001F1`，用于 `2100` 数据块读取。三字节头不属于 CAN `bus.hdr` / `src.hdr`，也不能放在请求级 `before` / `after`。初始化与重连都会按配置顺序重发；通用 OBD 的原生寻址不变。`check_connection` 应显式为 `2100`；响应数据偏移需要实车校准，此支持不表示任一雨燕年份已验证。

[English](PID_GUIDE.en.md) | 简体中文

这篇是教程向导读：怎么把挖到的一个 PID/CAN 位，写成车型档案里能跑的信号定义。
字段的完整权威规则在 [contracts/vehicle-rpn.md](../contracts/vehicle-rpn.md)
（事实源是设备端校验器 `main/vehicle/vp_validate.c`/`vp_rpn.c`）；这里只按真实
车型抽样例子，照着抄改字段即可。车型安装/激活流程见 [VEHICLES.md](VEHICLES.md)。

网页 `/vehicles/editor` 默认使用基础模式：先选择标准 OBD2、本地 JSON 或社区协议，再选择和修改采集数据。
新建配置列出内置的 9 项标准数据及 PID，每项默认 1 秒一次；可修改间隔、PID、请求头、应答地址和换算公式，也可新增其他 PID。
公式编辑或响应字节数修改时会同步生成并校验固件使用的 `rpn`。导入配置保留原调度；继承项修改时写入完整定义，“导入标准 OBD2 数据”仅补缺，不覆盖自定义或关闭项。
厂商原始帧、CAN 位、LUT 与回退来源通过高级模式编辑，基础操作保留其完整字段。此标准列表对应当前固件内置数据，不是全部 SAE PID。

## 1. 整体模型

一个车型档案（`vehicles/catalog/*.vehicle.json`）里的 `signals` 数组，每项是一个
信号（如 `obd.oil_temp`），每个信号有 1..4 个 `src`（来源，按顺序回退），每个来源
执行一段 `rpn`（逆波兰表达式）把原始字节换算成工程值。设备端求值顺序：

```
收到应答 → 按 src.len/multiframe 取数据字节 → 执行 rpn → src.valid 过滤
→ hold 防抖 → 源连续失败 fail_after 次换下一个 src → 信号级 post/valid
→ 换算成内部定点单位 → filter（如油温迟滞）→ 写入信号表
```

（完整管线见 [vehicle-rpn.md §3](../contracts/vehicle-rpn.md#3-求值管线)）

### 1.1 默认继承标准 OBD2

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

## 2. 最简单的例子：标准 OBD2 PID（水温）

`subaru.zc6.pid.vehicle.json` 里的水温，标准 Mode 01 PID `05`，响应 1 字节，
"温度偏移 40" 编码（0~255 代表 -40~215℃）：

```json
{
  "id": "obd.coolant_temp",
  "src": [
    {
      "req": "0105",
      "len": 1,
      "expr": "A-40",
      "rpn": [["A", 0], ["k", 40], "-"]
    }
  ]
}
```

- `req`：紧凑十六进制，`01`（Mode 01）+ `05`（PID）
- `len: 1`：公式可寻址 1 个数据字节（去掉 `41 05` 应答前缀之后的 `d[0]`）
- `expr`：给人看的创作形，**只是注释**；设备只执行 `rpn`
- `rpn`：`["A", 0]` 取字节 0，`["k", 40]` 压入常量 40，`"-"` 做减法

`expr` → `rpn` 用 `tools/vehicles/expr2rpn.py` 编译，不要手搓 `rpn`（除非表达式
本身就很简单，像上面这种一眼能照抄）。

## 3. 大端位域 + 换算单位：电瓶电压、空燃比

`obd.battery_voltage`（Mode 01 PID `42`，2 字节大端，单位 mV）：

```json
{
  "id": "obd.battery_voltage",
  "unit": "mV",
  "src": [
    { "req": "0142", "len": 2, "expr": "A*256+B", "rpn": [["R", 0, 16, "be"]] }
  ]
}
```

`["R", 0, 16, "be"]` 直接按大端取 16 位位域，等价于 `A*256+B` 但不用两条指令。
`unit: "mV"` 必须是该信号在 `contracts/signals.json` 里 `profile_units` 允许的
单位之一（`obd.battery_voltage` 允许 `V`/`mV`，缺省 `V`）。

`obd.afr`（Mode 01 PID `44`，命令空燃比，需要额外做量程校验）：

```json
{
  "id": "obd.afr",
  "src": [
    {
      "req": "0144", "len": 2, "valid": [8, 22],
      "expr": "(A*256+B)*14.7/32768",
      "rpn": [["R", 0, 16, "be"], ["k", 14.7], "*", ["k", 32768], "/"]
    }
  ]
}
```

`src.valid: [8, 22]` 是源级校验——结果不在 8..22 这个 AFR 合理区间就算这次采样
失败，不会把明显错误的瞬时值显示出来。

## 4. 响应长度不固定：Mode 21 多帧 + 从尾部取字节 + 防抖

BRZ（ZN/C6 PID 档案）的油温，走丰田/斯巴鲁私有 Mode 21 PID `01`，响应是多帧
ISO-TP，温度字节的位置随车型/固件版本漂移，所以不按正数第几个字节取，而是
**倒数第 5 个字节**：

配置里的 `req` 仍写诊断负载 `2101`。因为多帧响应由固件手动处理 ISO-TP，
发送前会在线组帧为 `022101`（单帧长度 `02` + Mode/PID）；不要把 `02` 加进 JSON。
固件使用 `300100` 请求逐帧流控；适配器执行流控后可能在同一条响应中返回一帧，
固件消费后会继续请求下一帧，直到 ISO-TP 响应拼接完成。

若适配器应自行处理 ISO-TP，可在保留 `multiframe: true` 的同时设置
`"multiframe_mode": "elm"`。此时引擎发送 `2101`，保持 `CAF1/CFC1` 自动格式与流控，
并解析 ELM 返回的编号多帧文本；默认不设置该字段时仍使用固件手动流控。

```json
{
  "id": "obd.oil_temp",
  "valid": [-20, 255],
  "filter": "temp_hyst",
  "src": [
    {
      "req": "2101",
      "multiframe": true,
      "multiframe_mode": "elm",
      "byte_from_end": 5,
      "valid": [-10, 255],
      "hold": { "delta": 8, "max_hold": 30, "gap_ms": 3000 },
      "expr": "X-40",
      "rpn": [["X"], ["k", 40], "-"]
    }
  ]
}
```

- `multiframe: true` + `byte_from_end: 5`：响应可能跨多帧，公式只看**倒数第 5
  个字节**，只能用 `["X"]`（= 这个被选中的字节），不能用 `["A", i]`
- `hold`：防抖——新值和上次被接受的值差 ≤8℃ 直接采用；差太大先沿用旧值，最多
  连续扛 30 次采样，或超过 3000ms 没更新就强制接受新值（抄自老固件
  `elm327_ble_client.c` 的油温防抖逻辑）
- 两层 `valid`：源级 `[-10,255]` 过滤明显坏值，信号级 `[-20,255]` 是最终显示范围
温度迟滞允许 -20～255，CAN 数据直接通过；其他读数差值小于 6 时平滑，6～不足 20 时需连续 4 次相同读数，20 及以上需连续 3 次；负数按最接近的整数输出。

- `filter: "temp_hyst"`：换算成内部单位后再过一层迟滞滤波

## 5. CAN 广播监听：直接扣位，不用问

`subaru.zc6.can.vehicle.json`（ZN/C6 CAN 档案）靠 `ATMA` 监听广播帧，水温/油温/
节气门直接从总线上扣位域，不用发请求：

```json
{
  "requires": { "elm": ["ATMA"] },
  "monitor": { "mode": "probe", "every_ms": 500, "window_ms": 120, "stale_ms": 15000 },
  "signals": [
    {
      "id": "obd.coolant_temp",
      "src": [{ "can": "360", "bit": 24, "bits": 8, "expr": "X-40", "rpn": [["X"], ["k", 40], "-"]} ]
    },
    {
      "id": "obd.throttle",
      "src": [{ "can": "140", "bit": 48, "bits": 8, "expr": "X*100/255", "rpn": [["X"], ["k", 100], "*", ["k", 255], "/"]} ]
    }
  ]
}
```

- 顶层 `monitor` 块声明监听窗口（必须 `requires.elm: ["ATMA"]`）；`mode: "probe"`
  表示周期性开窗监听，不是一直占着总线
- `can: "360"`：11 位 CAN ID；`bit`/`bits`：小端位域起点和宽度（`endian` 缺省
  `le`，与现行 `can_extract_bits_le` 一致）
- CAN 源用的也是 `["X"]` 取自己的位域，公式写法和请求源一样

## 6. 直读挡位（扩展寻址 UDS DID）+ 失败回退

BMW F/G 直接读 EGS 的挡位 DID，不用比值估算：

```json
{
  "id": "obd.gear",
  "fallback": "ratio",
  "src": [
    {
      "raw": "18 03 22 DA 2E",
      "hdr": "6F1",
      "rx": "618",
      "expect": "62DA2E",
      "len": 1,
      "valid": [0, 8],
      "fail_after": 1,
      "expr": "A",
      "rpn": [["A", 0]]
    }
  ]
}
```

- `raw`：裸 CAN 载荷（扩展寻址：目标 `18`，PCI `03`，服务 `22` DID `DA2E`）
- `hdr`/`rx`：这一次请求覆盖发送头/接收过滤，用完固件自动恢复默认值
- `expect`：正响应前缀，用来确认这是对应请求的应答，不是别的轮询串台
- `fail_after: 1`：连续失败 1 次就换下一个源——这里只有一个源，失败后触发
  `fallback: "ratio"`，回退到比值估算（见下一节）

## 7. 比值估算挡位：`derive: "ratio"`

没有可读的挡位 DID、也没有 CAN 广播挡位时，用传动比 + 车速/转速反推：

```json
{
  "drivetrain": { "fd": 4.1, "tire_m": 0.314, "gears": [3.626, 2.188, 1.541, 1.213, 1, 0.767], "tol": 0.15 },
  "signals": [ { "id": "obd.gear", "derive": "ratio" } ]
}
```

`derive: "ratio"` 的信号**不能**再写 `src`；需要 profile 同时有 `obd.rpm` 和
`obd.speed`。`fd`（最终传动比）、`tire_m`（轮胎滚动周长，米）、`gears`（各挡
齿比，≤8 挡）、`tol`（容差）都在顶层 `drivetrain` 里配。

`derive: "ratio"` 和 `fallback: "ratio"` 的区别：前者是"这台车只有比值估算一条
路"，不能带 `src`；后者是"先试试直读，读不到才回退比值"，`src` 和 `drivetrain`
同时存在。

## 8. 查找表 `luts`（非线性映射）

目前车型目录里还没有用到 `luts` 的例子，但字段已经支持——比如挡位 DID 返回的
不是挡位数字本身而是一个状态码，就可以用查找表映射：

```json
"luts": [[[0, 0], [1, 6, 1], [7, -1]]],
...
"rpn": [["A", 0], ["lut", 0]]
```

每张表 ≤16 项，每项是 `[key, out]`（精确匹配）或 `[lo, hi, out]`（区间匹配），
按顺序取第一个命中的项，没命中就是无效样本。完整语法见
[vehicle-rpn.md §2.4](../contracts/vehicle-rpn.md#24-查找表-luts)。

## 9. 能用哪些信号 id、单位是什么

`obd.*` 是固定注册表（[contracts/signals.json](../contracts/signals.json)），
不能自己起名；车型私有数据用 `veh.*`（≤32 个，`^veh\.[a-z0-9_]{1,24}$`）。
设备端目前只接受下表列出的信号字段；`decimals` 尚未实现，导入时会按未知字段拒绝。

| `obd.*` id | 缺省单位 | 可选单位 |
|---|---|---|
| `obd.rpm` | rpm | rpm |
| `obd.speed` | km/h | km/h, mph |
| `obd.boost` | bar | bar, 0.1bar, kPa_rel, psi |
| `obd.coolant_temp` | C | C |
| `obd.oil_pressure` | bar | bar, 0.1bar, kPa, hPa, psi |
| `obd.gear` | gear | gear |
| `obd.battery_voltage` | V | V, mV |
| `obd.oil_temp` | C | C |
| `obd.afr` | AFR | AFR, lambda |
| `obd.throttle` | % | % |
| `obd.intake_temp` | C | C |
| `obd.load` | % | % |

每个信号最多 4 个 `src`，整个档案最多 48 个信号（`veh.*` 占其中 ≤32 个）。

## 10. 能发哪些命令：服务号 / 寻址白名单

档案不能直接写 AT 命令（`bus.init` 的受限白名单除外），只能写结构化字段，由
固件拼命令：

| 项 | 允许 |
|---|---|
| 服务号（`req` 或裸帧里的服务） | `01`、`09`、`21`（各带 1 字节参数）、`22`（带 2 字节 DID） |
| 发送头 `hdr`/`bus.hdr` | 功能寻址 `7DF`/`18DB33F1`；物理寻址 `7E0`-`7E7`、`600`-`6FF`、`18DAxxF1` |
| 接收过滤 `rx` | `7E8`-`7EF`、`600`-`6FF`、`18DAF1xx` |
| CAN 监听 id `can` | 11 位 ≤7FF，或 29 位 ≤1FFFFFFF |
| 裸帧 `raw` | 1..8 字节，固件按普通寻址和扩展寻址两种方式尝试解释，服务号同样要在白名单内 |

完整白名单、`bus.init` 允许的 AT 命令、每一项对应的拒绝错误码见
[vehicle-rpn.md §4-5](../contracts/vehicle-rpn.md#4-服务号与寻址白名单)。

## 11. 写错了会在哪看到、怎么查

安装档案校验失败时，错误码（`E_SCHEMA`/`E_SIGNAL`/`E_SERVICE`/`E_HDR`/
`E_RPN_OP`/`E_RPN_STACK`/`E_RPN_INDEX`/`E_RPN_CONST`/`E_LIMIT`/`E_SIZE`/`E_NAME`/
`E_RATE` 等，完整列表见 [vehicle-rpn.md §7](../contracts/vehicle-rpn.md#7-错误码)）
会出现在：

- SD 卡安装：`OBD/IMPORT/IMPORT.LOG` 和同名 `.err.txt`
- App / 设备 API 安装：`POST /api/v1/vehicles` 的 422 响应体 `{errors:[{code,path,msg}]}`
- 已安装但激活失败：`GET /api/v1/vehicles` 的 `last_error`

## 12. 不知道某个数据走哪个 PID 怎么挖

不需要上真车。用仓库自带的伪 ELM327（`tools/fake_elm327.py`）配合手机
Car Scanner App 跑一遍 `tools/one_shot.py`，自动比对注入的伪信号和 App 实际
发出的请求顺序，反推出每个仪表对应哪个 PID、怎么解码。流程细节见
[DEVELOPMENT.md 工具脚本](DEVELOPMENT.md#工具脚本)。挖到结果后照着抄本文档的
例子写 `rpn`，再按 [VEHICLES.md 新增/修改车型](VEHICLES.md#新增修改车型)
一节装进设备验证。

## 13. 起草新档案最省事的方法

不要从零手写，直接复制 `vehicles/catalog/` 里跟目标车同平台/同协议的档案
改字段（比如改一款新福特可以抄 `obd2.generic`，改一款新 BMW 可以抄
`bmw.e.vehicle.json` 或 `bmw.fg.vehicle.json`）。改完用导入校验（第 11 节）
确认没有报错，再按 `drivetrain` 的齿比/胎径跑几圈比对真实挡位。

## 14. 直接修改完整导入模板：请求头、位与频率

[vehicles/import/](../vehicles/import/README.md) 提供 18 份完整模板，可直接安装。
标准 OBD2 从 `custom.obd2.generic` 开始；特殊寻址可参考 `custom.jeep.generic`、
`custom.bmw.e`；BMW G 挡位直读用 `custom.bmw.g.gear`（复用 F/G 的协议 6
请求），协议 7 的 `custom.bmw.g.obd` 仍单独保留。

下例同时指定发送请求、请求头、应答前缀、取数长度、16 位解码和最小轮询间隔：

```json
{
  "id": "obd.battery_voltage",
  "unit": "mV",
  "rate": "round",
  "period_ms": 1000,
  "src": [{
    "req": "0142",
    "hdr": "7E0",
    "expect": "4142",
    "len": 2,
    "fail_after": 5,
    "expr": "A*256+B",
    "rpn": [["R", 0, 16, "be"]]
  }]
}
```

`expect` 匹配成功后，`len` / `rpn` 针对的是去掉应答前缀后的数据。
`R` 可改起始位、位宽、大/小端，追加 `"s"` 表示有符号数；大端位编号从第一个
字节最高位起顺序递增，不是 DBC Motorola 锯齿编号。`expr` 不执行，改解码必须
同步 `rpn`。CAN 来源直接修改 `bit` / `bits` / `endian` / `signed`。

`period_ms` 范围 20..60000，表示最小间隔；不能和 `rate: "every"` 共用。
模板将请求信号设为 `round`：RPM 50 ms，速度 100 ms，挡位/负载/节气门/增压 200 ms，
AFR/油压 500 ms，温度/电压 1000 ms。固件仍逐个请求，实际频率受响应时间制约。
`rate: "every"` 信号会在每个常规轮询槽之前优先读取，以降低转速等快信号等待慢 PID 的时间；
它仍受 BLE/ELM 往返时间、`gap_ms` 和队列中其他快信号影响。Mode 01 请求若在物理 CAN 头
收到 NRC `7F 01 11` 或 `7F 01 12`，固件会将该请求改用功能头 `7DF` 重试，并在本次连接内沿用。
总线协议、ATST 超时和请求间隙在 `bus` 中设置，每份档案只有一个协议。

运行时只轮询当前仪表页绑定或展示所需的标准 `obd.*` 信号；RPM 始终轮询，以便其他页面也能响应转速闪烁。
比值推算挡位时会一并轮询车速。档案中的 `veh.*` 信号不受本机页面筛选，主表会持续采集并广播给从表。

复制模板到包外再修改，改 ID 时同步文件名，更新时递增 `rev`。用现有固件校验器
检查自己的文件：

```sh
/tmp/obd-host/vehicle/validate_catalog --strict /path/to/custom.vehicle.json
```

校验器的构建命令及导入步骤见导入包 README。生成模板的 `--check` 用于确认
分发文件与目录来源一致，不用于验证个人修改后的模板。

## 增压与高温读数

内置 Generic 和省略 `obd.boost` 的配置优先请求标准 `0170`，读取 10 字节多帧响应中 Boost Pressure Sensor A 的 D/E 字节，按 `(D*256+E)/32` 解出绝对压力，再减固定 `100 kPa` 转为相对压力；无效或不支持时回退到 `010B` 的 `MAP - 100 kPa` 估算。两条路径都保留负压，单位为 `kPa_rel`，内置显示按 0.1 bar 精度舍入；指针和曲线下限为 -1.0 bar。车型表的增压列列出显式定义。实车需支持相应 PID，缺失或过期仍显示 `--`。固定 100 kPa 参考受海拔和天气影响；准确表压应使用实车大气压力作差。

水温、油温的信号上限和内置仪表刻度上限改为 255°C，油温防抖和 ESP-NOW v5 接收范围同步放宽；大幅温度跳变仍需要连续确认。标准 `0105` / `015C` 的单字节 `A-40` 最大只能表达 215°C；255°C 需要原始公式支持，不能通过改上限生成。目录配置更新到 rev 3，导入模板同步更新；已安装配置中显式的旧 `valid` 或负压截断不会被固件覆盖，需要重新导入新版，定制公式也应检查自己的范围。

## 转速、车速与油门相关数据采集调度

固件优先安排当前页面需要的转速、车速、涡轮压力和节气门请求，再穿插后台参数。未指定 `period_ms` 时，转速／车速的最小请求间隔为 50 / 100 ms，压力／节气门每批优先读取；显式 `period_ms` 保持有效，压力／节气门的显式 `rate: "slow"` 仍沿用慢速策略。新导入模板使用转速 50 ms、车速 100 ms、压力／节气门 200 ms，已安装配置不会被自动改写。

调度按实际发送时间判断是否到期。同一批内共用 PID 的信号合并请求；较长批次在完成至少两个请求、仍有至少两个待处理请求时，最多补一次到期转速采样，然后继续处理余下参数。后台参数仍有轮询机会，不打断请求前后命令、多帧事务或提示符等待。连续失败的请求按 1 / 2 / 4 / 8 / 16 秒退避，成功和重新连接后恢复。仅请求当前页面或挡位估算需要的数据；转速保持常开。

速度缓存保留真实采样值，内置数字页、车速页与指针页的转速、车速、压力、节气门、负载直接显示最新样本，不再额外逐步追赶；温度等慢参数保留显示平滑。以上周期均为最小间隔，实际频率由适配器、协议、超时、请求数量及车型配置决定，不保证 20 / 10 Hz。更多优先信号同时显示时会共享串行带宽；模拟调度对比见 [PERFORMANCE.md](PERFORMANCE.md#油门相关数据优先调度验证)。
