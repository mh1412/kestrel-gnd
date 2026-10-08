#!/usr/bin/env python3
"""Host-only UI navigation, local IPC, stock-key and persistence integration."""
import argparse
from pathlib import Path
import socket
import stat
import subprocess
import tempfile
import time
import os
import pty
import termios


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("assets", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="kestrel-ui-control-") as directory:
        root = Path(directory)
        control, config = root / "control.sock", root / "ui.conf"
        process = subprocess.Popen([str(args.binary), "--demo", "--assets", str(args.assets),
                                    "--control", str(control), "--config", str(config), "--stock-keys"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        client = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        client.bind(str(root / "client.sock"))
        client.settimeout(2)
        try:
            deadline = time.monotonic() + 5
            while not control.exists():
                if process.poll() is not None or time.monotonic() > deadline:
                    out, error = process.communicate(timeout=2)
                    raise AssertionError("UI did not start: " + out + error)
                time.sleep(0.02)
            assert stat.S_IMODE(control.stat().st_mode) == 0o600
            client.connect(str(control))

            def command(key):
                client.send(key.encode())
                return client.recv(512).decode()

            def expect(key, *fields):
                result = command(key)
                assert result.startswith("OK ") and all(field in result for field in fields), (key, result, fields)
                return result

            expect("status", "stage=closed")
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                udp.sendto(bytes((0x40, 4, 0)), ("127.0.0.1", 6222))
            time.sleep(0.04)
            expect("status", "stage=pick", "side=goggle")
            expect("enter", "stage=browse", "tab=HUD")
            # Fragmented CRLF must start one edit, not immediately apply it.
            process.stdin.write("\r"); process.stdin.flush()
            time.sleep(0.02)
            process.stdin.write("\n"); process.stdin.flush()
            time.sleep(0.03)
            expect("status", "stage=edit", "theme=KESTREL")
            sent = subprocess.run([str(args.binary), "--control", str(control), "--send-key", "down"],
                                  capture_output=True, text=True, timeout=3)
            assert sent.returncode == 0 and "theme=EMBER" in sent.stdout, sent
            expect("back", "stage=browse", "theme=KESTREL")
            assert not config.exists(), "Cancelled preview must not write preferences"
            expect("enter", "stage=edit")
            expect("down", "theme=EMBER")
            expect("enter", "stage=browse", "theme=EMBER")
            assert "theme=1\n" in config.read_text() and stat.S_IMODE(config.stat().st_mode) == 0o600
            expect("right", "tab=DISPLAY")
            expect("enter", "stage=edit")
            expect("down", "fps=26")
            expect("back", "fps=25", "stage=browse")
            expect("back", "stage=pick")
            expect("right", "side=air")
            expect("enter", "stage=browse", "tab=RADIO")
            expect("right", "tab=SERIAL")
            expect("right", "tab=INFO")
            expect("right", "tab=INFO")
            expect("enter", "stage=browse")  # A reading cannot become an action.
            expect("menu", "stage=closed")
            expect("menu", "stage=pick")
            expect("enter", "tab=INFO")  # Each side remembers its section.
            before = command("status").split(" keys=", 1)[1].split()[0]
            assert command("bind").startswith("ERR ")
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                udp.sendto(bytes((0x40, 9, 0)), ("127.0.0.1", 6222))
            time.sleep(0.04)
            after = command("status").split(" keys=", 1)[1].split()[0]
            assert before == after, "Unmapped/debug input must not act on the UI"
            # A second UI cannot delete the live control socket.
            other = subprocess.run([str(args.binary), "--demo", "--assets", str(args.assets), "--control", str(control), "--seconds", "1"],
                                   capture_output=True, text=True, timeout=5)
            assert other.returncode != 0 and "already active" in other.stderr
            expect("status", "theme=EMBER")
            master, slave = pty.openpty()
            attributes = termios.tcgetattr(slave)
            interactive = subprocess.Popen([str(args.binary), "--control", str(control), "--interactive-control"],
                                           stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                deadline = time.monotonic() + 3
                while termios.tcgetattr(slave)[3] & termios.ICANON:
                    assert interactive.poll() is None and time.monotonic() < deadline, "Interactive keyboard client did not enter raw mode"
                    time.sleep(0.01)
                os.write(master, b"m")
                time.sleep(0.06)
                expect("status", "stage=closed")
                os.write(master, b"q")
                client_output, client_error = interactive.communicate(timeout=3)
                assert interactive.returncode == 0, client_output + client_error
                assert termios.tcgetattr(slave) == attributes, "Interactive controller did not restore terminal mode"
                assert process.poll() is None, "Q should exit the keyboard client, not the receiver UI"
            finally:
                if interactive.poll() is None:
                    interactive.terminate(); interactive.communicate(timeout=3)
                os.close(master); os.close(slave)
            expect("quit", "theme=EMBER")
            output, error = process.communicate(timeout=5)
            assert process.returncode == 0, output + error
            assert not control.exists(), "Owned socket was not removed on exit"
            print(output)
            # Stale owned socket recovery and preference reload.
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as stale:
                stale.bind(str(control))
            restarted = subprocess.run([str(args.binary), "--demo", "--assets", str(args.assets),
                                        "--control", str(control), "--config", str(config), "--seconds", "1"],
                                       capture_output=True, text=True, timeout=5)
            assert restarted.returncode == 0 and not control.exists() and "PREFERENCES theme=EMBER" in restarted.stdout, restarted.stdout + restarted.stderr
            # Unrelated file at the control path must survive.
            control.write_text("user data")
            conflict = subprocess.run([str(args.binary), "--demo", "--assets", str(args.assets), "--control", str(control)],
                                      capture_output=True, text=True, timeout=5)
            assert conflict.returncode != 0 and control.read_text() == "user data"
            print("PASS: side picker, sections, edit preview/apply/cancel, CRLF, stock keys, IPC ownership, saved preferences and stale socket recovery")
        finally:
            client.close()
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)


if __name__ == "__main__":
    main()
