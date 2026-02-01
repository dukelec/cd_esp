#!/usr/bin/env python3
#
# Software License Agreement (MIT License)
#
# Author: Duke Fong <d@d-l.io>

import sys
import time
from intelhex import IntelHex
from _ble_common import *

flash_addr = 0x187000


async def main():
    address = None
    if True:
        devices = await BleakScanner.discover()
        print(f'scan device list:')
        for d in devices:
            print(f' - {d}')
            if d.name.startswith('CD-ESP '):
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
        
        if not sys.argv[1].lower().endswith(".hex"):
            print("only support .hex file!")
            return
        
        dat = []
        ih = IntelHex()
        ih.loadhex(sys.argv[1])
        segs = ih.segments()
        print(f'parse ihex file, segments: {[list(map(hex, l)) for l in segs]} (end addr inclusive)')
        for seg in segs:
            s = [seg[0], ih.tobinstr(seg[0], size=seg[1]-seg[0])]
            dat.append(s)
        
        flash_addr = dat[0][0]
        bin_data = dat[0][1]
        file_crc = modbus_crc(bin_data)
        print(f"cal crc: {file_crc:04x}")
        
        await flash_erase(flash_addr, len(bin_data))
        
        start_time = time.monotonic()
        await flash_write(flash_addr, bin_data)
        
        print(f"read info... (os bluetooth queue draining)")
        ret = await send_command(b"\x40\x01")
        print(f"get info: {ret}")
        end_time = time.monotonic()
        print(f"time: {(end_time-start_time)*1000}")
        
        c = await flash_crc(flash_addr, len(bin_data))
        print(f"read crc: {c:04x} (file_crc: {file_crc:04x})")
        
        print('test ok')
        await asyncio.sleep(5.0)
        await client.stop_notify(MISO_UUID)
        await asyncio.sleep(1.0)


if __name__ == "__main__":
    asyncio.run(main())

