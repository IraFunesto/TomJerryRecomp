"""Live test of the floor column-window widen (debug server, port 4370).

Waits until the floor-grid function at 0x80043634 is resident (exact
instruction words), then rewrites the +-7 half-window constants to +-64.
Re-applies whenever the overlay is reloaded. Never writes other bytes.
"""
import sys, time
sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from ramsnap import rpc

SITES = [  # (addr, vanilla word, patched word)
    (0x80043734, 0x24A2FFF9, 0x24A2FFC0),  # addiu v0,a1,-7  -> -64
    (0x8004375C, 0x24A20007, 0x24A20040),  # addiu v0,a1,7   -> 64
]


def word(a):
    return int.from_bytes(bytes.fromhex(
        rpc({"id": 1, "cmd": "read_ram", "addr": hex(a), "len": 4})["hex"]), "little")


while True:
    try:
        words = [word(a) for a, _, _ in SITES]
        if all(w == v for w, (_, v, _) in zip(words, SITES)):
            for a, v, p in SITES:
                rpc({"id": 2, "cmd": "write_ram", "addr": hex(a), "val": hex(p & 0xFF)})
            print("patched", [hex(word(a)) for a, _, _ in SITES], flush=True)
    except (OSError, ValueError):
        print("debug server gone", flush=True)
        break
    time.sleep(0.25)
