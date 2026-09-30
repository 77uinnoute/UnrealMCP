"""Framed-protocol end-to-end test against the UnrealMCP C++ bridge (port 55557).

Covers Docs/mcp-bugfix-plan.md P0-2 acceptance:
- 64KB random multi-line payloads (incl. \\n\\n and non-ASCII), 20 iterations, zero loss
  (editor prints len + md5, compared against the sender's own values)
- bytes_received integrity echo on every command
- legacy raw-JSON backward compatibility
- deferred python job + poll_python_job (P1-1)
- take_screenshot with absolute path (P1-2)
- get_console_variable (P2-2)

Run:  python test_python_cmd.py
"""

import hashlib
import json
import os
import random
import socket
import string
import struct
import sys
import time

HOST, PORT = "127.0.0.1", 55557


def _recv_json(s):
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


def send_framed(cmd, params, timeout=120):
    """New protocol: 4-byte big-endian length prefix + JSON payload."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect((HOST, PORT))
    payload = json.dumps({"type": cmd, "params": params}).encode("utf-8")
    s.sendall(struct.pack(">I", len(payload)) + payload)
    resp = _recv_json(s)
    s.close()
    br = resp.pop("bytes_received", None)
    assert br == len(payload), f"bytes_received mismatch: sent {len(payload)}, bridge got {br}"
    return resp


def send_legacy(cmd, params, timeout=120):
    """Old protocol: raw JSON, no prefix (backward-compat check)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect((HOST, PORT))
    payload = json.dumps({"type": cmd, "params": params}).encode("utf-8")
    s.sendall(payload)
    resp = _recv_json(s)
    s.close()
    br = resp.pop("bytes_received", None)
    assert br == len(payload), f"legacy bytes_received mismatch: sent {len(payload)}, bridge got {br}"
    return resp


def py(code, timeout=120):
    resp = send_framed("execute_python_command", {"command": code}, timeout=timeout)
    assert resp.get("status") != "error", f"editor python failed: {resp.get('error')}"
    out = "".join(e.get("output", "") for e in (resp.get("result", {}).get("log") or []))
    return out


def main():
    # 1. ping (both protocols)
    assert send_framed("ping", {})["result"]["message"] == "pong"
    print("[OK] ping (framed)")
    assert send_legacy("ping", {})["result"]["message"] == "pong"
    print("[OK] ping (legacy backward compat)")

    # 2. 64KB random payloads x 20: length + md5 must match exactly
    alphabet = string.ascii_letters + string.digits + "!@#$%^&*()_+-=[]{}|;:',.<>?/~`\"\n\n\t \u4e2d\u6587\u2665"
    for i in range(20):
        random_str = "".join(random.choice(alphabet) for _ in range(64 * 1024))
        expect_md5 = hashlib.md5(random_str.encode("utf-8")).hexdigest()
        code = (
            "import hashlib\n"
            f"s = {random_str!r}\n"
            "print('LEN:%d' % len(s))\n"
            "print('MD5:%s' % hashlib.md5(s.encode('utf-8')).hexdigest())\n"
        )
        out = py(code)
        got_len = int([l for l in out.splitlines() if l.startswith("LEN:")][0][4:])
        got_md5 = [l for l in out.splitlines() if l.startswith("MD5:")][0][4:]
        assert got_len == len(random_str), f"iter {i}: len {got_len} != {len(random_str)}"
        assert got_md5 == expect_md5, f"iter {i}: md5 mismatch (middle-of-payload loss)"
    print("[OK] 64KB random payloads x 20: zero loss (len + md5)")

    # 3. deferred job + poll
    resp = send_framed("execute_python_command", {
        "command": "import time\nprint('JOB_START')\n"
                   "total = sum(i * i for i in range(3000000))\n"
                   "print('JOB_TOTAL:%d' % total)",
        "deferred": True,
    })
    job_id = resp["result"]["job_id"]
    print(f"[OK] deferred job queued: {job_id}")
    deadline = time.time() + 60
    state = None
    while time.time() < deadline:
        poll = send_framed("poll_python_job", {"job_id": job_id, "cleanup": True})
        state = poll["result"].get("state")
        if state == "done":
            job = poll["result"]["job"]
            assert job.get("success") is True, f"job failed: {job.get('error')}"
            job_out = "".join(e.get("output", "") for e in (job.get("log") or []))
            assert "JOB_TOTAL:" in job_out, f"job output missing: {job_out[:500]}"
            print("[OK] poll_python_job: done, job payload correct")
            break
        time.sleep(0.5)
    else:
        raise RuntimeError(f"deferred job never finished (state={state})")
    poll2 = send_framed("poll_python_job", {"job_id": job_id, "cleanup": False})
    assert poll2["result"]["state"] == "pending", "job file should be gone after cleanup"
    print("[OK] poll_python_job cleanup: pending after read")

    # 4. take_screenshot (absolute path, filename honored)
    shot = os.path.abspath(os.path.join(os.path.dirname(__file__), "test_shot.png")).replace("\\", "/")
    resp = send_framed("take_screenshot", {"filepath": shot})
    assert resp["status"] == "success", f"screenshot failed: {resp}"
    size = int(resp["result"].get("file_size") or 0)
    assert os.path.isfile(shot) and size > 10 * 1024, f"screenshot file bad: exists={os.path.isfile(shot)} size={size}"
    print(f"[OK] take_screenshot: {shot} ({size} bytes, {resp['result'].get('width')}x{resp['result'].get('height')})")
    os.remove(shot)

    # 5. get_console_variable (P2-2)
    resp = send_framed("get_console_variable", {"name": "r.CustomDepth"})
    print(f"[OK] get_console_variable r.CustomDepth = {resp['result']['value']}")

    print("\nALL TESTS PASSED")


if __name__ == "__main__":
    main()
