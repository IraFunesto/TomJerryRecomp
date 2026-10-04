"""Snapshot guest RAM via the psxrecomp debug server (127.0.0.1:4370).

  python ramsnap.py snap NAME      -> saves snaps/NAME.bin (2 MB main RAM)
  python ramsnap.py find A B C...  -> 16-bit halfwords that move monotonically
                                      across snapshots A,B,C (same direction)
"""
import json, os, socket, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SNAPS = os.path.join(HERE, "snaps")


def rpc(req):
    s = socket.create_connection(("127.0.0.1", 4370), timeout=30)
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(1 << 20)
        if not chunk:
            break
        buf += chunk
    s.close()
    return json.loads(buf)


def snap(name):
    r = rpc({"id": 1, "cmd": "read_ram", "addr": "0x80000000", "len": 0x200000})
    os.makedirs(SNAPS, exist_ok=True)
    data = bytes.fromhex(r["hex"])
    open(os.path.join(SNAPS, name + ".bin"), "wb").write(data)
    print(name, len(data), "bytes")


def find(names):
    snaps = [open(os.path.join(SNAPS, n + ".bin"), "rb").read() for n in names]
    n = len(snaps[0]) // 2
    cols = [struct.unpack("<%dh" % n, s) for s in snaps]
    hits = []
    for i in range(n):
        v = [c[i] for c in cols]
        d = [b - a for a, b in zip(v, v[1:])]
        if all(x > 0 for x in d) or all(x < 0 for x in d):
            hits.append((0x80000000 + 2 * i, v))
    for a, v in hits[:400]:
        print(hex(a), v)
    print(len(hits), "hits")


if __name__ == "__main__":
    if sys.argv[1] == "snap":
        snap(sys.argv[2])
    else:
        find(sys.argv[2:])
