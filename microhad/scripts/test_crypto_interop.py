#!/usr/bin/env python3
"""Verify MicroHAD's wire formats with Python cryptography (OpenSSL backend).

Requires the host-only `cryptography` package. All private keys are ephemeral
and live only in a temporary directory; no gateway credentials are used.
"""
import hashlib
import hmac
from pathlib import Path
import subprocess
import sys
import tempfile

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa, utils
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


def expand(prk, info, length):
    return hmac.new(prk, info + b"\x01", hashlib.sha256).digest()[:length]


def decrypt(record, private, public):
    assert record[16:21] == b"\x00\x00\x10\x00\x41"
    ephemeral = record[21:86]
    peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), ephemeral)
    shared = private.exchange(ec.ECDH(), peer)
    prk = hmac.new(bytes(16), shared, hashlib.sha256).digest()
    ikm = expand(prk, b"WebPush: info\0" + public + ephemeral, 32)
    prk = hmac.new(record[:16], ikm, hashlib.sha256).digest()
    cek = expand(prk, b"Content-Encoding: aes128gcm\0", 16)
    nonce = expand(prk, b"Content-Encoding: nonce\0", 12)
    return AESGCM(cek).decrypt(nonce, record[86:], None)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="microhad-crypto-") as directory:
        pem = Path(directory) / "ephemeral.pem"
        for bits in (2048, 4096):
            rsa_key = rsa.generate_private_key(public_exponent=65537, key_size=bits)
            pem.write_bytes(rsa_key.private_bytes(serialization.Encoding.PEM,
                            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
            output = subprocess.check_output([binary, str(pem)], text=True)
            data = {name: bytes.fromhex(value) for name, value in
                    (line.split("=", 1) for line in output.splitlines())}
            rsa_key.public_key().verify(data["rsa_signature"], data["message"],
                                        padding.PKCS1v15(), hashes.SHA256())
            private = ec.derive_private_key(int.from_bytes(data["private"], "big"), ec.SECP256R1())
            public = private.public_key().public_bytes(serialization.Encoding.X962,
                                                       serialization.PublicFormat.UncompressedPoint)
            assert public == data["public"]
            raw = data["ecdsa_signature"]
            assert len(raw) == 64
            signature = utils.encode_dss_signature(int.from_bytes(raw[:32], "big"),
                                                   int.from_bytes(raw[32:], "big"))
            private.public_key().verify(signature, data["message"], ec.ECDSA(hashes.SHA256()))
            assert decrypt(data["encrypted"], private, public) == data["message"]
            assert decrypt(data["large_encrypted"], private, public) == b"x" * 4094 + b"\x02"
        ec_key = ec.generate_private_key(ec.SECP256R1())
        pem.write_bytes(ec_key.private_bytes(serialization.Encoding.PEM,
                        serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        subprocess.check_call([binary, str(pem), "reject"])
        pem.write_text("invalid PEM")
        subprocess.check_call([binary, str(pem), "reject"])
    print("RSA, VAPID, Web Push decryption, invalid inputs, and concurrent PSA initialization passed")


if __name__ == "__main__":
    main()
