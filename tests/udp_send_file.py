#!/usr/bin/env python3
#
# Software License Agreement (MIT License)
#
# Author: Duke Fong <d@d-l.io>

import sys
import struct
import argparse
import queue
import time
import math
import socket
import _thread
from PyCRC.CRC16 import CRC16
from time import sleep
from encrypt import *

"""Example of batched packet writes for an mbrush2 dpt file

Usage:
  ./udp_send_file.py x.dpt
"""

CMD_TGT_PORT   = 0xcdcd

#TARGET_IP = "cd-esp.local"
#TARGET_IP = "240e:03b4:d0e4:f930:de1e:d5ff:fed7:2248"
TARGET_IP = "192.168.44.83"

# registers of cd-esp
R_k_en = 0x008d
R_k_pwd = 0x008e
R_proxy_intf = 0x0109
R_k_random = 0x011c
R_k_cnt_rx_udp = 0x0124
R_remote_ip = 0x0149
R_remote_port = 0x15a

# registers of other rs485 node proxied for read/write by cd-esp
RP_p_ctrl = 0x01ac
RP_d_ctrl = 0x01ad
RP_e_ctrl = 0x01ae
RP_w_offset = 0x0248

#socket_cmd = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
socket_cmd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
#socket_cmd.settimeout(2)
rx_queue = queue.Queue()

aes_key = None
aes_cnt = 0
k_pwd = "123456"

udp_ack_max = 7
pkt_in_udp = 5
sub_size = (253 - 2) * pkt_in_udp


def udp_rx():
    while True:
        msg, addr = socket_cmd.recvfrom(256)
        #print(f"udp_rx: {msg.hex()}")
        rx_queue.put(msg)

_thread.start_new_thread(udp_rx, ())


def get_queue(timeout=2.5):
    try:
        return rx_queue.get(timeout=timeout)
    except queue.Empty:
        return None


def send_command(msg, wait=True, encrypt=True, mac=None, whdr=False):
    global aes_cnt
    #print(f'send_comand, len: {len(msg)}, msg: {msg.hex()}')
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
    
    if whdr:
        whdr_init = 0x80
        if encrypt:
            whdr_init |= 0x40
        if mac != None:
            whdr_init |= 0x20
        payload = bytes([whdr_init]) + payload
    
    socket_cmd.sendto(payload, (TARGET_IP, CMD_TGT_PORT))
    if wait:
        result = get_queue()
        if result == None or not (result[0] & 0x80): # no whdr
            #print("ret no whdr")
            pass
        else:
            if len(result) == 1:
                print(f"whdr err code: {result}")
                return None
            if result[0] & 0x40:
                #print(f"enc ori: {result.hex()}")
                whdr_ret = result[0]
                result = aes256cbc_decrypt(aes_key, result[1:])
                #print(f"enc plain: {result.hex()}")
                result = result[2:] # skip aes cnt
            if mac != None:
                result = result[1:] # skip mac field
        print(f"send_command ret: {result.hex() if result else None}")
        return result


def csa_write(offset, dat, proxy=False, encrypt=True):
    if proxy:
        msg = b'\x60\x05\x20' + struct.pack("<H", offset) + dat
    else:
        msg = b'\x40\x05\x20' + struct.pack("<H", offset) + dat
    rx_queue.queue.clear()
    ret = send_command(msg, encrypt=encrypt)
    if ret == None or ret[2] != 0 or ret[0] != 5:
        print(f'csa_write error at: 0x{offset:x}: {dat.hex()}')
        return None
    return ret

def csa_read(offset, len_, proxy=False, encrypt=True):
    if proxy:
        msg = b'\x60\x05\x00' + struct.pack("<HB", offset, len_)
    else:
        msg = b'\x40\x05\x00' + struct.pack("<HB", offset, len_)
    rx_queue.queue.clear()
    ret = send_command(msg, encrypt=encrypt)
    if ret == None or ret[2] != 0 or ret[0] != 5:
        print(f'csa_read error at: 0x{offset:x}, len: {len_}, ret: {ret.hex()}')
        return None
    return ret


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


def write_data(dpt_pkts):
    global aes_cnt
    pend_ret = []
    w_idx = 0
    
    while True:
        if len(pend_ret) < 2 and w_idx < len(dpt_pkts):
            #print(f"\n tx group ... {len(pend_ret)}\n")
            while True:
                dat = dpt_pkts[w_idx]
                pkt_num = math.ceil(len(dat) / 253)
                last_port = dat[253*(pkt_num-1)]
                send_command(dat, False) # not wait
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
                elif (rx[0] & 0xc0) == 0xc0:
                    #print(f"rx ori: {rx.hex()}")
                    rx = aes256cbc_decrypt(aes_key, rx[1:])
                    #print(f"rx plain: {rx.hex()}")
                    rx = rx[2:] # skip aes cnt
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
                    cnt_rx_ = csa_read(R_k_cnt_rx_udp, 2, encrypt=False)
                    if cnt_rx_ == None or cnt_rx_[0] != 5:
                        print(f"k_cnt_rx_udp read err, dat: {cnt_rx_}")
                        continue
                    aes_cnt = struct.unpack("<H", cnt_rx_[3:])[0]
                    print(f"k_cnt_rx_udp: {aes_cnt:04x}")
                    
                    csa_dat = csa_read(RP_w_offset, 6, proxy=True)
                    if csa_dat == None or csa_dat[0] != 5:
                        continue
                    print(csa_dat.hex())
                    csa_ofs, csa_cnt, csa_err = struct.unpack("<IBB", csa_dat[3:])
                    ack_idx = math.floor(csa_ofs / sub_size)
                    set_ofs = ack_idx * sub_size
                    set_cnt = dpt_pkts[ack_idx][0] & 7
                    print(f"csa_offset {csa_ofs} -> {set_ofs} ({w_idx * sub_size}), cnt {csa_cnt} -> {set_cnt}, err {csa_err}")
                    set_dat = struct.pack("<IBB", set_ofs, set_cnt, 0)
                    csa_write(RP_w_offset, set_dat, proxy=True)
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

    ret = send_command(b"\x40\x01", encrypt=False)
    print(f"get info: {ret}")

    random = csa_read(R_k_random, 4, encrypt=False)
    random = struct.unpack("<I", random[3:])[0]
    key_str = f"cd_{random:08x}_{k_pwd}"
    print(f"key_str: {key_str}")

    aes_key = sha256_sum(bytes(key_str, 'utf-8'))
    print(f"aes_key: {aes_key.hex()}")

    cnt_rx_ = csa_read(R_k_cnt_rx_udp, 2, encrypt=False)
    aes_cnt = struct.unpack("<H", cnt_rx_[3:])[0]
    print(f"k_cnt_rx_udp: {aes_cnt:04x}")

    k_en_ = csa_read(R_k_en, 1)
    print(f"k_en: {k_en_.hex()}")
    
    csa_write(R_proxy_intf, b'\x02')
    csa_write(R_remote_port, b'\xff\xff')
    
    ret = send_command(b"\x60\x01")
    print(f"get info (proxy): {ret}")

    csa_write(RP_d_ctrl, b'\x10', proxy=True) # reset file
    csa_write(RP_e_ctrl, b'\x10', proxy=True) # reset encoder

    dpt_pkts = prepare_tx_pkts(bin_data)
    start_time = time.monotonic()
    write_data(dpt_pkts)
    end_time = time.monotonic()
    print(f"time: {(end_time-start_time)*1000}")

    csa_write(RP_d_ctrl, b'\x02', proxy=True) # submit file
    print('test ok')
