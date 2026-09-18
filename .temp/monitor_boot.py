#!/usr/bin/env python3
"""抓取 solar_os 启动日志，验证 PCA9557 时序修复效果。"""
import serial
import sys
import time

PORT = "/dev/cu.usbmodem14301"
BAUD = 115200
DURATION = 12.0

ser = serial.Serial(PORT, BAUD, timeout=1)
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
time.sleep(0.1)
ser.dtr = True

start = time.time()
buf = []
try:
    while time.time() - start < DURATION:
        data = ser.read(4096)
        if data:
            text = data.decode("utf-8", errors="replace")
            buf.append(text)
            sys.stdout.write(text)
            sys.stdout.flush()
except KeyboardInterrupt:
    pass
finally:
    ser.close()

log = "".join(buf)
with open("/Users/tianyaohui/solar_os/.temp/boot_log.txt", "w") as f:
    f.write(log)
print("\n--- 日志已保存 ---")
