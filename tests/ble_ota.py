#!/usr/bin/env python3
#
# Software License Agreement (MIT License)
#
# Author: Duke Fong <d@d-l.io>

import sys
import struct
import argparse
import asyncio
import logging
import time
from PyCRC.CRC16 import CRC16
from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic

logger = logging.getLogger(__name__)
logging.basicConfig(
    level=logging.INFO, #logging.DEBUG
    format="%(asctime)-15s %(name)-8s %(levelname)s: %(message)s",
)
queue = asyncio.Queue()


ADDRESS = None
MOSI_UUID="b3340002-56ba-40b1-8ecb-8fe18dfffddd"
MISO_UUID="b3340003-56ba-40b1-8ecb-8fe18dfffddd"

ble_client = None


def notification_handler(characteristic: BleakGATTCharacteristic, data: bytearray):
    #logger.info("%s: %r", characteristic.description, data)
    queue.put_nowait(data)


async def send_command(msg, wait=True):
    print(f'send_comand, len: {len(msg)}, msg: {msg.hex()}')
    await ble_client.write_gatt_char(MOSI_UUID, msg, response=False)
    if wait:
        result = await queue.get()
        return result


async def csa_write(offset, dat):
    msg = b'\x40\x05\x20' + struct.pack("<H", offset) + dat
    ret = await send_command(msg)
    if ret == None or ret[2] != 0:
        print(f'csa_write error at: 0x{offset:x}: {dat.hex()}')
    return ret


def modbus_crc(dat):
    return CRC16(modbus_flag=True).calculate(dat)


addr_begin = 0x110000
sub_size = 244-6

async def flash_read(len_):
    addr = addr_begin
    cur = addr
    rdat = b''
    while True:
        size = min(sub_size, len_ - (cur - addr))
        if size == 0:
            break
        tdat = b'\x40\x08\x00' + struct.pack("<IB", cur, size)
        ret = await send_command(tdat)
        rdat += ret[3:]
        cur += size
    return rdat


async def flash_write(dat):
    addr = addr_begin
    cur = addr
    while True:
        size = min(sub_size, len(dat) - (cur - addr))
        if size == 0:
            break
        # a0: 20 | 80
        tdat = b'\x40\x08\xa0' + struct.pack("<I", cur) + dat[cur-addr:cur-addr+size]
        await send_command(tdat, False)
        cur += size


async def flash_erase(len_):
    addr = addr_begin
    tdat = b'\x40\x08\x2f' + struct.pack("<II", addr, len_)
    print(f"flash_erase: len: {len_}")
    ret = await send_command(tdat)
    print(f"flash_erase: {ret.hex()}")


async def flash_crc(len_):
    addr = addr_begin
    tdat = b'\x40\x08\x10' + struct.pack("<II", addr, len_)
    print(f"flash_crc: len: {len_}")
    ret = await send_command(tdat)
    print(f"flash_crc: {ret.hex()}")
    return struct.unpack("<H", ret[3:])[0]


async def main():
    global rx_seq_num, ble_client
    ADDRESS = None
    if True:
        devices = await BleakScanner.discover()
        print(f'scan device list:')
        for d in devices:
            print(f' - {d}')
            if d.name.startswith('CD-ESP '):
                ADDRESS = d.address
        print('')
    
    logger.info("connecting to device: {ADDRESS} ...")
    async with BleakClient(ADDRESS) as client:
        logger.info("Connected.")
        ble_client = client

        await client.start_notify(MISO_UUID, notification_handler)
        await asyncio.sleep(1.0)
        
        ret = await send_command(b"\x40\x01")
        print(f"get info: {ret}")
        
        with open(sys.argv[1], 'rb') as f:
            bin_data = f.read()
        print(f"cal crc: {modbus_crc(bin_data):04x}")
        
        await flash_erase(len(bin_data))
        
        start_time = time.monotonic()
        await flash_write(bin_data)
        
        ret = await send_command(b"\x40\x01")
        print(f"get info: {ret}")
        end_time = time.monotonic()
        print(f"time: {(end_time-start_time)*1000}")
        
        c = await flash_crc(len(bin_data))
        print(f"read crc: {c:04x}")
        
        print('test ok')
        await asyncio.sleep(5.0)
        await client.stop_notify(MISO_UUID)
        await asyncio.sleep(1.0)


if __name__ == "__main__":
    asyncio.run(main())

