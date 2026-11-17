# Sparse vehicle profile examples

English | [简体中文](README.md)

The two sparse examples use default OBD2 inheritance and require firmware with that feature.
They are separate from the 17 catalog profiles and 18 complete import templates. Copy
one `*.vehicle.json` to the SD card's `OBD/IMPORT/`, install, activate on the Vehicle
page and reboot.

| 文件 | 显式定义 |
|---|---|
| `custom.obd2.sparse.vehicle.json` | 仅 schema / id / rev / name，默认补齐 9 个标准 OBD2 信号，不推算挡位 |
| `custom.bmw.g.sparse.vehicle.json` | 协议 6 + 功能头 `7DF`，仅定义 BMW G 的 EGS 原始请求 `18 03 22 DA 2E`；标准信号继承 OBD2 |

The BMW example reuses the existing F/G gear path without additional vehicle testing.
It only reads gear directly and displays `--` on failure. To add ratio fallback, supply
the vehicle's own `drivetrain` and `fallback: "ratio"`.

An explicit signal replaces its whole default definition. Add
`{"id":"obd.oil_temp","disabled":true}` to disable one default signal; root
`"defaults":"none"` disables all inheritance. Inherited OBD requests use `bus.hdr`,
while special requests use their own `src.hdr`. See
[PID_GUIDE.md](../../docs/PID_GUIDE.en.md) for the full rules.

The fixed-protocol example [`custom.obd2.iso9141.vehicle.json`](custom.obd2.iso9141.vehicle.json) explicitly declares standard Mode 01 requests and locks `bus.proto` to `3` (ISO 9141-2, 5 baud init, 10.4 kbaud). After installation, activate `custom.obd2.iso9141` through the web interface or app and reboot; it overrides the automatic protocol selection in settings. It contains no vehicle-specific PIDs or ratio-derived gear; unsupported signals still show `--`.

```sh
python3 contracts/check_contracts.py --profiles vehicles/examples
```
