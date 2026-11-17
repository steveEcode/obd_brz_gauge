# 精简车型配置示例

[English](README.en.md) | 简体中文

其中两份精简示例使用默认 OBD2 继承，需包含该功能的固件；不计入 17 份车型目录或
18 份完整导入模板。将选中的 `*.vehicle.json` 复制到 SD 卡 `OBD/IMPORT/`，
安装后在 Vehicle 页激活并重启。

| 文件 | 显式定义 |
|---|---|
| `custom.obd2.sparse.vehicle.json` | 仅 schema / id / rev / name，默认补齐 9 个标准 OBD2 信号，不推算挡位 |
| `custom.bmw.g.sparse.vehicle.json` | 协议 6 + 功能头 `7DF`，仅定义 BMW G 的 EGS 原始请求 `18 03 22 DA 2E`；标准信号继承 OBD2 |

BMW 示例复用现有 F/G 挡位路径，未新增实车验证。它仅直读挡位，失败显示 `--`；
若需齿比回退，必须自行添加本车的 `drivetrain` 和 `fallback: "ratio"`。

信号声明整体覆盖默认定义。禁用某个默认信号时加
`{"id":"obd.oil_temp","disabled":true}`；根字段 `"defaults":"none"`
关闭全部继承。继承的 OBD 请求使用 `bus.hdr`，特殊请求用自己的 `src.hdr`。
完整规则见 [PID_GUIDE.md](../../docs/PID_GUIDE.md)。

固定协议示例 [`custom.obd2.iso9141.vehicle.json`](custom.obd2.iso9141.vehicle.json) 完整声明标准 Mode 01 请求，`bus.proto` 固定为 `3`（ISO 9141-2，5 baud init，10.4 kbaud）。安装后需通过网页或 App 激活 `custom.obd2.iso9141` 并重启；它会覆盖界面的自动协议选择。不包含车型专用 PID 或挡位推算；ECU 不支持的信号仍显示 `--`。

```sh
python3 contracts/check_contracts.py --profiles vehicles/examples
```
