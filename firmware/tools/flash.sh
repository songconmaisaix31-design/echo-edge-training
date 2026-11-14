#!/usr/bin/env bash
# 在 Blink / Probe 两份固件之间切换（按分区烧录，不动 VFS）
#
# 用法：
#   bash flash.sh Blink            # 烧回 LED 一亮一灭
#   bash flash.sh Probe            # 烧硬件探测固件（I2C 扫描 + GPIO 快照）
#   bash flash.sh Blink COM4       # 指定端口
#
# 端口说明：板子插原生 USB 口时是 COM5（VID:303A）；
#           插板载 USB-UART 口(CH343)时是 COM4。脚本默认 COM5。

set -e
cd "$(dirname "$0")"

TARGET="${1:-}"
PORT="${2:-}"
BA="tools/data/packages/esp32/hardware/esp32/3.3.12/tools/partitions/boot_app0.bin"

# 不指定端口时自动找板子（ESP32-S3 原生 USB 或 CH343 转串口）
if [ -z "$PORT" ]; then
  PORT=$(python -c "import serial.tools.list_ports as p; ps=[x.device for x in p.comports() if x.vid in (0x303A,0x1A86)]; print(ps[0] if ps else '')" 2>/dev/null)
  if [ -z "$PORT" ]; then
    echo "没找到板子。请确认 USB 线插好了，而且这根线能传数据（不是纯充电线）。"
    exit 1
  fi
  echo "自动找到板子：$PORT"
fi

case "$TARGET" in
  Blink)      DIR="build" ;;
  Probe)      DIR="build_probe" ;;
  WireHelper) DIR="build_wirehelper" ;;
  ScreenLink) DIR="build_screenlink" ;;
  EpdTest)    DIR="build_epd" ;;
  EcwBridge)  DIR="build_ecwbridge" ;;
  EcwHud)     DIR="build_ecwhud" ;;
  SensorHub)  DIR="build_sensorhub" ;;
  OledTest)   DIR="build_oled" ;;
  *) echo "用法: bash flash.sh Blink|Probe|WireHelper|ScreenLink|EpdTest|OledTest|EcwBridge|EcwHud|SensorHub [PORT]"; exit 1 ;; 
esac

if [ ! -f "$DIR/$TARGET.ino.bin" ]; then
  echo "缺少 $DIR/$TARGET.ino.bin，请先编译"
  exit 1
fi

echo "烧录 $TARGET -> $PORT ..."
python -m esptool --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0000 "$DIR/$TARGET.ino.bootloader.bin" \
  0x8000 "$DIR/$TARGET.ino.partitions.bin" \
  0xe000 "$BA" \
  0x10000 "$DIR/$TARGET.ino.bin"

echo "完成。"
