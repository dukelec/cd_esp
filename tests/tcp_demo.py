import argparse
import ipaddress
import queue
import socket
import struct
import threading

from _encrypt import aes256cbc_decrypt, aes256cbc_encrypt, sha256_sum


CMD_TGT_PORT = 0xCDCD
# TARGET_IP = "cd-esp.local"
TARGET_IP = "192.168.1.12"

R_k_en = 0x008D
R_k_pwd = 0x008E
R_proxy_sel = 0x0109
R_k_random = 0x011C
R_k_cnt_rx_udp = 0x0124
R_wifi_state = 0x0148
R_remote_ip = 0x0149
R_remote_port = 0x015A
R_local_ip = 0x015C

rx_queue: "queue.Queue[bytes]" = queue.Queue()

cst = {
    "aes_key": None,
    "aes_cnt": 0,
    "k_pwd": "123456",
    "csa_cnt": 0,
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
            if length == 0 or length > 2048:
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


def _csa_data(ret: bytes | None, length: int) -> bytes | None:
    if ret is None or len(ret) < 1 + length or ret[0] != 0:
        return None
    return ret[1 : 1 + length]


def _fmt_ip16(ip16: bytes) -> str:
    if len(ip16) != 16:
        return f"<bad len {len(ip16)}>"
    if ip16[0:2] == b"\xff\xff":
        return "<empty>"
    if ip16[0:10] == b"\x00" * 10 and ip16[10:12] == b"\xff\xff":
        return str(ipaddress.IPv4Address(ip16[12:16]))
    return str(ipaddress.IPv6Address(ip16))


def read_show_status(sock: socket.socket, encrypt: bool) -> None:
    proxy_sel = _csa_data(csa_read(sock, R_proxy_sel, 1, encrypt=encrypt), 1)
    if proxy_sel is not None:
        proxy_sel_name = {0: "empty", 1: "ble", 2: "wifi"}.get(proxy_sel[0], "unknown")
        print(f"proxy_sel: {proxy_sel[0]} ({proxy_sel_name})")

    wifi_state = _csa_data(csa_read(sock, R_wifi_state, 1, encrypt=encrypt), 1)
    if wifi_state is not None:
        v = wifi_state[0]
        scan = (v >> 0) & 1
        connected = (v >> 1) & 1
        connecting = (v >> 4) & 1
        disabled = (v >> 7) & 1
        print(
            f"wifi_state: 0x{v:02x} (disabled={disabled}, connecting={connecting}, connected={connected}, scan={scan})"
        )

    remote_ip = _csa_data(csa_read(sock, R_remote_ip, 16, encrypt=encrypt), 16)
    if remote_ip is not None:
        print(f"remote_ip: {_fmt_ip16(remote_ip)}")

    remote_port = _csa_data(csa_read(sock, R_remote_port, 2, encrypt=encrypt), 2)
    if remote_port is not None:
        (port_net,) = struct.unpack(">H", remote_port)
        if port_net == 0xFFFF:
            print("remote_port: <empty>")
        else:
            print(f"remote_port: {port_net}")

    for i in range(4):
        ip_i = _csa_data(csa_read(sock, R_local_ip + 16 * i, 16, encrypt=encrypt), 16)
        if ip_i is None:
            continue
        print(f"local_ip[{i}]: {_fmt_ip16(ip_i)}")


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
    args = parser.parse_args()

    sock = connect(args.target, args.port)

    ret = send_command(sock, b"\x40\x01")
    if ret is None:
        print("get info: <timeout/err>")
    elif len(ret) < 2:
        print(f"get info: {ret.hex()}")
    else:
        cmd = ret[0]
        server = ret[1]
        payload = ret[2:]
        try:
            info_str = payload.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            info_str = None
        if info_str is not None:
            print(f"get info: cmd=0x{cmd:02x} server={server} info={info_str}")
        else:
            print(f"get info: cmd=0x{cmd:02x} server={server} payload={payload.hex()}")

    if args.encrypt:
        init_encrypt(sock, args.k_pwd)

    k_en = csa_read(sock, R_k_en, 1, encrypt=args.encrypt)
    k_en_b = _csa_data(k_en, 1)
    print(f"k_en: {k_en_b[0] if k_en_b is not None else None}")

    read_show_status(sock, encrypt=args.encrypt)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
