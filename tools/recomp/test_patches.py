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


# movss xmm0, [DATA] ; ret
LOAD_XMM0 = f"f30f1005{D32}c3"
# movss [eax + 0x48], xmm1 ; ret
STORE_XMM1 = "f30f114848c3"
# mov ecx, [eax + 8] ; ret
LOAD_ECX_EAX8 = "8b4808c3"
# mov [eax + 0x20], ecx ; ret
STORE_ECX = "894820c3"
OFF = "(640 - 480 * aspect) / 2"
C_OFF = '(640.0 - 480.0 * recomp_patch_var("aspect")) / 2.0'


def _translate_f(hexcode, kind, expr, latch=None, guard=None):
    image = bytes.fromhex(hexcode)
    config._install([config.Section(".text", BASE, len(image), 0, len(image), True),
                     config.Section(".data", DATA, 0x1000, len(image), 0, False)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin="patch-test")
    db = {BASE: {"start": hex(BASE), "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    t = FunctionTranslator(image, db)
    report = PatchReport(PatchConfig([Patch(
        BASE, kind, "display.hud_fix", "1", None, latch=latch, expr=expr,
        guard=guard)]))
    t.lifter.patches = report
    return t.translate_function(BASE, db[BASE]), report


def test_float_load_into_xmm():
    code, report = _translate_f(LOAD_XMM0, "load_f32", "loaded + " + OFF)
    want = ('xmm0.f[0] = RECOMP_PATCH_F32("display.hud_fix", "1", 0, '
            f"((double)(xmm0.f[0]) + {C_OFF}), xmm0.f[0]);")
    assert want in code, code
    assert code.index("XMM_SCALAR(MEMF(0x20000))") < code.index(want)
    assert report.finish() == []


def test_float_load_as_bits_with_guard_and_latch():
    code, report = _translate_f(
        LOAD_ECX_EAX8, "load_f32", "loaded * (4 / 3) / aspect", latch="use",
        guard=("edi", [0x60EA14, 0x60EA84]))
    want = ('if (edi == 0x60EA14u || edi == 0x60EA84u) { ecx = '
            'RECOMP_PATCH_F32_BITS("display.hud_fix", "1", 2, '
            "((double)(RECOMP_F32_OF_BITS(ecx)) * (4.0 / 3.0) / "
            'recomp_patch_var("aspect")), ecx); }')
    assert want in code, code
    assert report.finish() == []


def test_two_guarded_patches_share_a_site():
    image = bytes.fromhex(LOAD_ECX_EAX8)
    config._install([config.Section(".text", BASE, len(image), 0, len(image), True),
                     config.Section(".data", DATA, 0x1000, len(image), 0, False)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin="patch-test")
    db = {BASE: {"start": hex(BASE), "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    t = FunctionTranslator(image, db)
    mk = lambda expr, va: Patch(BASE, "load_f32", "display.hud_fix", "1", None,
                                expr=expr, guard=("edi", [va]))
    report = PatchReport(PatchConfig([mk("loaded * 2", 0x100), mk("loaded * 3", 0x200)]))
    t.lifter.patches = report
    code = t.translate_function(BASE, db[BASE])
    a = code.index("if (edi == 0x100u)")
    b = code.index("if (edi == 0x200u)")
    assert a < b and "* 2.0" in code[a:b] and "* 3.0" in code[b:], code
    assert report.finish() == []
    for bad in ([Patch(BASE, "load_f32", "a.b", "1", None, expr="loaded"),
                 mk("loaded", 0x100)],
                [Patch(BASE, "load_u32", "a.b", "1", 2),
                 Patch(BASE, "load_u32", "a.b", "1", 3)]):
        try:
            PatchConfig(bad)
        except ValueError:
            continue
        raise AssertionError("accepted an unguarded pair")


def test_float_store_rewrites_the_stored_value():
    code, report = _translate_f(STORE_XMM1, "store_f32", "loaded + " + OFF,
                                latch="set")
    want = ('MEMF(eax + 0x48) = RECOMP_PATCH_F32("display.hud_fix", "1", 1, '
            f"((double)(MEMF(eax + 0x48)) + {C_OFF}), MEMF(eax + 0x48));")
    assert want in code, code
    assert code.index("MEMF(eax + 0x48) = xmm1.f[0];") < code.index(want)
    code, report = _translate_f(STORE_ECX, "store_f32", "loaded")
    assert ('MEM32(eax + 0x20) = RECOMP_PATCH_F32_BITS("display.hud_fix", '
            '"1", 0, ((double)(RECOMP_F32_OF_BITS(MEM32(eax + 0x20)))), '
            "MEM32(eax + 0x20));") in code, code
    assert report.finish() == []


def test_float_kinds_refuse_other_shapes():
    for hexcode, kind in ((STORE_XMM1, "load_f32"), (LOAD_XMM0, "store_f32"),
                          (LOAD_BYTE, "load_f32"), (STORE, "load_f32")):
        code, report = _translate_f(hexcode, kind, "loaded")
        assert "RECOMP_PATCH_F32" not in code
        assert any("is not a" in e for e in report.finish()), (hexcode, kind)


def test_expressions():
    from tools.recomp.patches import compile_expr
    c, names = compile_expr("loaded + 320 * (1 - (4 / 3) / aspect)")
    assert names == ["aspect"]
    # The patched values equal the title's own in 4:3, to the bit.
    for expr, loaded in (("loaded + 320 * (1 - (4 / 3) / aspect)", 0.0),
                         ("loaded * (4 / 3) / aspect", 1.0),
                         ("loaded + " + OFF, 30.0), ("loaded - " + OFF, 610.0)):
        c, _ = compile_expr(expr)
        py = c.replace('recomp_patch_var("aspect")', "(4.0 / 3.0)").replace(
            "{loaded}", repr(loaded))
        assert eval(py) == loaded, (expr, eval(py))
    c, names = compile_expr(
        "f32[esp + 0x30] + (loaded - f32[esp+0x30]) * (4 / 3) / aspect")
    assert names == ["aspect"]
    assert c == ('((double)MEMF(esp + 0x30) + ({loaded} - (double)MEMF(esp + 0x30))'
                 ' * (4.0 / 3.0) / recomp_patch_var("aspect"))'), c
    assert compile_expr("f32[eax] - f32[ebp - 0x8]")[0] == \
        "((double)MEMF(eax) - (double)MEMF(ebp - 0x8))"
    for bad in ("a b", "1 +", "f(x)", "x; y", "(1", '"a"', "", "1 ** 2",
                "x ? 1 : 2", "a[0]", "f32[xmm0]", "f32[esp + 48]",
                "f32[esp + eax]", "f32[0x1000]", "f32"):
        try:
            compile_expr(bad)
        except ValueError:
            continue
        raise AssertionError(f"accepted {bad!r}")


def test_float_file_keys():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "patches.json")
        good = {"va": "0x00010000", "kind": "load_f32", "setting": "a.b",
                "when": "1", "expr": "loaded * 2",
                "if": {"reg": "edi", "in": ["0x60EA14"]}}
        with open(path, "w") as f:
            json.dump({"patches": [good]}, f)
        p = PatchConfig.load(path).patches[0x10000]
        assert p.guard == ("edi", [0x60EA14]) and p.names == []
        for change in ({"value": 2}, {"expr": 3}, {"expr": "f(1)"},
                       {"if": {"reg": "xmm0", "in": ["0x1"]}},
                       {"if": {"reg": "edi", "in": []}},
                       {"kind": "load_u32"}):
            with open(path, "w") as f:
                json.dump({"patches": [dict(good, **change)]}, f)
            try:
                PatchConfig.load(path)
            except (ValueError, KeyError):
                continue
            raise AssertionError(f"accepted {change}")


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
