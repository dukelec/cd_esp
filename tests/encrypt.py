#!/usr/bin/env python3
#
# Software License Agreement (MIT License)
#
# Author: Duke Fong <d@d-l.io>

import hashlib
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import padding
from cryptography.hazmat.backends import default_backend


def sha256_sum(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


_BLOCK_BITS = 128  # AES block size in bits
_ZERO_IV = b"\x00" * 16


def aes256cbc_encrypt(key: bytes, data: bytes) -> bytes:
    padder = padding.PKCS7(_BLOCK_BITS).padder()
    padded = padder.update(data) + padder.finalize()
    cipher = Cipher(algorithms.AES(key), modes.CBC(_ZERO_IV), backend=default_backend())
    encryptor = cipher.encryptor()
    return encryptor.update(padded) + encryptor.finalize()


def aes256cbc_decrypt(key: bytes, data: bytes) -> bytes:
    cipher = Cipher(algorithms.AES(key), modes.CBC(_ZERO_IV), backend=default_backend())
    decryptor = cipher.decryptor()
    padded = decryptor.update(data) + decryptor.finalize()
    unpadder = padding.PKCS7(_BLOCK_BITS).unpadder()
    return unpadder.update(padded) + unpadder.finalize()


if __name__ == "__main__":
    key = b"A" * 32
    msg = b"hello, world"
    ct = aes256cbc_encrypt(key, msg)
    pt = aes256cbc_decrypt(key, ct)

    print("cypher:", ct.hex())
    print("plain:", pt)
    print("sha256:", sha256_sum(b"hello").hex())
