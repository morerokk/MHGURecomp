"""Read-only anchors for early Armor Fusion research; emits no game code or data.

Usage: python tools/fusion_inspect.py /path/to/exefs/main
Requires the same lz4 dependency as tools/gen.py.
"""
import argparse
import json
from nso import Nso

BASE = 0x200000
ANCHORS = {
    "smithy_update": (0x8F1AE8, 0xE92D41F0),
    "fusion_constructor": (0x85FCEC, 0xE92D4010),
    "fusion_update": (0x860314, 0xE92D4830),
    "equipment_validity": (0x340B7C, 0xE92D4070),
    "fusion_pair_validation": (0x340D60, 0xE92D4FF0),
    "fusion_quote": (0x341060, 0xE92D41F0),
    "fusion_affordability": (0x341288, 0xE92D4010),
    "fusion_explicit_cost_check": (0x3412E0, 0xE92D4830),
    "fusion_equipment_commit": (0x348F00, 0xE92D47F0),
    "consume_item": (0x74FF80, 0xE92D4070),
}


def inspect(path):
    nso = Nso(path)
    if not nso.build_id.lower().startswith("fb08f1d20fd1204f"):
        raise ValueError("Expected MHGU 1.4.0 main build FB08F1D20FD1204F")
    anchors = {}
    for name, (address, expected) in ANCHORS.items():
        actual = nso.u32(address - BASE)
        if actual != expected:
            raise ValueError(f"{name}: unexpected instruction at {address:#x}")
        anchors[name] = hex(address)
    return {"build_id": nso.build_id, "anchors": anchors,
            "status": "read-only image verification; this tool does not install hooks"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("main")
    args = parser.parse_args()
    print(json.dumps(inspect(args.main), indent=2))
