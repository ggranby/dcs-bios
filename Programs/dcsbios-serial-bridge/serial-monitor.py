"""
DCS-BIOS Serial Bridge - COM Port Monitor
Usage:  python serial-monitor.py [COM_NUMBER]
        python serial-monitor.py 11          # monitor COM11

If no argument is given, COM11 is used by default.

Requires: pip install pyserial
"""

import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is not installed.  Run:  pip install pyserial")
    sys.exit(1)

BAUD = 250000

def main():
    port_num = int(sys.argv[1]) if len(sys.argv) > 1 else 11
    port_name = f"COM{port_num}"

    print(f"DCS-BIOS Serial Monitor  |  {port_name} @ {BAUD} 8N1")
    print("Press Ctrl-C to stop.\n")

    try:
        ser = serial.Serial(
            port=port_name,
            baudrate=BAUD,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=1.0,
            dsrdtr=False,
            rtscts=False,
        )
    except serial.SerialException as exc:
        print(f"Cannot open {port_name}: {exc}")
        sys.exit(1)

    total_bytes = 0
    read_count = 0
    last_report = time.monotonic()
    bytes_since_report = 0
    last_rx = None

    print(f"Opened {port_name}.  Waiting for data ...\n")

    try:
        while True:
            chunk = ser.read(ser.in_waiting or 1)
            if chunk:
                total_bytes += len(chunk)
                bytes_since_report += len(chunk)
                read_count += 1
                last_rx = time.monotonic()
                hex_preview = chunk[:32].hex(" ").upper()
                suffix = " ..." if len(chunk) > 32 else ""
                print(f"  [{read_count:>6}]  {len(chunk):>4} bytes  |  {hex_preview}{suffix}")

            now = time.monotonic()
            if now - last_report >= 5.0:
                elapsed = now - last_report
                rate = bytes_since_report / elapsed
                idle = (now - last_rx) if last_rx is not None else None
                idle_str = f"  last rx {idle:.1f}s ago" if idle is not None else ""
                print(f"\n  --- 5s summary: {bytes_since_report} bytes  |  {rate:.0f} B/s  |  total {total_bytes}{idle_str} ---\n")
                bytes_since_report = 0
                last_report = now

    except KeyboardInterrupt:
        print(f"\nStopped.  Total bytes received: {total_bytes}")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
