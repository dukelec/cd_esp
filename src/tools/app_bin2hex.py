#!/usr/bin/env python3
#
# Software License Agreement (MIT License)
#
# Author: Duke Fong <d@d-l.io>

import struct
import sys

OTA_ADDR = 0x187000


def modbus_crc(frame):
    crc = 0xFFFF
    for b in frame:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def _ihex_checksum(record_bytes):
    return ((-sum(record_bytes)) & 0xFF)


def _ihex_record(record_type, address, data):
    ll = len(data)
    rec = bytearray()
    rec.append(ll & 0xFF)
    rec.append((address >> 8) & 0xFF)
    rec.append(address & 0xFF)
    rec.append(record_type & 0xFF)
    rec.extend(data)
    rec.append(_ihex_checksum(rec))
    return ":" + rec.hex().upper()


def write_intel_hex(start_address, data, hex_file, record_size=16):
    with open(hex_file, "w", newline="\n") as f:
        offset = 0
        current_upper = None

        while offset < len(data):
            addr = start_address + offset
            upper = (addr >> 16) & 0xFFFF
            if upper != current_upper:
                current_upper = upper
                f.write(_ihex_record(0x04, 0x0000, current_upper.to_bytes(2, byteorder="big")) + "\n")

            lower = addr & 0xFFFF
            chunk = data[offset: offset + min(record_size, len(data) - offset)]
            f.write(_ihex_record(0x00, lower, chunk) + "\n")
            offset += len(chunk)

        f.write(_ihex_record(0x01, 0x0000, b"") + "\n")


def bin_to_hex_with_size_header(bin_file, hex_file):
    with open(bin_file, 'rb') as f:
        bin_data = f.read()

    size = len(bin_data)
    print(f"Binary size: {size} bytes")
    bin_data += modbus_crc(bin_data).to_bytes(2, byteorder='little')

    size_header = struct.pack('<I', size | 0xcd000000)
    full_data = size_header + bin_data

    write_intel_hex(OTA_ADDR, full_data, hex_file)
    print(f"Intel HEX file written to {hex_file}")


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print("usage: ./script.py input.bin")
    else:
        bin_to_hex_with_size_header(sys.argv[1], f'{sys.argv[1][:-4]}.hex')

