#!/usr/bin/env python3
#
# Software License Agreement (MIT License)
#
# Author: Duke Fong <d@d-l.io>

import sys
import argparse
import time
import math
from time import sleep
from _udp_common import *

udp_encrypt = False

# register of other rs485 node proxied for read/write by cd-esp
RP_capture = 0x005f
RP_img_len = 0x0074
RP_img_read = 0x0078

cam_mac = 0x30
batch_pkts = 2
blk_len = 251 * 5 * batch_pkts


if __name__ == "__main__":
    print("start test ...")
    ret = send_command(b"\x40\x01")
    print(f"get info: {ret}")

    if udp_encrypt:
        random = csa_read(R_k_random, 4)
        random = struct.unpack("<I", random[1:])[0]
        key_str = f"cd_{random:08x}_{cst['k_pwd']}"
        print(f"key_str: {key_str}")

        cst['aes_key'] = sha256_sum(bytes(key_str, 'utf-8'))
        print(f"aes_key: {cst['aes_key'].hex()}")

        cnt_rx_ = csa_read(R_k_cnt_rx_udp, 2)
        cst['aes_cnt'] = struct.unpack("<H", cnt_rx_[1:])[0]
        print(f"k_cnt_rx_udp: {cst['aes_cnt']:04x}")

    k_en_ = csa_read(R_k_en, 1, encrypt=udp_encrypt)
    print(f"k_en: {k_en_.hex()}")
    
    csa_write(R_proxy_intf, b'\x02', encrypt=udp_encrypt)
    csa_write(R_remote_port, b'\xff\xff', encrypt=udp_encrypt)
    
    ret = send_command(b"\x60\x01", encrypt=udp_encrypt, mac=cam_mac)
    print(f"get info (proxy): {ret}")

    ret = csa_write(RP_capture, b'\x02', proxy=True, encrypt=udp_encrypt, mac=cam_mac)
    print(f"RP_capture = 0x02: {ret.hex()}")
    sleep(0.5)
    
    cnt_rx_ = csa_read(RP_img_len, 4, proxy=True, encrypt=udp_encrypt, mac=cam_mac)
    img_len = struct.unpack("<I", cnt_rx_[1:])[0]
    print(f"RP_img_len: {img_len}")

    img_dat = b''
    cnt = 0
    pend_cnt = 0
    batch_cnt = 0
    req_ofs = 0
    img_done = False
    
    while not img_done:
        if pend_cnt < 2 and req_ofs < img_len:
            req_set = struct.pack("<II", req_ofs, blk_len)
            csa_write(RP_img_read, req_set, proxy=True, encrypt=udp_encrypt, mac=cam_mac, need_reply=False)
            print(f"\n" + f"tx pend_cnt: {pend_cnt}, req_ofs: {req_ofs} ++++\n")
            pend_cnt += 1
            req_ofs += blk_len
        elif pend_cnt:
            rx = get_queue()
            if rx == None:
                print(f"timeout")
                exit(-1)
            if len(rx) == 1:
                print(f"whdr err code: {rx}")
                exit(-1)
            
            if rx[0] & 0x80:
                whdr = rx[0]
                if whdr & 0b01000000:
                    #print(f"rx ori: {rx.hex()}")
                    rx = aes256cbc_decrypt(cst['aes_key'], rx[1:])
                    #print(f"rx plain: {rx.hex()}")
                    rx = rx[2:] # skip aes cnt
                else:
                    rx = rx[1:] # skip whdr
                if whdr & 0b00100000:
                    rx = rx[1:] # skip tmac
            batch_cnt += 1
            if batch_cnt == batch_pkts:
                batch_cnt = 0
                pend_cnt -= 1
                print(f"rx: {rx.hex()}, pend_cnt: {pend_cnt}")
            else:
                print(f"rx: {rx.hex()}")
            
            idx = 0
            while True:
                cdn_pkt = rx[253*idx:253*idx+253]
                if not len(cdn_pkt):
                    break
                if len(img_dat) == 0:
                    if (cdn_pkt[0] & 0b11110000) != 0b01010000:
                        print(f'first pkt is not frag 01: {cdn_pkt[0]:02x}')
                        exit(-1)
                    cnt = cdn_pkt[0] & 0xf
                    img_dat += cdn_pkt[2:]
                else:
                    if ((cnt + 1) & 0xf) != (cdn_pkt[0] & 0xf):
                        print(f'img cnt err: {cnt + 1 :02x} != {cdn_pkt[0] & 0xf :02x}')
                        exit(-1)
                    cnt += 1
                    img_dat += cdn_pkt[2:]
                    
                    if (cdn_pkt[0] & 0b11110000) == 0b01110000:
                        with open('test.jpg', 'wb') as f:
                            f.write(img_dat)
                        print(f'img save ok, len: {len(img_dat)}, ori_len: {img_len}')
                        img_done = True
                        break
                idx += 1
        else:
            print(f"rx img missing end")
            exit(-1)
    
    print('test ok')
