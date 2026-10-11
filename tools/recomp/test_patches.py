"""Guest load patches (--patches): the call after a listed `mov reg, [mem]`,
the refusals, and that nothing changes without it. Synthetic one-function
images, as in test_spin_waits."""

import json
import os
import tempfile

from tools.recomp import config
from tools.recomp.translator import FunctionTranslator
from tools.recomp.patches import Patch, PatchConfig, PatchReport

BASE = 0x10000
DATA = 0x20000
D32 = DATA.to_bytes(4, "little").hex()

# mov eax, [DATA] ; ret
LOAD_EAX = f"a1{D32}c3"
# mov ebx, [DATA] ; ret
LOAD_EBX = f"8b1d{D32}c3"
# movzx ecx, byte [DATA] ; ret
LOAD_BYTE = f"0fb60d{D32}c3"
# mov [DATA], eax ; ret
STORE = f"a3{D32}c3"


def _translate(hexcode, patch=None, va=BASE, latch=None):
    image = bytes.fromhex(hexcode)
    config._install([config.Section(".text", BASE, len(image), 0, len(image), True),
                     config.Section(".data", DATA, 0x1000, len(image), 0, False)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin="patch-test")
    db = {BASE: {"start": hex(BASE), "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    t = FunctionTranslator(image, db)
    report = None
    if patch is not None:
        report = PatchReport(PatchConfig([Patch(
            va, "load_u32", "game.speed_units", "kmh", 2, latch=latch)]))
        t.lifter.patches = report
    return t.translate_function(BASE, db[BASE]), report


def test_load_is_followed_by_the_call():
    code, report = _translate(LOAD_EAX, patch=True)
    assert ('eax = RECOMP_PATCH_U32("game.speed_units", "kmh", 0x2u, eax);'
            in code), code
    assert code.index("MEM32(0x20000)") < code.index("RECOMP_PATCH_U32")
    assert report.finish() == []


def test_latch_sites_use_the_latch_call():
    for latch, mode in (("set", 1), ("use", 2)):
        code, report = _translate(LOAD_EAX, patch=True, latch=latch)
        assert ('eax = RECOMP_PATCH_U32_LATCH("game.speed_units", "kmh", '
                f"0x2u, eax, {mode});") in code, code
        assert report.finish() == []


def test_the_register_is_the_loaded_one():
    code, report = _translate(LOAD_EBX, patch=True)
    assert 'ebx = RECOMP_PATCH_U32("game.speed_units", "kmh", 0x2u, ebx);' in code
    assert report.finish() == []


def test_nothing_changes_without_patches():
    code, _ = _translate(LOAD_EAX)
    assert "RECOMP_PATCH_U32" not in code


def test_other_addresses_are_left_alone():
    code, report = _translate(LOAD_EAX, patch=True, va=BASE + 0x100)
    assert "RECOMP_PATCH_U32" not in code
    assert any("no lifted body" in e for e in report.finish())


def test_a_byte_load_or_store_is_refused():
    for hexcode in (LOAD_BYTE, STORE):
        code, report = _translate(hexcode, patch=True)
        assert "RECOMP_PATCH_U32" not in code
        assert any("is not a 32-bit" in e for e in report.finish()), hexcode


def test_file_round_trip_and_bad_keys():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "patches.json")
        with open(path, "w") as f:
            json.dump({"patches": [{"va": "0x00010000",
                                    "setting": "game.speed_units",
                                    "when": "kmh", "value": 2}]}, f)
        cfg = PatchConfig.load(path)
        assert cfg.patches[0x10000].value == 2
        for bad in ({"va": "0x1", "setting": "Speed", "when": "kmh", "value": 2},
                    {"va": "0x1", "setting": "a.b", "when": "k mh", "value": 2},
                    {"va": "0x1", "setting": "a.b", "when": "k", "value": -1},
                    {"va": "0x1", "setting": "a.b", "when": "k", "value": 1,
                     "kind": "store"}):
            with open(path, "w") as f:
                json.dump({"patches": [bad]}, f)
            try:
                PatchConfig.load(path)
            except ValueError:
                continue
            raise AssertionError(f"accepted {bad}")


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
