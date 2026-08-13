# 自动协议检测功能说明

## 功能概述

现在仪表支持 **自动 OBD 协议检测**！这意味着当你连接到 OBD 设备时，仪表会自动尝试所有协议（1-11），找到能正常工作的那个协议，无需手动尝试。

## 使用方法

### 第一次使用（推荐）

1. **进入设置 → OBD 协议**（页面路径：主屏 → 上/下滑到版本页 → 下滑进设置页）
2. **选择 "0 - Automatic"**（第一个选项），屏幕提示 "Saved"
3. **进入 BLE SCAN 页面，连接你的 OBD 设备**（主屏左滑可达）
4. 仪表会自动：
   - 尝试协议 1-11
   - 对每个协议发送标准查询命令 `01 0C`（读转速 RPM）
   - 当看到有效响应时停止并保存该协议
5. **完成！** 协议已自动保存到仪表，下次不需要重新检测

### 工作原理

```
设置 Protocol = 0 (Auto)
     ↓
BLE 连接
     ↓
发送初始化命令 (ATZ, ATE0 等)
     ↓
循环尝试协议 1-11：
  - 发送 ATSP[1-11] 设置协议
  - 发送 01 0C 读 RPM 测试
  - 如果收到有效 RPM 数据 → 成功！保存到 NVS
  - 如果超时 → 下一个协议
     ↓
检测完成 → 正常轮询开始
```

## 特点

| 功能 | 说明 |
|-----|------|
| **自动保存** | 检测成功后自动保存到 NVS，下次启动无需重检 |
| **快速检测** | 每个协议只等待 2 秒，总耗时 < 30 秒 |
| **多车型适配** | 支持所有 ELM327 兼容的 OBD 协议 |
| **无损检测** | 检测过程中不会更新异常数据到显示 |
| **发生断开时重检** | 下次重新连接时不会自动重检（除非协议设为 0） |

## 日志输出示例

启用 DEBUG 日志后（`idf.py monitor`），会看到类似的检测过程：

```
elm327_ble: === Starting protocol auto-detect ===
elm327_ble: [DETECT] Trying protocol 1...
elm327_ble: [DETECT] Sent 01 0C, waiting...
elm327_ble: [DETECT] Protocol 1: No valid response (timeout)
elm327_ble: [DETECT] Trying protocol 2...
...
elm327_ble: [DETECT] Trying protocol 6...
elm327_ble: [DETECT] Sent 01 0C, waiting...
elm327_ble: [DETECT] Protocol 6: SUCCESS! (RPM=800)
elm327_ble: [DETECT] Protocol 6: SUCCESS! Saving protocol 6 to NVS
elm327_ble: === Protocol auto-detect completed in ~6 seconds ===
elm327_ble:  AT init Cmd send ATZ
elm327_ble:  AT init Cmd send ATE0
... (正常轮询开始)
```

## 什么时候需要手动选择协议

如果遇到以下情况，可手动指定协议而不使用自动检测：

1. **你已知道正确的协议**（如 BRZ ZC6 用协议 6）
   → 直接选择该协议，跳过自动检测过程

2. **自动检测失败（所有协议都无响应）**
   → 尝试手动逐一测试协议 1-11
   → 查看我们的 [BRZ_ZD8_PROTOCOL_GUIDE.md](./BRZ_ZD8_PROTOCOL_GUIDE.md)

3. **OBD 设备连接不稳定**
   → 在设置中关闭自动检测，使用固定协议

## 协议列表

| 编号 | 描述 | 推荐车型 |
|-----|------|--------|
| 0 | **自动检测** (推荐) | 所有车型 |
| 1 | SAE J1850 PWM | 美国车（Ford 等） |
| 2 | SAE J1850 VPW | 美国车（GM 等） |
| 3 | ISO 9141-2 (10.4k) | 欧洲车（旧） |
| 4 | ISO KWP2000 (5 baud) | 欧洲车 |
| 5 | ISO KWP2000 (fast) | 欧洲车（速度快） |
| 6 | ISO 15765-4 CAN @500k | **Subaru BRZ ZC6** |
| 7 | ISO 15765-4 CAN @500k (29bit) | 现代车型 |
| 8 | ISO 15765-4 CAN @250k | 某些车型 |
| 9 | ISO 15765-4 CAN @250k (29bit) | 某些车型 |
| 10 | 标准协议 | 备选 |
| 11 | 自适应 | 备选 |

## 常见问题

### Q: 自动检测花了多久？
**A:** 
- 每个协议 2 秒超时 × 11 个协议 = 最多 22 秒
- 通常在 3-8 秒内成功（取决于第几个协议有效）

### Q: 检测失败会怎样？
**A:** 
- 若所有协议都失败，系统自动降级到协议 6（Subaru 默认）
- 日志输出："Protocol auto-detect FAILED, using fallback protocol 6"
- 下次仍会再次尝试自动检测

### Q: 如何禁用自动检测？
**A:** 
- 进入设置，选择具体协议号（1-11 之一），而不是 "0 - Automatic"
- 系统会跳过检测，直接使用选定的协议

### Q: 检测时会看到错误的数据吗？
**A:** 
- **不会！** 检测过程中只记录 RPM 值，不会更新其他参数到缓存
- 检测完成后才开始正常的数据轮询

### Q: BRZ ZD8 用什么协议？
**A:** 
- 未知。请首次使用自动检测，仪表会自动找到正确的协议
- 检测完成后，在日志/故障排查文档中可查看

---

### Q: 可以中途取消检测吗？

可以，断开 BLE 连接即可。检测状态不会残留，下次连接重新开始。

### Q: 多台设备可以用不同协议吗？

可以。协议是各表自己存在本机 NVS 里的，互不影响；每块表记住自己最后一次检测成功的协议。

## 调试技巧

### 强制重新检测
1. 设置协议为 "0 - Automatic"
2. **断开 BLE 连接**
3. 重新连接 OBD 设备
4. 仪表会再次自动检测

### 查看详细日志
```bash
idf.py monitor -l DEBUG  # 显示所有 DEBUG 级别日志
# 或仅查看 BLE 日志:
idf.py monitor | grep elm327
```

### 保存检测结果
```bash
idf.py monitor | grep "PROTOCOL_DETECT\|SUCCESS\|FAILED"
```

---

如有任何问题，参考 [OBD_TROUBLESHOOTING.md](./OBD_TROUBLESHOOTING.md) 或 [BRZ_ZD8_PROTOCOL_GUIDE.md](./BRZ_ZD8_PROTOCOL_GUIDE.md)。
