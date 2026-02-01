#!/usr/bin/env python3
#
# Software License Agreement (MIT License)
#
# Author: Duke Fong <d@d-l.io>

import struct
import queue
import socket
import _thread
from PyCRC.CRC16 import CRC16
from _encrypt import *

CMD_TGT_PORT = 0xcdcd

TARGET_IP = "cd-esp.local"
#TARGET_IP = "240e:03b4:d0e4:f930:de1e:d5ff:fed7:2248"
#TARGET_IP = "192.168.44.83"

# registers of cd-esp
R_k_en = 0x008d
R_k_pwd = 0x008e
R_proxy_intf = 0x0109
R_k_random = 0x011c
R_k_cnt_rx_udp = 0x0124
R_remote_ip = 0x0149
R_remote_port = 0x15a

socket_cmd = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
#socket_cmd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
#socket_cmd.settimeout(2)
rx_queue = queue.Queue()

cst = {
    'aes_key': None,
    'aes_cnt': 0,
    'k_pwd': '123456'
}


def udp_rx():
    while True:
        msg, addr = socket_cmd.recvfrom(1500)
        #print(f"udp_rx: {msg.hex()}")
        rx_queue.put(msg)

_thread.start_new_thread(udp_rx, ())


def get_queue(timeout=2.5):
    try:
        return rx_queue.get(timeout=timeout)
    except queue.Empty:
        return None


def send_command(msg, wait=True, encrypt=False, mac=None, whdr=False):
    #print(f'send_comand, len: {len(msg)}, msg: {msg.hex()}')
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
                result = aes256cbc_decrypt(cst['aes_key'], result[1:])
                #print(f"enc plain: {result.hex()}")
                result = result[2:] # skip aes cnt
            else:
                result = result[1:] # skip whdr
            if mac != None:
                result = result[1:] # skip mac field
        print(f"send_command ret: {result.hex() if result else None}")
        return result


def csa_write(offset, dat, proxy=False, encrypt=False, mac=None, need_reply=True):
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
    if need_reply:
        rx_queue.queue.clear()
    ret = send_command(msg, wait=need_reply, encrypt=encrypt, mac=mac)
    if not need_reply:
        return None
    if ret == None or ret[2] != 0 or ret[0] != 5:
        print(f'csa_write error at: 0x{offset:x}: {dat.hex()}')
        return None
    return ret[2:]

def csa_read(offset, len_, proxy=False, encrypt=False, mac=None):
    if proxy:
        msg = b'\x60\x05\x00' + struct.pack("<HB", offset, len_)
    else:
        msg = b'\x40\x05\x00' + struct.pack("<HB", offset, len_)
    rx_queue.queue.clear()
    ret = send_command(msg, encrypt=encrypt, mac=mac)
    if ret == None or ret[2] != 0 or ret[0] != 5:
        print(f'csa_read error at: 0x{offset:x}, len: {len_}, ret: {ret.hex()}')
        return None
    return ret[2:]

