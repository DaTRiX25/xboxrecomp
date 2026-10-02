"""Branches that leave a lifted body land on an entry, not a stub.

Each function and each alias is lifted as its own C body over [start, end). A
direct jmp or jcc whose target is outside that range becomes a tail call to
sub_<target>. When nothing starts at the target, that is an unresolved stub
which adds 4 to esp and returns, as if the guest had executed a `ret`. The
native chain unwinds and the guest frame does not.

BLiNX 2's sub_00044900 is split at a lone int3 (0x00044CFA) into two primary
bodies. The halves and their aliases branch into each other's interiors:

- sub_00044CFB has `jne 0x00044C20`, a jcc into the middle of sub_00044900.
  The tail-jump pass looks only at unconditional jumps, and the orphan pass
  looks only at gaps.
- The alias sub_00044C0B (from 0x00044C0B to sub_00044900's end) has
  `jle 0x000449B7`, before its own start but inside its primary. Every pass
  tests "leaves its function" against the primary bodies, so it looks like an
  intra-function branch.

Both went to stubs. The display-list interpreter returned with 0x88 bytes of
frame still on the guest stack, and its caller's scene walk recursed until
the native stack overflowed.

Run: python3 -m pytest tools/disasm/test_branch_alias_closure.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import Function, FunctionDetector  # noqa: E402


class _Insn:
    def __init__(self, addr, size=2, mnemonic="mov", target=None,
                 is_jump=False, is_cond_jump=False):
        self.address = addr
        self.size = size
        self.mnemonic = mnemonic
        self.jump_target = target
        self.is_jump = is_jump
        self.is_cond_jump = is_cond_jump


class _Engine:
    def __init__(self, insns):
        self.instructions = {i.address: i for i in insns}

    def get_instructions_in_range(self, start, end):
        return [self.instructions[a] for a in sorted(self.instructions)
                if start <= a < end]


def _detector(insns, functions, aliases=None):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns)
    det.functions = {s: Function(start=s, end=e, name=f"sub_{s:08X}")
                     for s, e in functions}
    det._alias_entries = dict(aliases or {})
    det._candidates = {}
    return det


# Two primary bodies, as the int3 split leaves them, plus one alias the
# earlier passes already found.
A_START, A_END = 0x44900, 0x44CFB
B_START, B_END = 0x44CFB, 0x44F3E
ALIAS_C0B = 0x44C0B


def _blinx_shape():
    insns = [_Insn(a) for a in range(A_START, B_END, 0x10)]
    insns += [
        _Insn(0x449B7),                                   # loop head
        _Insn(0x44C20),                                   # loop body label
        _Insn(0x44C30, mnemonic="jle", target=0x449B7, is_cond_jump=True),
        _Insn(0x44E00, mnemonic="jne", target=0x44C20, is_cond_jump=True),
        _Insn(0x44E10, mnemonic="jmp", target=A_START + 0x10, is_jump=True),
    ]
    return insns


def test_jcc_into_another_body_becomes_alias():
    det = _detector(_blinx_shape(), [(A_START, A_END), (B_START, B_END)])
    det._pass_branch_alias_closure()
    assert det._alias_entries.get(0x44C20) == A_END


def test_branch_back_out_of_an_alias_becomes_alias():
    det = _detector(_blinx_shape(), [(A_START, A_END), (B_START, B_END)],
                    aliases={ALIAS_C0B: A_END})
    det._pass_branch_alias_closure()
    # 0x44C30 sits inside the alias [0x44C0B, A_END) and branches to before
    # its start: an entry is needed there even though the primary covers it.
    assert det._alias_entries.get(0x449B7) == A_END


def test_closure_follows_new_aliases():
    # Only the jcc in B is visible at first. The alias it creates at 0x44C20
    # contains the jle to 0x449B7, which needs an entry of its own.
    det = _detector(_blinx_shape(), [(A_START, A_END), (B_START, B_END)])
    det._pass_branch_alias_closure()
    assert 0x44C20 in det._alias_entries
    assert det._alias_entries.get(0x449B7) == A_END


def test_intra_body_and_known_targets_are_left_alone():
    insns = _blinx_shape()
    det = _detector(insns, [(A_START, A_END), (B_START, B_END)])
    det._pass_branch_alias_closure()
    # The jmp to A_START + 0x10 is a known instruction inside A, so it gets
    # an alias too. A branch to a function start must not.
    assert A_START not in det._alias_entries
    assert B_START not in det._alias_entries
    det2 = _detector([_Insn(0x100), _Insn(0x110, mnemonic="jne", target=0x100,
                                          is_cond_jump=True)],
                     [(0x100, 0x200)])
    assert det2._pass_branch_alias_closure() == 0


def test_targets_in_gaps_or_mid_instruction_are_skipped():
    insns = [
        _Insn(0x100),
        _Insn(0x110, mnemonic="jne", target=0x250, is_cond_jump=True),  # gap
        _Insn(0x120, mnemonic="jne", target=0x305, is_cond_jump=True),  # mid
        _Insn(0x250),        # decodes, but no body covers it
        _Insn(0x300, size=8),
    ]
    det = _detector(insns, [(0x100, 0x200), (0x300, 0x400)])
    assert det._pass_branch_alias_closure() == 0
    assert det._alias_entries == {}
