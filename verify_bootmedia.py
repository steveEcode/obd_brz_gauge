import sys
sys.path.append('/Users/parklu/.espressif/v5.5.3/esp-idf/components/esptool_py/esptool')
import esptool

# 连接设备
esp = esptool.ESP32S3ROM('/dev/cu.usbmodem101', baud=115200)
esp.connect()

# 读取 bootmedia 分区前 512 字节（manifest 区域）
print("Reading bootmedia partition at 0xA20000...")
data = esp.read_flash(0xA20000, 512)

# 显示前 256 字节
print("\n=== First 256 bytes (should be boot_block.txt manifest) ===")
try:
    text = data[:256].decode('utf-8', errors='replace')
    print(text)
except:
    print("Not valid UTF-8, showing hex:")
    print(data[:256].hex())

# 检查是否全是 0xFF (未写入)
if data == b'\xff' * 512:
    print("\n❌ Partition is EMPTY (all 0xFF) - flash failed!")
else:
    print("\n✅ Partition has data")

esp._port.close()
