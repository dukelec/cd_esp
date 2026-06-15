import argparse
import queue
import socket
import struct
import threading
from time import sleep

from _encrypt import aes256cbc_decrypt, aes256cbc_encrypt, sha256_sum


CMD_TGT_PORT = 0xCDCD
# TARGET_IP = "cd-esp.local"
TARGET_IP = "192.168.1.12"

R_k_en = 0x008D
R_proxy_intf = 0x0109
R_k_random = 0x011C
R_k_cnt_rx_udp = 0x0124
R_remote_port = 0x015A

RP_capture = 0x005F
RP_img_len = 0x0074
RP_img_read = 0x0078


rx_queue: "queue.Queue[bytes]" = queue.Queue()

cst = {
    "aes_key": None,
    "aes_cnt": 0,
    "k_pwd": "123456",
}


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed")
        buf += chunk
    return buf


def _tcp_rx(sock: socket.socket) -> None:
    while True:
        try:
            hdr = _recv_exact(sock, 2)
            (length,) = struct.unpack("!H", hdr)
            if length == 0 or length > 4096:
                raise ValueError(f"invalid length: {length}")
            payload = _recv_exact(sock, length)
            rx_queue.put(payload)
        except Exception:
            return


def connect(target_ip: str, port: int, timeout: float = 3.0) -> socket.socket:
    sock = socket.create_connection((target_ip, port), timeout=timeout)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    t = threading.Thread(target=_tcp_rx, args=(sock,), daemon=True)
    t.start()
    return sock


def get_queue(timeout: float = 2.5) -> bytes | None:
    try:
        return rx_queue.get(timeout=timeout)
    except queue.Empty:
        return None


def _clear_queue() -> None:
    with rx_queue.mutex:
        rx_queue.queue.clear()


def send_command(
    sock: socket.socket,
    msg: bytes,
    wait: bool = True,
    encrypt: bool = False,
    mac: int | None = None,
    whdr: bool = False,
) -> bytes | None:
    if encrypt or mac is not None:
        whdr = True

    payload = b""
    if encrypt:
        payload += struct.pack("<H", cst["aes_cnt"])
        cst["aes_cnt"] = (cst["aes_cnt"] + 1) & 0xFFFF
    if mac is not None:
        payload += struct.pack("<B", mac)
    payload += msg

    if encrypt:
        if cst["aes_key"] is None:
            raise RuntimeError("encrypt enabled but aes_key is not set")
        payload = aes256cbc_encrypt(cst["aes_key"], payload)

    if whdr:
        whdr_init = 0x80
        if encrypt:
            whdr_init |= 0x40
        if mac is not None:
            whdr_init |= 0x20
        payload = bytes([whdr_init]) + payload

    sock.sendall(struct.pack("!H", len(payload)) + payload)

    if not wait:
        return None

    result = get_queue()
    if result is None or not (result[0] & 0x80):
        return result

    if len(result) == 1:
        return None

    whdr_ret = result[0]
    if whdr_ret & 0x40:
        if cst["aes_key"] is None:
            raise RuntimeError("rx encrypted but aes_key is not set")
        result = aes256cbc_decrypt(cst["aes_key"], result[1:])
        result = result[2:]
    else:
        result = result[1:]

    if mac is not None:
        result = result[1:]

    return result


def csa_write(
    sock: socket.socket,
    offset: int,
    dat: bytes,
    proxy: bool = False,
    encrypt: bool = False,
    mac: int | None = None,
    need_reply: bool = True,
) -> bytes | None:
    h0 = 0x60 if proxy else 0x40
    h2 = 0x20 if need_reply else 0xA0
    msg = struct.pack("<BBBH", h0, 0x05, h2, offset) + dat
    if need_reply:
        _clear_queue()
    ret = send_command(sock, msg, wait=need_reply, encrypt=encrypt, mac=mac)
    if not need_reply:
        return None
    if ret is None or len(ret) < 3 or ret[2] != 0 or ret[0] != 5:
        return None
    return ret[2:]


def csa_read(
    sock: socket.socket,
    offset: int,
    length: int,
    proxy: bool = False,
    encrypt: bool = False,
    mac: int | None = None,
) -> bytes | None:
    h0 = 0x60 if proxy else 0x40
    msg = struct.pack("<BBBHB", h0, 0x05, 0x00, offset, length)
    _clear_queue()
    ret = send_command(sock, msg, encrypt=encrypt, mac=mac)
    if ret is None or len(ret) < 3 or ret[2] != 0 or ret[0] != 5:
        return None
    return ret[2:]


def init_encrypt(sock: socket.socket, k_pwd: str) -> None:
    cst["k_pwd"] = k_pwd
    random = csa_read(sock, R_k_random, 4)
    if random is None or len(random) < 5:
        raise RuntimeError("read k_random failed")
    random_u32 = struct.unpack("<I", random[1:])[0]
    key_str = f"cd_{random_u32:08x}_{cst['k_pwd']}"
    cst["aes_key"] = sha256_sum(bytes(key_str, "utf-8"))

    cnt_rx = csa_read(sock, R_k_cnt_rx_udp, 2)
    if cnt_rx is None or len(cnt_rx) < 3:
        raise RuntimeError("read k_cnt_rx_udp failed")
    cst["aes_cnt"] = struct.unpack("<H", cnt_rx[1:])[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", default=TARGET_IP)
    parser.add_argument("--port", type=lambda s: int(s, 0), default=CMD_TGT_PORT)
    parser.add_argument("--encrypt", action="store_true")
    parser.add_argument("--k-pwd", default="123456")
    parser.add_argument("--cam-mac", type=lambda s: int(s, 0), default=0x30)
    parser.add_argument("--batch-pkts", type=int, default=2)
    args = parser.parse_args()

    cam_mac = args.cam_mac
    batch_pkts = max(1, min(4, args.batch_pkts))
    blk_len = 251 * 5 * batch_pkts

    sock = connect(args.target, args.port)

    ret = send_command(sock, b"\x40\x01")
    print(f"get info: {ret}")

    if args.encrypt:
        init_encrypt(sock, args.k_pwd)

    k_en = csa_read(sock, R_k_en, 1, encrypt=args.encrypt)
    print(f"k_en: {k_en.hex() if k_en else None}")

    csa_write(sock, R_proxy_intf, b"\x02", encrypt=args.encrypt)
    csa_write(sock, R_remote_port, b"\xff\xff", encrypt=args.encrypt)

    ret = send_command(sock, b"\x60\x01", encrypt=args.encrypt, mac=cam_mac)
    print(f"get info (proxy): {ret}")

    img_count = 0
    while True:
        ret = csa_write(sock, RP_capture, b"\x80", proxy=True, encrypt=args.encrypt, mac=cam_mac)
        if ret is None:
            raise RuntimeError("RP_capture write 0x80 failed")
        print(f"RP_capture = 0x80: {ret.hex()}")

        ret = csa_write(sock, RP_capture, b"\x02", proxy=True, encrypt=args.encrypt, mac=cam_mac)
        if ret is None:
            raise RuntimeError("RP_capture write 0x02 failed")
        print(f"RP_capture = 0x02: {ret.hex()}")

        while True:
            cnt_rx_ = csa_read(sock, RP_img_len, 4, proxy=True, encrypt=args.encrypt, mac=cam_mac)
            if cnt_rx_ is None or len(cnt_rx_) < 5:
                raise RuntimeError("read RP_img_len failed")
            img_len = struct.unpack("<I", cnt_rx_[1:])[0]
            print(f"RP_img_len: {img_len}")
            if img_len != 0:
                break
            sleep(0.2)

        img_dat = b""
        cnt = 0
        pend_cnt = 0
        batch_cnt = 0
        req_ofs = 0
        img_done = False

        while not img_done:
            if pend_cnt < 2 and req_ofs < img_len:
                req_set = struct.pack("<II", req_ofs, blk_len)
                csa_write(
                    sock,
                    RP_img_read,
                    req_set,
                    proxy=True,
                    encrypt=args.encrypt,
                    mac=cam_mac,
                    need_reply=False,
                )
                pend_cnt += 1
                req_ofs += blk_len
            elif pend_cnt:
                rx = get_queue()
                if rx is None:
                    raise TimeoutError("timeout waiting tcp payload")
                if len(rx) == 1:
                    raise RuntimeError(f"whdr err code: {rx!r}")

                if rx[0] & 0x80:
                    whdr = rx[0]
                    if whdr & 0b01000000:
                        rx = aes256cbc_decrypt(cst["aes_key"], rx[1:])
                        rx = rx[2:]
                    else:
                        rx = rx[1:]
                    if whdr & 0b00100000:
                        rx = rx[1:]

                batch_cnt += 1
                if batch_cnt == batch_pkts:
                    batch_cnt = 0
                    pend_cnt -= 1
                    print(f"rx: ... pend_cnt: {pend_cnt}")
                else:
                    print("rx: ...")

                idx = 0
                while True:
                    cdn_pkt = rx[253 * idx : 253 * idx + 253]
                    if not cdn_pkt:
                        break
                    if len(img_dat) == 0:
                        if (cdn_pkt[0] & 0b11110000) != 0b01010000:
                            raise RuntimeError(f"first pkt is not frag 01: {cdn_pkt[0]:02x}")
                        cnt = cdn_pkt[0] & 0xF
                        img_dat += cdn_pkt[2:]
                    else:
                        if ((cnt + 1) & 0xF) != (cdn_pkt[0] & 0xF):
                            raise RuntimeError(
                                f"img cnt err: {(cnt + 1) & 0xF:02x} != {cdn_pkt[0] & 0xF:02x}"
                            )
                        cnt += 1
                        img_dat += cdn_pkt[2:]

                        if (cdn_pkt[0] & 0b11110000) == 0b01110000:
                            with open("test.jpg", "wb") as f:
                                f.write(img_dat)
                            print(f"img save ok, len: {len(img_dat)}, ori_len: {img_len}")
                            img_done = True
                            break
                    idx += 1
            else:
                raise RuntimeError("rx img missing end")

        img_count += 1
        print(f"test ok, cnt: {img_count}")


if __name__ == "__main__":
    raise SystemExit(main())
