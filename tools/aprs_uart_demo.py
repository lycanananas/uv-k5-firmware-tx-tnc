#!/usr/bin/env python3
"""Send one APRS object report to the APRS TX firmware over UART."""

import argparse
import sys
from datetime import datetime, timezone

try:
    import serial
except ImportError as error:
    raise SystemExit("Install pyserial first: python3 -m pip install pyserial") from error


SOURCE = "SQ5FOX-1"
DESTINATION = "APZ001"
OBJECT_NAME = "SQ5FOX-T "


def build_frame() -> str:
    timestamp = datetime.now(timezone.utc).strftime("%d%H%Mz")
    return f"{SOURCE}>{DESTINATION}:;{OBJECT_NAME}*{timestamp}5214.77N/02107.48E>APRS demo"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="serial port connected to the radio, e.g. /dev/ttyUSB0")
    parser.add_argument("--baudrate", type=int, default=38400, help="UART speed (default: 38400)")
    parser.add_argument("--qrg", help="set radio frequency only, e.g. 144.800")
    args = parser.parse_args()

    frame = f"!QRG {args.qrg}" if args.qrg else build_frame()
    payload = (frame + "\n").encode("ascii")
    with serial.Serial(args.port, args.baudrate, timeout=1) as connection:
        connection.write(payload)
        connection.flush()

    print(frame)
    return 0


if __name__ == "__main__":
    sys.exit(main())
