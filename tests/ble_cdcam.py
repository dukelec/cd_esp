#!/usr/bin/env python3
#
# Software License Agreement (MIT License)
#
# Author: Duke Fong <d@d-l.io>

from _ble_common import *

ble_encrypt = False

# register of other rs485 node proxied for read/write by cd-esp
RP_capture = 0x005f
RP_img_len = 0x0074
RP_img_read = 0x0078

cam_mac = 0x30
blk_len = 251 * 15 + 152 - 1 # or: 251 * 15 + 88 - 1


async def main():
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

    logger.info(f"connecting to device: {address} ...")
    async with BleakClient(address) as client:
        cst['ble_client'] = client
        logger.info("Connected.")

        await client.start_notify(MISO_UUID, notification_handler)
        await asyncio.sleep(1.0)
        
        ret = await send_command(b"\x40\x01")
        print(f"get info: {ret}")
        
        if ble_encrypt:
            random = await csa_read(R_k_random, 4)
            random = struct.unpack("<I", random[1:])[0]
            key_str = f"cd_{random:08x}_{cst['k_pwd']}"
            print(f"key_str: {key_str}")
            
            cst['aes_key'] = sha256_sum(bytes(key_str, 'utf-8'))
            print(f"aes_key: {cst['aes_key'].hex()}")
            
            cnt_rx_ = await csa_read(R_k_cnt_rx_ble, 2)
            cst['aes_cnt'] = struct.unpack("<H", cnt_rx_[1:])[0]
            print(f"k_cnt_rx_ble: {cst['aes_cnt']:04x}")
        
        k_en_ = await csa_read(R_k_en, 1, encrypt=ble_encrypt)
        print(f"k_en: {k_en_.hex()}")
        
        img_count = 0
        while True:
            ret = await csa_write(RP_capture, b'\x80', proxy=True, encrypt=ble_encrypt, mac=cam_mac)
            print(f"RP_capture = 0x80: {ret.hex()}")
            ret = await csa_write(RP_capture, b'\x02', proxy=True, encrypt=ble_encrypt, mac=cam_mac)
            print(f"RP_capture = 0x02: {ret.hex()}")
            #await asyncio.sleep(0.5)
            
            while True:
                cnt_rx_ = await csa_read(RP_img_len, 4, proxy=True, encrypt=ble_encrypt, mac=cam_mac)
                img_len = struct.unpack("<I", cnt_rx_[1:])[0]
                print(f"RP_img_len: {img_len}")
                if img_len != 0:
                    break
            
            img_cdnet_dat = []
            pend_cnt = 0
            req_ofs = 0
            frag_buf = b''
            frag_cnt = 0
            while True:
                if pend_cnt < 2 and req_ofs < img_len:
                    req_set = struct.pack("<II", req_ofs, blk_len)
                    await csa_write(RP_img_read, req_set, proxy=True, encrypt=ble_encrypt, mac=cam_mac, need_reply=False)
                    print(f"\n" + f"tx pend_cnt: {pend_cnt}, req_ofs: {req_ofs} ++++\n")
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
                            if ((frag_cnt + 1) & 7) != (rx_whdr & 7):
                                print(f"frag_cnt err: {frag_cnt} != {rx_whdr & 7}")
                                break
                            frag_cnt = rx_whdr & 7
                        
                        frag_buf += rx[1:]
                        if (rx_whdr & 0b00011000) == 0 or (rx_whdr & 0b00011000) == 0b00011000:
                            if ble_encrypt:
                                rx_plain = aes256cbc_decrypt(cst['aes_key'], frag_buf)
                                rx_aes_cnt = struct.unpack("<H", rx_plain[0:2])[0]
                                rx_tmac = rx_plain[2]
                                rx_plain = rx_plain[3:] # skip aes cnt and tmac
                            else:
                                rx_aes_cnt = -1
                                rx_tmac = frag_buf[0]
                                rx_plain = frag_buf[1:] # skip tmac
                            pend_cnt -= 1
                            frag_buf = b''
                            img_cdnet_dat.append(rx_plain)
                            print(f"\nrx_plain pend_cnt: {pend_cnt}, len: {len(rx_plain)}, rx_aes_cnt: {rx_aes_cnt}, tmac: {rx_tmac:02x}: {rx_plain.hex()} ----\n")
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
                        if ((cnt + 1) & 0xf) != (cdn_pkt[0] & 0xf):
                            print(f'img cnt err: {(cnt + 1) & 0xf} != {cdn_pkt[0] & 0xf}')
                            break
                        cnt = (cnt + 1) & 0xf
                        img_dat += cdn_pkt[2:]
                        
                        if (cdn_pkt[0] & 0b11110000) == 0b01110000:
                            with open('test.jpg', 'wb') as f:
                                f.write(img_dat)
                            print(f'img save ok, len: {len(img_dat)}, ori_len: {img_len}')
                            break
                    idx += 1
            
            img_count += 1
            print(f'\ntest ok, cnt: {img_count}\n')
        
        await asyncio.sleep(5.0)
        await client.stop_notify(MISO_UUID)
        await asyncio.sleep(1.0)


if __name__ == "__main__":
    asyncio.run(main())

