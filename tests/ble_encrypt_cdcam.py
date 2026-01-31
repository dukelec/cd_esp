#!/usr/bin/env python3
#
# Software License Agreement (MIT License)
#
# Author: Duke Fong <d@d-l.io>

import struct
import argparse
import asyncio
import logging
from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic
from encrypt import *

logger = logging.getLogger(__name__)
logging.basicConfig(
    level=logging.INFO, #logging.DEBUG
    format="%(asctime)-15s %(name)-8s %(levelname)s: %(message)s",
)
queue = asyncio.Queue()


MOSI_UUID="b3340002-56ba-40b1-8ecb-8fe18dfffddd"
MISO_UUID="b3340003-56ba-40b1-8ecb-8fe18dfffddd"

SUB_MAX_SIZE = 495 # or 244

ble_client = None
aes_key = None
aes_cnt = 0
k_pwd = "123456"

# registers of cd-esp
R_k_en = 0x008d
R_k_pwd = 0x008e
R_k_st = 0x0118
R_k_random = 0x011c
R_k_cnt_rx_ble = 0x0120

# register of other rs485 node proxied for read/write by cd-esp
RP_capture = 0x005f
RP_img_len = 0x0074
RP_img_read = 0x0078

cam_mac = 0x30
blk_len = 251 * 15 + 152 - 1 # or: 251 * 15 + 88 - 1


def notification_handler(characteristic: BleakGATTCharacteristic, data: bytearray):
    #logger.info("%s: %r", characteristic.description, data)
    queue.put_nowait(data)


async def send_command(msg, wait=True, encrypt=False, mac=None, whdr=False):
    global aes_cnt
    print(f'send_comand, len: {len(msg)}')
    if encrypt or mac != None:
        whdr = True
    
    payload = b''
    if encrypt:
        payload += struct.pack("<H", aes_cnt)
        aes_cnt += 1
    if mac != None:
        payload += struct.pack("<B", mac)
    payload += msg
    
    if encrypt:
        payload = aes256cbc_encrypt(aes_key, payload)
    
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
        await ble_client.write_gatt_char(MOSI_UUID, payload_multi[i], response=False)
    if wait:
        result = await queue.get()
        if not (result[0] & 0x80): # no whdr
            print("ret no whdr")
            return result
        if len(result) == 1:
            print(f"whdr err code: {result}")
            return None
        if encrypt:
            print(f"enc ori: {result.hex()}")
            whdr_ret = result[0]
            result = aes256cbc_decrypt(aes_key, result[1:])
            print(f"enc plain: {result.hex()}")
            result = result[2:] # skip aes cnt
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


async def main():
    global ble_client, aes_key, aes_cnt
    address = None
    if True:
        devices = await BleakScanner.discover()
        print(f'scan device list:')
        for d in devices:
            print(f' - {d}')
            if d.name and d.name.startswith('CD-ESP '):
                address = d.address
        print('')

    if address is None:
        logger.error("could not find device, exit...")
        return

    logger.info("connecting to device: {address} ...")
    async with BleakClient(address) as client:
        ble_client = client
        logger.info("Connected.")

        await client.start_notify(MISO_UUID, notification_handler)
        await asyncio.sleep(1.0)
        
        ret = await send_command(b"\x40\x01")
        print(f"get info: {ret}")
        
        random = await csa_read(R_k_random, 4)
        random = struct.unpack("<I", random[1:])[0]
        key_str = f"cd_{random:08x}_{k_pwd}"
        print(f"key_str: {key_str}")
        
        aes_key = sha256_sum(bytes(key_str, 'utf-8'))
        print(f"aes_key: {aes_key.hex()}")
        
        cnt_rx_ = await csa_read(R_k_cnt_rx_ble, 2)
        aes_cnt = struct.unpack("<H", cnt_rx_[1:])[0]
        print(f"k_cnt_rx_ble: {aes_cnt:04x}")
        
        k_en_ = await csa_read(R_k_en, 1, encrypt=True)
        print(f"k_en: {k_en_.hex()}")
        
        # proxy read the predefined remote rs485 node via cd-esp
        #ret = await csa_write(RP_d_ctrl, b'\x10', proxy=True, encrypt=True)
        #print(f"d_ctrl = 0x10: {ret.hex()}")
        
        ret = await csa_write(RP_capture, b'\x02', proxy=True, encrypt=True, mac=cam_mac)
        print(f"RP_capture = 0x02: {ret.hex()}")
        await asyncio.sleep(1.0)
        
        cnt_rx_ = await csa_read(RP_img_len, 4, proxy=True, encrypt=True, mac=cam_mac)
        img_len = struct.unpack("<I", cnt_rx_[1:])[0]
        print(f"RP_img_len: {img_len}")
        
        img_cdnet_dat = []
        pend_cnt = 0
        req_ofs = 0
        frag_buf = b''
        frag_cnt = 0
        while True:
            if pend_cnt < 2 and req_ofs < img_len:
                req_set = struct.pack("<II", req_ofs, blk_len)
                await csa_write(RP_img_read, req_set, proxy=True, encrypt=True, mac=cam_mac, need_reply=False)
                print(f"\n" + f"tx pend_cnt: {pend_cnt}, req_ofs: {req_ofs} ++++++++\n")
                pend_cnt += 1
                req_ofs += blk_len
            elif pend_cnt:
                rx = await queue.get()
                if not (rx[0] & 0x80): # no whdr
                    print("rx no whdr")
                    break
                if len(rx) == 1:
                    print(f"whdr err code: {rx}")
                    break
                print(f"rx: {rx.hex()}")
                rx_whdr = rx[0]
                if (rx_whdr & 0b00011000) == 0b00001000:
                    frag_cnt = rx_whdr & 7
                    frag_buf += rx[1:]
                else:
                    if (rx_whdr & 0b00011000) != 0:
                        if ((frag_cnt + 1) & 7) != rx_whdr & 7:
                            print(f"frag_cnt err: {frag_cnt} != {rx_whdr & 7}")
                            break
                        frag_cnt = rx_whdr & 7
                    
                    frag_buf += rx[1:]
                    if (rx_whdr & 0b00011000) == 0 or (rx_whdr & 0b00011000) == 0b00011000:
                        rx_plain = aes256cbc_decrypt(aes_key, frag_buf)
                        rx_aes_cnt = struct.unpack("<H", rx_plain[0:2])[0]
                        rx_tmac = rx_plain[2]
                        rx_plain = rx_plain[3:] # skip aes cnt and tmac
                        pend_cnt -= 1
                        frag_buf = b''
                        img_cdnet_dat.append(rx_plain)
                        print(f"\nrx_plain pend_cnt: {pend_cnt}, len: {len(rx_plain)}, rx_aes_cnt: {rx_aes_cnt}, tmac: {rx_tmac:02x}: {rx_plain.hex()} --------\n")
            else:
                print(f"rx img done")
                break
        
        img_dat = b''
        idx = 0
        cnt = 0
        for i in range(len(img_cdnet_dat)):
            while True:
                cdn_pkt = img_cdnet_dat[i][253*idx:253*idx+253]
                if not len(cdn_pkt):
                    idx = 0
                    break
                print(f'port: {cdn_pkt[0]:02x} -> {cdn_pkt[1]:02x}, i: {i}, idx: {idx}')
                if cdn_pkt[1] != 0x10:
                    print(f'img port err: {cdn_pkt[0]:02x} -> {cdn_pkt[1]:02x}, i: {i}, idx: {idx}')
                    break
                if i == 0 and idx == 0:
                    if (cdn_pkt[0] & 0b11110000) != 0b01010000:
                        print(f'first pkt is not frag 01: {cdn_pkt[0]:02x}')
                        break
                    cnt = cdn_pkt[0] & 0xf
                    img_dat += cdn_pkt[2:]
                else:
                    if ((cnt + 1) & 0xf) != cdn_pkt[0] & 0xf:
                        print(f'img cnt err: {cnt + 1} != {cdn_pkt[0] & 0xf}')
                        break
                    cnt += 1
                    img_dat += cdn_pkt[2:]
                    
                    if (cdn_pkt[0] & 0b11110000) == 0b01110000:
                        with open('test.jpg', 'wb') as f:
                            f.write(img_dat)
                        print(f'img save ok, len: {len(img_dat)}, ori_len: {img_len}')
                        break
                idx += 1
        
        
        print('test ok')
        await asyncio.sleep(5.0)
        await client.stop_notify(MISO_UUID)
        await asyncio.sleep(1.0)


if __name__ == "__main__":
    asyncio.run(main())

