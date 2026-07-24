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

"""Example of batched packet writes for an mbrush2 dptz file

Usage:
  ./udp_send_file.py x.dptz
"""

udp_encrypt = True

# registers of other rs485 node proxied for read/write by cd-esp
RP_p_ctrl = 0x01ac
RP_d_ctrl = 0x01ad
RP_e_ctrl = 0x01ae
RP_dptz_rx = 0x023c
RP_p14_cnt = 0x024c

udp_ack_max = 7
pkt_in_udp = 5
sub_size = (253 - 2) * pkt_in_udp


def prepare_tx_pkts(dat):
    cur = 0;
    pkt_cnt = 0;
    ack_cnt = 0;
    dpt_pkts = []
    while True:
        size = min(sub_size, len(dat) - cur)
        if size == 0:
            break
        wdat = dat[cur:cur+size]
        ack_bit = 0x8
        pkt_num = math.ceil(size / 251)
        ack_cnt += 1
        if ack_cnt >= udp_ack_max or size < sub_size:
            ack_bit = 0
            ack_cnt = 0
        msg = b''
        for i in range(pkt_num):
            port = 0x68 if i + 1 != pkt_num else (0x60 | ack_bit)
            msg += struct.pack("<BB", port | (pkt_cnt & 7), 20) + wdat[251*i:251*i+251]
            pkt_cnt += 1
        dpt_pkts.append(msg)
        cur += size
    return dpt_pkts


def write_data(dpt_pkts, dptz_size):
    pend_ret = []
    w_idx = 0
    
    while True:
        if len(pend_ret) < 2 and w_idx < len(dpt_pkts):
            #print(f"\n tx group ... {len(pend_ret)}\n")
            while True:
                dat = dpt_pkts[w_idx]
                pkt_num = math.ceil(len(dat) / 253)
                last_port = dat[253*(pkt_num-1)]
                send_command(dat, wait=False, encrypt=udp_encrypt) # not wait
                w_idx += 1
                if not (last_port & 0x8):
                    pend_ret.append(last_port)
                    break
        elif len(pend_ret):
            rx = get_queue()
            if rx != None:
                if len(rx) == 1:
                    print(f"whdr err code: {rx}")
                    rx = None
                    rx_queue.queue.clear()
                elif rx[0] & 0x80:
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
                print(f"rx: {rx.hex()}")
            if rx != None and rx[2] == 0: # no err
                if rx[1] == pend_ret[0]:
                    #print(f"\n rx0 group ... {len(pend_ret)-1}\n")
                    pend_ret.pop(0)
                elif len(pend_ret) > 1 and rx[1] == pend_ret[1]:
                    print(f"\n rx1 group ... {len(pend_ret)-1}\n")
                    pend_ret.clear()
                else:
                    print("rx port err")
            else:
                while True:
                    if udp_encrypt:
                        cnt_rx_ = csa_read(R_k_cnt_rx_udp, 2)
                        if cnt_rx_ == None:
                            print(f"k_cnt_rx_udp read err, dat: {cnt_rx_}")
                            continue
                        cst['aes_cnt'] = struct.unpack("<H", cnt_rx_[1:])[0]
                        print(f"k_cnt_rx_udp: {cst['aes_cnt']:04x}")
                    
                    csa_dat = csa_read(RP_dptz_rx, 18, proxy=True, encrypt=udp_encrypt)
                    if csa_dat == None or len(csa_dat) < 19:
                        continue
                    print(csa_dat.hex())
                    dptz_rx, csa_cnt, csa_err = struct.unpack_from("<I12xBB", csa_dat, 1)
                    if dptz_rx > dptz_size:
                        print(f"dptz_rx error: {dptz_rx} > {dptz_size}")
                        return
                    if dptz_rx == dptz_size:
                        print(f"dptz_rx {dptz_rx}, all data received")
                        w_idx = len(dpt_pkts)
                        pend_ret.clear()
                        break
                    ack_idx = math.floor(dptz_rx / sub_size)
                    set_cnt = dpt_pkts[ack_idx][0] & 7
                    print(f"dptz_rx {dptz_rx} -> {ack_idx * sub_size} ({w_idx * sub_size}), cnt {csa_cnt} -> {set_cnt}, err {csa_err}")
                    set_dat = struct.pack("<BB", set_cnt, 0)
                    csa_write(RP_p14_cnt, set_dat, proxy=True, encrypt=udp_encrypt)
                    w_idx = ack_idx
                    pend_ret.clear()
                    break
        else:
            print("write_data done")
            break


if __name__ == "__main__":
    with open(sys.argv[1], 'rb') as f:
        bin_data = f.read()

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
    
    ret = send_command(b"\x60\x01", encrypt=udp_encrypt)
    print(f"get info (proxy): {ret}")

    csa_write(RP_d_ctrl, b'\x10', proxy=True, encrypt=udp_encrypt) # reset file
    csa_write(RP_e_ctrl, b'\x10', proxy=True, encrypt=udp_encrypt) # reset encoder

    dpt_pkts = prepare_tx_pkts(bin_data)
    start_time = time.monotonic()
    write_data(dpt_pkts, len(bin_data))
    end_time = time.monotonic()
    print(f"time: {(end_time-start_time)*1000}")

    csa_write(RP_d_ctrl, b'\x02', proxy=True, encrypt=udp_encrypt) # submit file
    print('test ok')
