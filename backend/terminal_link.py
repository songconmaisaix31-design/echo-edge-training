import os
import re
from threading import Lock
from time import monotonic

import serial
from fastapi import HTTPException


TERMINAL_LOCK = Lock()
HINTS = {"A4C": "VIEW A4C", "PSAX": "VIEW PSAX", None: "VIEW UNKNOWN"}
COUNTER = re.compile(r"提示收/回\s*:\s*(\d+)\s*/\s*(\d+)")
ACK = re.compile(r"ACK,(\d+)")


def wait_for(port, pattern):
    deadline = monotonic() + 3
    while monotonic() < deadline:
        match = pattern.fullmatch(port.readline().decode("utf-8", "replace").strip())
        if match:
            return match
    raise HTTPException(504, "终端未在规定时间内回执，请先确认设备状态")


def send_view(view):
    port_name = os.environ.get("ECHO_TERMINAL_PORT")
    if not port_name:
        raise HTTPException(503, "未配置 ESP32-S3 串口")
    hint = HINTS[view]
    if not TERMINAL_LOCK.acquire(blocking=False):
        raise HTTPException(503, "正在向终端发送另一条提示")
    try:
        with serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=2) as port:
            port.port = port_name
            port.dtr = False
            port.rts = False
            port.open()
            port.reset_input_buffer()
            port.write(b"STAT\r\n")
            count = wait_for(port, COUNTER)
            received, acknowledged = map(int, count.groups())
            if received != acknowledged:
                raise HTTPException(502, "设备提示计数与回执计数不一致")
            started = monotonic()
            port.write(f"HINT {hint}\r\n".encode("ascii"))
            sequence = int(wait_for(port, ACK)[1])
            if sequence != (received + 1) % (1 << 32):
                raise HTTPException(502, "终端回执序号不符合预期，请先确认设备状态")
            return {"hint": hint, "sequence": sequence, "ack_ms": round((monotonic() - started) * 1000, 3)}
    except (OSError, serial.SerialException) as exc:
        raise HTTPException(503, "无法通过已配置的串口连接 ESP32-S3") from exc
    finally:
        TERMINAL_LOCK.release()
