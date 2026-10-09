#!/usr/bin/env python3
"""Exercise the standalone UI over real sockets; no hardware or vendor files."""
import argparse
from functools import reduce
from operator import xor
from pathlib import Path
import socket
import struct
import subprocess
import time


def packet(payload, sequence=0x5A, channel=1, flags=0x80):
    header = bytearray(b"\x8e\x25\0" + bytes((sequence, channel, flags)) + struct.pack("<H", len(payload)))
    header[2] = reduce(xor, header[3:] + payload, 0)
    return bytes(header) + payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("assets", type=Path)
    args = parser.parse_args()
    with socket.socket() as reserved:
        reserved.bind(("127.0.0.1", 0))
        port = reserved.getsockname()[1]
        # Port conflict must fail before even opening the bogus framebuffer.
        busy = subprocess.run([str(args.binary), "--stock", "--port", str(port),
                               "--assets", str(args.assets), "--fb", "/dev/null"],
                              capture_output=True, text=True, timeout=8)
        assert busy.returncode != 0 and "Cannot listen" in busy.stderr, busy
    process = subprocess.Popen([str(args.binary), "--stock", "--port", str(port),
                                "--assets", str(args.assets), "--seconds", "5"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 3
        while True:
            try:
                connection = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise AssertionError("UI listener did not start")
                time.sleep(0.02)
        body = bytearray(201)
        body[0] = 1
        struct.pack_into("<i", body, 5, 4)
        struct.pack_into("<i", body, 9, 1)
        struct.pack_into("<i", body, 41, 11512)
        struct.pack_into("<i", body, 101, 4889)
        wire = packet(body)
        with connection:
            connection.settimeout(2)
            # Fragment the header, then coalesce corrupt + unknown + status.
            for byte in wire[:8]:
                connection.sendall(bytes((byte,)))
            connection.sendall(wire[8:])
            ack = bytearray()
            while len(ack) < 8:
                block = connection.recv(8 - len(ack))
                assert block, "Truncated ACK"
                ack.extend(block)
            assert bytes(ack) == packet(b"", flags=0), ack.hex()
            corrupt = bytearray(wire)
            corrupt[2] ^= 1
            connection.sendall(corrupt + packet(b"unvalidated", channel=0, flags=0) + packet(body, flags=0))
            # Leave an incomplete inner MSP frame at the first disconnect.
            inner_status = bytes.fromhex("24 4d 3e 1b 65 7c 00 00 00 23 00 00 00 00 00 00 19 00 00 00 00 1e 04 01 00 00 00 20 00 04 03 00 04")
            connection.sendall(packet(b"\0" + inner_status[:9], channel=0, flags=0))
            time.sleep(0.1)
        # Verify a new backend can reconnect without restarting the UI.
        time.sleep(0.1)
        with socket.create_connection(("127.0.0.1", port), timeout=1) as again:
            # The old partial frame must not contaminate this new stream.
            again.sendall(packet(body, flags=0) + packet(b"\0" + inner_status[9:], channel=0, flags=0))
            analog = bytes.fromhex("24 4d 3e 09 6e 74 1b 00 d1 01 00 00 86 04 5a")
            variant = bytes.fromhex("24 4d 3e 04 02 42 54 46 4c 1a")
            firmware = bytes.fromhex("24 4d 3e 13 03 1a 0c 00 0f 32 30 32 36 2e 31 32 2e 30 2d 61 6c 70 68 61 65")
            again.sendall(packet(b"\0" + inner_status + analog + variant + firmware, channel=0, flags=0))
            again.settimeout(0.1)
            try:
                assert not again.recv(1), "Serial decoder sent an unsolicited request"
            except socket.timeout:
                pass
            time.sleep(2.1)  # Keep peer alive through the first serial log.
        process.stdin.write("md\n")
        process.stdin.flush()
        output, error = process.communicate(timeout=8)
        assert process.returncode == 0, output + error
        assert "EXIT frames=3" in output, output
        assert "unknown=1 bad=1" in output, output
        assert 'state="BACKEND STALE"' in output, output
        assert output.count("STOCK backend connected") == 2, output
        assert 'msp=4 status=1 analog=1 malformed=0' in output, output
        assert 'variant="BTFL" firmware="2026.12.0-alpha" active=1 arm=0 pack_v=11.58 amps=0.00' in output, output
        assert 'rc_source="RC RSSI" rc_pct=45' in output and 'rc_source="RC --" rc_pct=0' in output, output
        print(output)
        print("PASS: port exclusion, ACK, serial decoding without writes, partial-frame reconnect reset and freshness expiry")
    finally:
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=5)


if __name__ == "__main__":
    main()
