"""
Opt-in guest patches (--patches): a value a guest instruction loads, replaced
while a setting says so.

A title sometimes carries a behaviour it never switches on for the player's
region: Burnout 3's US build has the km/h speedometer of its other languages,
chosen by the language index it loads at nine places. Rather than rewrite the
function or the data, the recompiler follows the load with a call:

    eax = MEM32(0x45B9BC);
    eax = RECOMP_PATCH_U32("game.speed_units", "kmh", 2u, eax); /* patch 0x... */

RECOMP_PATCH_U32 (templates/runtime/recomp_types.h) returns the value the
instruction loaded unless the setting named by the first argument currently
reads the second, then it returns the third. The runtime (src/platform/
recomp_patch.h) maps the setting key to the environment key the overlay and
the settings file already drive, so the choice is live, and a host that never
enables the setting runs the title as it was. Memory is untouched: the guest
variable keeps its real value for every other reader.

The file (JSON):
    {"patches": [{"va": "0x0005051A", "kind": "load_u32",
                  "setting": "game.speed_units", "when": "kmh", "value": 2,
                  "note": "..."}]}

Optional "latch": "set" or "use" (RECOMP_PATCH_U32_LATCH): the title builds
part of a display once from the choice and reads it again every frame (the
speedometer's unit label and its number). The "set" site samples the setting
and the "use" sites follow that sample, so a change made mid-race shows
whole, at the next set-up, and never as 171 "mph".

kind "load_u32": the instruction at va must be a 32-bit `mov reg, [mem]`.
Its address may sit in several lifted bodies (the lifter splits a function
that another one jumps into); every body gets the call. A listed site that
is not such an instruction, or that no body contains, is an error: a shape
the patcher does not know is never forced through.
"""

import json
import re

_KINDS = frozenset({"load_u32"})
_SETTING_RE = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$")
_WHEN_RE = re.compile(r"^[A-Za-z0-9_+./-]+$")
_REG32 = frozenset({"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"})


def _parse_va(v):
    return int(v, 16) if isinstance(v, str) else int(v)


class Patch:
    def __init__(self, va, kind, setting, when, value, note="", latch=None):
        self.va, self.kind, self.setting = va, kind, setting
        self.when, self.value, self.note = when, value, note
        self.latch = latch


class PatchConfig:
    """The parsed patch file."""

    def __init__(self, patches=()):
        self.patches = {}
        for p in patches:
            if p.va in self.patches:
                raise ValueError(f"patch 0x{p.va:08X} listed twice")
            self.patches[p.va] = p

    @classmethod
    def load(cls, path):
        with open(path) as f:
            doc = json.load(f)
        if not isinstance(doc, dict):
            raise ValueError(f"{path}: expected a JSON object")
        unknown = set(doc) - {"patches", "about"}
        if unknown:
            raise ValueError(f"{path}: unknown keys {sorted(unknown)}")
        out = []
        for s in doc.get("patches", []):
            bad = set(s) - {"va", "kind", "setting", "when", "value", "note",
                            "latch"}
            if bad:
                raise ValueError(f"{path}: unknown patch keys {sorted(bad)}")
            kind = s.get("kind", "load_u32")
            if kind not in _KINDS:
                raise ValueError(f"{path}: unknown patch kind {kind!r}")
            setting, when = s["setting"], s["when"]
            if not _SETTING_RE.match(setting):
                raise ValueError(f"{path}: bad setting key {setting!r}")
            if not _WHEN_RE.match(when):
                raise ValueError(f"{path}: bad 'when' {when!r}")
            value = _parse_va(s["value"])
            if not 0 <= value <= 0xFFFFFFFF:
                raise ValueError(f"{path}: value {value} is not a uint32")
            latch = s.get("latch")
            if latch not in (None, "set", "use"):
                raise ValueError(f"{path}: latch must be 'set' or 'use'")
            out.append(Patch(_parse_va(s["va"]), kind, setting, when, value,
                             s.get("note", ""), latch))
        return cls(out)


class PatchReport:
    """Which listed sites were patched, per VA, across all bodies."""

    def __init__(self, config):
        self.config = config
        self.applied = {va: 0 for va in config.patches}
        self.errors = []

    def _err(self, msg):
        if msg not in self.errors:       # a block is lifted more than once
            self.errors.append(msg)

    def after(self, insn, func_addr=None):
        """Statements to emit after the lifted instruction, or []."""
        p = self.config.patches.get(insn.address)
        if p is None:
            return []
        ops = insn.operands
        if (insn.mnemonic != "mov" or len(ops) != 2 or ops[0].type != "reg"
                or ops[0].reg not in _REG32 or ops[1].type != "mem"
                or ops[1].mem_size != 4):
            self._err(
                f"patch 0x{p.va:08X}: {insn.mnemonic} is not a 32-bit "
                f"`mov reg, [mem]` load")
            return []
        reg = ops[0].reg
        self.applied[p.va] += 1
        if p.latch:
            return [f'{reg} = RECOMP_PATCH_U32_LATCH("{p.setting}", '
                    f'"{p.when}", 0x{p.value:X}u, {reg}, '
                    f"{1 if p.latch == 'set' else 2}); "
                    f"/* patch 0x{p.va:08X}, latch {p.latch} */"]
        return [f'{reg} = RECOMP_PATCH_U32("{p.setting}", "{p.when}", '
                f"0x{p.value:X}u, {reg}); /* patch 0x{p.va:08X} */"]

    def finish(self):
        """Listed sites no body contained are errors too."""
        for va, n in sorted(self.applied.items()):
            if n == 0:
                self._err(
                    f"patch 0x{va:08X}: no lifted body has an instruction there")
        return self.errors

    def to_json(self):
        return {"sites": [{"va": f"0x{va:08X}", "patched": n > 0,
                           "setting": self.config.patches[va].setting,
                           "note": self.config.patches[va].note}
                          for va, n in sorted(self.applied.items())],
                "patched_sites": sum(1 for n in self.applied.values() if n),
                "errors": list(self.errors)}
