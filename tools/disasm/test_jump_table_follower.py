"""A function packed straight after another function's switch table.

MSVC parks a switch table after the function's last ret, aligns it with
`mov edi, edi`, and the linker then packs the next function right after the
table. Nothing marks that start. The int3 boundary pass finds a table, not a
ret, in front of it, and the gap-prologue pass looks only after a ret. Worse,
the gap-prologue pass used to take the `mov edi, edi` for a hot-patch prologue.
It started a function on the padding, and that function stepped over the table
and swallowed the real one behind it.

Fixtures are synthetic: hand-assembled bytes, no game data.
"""
import os
import struct
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import Function, FunctionDetector  # noqa: E402
from tools.disasm.labels import LabelManager  # noqa: E402
from tools.disasm.xrefs import XRefTracker  # noqa: E402

BASE = 0x10000
PAD = b"\x8b\xff"                       # mov edi, edi


class Image:
    base_address = BASE
    entry_point = BASE

    def __init__(self, data):
        self.data = data
        self.image_size = len(data)
        self.section = SimpleNamespace(name=".text", virtual_addr=BASE,
                                       virtual_size=len(data), executable=True)
        self.sections = [self.section]

    def get_section_at_va(self, addr):
        return self.section if BASE <= addr < BASE + len(self.data) else None

    def get_section_data(self, section):
        return self.data

    def read_bytes_at_va(self, addr, size):
        return self.data[addr - BASE:addr - BASE + size]

    def read_u32_at_va(self, addr):
        off = addr - BASE
        if off < 0 or off + 4 > len(self.data):
            return None
        return struct.unpack_from("<I", self.data, off)[0]


def _engine(data):
    image = Image(data)
    engine = DisasmEngine(image)
    engine.linear_sweep(image.section)
    engine.resync_jump_tables()
    return engine, image


def _detect(data):
    engine, image = _engine(data)
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det.detect_all([image.section])
    return det, engine


def _arm(value):
    """mov eax, value ; pop ebp ; ret -- 7 bytes"""
    return b"\xb8" + struct.pack("<I", value) + b"\x5d\xc3"


# The follower: frameless, no prologue the gap pass knows, reached by nothing.
#   mov edx, [esp+4] ; mov eax, [edx] ; add eax, 1 ; ret
FOLLOWER = b"\x8b\x54\x24\x04" + b"\x8b\x02" + b"\x83\xc0\x01" + b"\xc3"


def _owner_with_table(index_table=b""):
    """A switch function at BASE, its table (and optional index bytes) after
    the last ret, then FOLLOWER. Returns (data, table, follower address,
    padding address).

        BASE+0:  push ebp ; mov ebp, esp
        BASE+3:  mov eax, [ebp+8]
        BASE+6:  [movzx eax, byte ptr [eax + INDEX]]   when index_table
                 jmp dword ptr [eax*4 + TABLE]
                 arm0 ; arm1 ; arm2 ; arm3              7 bytes each
                 mov edi, edi                            padding
                 TABLE: 4 dwords
                 [INDEX bytes ; int3 padding]
                 FOLLOWER
    """
    head = b"\x55\x8b\xec" + b"\x8b\x45\x08"
    movzx_len = 7 if index_table else 0
    dispatch_at = BASE + len(head) + movzx_len
    arms_at = dispatch_at + 7
    arms = [arms_at + 7 * i for i in range(4)]
    pad_at = arms_at + 28
    table = pad_at + len(PAD)
    index_at = table + 16
    tail = index_table
    if index_table:
        tail += b"\xcc" * ((-(index_at + len(index_table))) % 16)
    follower = index_at + len(tail)

    code = head
    if index_table:
        code += b"\x0f\xb6\x80" + struct.pack("<I", index_at)
    code += b"\xff\x24\x85" + struct.pack("<I", table)
    code += b"".join(_arm(i) for i in range(4))
    code += PAD
    code += struct.pack("<IIII", *arms)
    code += tail
    assert BASE + len(code) == follower
    code += FOLLOWER + b"\xcc" * 16
    return code, table, follower, pad_at


def test_function_after_a_switch_table_is_found():
    data, table, follower, pad = _owner_with_table()
    det, engine = _detect(data)
    assert table in engine.jump_tables
    assert follower in det.functions, sorted(hex(a) for a in det.functions)
    assert det.functions[follower].detection_method == "jump_table_follower"
    assert det.functions[follower].end == follower + len(FOLLOWER)


def test_padding_in_front_of_a_table_is_not_a_prologue():
    # The old failure: a function on the `mov edi, edi`, swallowing the table
    # and the follower behind it.
    data, _, follower, pad = _owner_with_table()
    det, _ = _detect(data)
    assert pad not in det.functions, hex(pad)
    owner = det.functions[BASE]
    assert owner.end <= pad, hex(owner.end)


def test_a_two_level_switch_index_table_is_stepped_over():
    # movzx eax, byte ptr [eax + INDEX] ; jmp [eax*4 + TABLE]. The index bytes
    # are small arm numbers, decode as `add`, and probe as a body easily.
    index = bytes([0, 1, 1, 2, 0, 3, 3, 3, 2, 1, 0])
    data, table, follower, _ = _owner_with_table(index)
    det, engine = _detect(data)
    index_at = table + 16
    assert index_at in engine.byte_tables
    assert follower in det.functions, sorted(hex(a) for a in det.functions)
    for addr in range(index_at, index_at + len(index)):
        assert addr not in det.functions, hex(addr)


def test_a_switch_arm_after_its_table_is_not_a_follower():
    # MSVC's memcpy parks arms straight after a table. When the owner was
    # measured short, the arm sits in a gap, but starting a function there
    # would clamp the owner: it is the owner's code.
    #   BASE+0: jmp [eax*4 + TABLE] ; ret ; mov edi, edi
    #   TABLE:  3 dwords, the last pointing just past the table
    #   ARM:    xor eax, eax ; ret
    table = BASE + 10
    arm = table + 12
    data = b"\xff\x24\x85" + struct.pack("<I", table) + b"\xc3" + PAD
    data += struct.pack("<III", BASE + 7, BASE + 7, arm)
    data += b"\x31\xc0\xc3" + b"\xcc" * 16
    engine, image = _engine(data)
    assert table in engine.jump_tables
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det.functions = {BASE: Function(BASE, BASE + 8, "sub_owner")}
    assert not det._pass_jump_table_followers([image.section])
    assert arm not in det._candidates


def test_a_table_inside_a_function_has_no_follower():
    # When the owning body covers the bytes after the table, they are its own
    # code, not a new function.
    data, _, follower, _ = _owner_with_table()
    engine, image = _engine(data)
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det.functions = {BASE: Function(BASE, follower + len(FOLLOWER), "sub_owner")}
    assert not det._pass_jump_table_followers([image.section])
