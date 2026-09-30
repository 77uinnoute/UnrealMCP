"""Acceptance tests for fix-nightsoul-session-bugs (bridge level).

- 5.1 64KB random multi-line script written to disk, executed via execute_python_file;
     the script self-verifies len + md5 against its own source on disk.
- 5.2 execute_python_file(deferred=True, description=...) -> poll -> job carries
     file_path + description.
- 5.3 missing file / non-.py extension -> structured error, nothing executed.

Run:  python test_python_file.py
"""

import hashlib
import json
import os
import random
import socket
import string
import struct
import time

HOST, PORT = "127.0.0.1", 55557


def call(cmd, params, timeout=120):
    s = socket.socket()
    s.settimeout(timeout)
    s.connect((HOST, PORT))
    payload = json.dumps({"type": cmd, "params": params}).encode("utf-8")
    s.sendall(struct.pack(">I", len(payload)) + payload)
    chunks = []
    while True:
        c = s.recv(65536)
        if not c:
            break
        chunks.append(c)
        try:
            return json.loads(b"".join(chunks))
        except json.JSONDecodeError:
            continue
    raise RuntimeError("connection closed")


def out_text(resp):
    return "".join(e.get("output", "") for e in ((resp.get("result") or {}).get("log") or []))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    script_path = os.path.join(here, "big_script_test.py").replace("\\", "/")

    # --- 5.1 64KB random script via file ---
    alphabet = string.ascii_letters + string.digits + "!@#$%^&*()_+-=[]{}|;:',.<>?/~`\"\n\n\t \u4e2d\u6587\u2665"
    blob = "".join(random.choice(alphabet) for _ in range(64 * 1024))
    md5 = hashlib.md5(blob.encode("utf-8")).hexdigest()
    script = (
        "import hashlib\n"
        f"blob = {blob!r}\n"
        "print('LEN:%d' % len(blob))\n"
        "print('MD5:%s' % hashlib.md5(blob.encode('utf-8')).hexdigest())\n"
        "print('FILE_OK')\n"
    )
    with open(script_path, "w", encoding="utf-8") as f:
        f.write(script)

    resp = call("execute_python_file", {"file_path": script_path})
    assert resp["status"] == "success", f"file exec failed: {resp.get('error')}"
    out = out_text(resp)
    assert "FILE_OK" in out, out[:500]
    got_len = int([l for l in out.splitlines() if l.startswith("LEN:")][0][4:])
    got_md5 = [l for l in out.splitlines() if l.startswith("MD5:")][0][4:]
    assert got_len == len(blob), f"len mismatch {got_len} != {len(blob)}"
    assert got_md5 == md5, "md5 mismatch"
    assert int(resp["result"]["code_bytes"]) > 64000, resp["result"]
    print(f"[OK] 5.1 execute_python_file: 64KB script executed intact (len+md5, code_bytes={resp['result']['code_bytes']})")

    # --- 5.2 deferred + description ---
    job_script = os.path.join(here, "job_script_test.py").replace("\\", "/")
    with open(job_script, "w", encoding="utf-8") as f:
        f.write("print('JOB_FROM_FILE_OK')\n")
    resp = call("execute_python_file", {
        "file_path": job_script,
        "deferred": True,
        "description": "acceptance-job",
    })
    job_id = resp["result"]["job_id"]
    deadline = time.time() + 60
    job = None
    while time.time() < deadline:
        poll = call("poll_python_job", {"job_id": job_id, "cleanup": True})
        if poll["result"].get("state") == "done":
            job = poll["result"]["job"]
            break
        time.sleep(0.5)
    assert job is not None and job.get("success") is True, job
    assert job.get("description") == "acceptance-job", job
    assert job.get("file_path") == job_script, job
    job_out = "".join(e.get("output", "") for e in (job.get("log") or []))
    assert "JOB_FROM_FILE_OK" in job_out, job_out
    print("[OK] 5.2 deferred file job: done, carries file_path + description")

    # --- 5.3 error paths ---
    resp = call("execute_python_file", {"file_path": "Z:/definitely/not/there.py"})
    assert resp["status"] == "error" and "Failed to read" in resp.get("error", ""), resp
    resp = call("execute_python_file", {"file_path": script_path + ".txt"})
    assert resp["status"] == "error" and ".py" in resp.get("error", ""), resp
    print("[OK] 5.3 error paths: missing file / bad extension -> structured errors")

    os.remove(script_path)
    os.remove(job_script)
    print("\nALL FILE-EXEC TESTS PASSED")


if __name__ == "__main__":
    main()
