# 车型 profile：RPN 公式与请求安全规范（`skygauge.vehicle/1`）

本文件是 [`vehicle-profile.schema.json`](vehicle-profile.schema.json) 的配套规范，定义 JSON Schema 表达不了的部分：
RPN 指令集与执行语义、导入时的静态校验、服务号/寻址/AT 白名单、数量上限和错误码。
教程向、照着真实车型例子抄字段的版本见 [`docs/PID_GUIDE.md`](../docs/PID_GUIDE.md)。

**事实源是设备端校验器**：`main/vehicle/vp_validate.c`、`vp_rpn.c`、`vp_rpn_json.c`（P3，纯 C，host 可编译，
测试在 `test/host/vehicle`）。本文件描述它的行为；两者不一致时以代码为准并同步修本文件。
schema 故意比设备严：未知字段在 schema 里是错误（设备只给 `W_UNKNOWN` 警告），`obd.*` 的 `unit` 必须在
[`signals.json`](signals.json) 的 `profile_units` 里。

依据：

- `REFACTOR_PLAN.md` P3「安全的 RPN 执行器」「校验器」两行、5.5 节示例；
- 调研 `finish_vehicle.json` R2（profile 字段清单）、R3（RPN 指令集与约束）；
- 调研 `delta_import_channels.json` 第 6 节（安全约束、数量上限、RPN 静态检查）、2.2 节（统一错误码）；
- 现行代码：`main/app_obd_dsp/vehicle_custom_config.h:28-35,278-310`（CAN 位域）、
  `main/bsp_obd_dsp/elm327_ble_client.c:1287-1325`（Mode 21 尾偏移与 hold）、`:445-458`（车速后处理）、
  `:886-908`（AT 初始化序列）。

与计划示例的差异：

- 5.5 节示例只写了 `expr`。`expr` 是**创作形**，导入前必须编译成 `rpn`（工具 `tools/vehicles/expr2rpn.py`，由 P3 提供）；设备只执行 `rpn`，缺 `rpn` 报 `E_SCHEMA`。
- 5.5 节 `hold` 写的是 `"max":30`，实际字段名为 `max_hold`，且 `delta`、`max_hold`、`gap_ms` 三项都必填。
- 服务号白名单按计划取 01/09/21/22；调研 6 节里的 02（冻结帧）未纳入。

## 1. 顶层结构

| 字段 | 必填 | 取值 |
|------|------|------|
| `schema` | 是 | `skygauge.vehicle/1`，可带次版本 `skygauge.vehicle/1.N`；主版本不是 1 一律拒绝 |
| `id` | 是 | `[a-z0-9._-]`，1..31 字符 |
| `rev` | 是 | 1..65535；同 id 低于已安装版本时需要 `"force": true` |
| `name` | 是 | 可打印 ASCII，1..23 字符 |
| `origin`、`meta`、`notes` | 否 | 仅供参考，设备忽略 |
| `aliases` | 否 | 本 profile 也可代表的其他 id（如合并掉的重复车型） |
| `requires.elm` | 否 | 能力列表，只认 `ATMA`、`ATCEA` |
| `bus` | 否（可省略或为 `{}`） | `proto` 0..12（省略/0 = 使用 `ATSP0` 让适配器自动选择协议；1..12 → `ATSP1`..`ATSPC`）、`hdr`（缺省 `7E0`）、`atst` 2..255（缺省 0x19）、`gap_ms` 0..1000（缺省 30）、`check_connection`（缺省 `0100`）、可选 `tester_present`/`tester_present_every_ms`、`init`（≤ 16 条额外 AT，见第 5 节） |
| `drivetrain` | 否 | `fd` 0.1..30、`tire_m` 0.1..1、`gears`（≤ 8 个，0.05..30）、`tol` 0..0.5、`speed_scale` 0.5..2；比值推算挡位需要 `fd`+`tire_m`+至少 1 个齿比 |
| `monitor` | 否 | `mode`：`probe`（需 `every_ms` 250..60000、`window_ms` 10..200）或 `continuous`（不能再有请求源）；`stale_ms` 0..60000；必须声明 `requires.elm: ["ATMA"]` |
| `luts` | 否 | ≤ 8 张查找表，见 2.4 |
| `defaults` | 否 | `obd2`（缺省）或 `none`；缺省时补齐未声明的标准信号 |
| `signals` | 否 | 缺省/空数组继承标准 OBD2；`defaults: "none"` 时必须非空；补齐后的数组（含禁用声明）≤48 项 |

### 1.0 默认信号补齐

根字段 `defaults` 缺省为 `"obd2"`。未声明的标准信号继承内置 Generic OBD2
中的完整定义（请求、公式、单位、过滤及频率），只继承有 `src` 的 10 个信号：

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
| `obd.boost` | `010B` |

显式声明整体替换同 id 默认信号，不能只写 id / 调度字段；仍需完整 `src` 或
`derive`。自定义源失败不会自动追加标准 PID 回退，需自行写入 `src`。
禁用项为 `{"id":"obd.oil_temp","disabled":true}`，只允许 `id`、`disabled`、
`notes`，阻止默认补齐且不进入编译态信号表。重复 id 和未知信号仍报错。
`defaults: "none"` 关闭全部继承，纯 CAN continuous 配置通常应使用此值，
否则补齐的 OBD 请求与 continuous 模式冲突。传动比和比值挡位不自动继承。
继承的请求使用车型 `bus.hdr`（缺省 `7E0`）；`bus` 缺省协议为自动检测，
`ATST=0x19`、`gap_ms=30`。资源限制、频率和依赖在补齐后校验。
运行时仍使用扁平编译态与单线程轮询；原始 JSON 不被修改。
完整可导入的精简示例见 [`vehicles/examples/`](../vehicles/examples/README.md)。

### 1.1 信号

| 字段 | 取值 |
|------|------|
| `id` | 注册表里的 `obd.*` 键，或 `veh.[a-z0-9_]`（总长 ≤ 31）；`sys.*`、已退役的 `obd.brake_temp` 不可用；同一 id 只能出现一次；`veh.*` ≤ 32 个 |
| `name` | 显示名，ASCII 1..23（`veh.*` 用） |
| `unit` | ASCII 1..7。`obd.*`：RPN 结果的单位，必须是 `signals.json` 该信号 `profile_units` 的键，缺省为 `profile_unit_default`；`veh.*`：显示文本 |
| `rate` | `round`（缺省，每轮一次）、`every`（每个请求之后插一次，等同现行 RPM 插队，全 profile 最多 2 个）、`slow`（每 `div` 轮一次，`div` 2..64，缺省 4）、`on_demand`（只在当前页面绑定或规则用到时轮询） |
| `period_ms` | 20..60000，最小轮询间隔；不能和 `every` 同用；所有 `period_ms` 折算的总请求率 ≤ 100 次/秒 |
| `valid` | `[lo, hi]`（RPN 结果单位），最终值在区间外 → 信号无效 |
| `post` | `zero_le`（0..1e6，`v <= zero_le` 时置 0）、`zero_if_rpm_lt`（1..20000，profile 必须有 `obd.rpm`）、`clamp` `[lo, hi]` |
| `filter` | `none`（缺省）、`temp_hyst`（沿用现行油温迟滞）、`ema` |
| `derive` | `ratio`：仅 `obd.gear`，由转速和车速按 `drivetrain` 推算，**不能**再写 `src`；需要 profile 同时有 `obd.rpm` 和 `obd.speed` |
| `fallback` | `ratio`：仅 `obd.gear`，所有源都失败后回退到比值推算；`none`（缺省）表示读不到就无效（显示 `--`） |
| `disabled` | 布尔值，缺省 false；true 时只允许 `id`、`disabled`、`notes`，不轮询该信号 |
| `src` | 1..4 个源，按顺序回退；未禁用且没有 `derive: "ratio"` 时必填 |

### 1.2 源

请求源（有 `req` 和/或 `raw`）：

| 字段 | 取值 |
|------|------|
| `req` | 紧凑十六进制（不带空格，大小写均可）：`03` 无参数；`01`/`02`/`06`/`07`/`09`/`0A`/`1A`/`21` + 1 字节；`22`/`23` + 2 字节 |
| `raw` | 裸 CAN 载荷 1..8 字节（可用单个空格分隔），见第 4 节；必须同时写 `hdr`，不能带 `cea`；同时写了 `req` 时 `req` 必须等于裸帧里的服务载荷 |
| `hdr` | 本请求的头，覆盖 `bus.hdr`（请求后固件恢复） |
| `rx` | 接收过滤 → `ATCRA`（请求后恢复） |
| `cea` | 1 个字节 → `ATCEA`；需要 `requires.elm` 含 `ATCEA`，且头必须是物理寻址 |
| `before` | 当前请求发送前执行的 0..4 条请求级 AT 命令；只允许第 5 节的 request-local 白名单 |
| `after` | 当前请求完成后挂起的 0..4 条请求级 AT 命令；Car Scanner 语义是在下一次请求发生头切换时执行 |
| `len` | 1..64，公式可寻址的数据字节数（响应不足则样本失败） |
| `multiframe` | 响应可能是多帧（ISO-TP 重组） |
| `multiframe_mode` | 可选 `"elm"`：保留多帧响应解析，但交由 ELM 自动处理 ISO-TP；需 `multiframe: true`，且不能用于 `raw` 请求。未设置时沿用固件手动流控 |
| `byte_from_end` | 1..64，需 `multiframe: true`：公式只看响应**最后 n 个字节**，且只能用 `["X"]`（= 倒数第 n 个字节） |
| `hold` | `{delta 0.001..1000, max_hold 1..255, gap_ms 0..60000}`，三项必填，见第 3 节第 4 步 |
| `expect` | 正响应前缀，紧凑十六进制 1..8 字节，首字节必须是服务号 + 0x40；缺省由请求推出（`010C` → `410C`，`221302` → `621302`） |
| `valid` | `[lo, hi]`，样本在区间外算源失败 |
| `fail_after` | 1..50，缺省 5：连续失败这么多次后切到下一个源 |
| `rpn` | 必填，见第 2 节 |
| `expr`、`notes` | 仅供展示 |

CAN 监听源（有 `can`，profile 必须有 `monitor`）：

| 字段 | 取值 |
|------|------|
| `can` | 3 位十六进制（≤ 7FF）或 8 位（≤ 1FFFFFFF） |
| `bit`、`bits` | 必填，`bit` 0..63，`bits` 1..32，`bit + bits <= 64`：这就是本源的 `X` 字段 |
| `endian` | `le`（缺省，Intel，与现行 `can_extract_bits_le` 一致）或 `be`（网络序），定义见 2.3 |
| `signed` | 按 `bits` 宽度补码解释 |
| `valid`、`fail_after`、`rpn`、`expr`、`notes` | 同请求源 |

## 2. RPN

### 2.1 编码

`rpn` 是数组，每个元素是一条指令。**无操作数的指令写成裸字符串，有操作数的写成数组**（`vp_rpn.h` 的 JSON spelling）。
操作码名字（固件枚举 `vp_op_t`）与 JSON 写法一一对应：

| 操作码 | JSON | 栈效果（栈顶在右） | 语义 |
|--------|------|--------------------|------|
| `PUSH_BYTE` | `["A", i]` | `→ d[i]` | 数据字节 `i`（无符号 0..255），`i < len`；中缀记法 A..H = `["A",0]`..`["A",7]` |
| `PUSH_RAW` | `["R", start, bits, opt…]` | `→ field` | 数据中的位域，`bits` 1..32，`start + bits <= len*8`；`opt` 取 `"le"`/`"be"`（缺省 le）和/或 `"u"`/`"s"`（缺省 u） |
| `PUSH_RAW` | `["X"]` | `→ field` | 本源自己的原始字段：CAN 源的 `bit/bits/endian/signed`，或 `byte_from_end` 选中的字节；其他源不可用 |
| `PUSH_CONST` | `["k", c]` | `→ c` | 有限数，`|c| <= 1e7`，加载时舍入为 float |
| `ADD` | `"+"` | `a b → a+b` | |
| `SUB` | `"-"` | `a b → a-b` | |
| `MUL` | `"*"` | `a b → a*b` | |
| `DIV` | `"/"` | `a b → a/b` | `b == 0` → 无效；紧跟在 `["k",0]` 之后静态拒绝 |
| `NEG` | `"neg"` | `a → -a` | |
| `MIN` | `"min"` | `a b → min(a,b)` | |
| `MAX` | `"max"` | `a b → max(a,b)` | |
| `CLAMP` | `"clamp"` | `x lo hi → y` | `y = min(max(x,lo),hi)`；`lo > hi` → 无效 |
| `AND` | `"&"` | `a b → a & b` | 整数运算，见 2.2 |
| `OR` | `"\|"` | `a b → a \| b` | 整数运算 |
| `SHR` | `">>"` | `a n → a >> n` | 逻辑右移，`n` 0..31 |
| `SHL` | `"<<"` | `a n → a << n` | 移出 bit 31 的位丢弃，`n` 0..31 |
| `SEXT8` | `"sext8"` | `a → s` | 低 8 位按有符号解释（-128..127） |
| `SEXT16` | `"sext16"` | `a → s` | 低 16 位按有符号解释 |
| `LUT` | `["lut", n]` | `x → y` | 查 profile 的 `luts[n]`，见 2.4 |

例（RPM，`(A*256+B)/4`）：`[["A",0],["k",256],"*",["A",1],"+",["k",4],"/"]`。

写成 `["PUSH_BYTE",0]`、`["+"]`、`"A"` 之类都是 `E_RPN_OP`。

### 2.2 数值域

- 栈元素是 IEEE-754 binary32（`float`），每条指令的结果都是 float。host 与设备执行同样的 float 运算（实现不得把乘加融合成 FMA）。
- 任何中间值或结果为 NaN/±Inf、除以 0、整数运算越界、数据不够、LUT 未命中 → 本次求值作废，样本记为失败（`vp_rpn_eval` 返回非 `VP_RPN_OK`）。
- 整数运算（`& | >> << sext8 sext16`）：操作数先向零截断，必须落在 `[-2^31, 2^32-1]`，按 32 位补码位型参与运算；结果作为**无符号**值压栈（`sext8/sext16` 的结果有符号）。移位数 `n` 必须在 0..31。

### 2.3 数据寻址

| 源 | 数据 `d[]` | `len` |
|----|-----------|-------|
| 请求源（`req`） | 正响应中**去掉 `expect` 前缀**之后的字节：`010C` → `41 0C AA BB` 中 `d[0]=AA`；多帧时为重组后的整段载荷 | `len` 字段 |
| 裸帧源（`raw`） | 去掉 `expect` 前缀之后的字节 | `len` 字段 |
| `byte_from_end:n` 的源 | 响应的最后 n 个字节；只能用 `["X"]`（= 倒数第 n 个字节），写 `["A",…]`/`["R",…]` 报 `E_RPN_INDEX` | n |
| CAN 源（`can`） | 8 字节帧数据；`["A",i]`、`["R",…]`、`["X"]` 都可用 | 8 |

位域（`["R",start,bits,opt]` 和 CAN 源的 `X`）：

- `le`：位号 `k` 是 `d[k/8]` 的第 `k%8` 位（LSB = 0），从 `start` 起取 `bits` 位、按 LSB 先拼（等同现行 `can_extract_bits_le`）。
- `be`：位号 `k` 是 `d[k/8]` 的第 `7 - k%8` 位，第一个位是 MSB（网络序：`["R",16,16,"be"]` = `d[2]*256+d[3]`）。注意这**不是** DBC 的 Motorola 锯齿编号。
- `s`：按 `bits` 宽度补码解释。

### 2.4 查找表 `luts`

`luts` 写在 profile 顶层，最多 8 张，每张 1..16 项；所有公式共用。每项是 `[key, out]`（`x == key`）或 `[lo, hi, out]`（`lo <= x <= hi`，`lo <= hi`），数值 `|v| <= 1e7`。按顺序取第一个命中的项；没有命中 → 样本无效（没有缺省值）。比较用 float 原值，不取整。

```json
"luts": [[[0, 0], [1, 6, 1], [7, -1]]]
... "rpn": [["A",0],["lut",0]]
```

### 2.5 静态校验（导入时，设备永远重新校验）

1..32 条指令；按栈深模拟不下溢、深度 ≤ 8、结束时栈上恰好 1 个值；`["A",i]` 中 `i < len`；位域在 `len*8` 位以内；`["X"]` 只出现在有原始字段的源里；`["lut",n]` 中 `n < luts 张数`；常量有限且 `|c| <= 1e7`；不允许除以常量 0。

## 3. 求值管线

单个样本按下列顺序处理（P3 引擎与 host 测试必须一致）：

1. 收到应答 → 匹配在途请求的 `expect`（不做全局 `strstr`）→ 取数据字节，检查 `len`；
2. 执行 `rpn`（float），作废则样本失败；
3. `src.valid` 区间外 → 样本失败；
4. `src.hold`：首个样本、或距上次被接受的样本超过 `gap_ms`、或 `|v - last| <= delta` 时接受 `v`；否则重发上一个值，最多连续 `max_hold` 次，之后接受 `v`（照搬 `elm327_ble_client.c:1300-1323`）；
5. 源连续失败 `fail_after` 次 → 切到下一个源；全部失败且 `fallback: "ratio"` → 比值推算；
6. 信号级 `post`：`clamp` 钳位；`obd.rpm < zero_if_rpm_lt` 时置 0；`v <= zero_le` 时置 0；
7. 信号级 `valid` 区间外 → 信号无效（源本身不算失败）；
8. 换算成内部单位：`internal = R(v × factor)`，`R(x) = floor(x + 0.5)`，double 计算；`obd.*` 的 `factor` 取 `signals.json` 中 `profile_units[unit]`，`veh.*` 当前按整数存储（`factor = 1`）；超出 int32 → 无效；
9. 超出注册表 `internal.min/max` → 无效；
10. `filter` 在内部单位上执行；写入信号表（值、valid 位、时间戳）。

## 4. 服务号与寻址白名单

profile 不能任意写 AT 命令；`bus.init` 和请求源的 `before`/`after` 都只能使用受限白名单，其他连接行为仍由固件根据结构化字段拼出命令。

| 项 | 允许 | 拒绝（错误码） |
|----|------|----------------|
| 服务号（`req`，或裸帧里的服务） | 只读：`03`（无参数）；`01`、`02`、`06`、`07`、`09`、`0A`、`1A`、`21`（各带 1 字节参数）；`22`、`23`（各带 2 字节参数） | 其他一切，尤其是会话、复位、安全访问、写入、例程、Tester Present、清除 DTC 等服务，如 `04/10/11/14/19/27/28/2A/2C/2E/2F/31/34-37/3B/3D/3E/85/86/87`（`E_SERVICE`）；参数长度不对（`E_SERVICE`） |
| 发送头 `hdr` / `bus.hdr` | 功能寻址 `7DF`、`18DB33F1`；物理寻址 `7E0`-`7E7`、`600`-`6FF`、`18DAxxF1` | 其他（`E_HDR`） |
| 功能寻址头 | 只用于标准广播安全的 `01`、`02`、`03`、`06`、`07`、`09`、`0A`；不能带 `cea` | `1A`、`21`、`22`、`23` 走功能寻址（`E_HDR`） |
| 接收过滤 `rx` | `7E8`-`7EF`、`600`-`6FF`、`18DAF1xx` | 其他（`E_HDR`） |
| CAN 监听 id `can` | 3 位 ≤ 7FF，或 8 位 ≤ 1FFFFFFF | 其他（`E_HDR`） |
| 裸帧 `raw` | ISO-TP 单帧，1..8 字节。设备同时尝试两种解释：普通寻址（字节 0 = PCI `0x0L`，L = 其后字节数，1..7）和扩展寻址（字节 0 = 目标地址，字节 1 = PCI，L = 其后字节数，1..6）；**每一种成立的解释**里的服务都必须在白名单内、参数长度正确。固件自动加 `ATCAF0`/`ATCAF1` | 两种都不成立（`E_RAWFRAME`）；某种解释的服务被禁止（只有一种解释时 `E_SERVICE`，有歧义时 `E_RAWFRAME`） |

例：`18 03 22 DA 2E` 只能按扩展寻址解释（目标 0x18，PCI 3，服务 22 DID DA2E）。

## 5. AT 命令

固件自己生成的命令（Car Scanner 风格的 `ATZ/ATE0/ATH1/ATSP/ATS0/ATM0/ATAT1` 初始化、按字段生成的 `ATSH/ATCRA/ATCEA/ATCAF0/ATCAF1`、`monitor` 窗口的 `ATH1/ATMA/ATH0/ATAR`、验车用的 `ATRV`/`ATI`）都在固件代码里，不由 profile 决定。请求源的 `before`/`after` 可以增加只作用于该请求链的 AT 命令：`after` 不会在同一头下的下一个请求前立即执行，而是保留到下一次请求需要切换 CAN 头时，先执行前一请求的 `after`，再设置新请求状态。`ATAL`、`ATST` 等旧适配器调优命令只有在 `bus.init` 显式提供时才发送。发送层在发出前用 `vp_at_check()` 做白名单断言（纵深防御）。

请求级命令失败时，当前请求链 fail-closed：不再发送同一请求剩余的 `before` 命令，也不发送 OBD 请求本身；已经挂起的 `after` 仍可在后续头切换时尝试执行。`after` 命令失败不会伪造请求成功，也不会阻止后续请求继续调度。

`bus.init` 允许最多 16 条额外命令；AT 命令去掉空格、转大写后只接受。另有一个仅用于 Nissan Consult II 的精确原始初始化帧 `2212010401`，它不作为通用 AT 转义开放，只能出现在 `bus.init`：

翻译 Car Scanner 的完整 `InitSequence` 时，`ATZ`、`ATD`、`ATD0`、`ATE0`、`ATH1`、`ATS0`、`ATM0`、`ATAT1`、`ATAL` 也可以按原顺序放入 `bus.init`。只有同时包含复位命令（`ATZ`、`ATD` 或 `ATD0`）、`ATSP*`、`ATE0`、`ATH1`、`ATS0`、`ATM0`、`ATAT1` 时，固件才把这个数组视为完整初始化序列并按原顺序发送；只有部分命令的配置会在 profile 命令后补齐缺失的通用序列。`DELAY<n>` / `WAIT<n>` 只作为本地等待，不会发送给适配器。

| 命令 | 条件 |
|------|------|
| `ATSH<hdr>` | 头在第 4 节白名单内 |
| `ATSH8111F1` / `ATSH8101F1` / `ATSH8001F1` | 仅 `bus.init`，且显式 `bus.proto` 为 4/5；铃木 KWP 三字节头，非 CAN 字段 |
| `ATCRA` / `ATCRA<rx>` | 过滤在白名单内 |
| `ATCAF1` | — |
| `ATST<hh>` | 02..FF |
| `ATSP<h>` / `ATSPA<h>` | `h` = 0-9、A-C（`SPA` 时 1-9、A-C） |
| `ATFCSH<hdr>` | 11 位或 29 位 CAN 头，用于适配器流控 |
| `ATFCSD<hh...>` | 3 或 4 个 CAN 数据字节，用于适配器流控 |
| `ATFCSM0` / `ATFCSM1` | 适配器流控模式 |
| `ATCP<hh>` | CAN 优先级/协议字节 |
| `ATIB10` | ISO 9141 初始化波特率 |
| `ATSW<hh>` | 旧协议帧间隔/时序参数 |
| `ATIIA` / `ATIIA<hh>` | ISO 初始化地址 |
| `ATSI` / `ATFI` | 旧协议初始化/快速初始化 |
| `ATWM<hh...>` | 旧协议唤醒数据，2..14 个十六进制字符 |
| `ATTAF1` / `ATCER` | 旧适配器初始化分支命令 |

`bus.init` 里 `ATCEA`、`ATCAF0` 仍拒绝（它们会改变后续所有请求的帧格式，只能经源的 `cea`/`raw` 字段、由固件包在单个请求两侧）。上表中的旧协议命令只在 `bus.init` 中按参数形状接受；在固件生成上下文和请求级 `before`/`after` 中仍拒绝。明确拒绝并给出原因的：`ATPP*`（写适配器 EEPROM）、`ATBRD`/`ATBRT`、`ATWS`、`ATLP`、`ATMA`/`ATMR`/`ATMT`（监听只能经 `monitor` 块）、`ATSD`/`ATKW`/`ATCSM`/`ATCV`/`ATR0`/`ATE1`/`ATL1`/`ATH*`（格式由固件固定），以及任何非 AT 命令。

请求源的 `before`/`after` 使用更窄的 request-local 白名单：允许 `ATCAF0`/`ATCAF1`、`ATCFC0`/`ATCFC1`、`ATAL`，以及受控的 `ATCEA<hh>`；不允许复位、协议选择、全局头/过滤器、监听、持久化或其他会逃逸当前请求作用域的命令。`ATCEA<hh>` 仍需满足该请求的扩展寻址和 ELM 能力约束。

## 6. 上限

| 项 | 上限 |
|----|------|
| profile 文本 | 16384 字节（`E_SIZE`） |
| 信号 | 48；其中 `veh.*` 32 |
| 每个信号的源 | 4；全 profile 96 |
| 不同的请求（按头、过滤、扩展地址、发送字节去重） | 32 |
| CAN 监听源 | 16 |
| `rate: "every"` 的信号 | 2 |
| 总请求率（`period_ms` 折算） | 100 次/秒；单个 `period_ms` ≥ 20 |
| 查找表 | 8 张 × 16 项 |
| 齿比 | 8 |
| `bus.init` | 16 条 |
| 公式 | 32 条指令、栈深 8、常量 `|c| <= 1e7`、数据 64 字节 |

## 7. 错误码

在 HTTP（422 `{errors:[{code,path,msg}]}`）、SD `IMPORT.LOG`、UI 四处统一（`vp_validate.h`）：

| 码 | 含义 |
|----|------|
| `E_NOMEM` | 默认 OBD2 补齐的临时 JSON 内存分配失败 |
| `E_SIZE` | 文本超过 16384 字节 |
| `E_SCHEMA` | 不是 JSON、类型错、缺字段、未知枚举值、主版本不支持、字段组合不一致（如 `raw` 带 `cea`、`byte_from_end` 无 `multiframe`、`derive` 与 `src` 同时出现、缺 `rpn`） |
| `E_LIMIT` | 数量或大小超限（含公式 > 32 条指令） |
| `E_NAME` | id、name、信号 id 格式不对 |
| `E_SIGNAL` | `obd.*` 不在注册表、重复信号、id 不是 `obd.*`/`veh.*` |
| `E_SERVICE` | 服务号不在白名单或参数长度不对 |
| `E_AT` | `bus.init` 命令不在白名单；`bus.proto`/`bus.atst` 越界 |
| `E_HDR` | 头/接收过滤/CAN id/扩展地址格式不对；功能寻址发 21/22 |
| `E_RAWFRAME` | 裸帧不是单帧，或某种解释被禁止 |
| `E_RPN_OP` | 未知或格式错误的操作码 |
| `E_RPN_STACK` | 栈下溢/溢出/结束时不是恰好 1 个值/空程序 |
| `E_RPN_INDEX` | 字节下标、位域、`X`、LUT 下标越界 |
| `E_RPN_CONST` | 常量非有限、过大、除以常量 0 |
| `E_RATE` | 轮询/监听时序超出速率限制 |
| `W_UNKNOWN` | （警告）未知字段，忽略 |
| `W_UNUSED` | （警告）声明了但没用上（如有 `monitor` 却没有 CAN 源） |

警告不会让 profile 失效。HTTP/SD 侧另有 `E_SHA`、`E_BUSY`、`E_NO_SD`、`E_NO_SPACE`、`E_STATE`、`E_REV_DOWNGRADE`，运行时有 `E_ADAPTER_CAPS`（`requires` 不满足）。

## 8. 运行时执行器约束

- 无跳转、无循环；栈在调用方栈帧上（`float st[8]`），执行器不分配内存；
- 编译后的 profile 是一块位置无关的 blob，经可注入的分配器放 PSRAM（`vp_profile.h`）；
- 静态校验已排除越界，执行器仍逐条检查栈和数据边界（纵深防御）；
- 调度保持单线程、靠 `>` 提示符门控（AGENTS.md 红线）。

## 9. 示例

`vehicles/catalog/subaru.zc6.can.vehicle.json` 节选（由 `tools/vehicles/legacy_to_v1.py` 从旧表生成；水温、
油门、机油温度来自 CAN 广播，旧固件在这三项上不发 OBD 请求，所以只有 CAN 源）：

```json
{"schema":"skygauge.vehicle/1","id":"subaru.zc6.can","rev":1,"name":"ZN/C6 CAN",
 "requires":{"elm":["ATMA"]},"bus":{"proto":6,"atst":10,"gap_ms":1},
 "drivetrain":{"fd":4.1,"tire_m":0.314,"gears":[3.626,2.188,1.541,1.213,1,0.767],"tol":0.15},
 "monitor":{"mode":"probe","every_ms":500,"window_ms":120,"stale_ms":15000},
 "signals":[
  {"id":"obd.rpm","rate":"every","src":[{"req":"010C","len":2,"expr":"(A*256+B)/4",
    "rpn":[["A",0],["k",256],"*",["A",1],"+",["k",4],"/"]}]},
  {"id":"obd.speed","post":{"zero_le":2,"zero_if_rpm_lt":800},"src":[{"req":"010D","len":1,"expr":"A","rpn":[["A",0]]}]},
  {"id":"obd.coolant_temp","src":[{"can":"360","bit":24,"bits":8,"expr":"X-40","rpn":[["X"],["k",40],"-"]}]},
  {"id":"obd.oil_temp","valid":[-20,255],"src":[{"can":"360","bit":16,"bits":8,"expr":"X-40","rpn":[["X"],["k",40],"-"]}]},
  {"id":"obd.battery_voltage","unit":"mV","src":[{"req":"0142","len":2,"expr":"A*256+B","rpn":[["R",0,16,"be"]]}]},
  {"id":"obd.gear","derive":"ratio"}]}
```

ZN/C6 PID 的机油温度（Mode 21 多帧，倒数第 5 字节，跳变保持）：

```json
{"id":"obd.oil_temp","valid":[-20,255],"filter":"temp_hyst","src":[{"req":"2101","multiframe":true,"multiframe_mode":"elm","byte_from_end":5,
  "valid":[-10,255],"hold":{"delta":8,"max_hold":30,"gap_ms":3000},"expr":"X-40","rpn":[["X"],["k",40],"-"]}]}
```

BMW F/G 挡位（EGS 扩展寻址裸帧；一个越界或失败的样本就回落到速比推算，与旧固件一致）：

```json
{"id":"obd.gear","fallback":"ratio","src":[{"raw":"18 03 22 DA 2E","hdr":"6F1","rx":"618","expect":"62DA2E",
  "len":1,"valid":[0,8],"fail_after":1,"expr":"A","rpn":[["A",0]]}]}
```

## 10. 待办（契约与设备实现之间的已知缺口）

目前没有已知的 profile 字段兼容缺口；未实现的字段不会进入可导入契约。

已完成（留作记录）：`obd.*` 的 `unit` 由校验器按注册表的 `profile_units` 检查（不在其中报 `E_SCHEMA`），
`vp_compile` 把缺省单位（`profile_units` 第一项）写进 `vp_signal_t.unit`，引擎按它取 `factor` 做第 3 节第 8 步；
`tools/vehicles/legacy_to_v1.py` 按本文件输出 17 个目录车型，`tools/vehicles/expr2rpn.py --check` 验证每个
`expr` 与 `rpn` 等值；旧固件行为基线见 `test/host/vehicle/fixtures/golden/README.md`。
