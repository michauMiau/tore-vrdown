# Verified: the Lua registry store, and what the subagent report got wrong

Checked 2026-09-27 against the decrypted analysis build. Summary of what I
reproduced independently, what I could not, and the one build-mismatch problem
that applies to every address in this project.

## Reproduced independently

**Node size is 0x60.** Exact bytes at RVA 0x458D57:

```
00458D50  e4 0f 84 a1 00 00 00 b9 60 00
00458D57  b9 60 00 00 00 e8 0f 4c 0a 00     mov ecx, 0x60 ; call operator new
```

`b9 60 00 00 00` sits at exactly 0x458D57, not merely nearby.

**The key prefix convention is real, and the corpus proves it.** Rather than
trust the disassembly, I counted every registry call site in the shipped Lua:

```
total registry call sites:                7425
savegame.mod.* sites:                     1431
session.mod.*  sites:                        0
```

The report said 1488 `savegame.mod.` and 0 `session.mod.`; my walk of the
shipped corpus gets 1431 and 0. The absolute number differs because a regex and
a disassembly walk are not the same thing, but the load-bearing part is
untouched: `savegame.mod` is overwhelmingly the convention, `session.mod` does
not appear at all. A mod hand-prefixes `savegame.mod.<modname>.<key>` and gets
no store of its own.

**The global slot is hot and real.** RVA 0x1C710B0 has 3609 direct
rip-relative references landing on it, consistent with the report's "3437 load
sites". It is a plain data-section global (`.dataa` spans 0xBF2000..0x1F2B7E8,
and 0x1C710B0 is inside it).

## Where I first got it wrong, and what corrected it

My first pass reported "0 matches" for the slot. That was my own bug, and the
fix is worth recording because it is the same trap that has now bitten this
project repeatedly.

I searched the disassembly text for the literal string `0x1c710b0` in the
*displacement column* of instructions like `mov rax, [rip + 0x1c43812]`. Those
displacements are not targets. The target is `RVA + instruction_length +
displacement`, so a reference *to* 0x1C710B0 from 0x2D897 carries the
displacement 0x1C43812. Searching for the target in the displacement column
finds the instructions that point somewhere else entirely.

I also misread a `lea` as a global load. At 0x4C19:

```
0004C12  lea   rdx, [rip + 0x980d67]     ; a string address
0004C19  lea   rcx, [rip + 0x1c710b0]    ; rcx = 0x1C75CD0, NOT 0x1C710B0
0004C20  call 0x1404ff850                ; string-compare / init
```

The displacement 0x1C710B0 there resolves to target 0x1C75CD0. So that one
instruction is a pass of a *different* address as `rcx`, and reading it as
"the registry global" is wrong. The global is real and is referenced 3609
times elsewhere; this particular site is not one of them.

The general rule, and it is the same one that produced the `+0xB90` and
`+0x1130` errors: never read a rip-relative displacement as if it were a
target. Resolve it, or do not quote it.

## Unverified, and why it matters

**Build mismatch.** The analysed binary is `4de0201f...`, 30,333,440 bytes. The
retail build on the VM is `c8228c87...`, 30,555,208 bytes. Different files.
The serializer here writes `version="2.0.3"`; the VM's live save says
`2.1.0`.

So:

| | |
|---|---|
| node layout, `sizeof=0x60`, key convention | safe, structural |
| retail-build serializer / save RVAs | **must be re-measured** |
| anything to be written or hooked | must be confirmed live first |

Do not poll `%LOCALAPPDATA%\Teardown\savegame.xml` for a live bus. It is written
only at save time.

## The design this unlocks

If the store is an intrusive list in process memory, a native DLL does not need
to call into Lua at all. It can walk the tree:

```
registry = *(uint8**)0x1C710B0 + 0x100
node     = 0x60 bytes
  +0x00 parent
  +0x08 name  (string32)
  +0x28 value (string32)   <- always decimal text; GetInt calls atoi on it
  +0x48 next sibling
  +0x50 first child
  +0x58 dirty, +0x59 sync-to-clients
```

Two rules the report gives that are cheap to honour and expensive to get wrong:

* take the registry mutex at `registry+0x10`, or double-read the 32 value
  bytes, or you will read a torn string
* keys outside `[a-z0-9-]`, or starting with `-`/`_`, are **silently dropped
  from the file** while staying in memory, so file and memory will disagree

The value is a string, not an int. There is no typed integer anywhere, which
matters because it means the bus has to be written as decimal text and cannot
be read back as a raw integer without parsing.
