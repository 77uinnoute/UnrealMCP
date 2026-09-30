"""Connection-keepalive & first-frame-race tests for fix-bridge-recv-wouldblock-disconnect.

Covers tasks 3.1 / 3.3 / 3.5:
- Race: connect, WAIT before sending first byte (old code closed the connection
  during the wait -> WinError 10053), expect pong.
- Reuse: send N commands back-to-back over ONE connection (old code closed it
  after the first response), expect all to succeed.
- Peer-close: sequential single-use connections still work.

Run:  python test_conn_race.py
"""

import json
import socket
import struct
import time

HOST, PORT = "127.0.0.1", 55557


def _recv_json(s, timeout=30):
    s.settimeout(timeout)
    chunks = []
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        chunks.append(chunk)
        try:
            return json.loads(b"".join(chunks))
        except json.JSONDecodeError:
            continue
    raise RuntimeError("connection closed before a complete response")


def send_frame(s, cmd, params):
    payload = json.dumps({"type": cmd, "params": params}).encode("utf-8")
    s.sendall(struct.pack(">I", len(payload)) + payload)
    return _recv_json(s)


def main():
    # 1) first-frame race: idle gap between connect and first send
    for i in range(10):
        s = socket.socket()
        s.connect((HOST, PORT))
        time.sleep(0.3)  # old code: bridge already closed the socket by now
        resp = send_frame(s, "ping", {})
        assert resp["result"]["message"] == "pong", f"iter {i}: {resp}"
        s.close()
    print("[OK] first-frame race x10: no premature disconnect (zero 10053)")

    # 2) connection reuse: 5 commands over ONE socket
    s = socket.socket()
    s.connect((HOST, PORT))
    for i in range(5):
        resp = send_frame(s, "ping", {})
        assert resp["result"]["message"] == "pong", f"cmd {i}: {resp}"
        time.sleep(0.2)  # idle gap between commands on the same connection
    # legacy mode on the same socket is not possible (mode is per-connection),
    # but a long framed command works:
    code = "print('REUSE_OK')"
    payload_cmd = {"type": "execute_python_command", "params": {"command": code}}
    resp = send_frame(s, payload_cmd["type"], payload_cmd["params"])
    assert resp["status"] == "success", resp
    s.close()
    print("[OK] connection reuse: 6 commands over one socket, all succeeded")

    # 3) peer close then new connection (sequential single-use clients)
    for i in range(3):
        s = socket.socket()
        s.connect((HOST, PORT))
        resp = send_frame(s, "ping", {})
        assert resp["result"]["message"] == "pong"
        s.close()
    print("[OK] peer close -> new connection: sequential clients fine")

    print("\nALL CONNECTION TESTS PASSED")


if __name__ == "__main__":
    main()
