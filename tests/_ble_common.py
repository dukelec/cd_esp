#!/usr/bin/env python3
#
# Software License Agreement (MIT License)
#
# Author: Duke Fong <d@d-l.io>

import struct
import argparse
import asyncio
import logging
from PyCRC.CRC16 import CRC16
from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic
from _encrypt import *

logger = logging.getLogger(__name__)
logging.basicConfig(
    level=logging.INFO, #logging.DEBUG
    format="%(asctime)-15s %(name)-8s %(levelname)s: %(message)s",
)
queue = asyncio.Queue()


MOSI_UUID="b3340002-56ba-40b1-8ecb-8fe18dfffddd"
MISO_UUID="b3340003-56ba-40b1-8ecb-8fe18dfffddd"

SUB_MAX_SIZE = 495 # or 244

cst = {
    'ble_client': None,
    'aes_key': None,
    'aes_cnt': 0,
    'k_pwd': '123456'
}

# registers of cd-esp
R_k_en = 0x008d
R_k_pwd = 0x008e
R_k_st = 0x0118
R_k_random = 0x011c
R_k_cnt_rx_ble = 0x0120


def notification_handler(characteristic: BleakGATTCharacteristic, data: bytearray):
    #logger.info("%s: %r", characteristic.description, data)
    queue.put_nowait(data)


async def send_command(msg, wait=True, encrypt=False, mac=None, whdr=False):
    print(f'send_comand, len: {len(msg)}')
    if encrypt or mac != None:
        whdr = True
    
    payload = b''
    if encrypt:
        payload += struct.pack("<H", cst['aes_cnt'])
        cst['aes_cnt'] += 1
    if mac != None:
        payload += struct.pack("<B", mac)
    payload += msg
    
    if encrypt:
        payload = aes256cbc_encrypt(cst['aes_key'], payload)
    
    if whdr or len(payload) > SUB_MAX_SIZE:
        sub_size = SUB_MAX_SIZE - 1
        payload_multi = [payload[i:i+sub_size] for i in range(0, len(payload), sub_size)]
        frag_cnt = 0
        whdr_init = 0x80
        if encrypt:
            whdr_init |= 0x40
        if mac != None:
            whdr_init |= 0x20
        if len(payload_multi) == 1:
            payload_multi[0] = bytes([whdr_init]) + payload_multi[0]
        else:
            for i in range(len(payload_multi)):
                if i == 0:
                    whdr_cur = whdr_init | 0x08
                elif i+1 == len(payload_multi):
                    whdr_cur = whdr_init | 0x18 | frag_cnt
                else:
                    whdr_cur = whdr_init | 0x10 | frag_cnt
                frag_cnt += 1
                if frag_cnt > 7:
                    frag_cnt = 0
                payload_multi[i] = bytes([whdr_cur])+ payload_multi[i]
    else:
        payload_multi = [payload]
    
    for i in range(len(payload_multi)):
        print(f"send sub: {payload_multi[i].hex()}")
        await cst['ble_client'].write_gatt_char(MOSI_UUID, payload_multi[i], response=False)
    if wait:
        result = await queue.get() # TODO: support packet reassembly
        if not (result[0] & 0x80): # no whdr
            print("ret no whdr")
            return result
        if len(result) == 1:
            print(f"whdr err code: {result}")
            return None
        if result[0] & 0x40:
            print(f"enc ori: {result.hex()}")
            whdr_ret = result[0]
            result = aes256cbc_decrypt(cst['aes_key'], result[1:])
            print(f"enc plain: {result.hex()}")
            result = result[2:] # skip aes cnt
        else:
            result = result[1:] # skip whdr
        if mac != None:
            result = result[1:] # skip mac field
        return result


async def csa_write(offset, dat, proxy=False, encrypt=False, mac=None, need_reply=True):
    if proxy:
        if need_reply:
            msg = b'\x60\x05\x20' + struct.pack("<H", offset) + dat
        else:
            msg = b'\x60\x05\xa0' + struct.pack("<H", offset) + dat
    else:
        if need_reply:
            msg = b'\x40\x05\x20' + struct.pack("<H", offset) + dat
        else:
            msg = b'\x40\x05\xa0' + struct.pack("<H", offset) + dat
    ret = await send_command(msg, wait=need_reply, encrypt=encrypt, mac=mac)
    if not need_reply:
        return None
    if ret == None or ret[2] != 0 or ret[0] != 5:
        print(f'csa_write error at: 0x{offset:x}: {dat.hex()}')
    return ret[2:]


async def csa_read(offset, len_, proxy=False, encrypt=False, mac=None):
    if proxy:
        msg = b'\x60\x05\x00' + struct.pack("<HB", offset, len_)
    else:
        msg = b'\x40\x05\x00' + struct.pack("<HB", offset, len_)
    ret = await send_command(msg, encrypt=encrypt, mac=mac)
    if ret == None or ret[2] != 0 or ret[0] != 5:
        print(f'csa_read error at: 0x{offset:x}, len: {len_}')
    return ret[2:]


def modbus_crc(dat):
    return CRC16(modbus_flag=True).calculate(dat)


flash_sub_size = 128

async def flash_read(addr, len_, proxy=False, encrypt=False, mac=None):
    cur = addr
    rdat = b''
    while True:
        size = min(flash_sub_size, len_ - (cur - addr))
        if size == 0:
            break
        if proxy:
            tdat = b'\x60\x08\x00' + struct.pack("<IB", cur, size)
        else:
            tdat = b'\x40\x08\x00' + struct.pack("<IB", cur, size)
        ret = await send_command(tdat, encrypt=encrypt, mac=mac)
        rdat += ret[3:]
        cur += size
    return rdat


# TODO: request a response at the end of every 4KB of data
async def flash_write(addr, dat, proxy=False, encrypt=False, mac=None):
    cur = addr
    while True:
        size = min(flash_sub_size, len(dat) - (cur - addr))
        if size == 0:
            break
        # a0: 20 | 80
        if proxy:
            tdat = b'\x60\x08\xa0' + struct.pack("<I", cur) + dat[cur-addr:cur-addr+size]
        else:
            tdat = b'\x40\x08\xa0' + struct.pack("<I", cur) + dat[cur-addr:cur-addr+size]
        await send_command(tdat, wait=False, encrypt=encrypt, mac=mac)
        cur += size


async def flash_erase(addr, len_, proxy=False, encrypt=False, mac=None):
    if proxy:
        tdat = b'\x60\x08\x2f' + struct.pack("<II", addr, len_)
    else:
        tdat = b'\x40\x08\x2f' + struct.pack("<II", addr, len_)
    print(f"flash_erase: len: {len_}")
    ret = await send_command(tdat, encrypt=encrypt, mac=mac)
    print(f"flash_erase: {ret.hex()}")


async def flash_crc(addr, len_, proxy=False, encrypt=False, mac=None):
    if proxy:
        tdat = b'\x60\x08\x10' + struct.pack("<II", addr, len_)
    else:
        tdat = b'\x40\x08\x10' + struct.pack("<II", addr, len_)
    print(f"flash_crc: len: {len_}")
    ret = await send_command(tdat, encrypt=encrypt, mac=mac)
    print(f"flash_crc: {ret.hex()}")
    return struct.unpack("<H", ret[3:])[0]

