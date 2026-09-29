# Where Teardown's SetInt/GetInt actually stores its data

**Status: SOLVED for the memory shape, and the file shape is real too. Both are documented below with the disassembly that proves each claim.**

Scope: what a Lua script's `SetInt`/`GetInt` write to, how to read it from an
injected native DLL, and exactly what one entry looks like in each representation.

Binary analysed: `/root/steamless/Teardown/teardown.exe`
SHA256 `4de0201f3f40774164a2eaf89fa4c65358e57291cb010539983cd9606e3ca977`,
30,333,440 bytes, PE32+ x86-64, ImageBase `0x140000000`.
**This is the "steamless"/cracked build, not the build running on the test VM**
(see *Build-version caveat* at the end — it matters and is easy to get wrong).

Every RVA below was resolved with `/home/truenas_admin/teardown-analysis/ripdis.py`,
which takes the rip-relative displacement base from the decoded instruction
length. The earlier `0x400` class of error (rva = file - 0x400 + 0x1000 applied
outside `.text`) does not affect any number in this document, because
`tdmap.py` reads the real section table:
`.text vaddr 0x1000 raw 0x400`, `.rdata vaddr 0x982000 raw 0x980C00`,
`.dataa vaddr 0xBF2000 raw 0xBF0400`.

---

## 1. Answer in one paragraph

`SetInt` does **not** write to a settings file and does **not** keep a typed
integer. It writes a **32-byte heap-or-SSO string** holding the decimal text of
the number, into a **linked-list tree node** in process memory. The whole tree is
owned by a single object reachable at a fixed address, and the same tree is
serialised to `%LOCALAPPDATA%\Teardown\savegame.xml` on quit. A native DLL can
read the value either by walking the node tree in memory (fast, live, no file
I/O) or by parsing the XML (survives restarts, but only updates on save).
**The memory walk is the right answer for a haptics bus.**

---

## 2. How SetInt reaches the store

### 2.1 Registration table

`"SetInt"` is a C string at RVA `0xA42694`. The only 8-byte pointer to it is at
RVA `0xC46CA8` in `.dataa`, next to the function pointer `0x4AF710`:

```
rva 0x000C46CA0  0x00000001404AF710  -> rva 0x4AF710 (.text)   ; impl
rva 0x000C46CA8  0x0000000140A42694  -> rva 0xA42694 (.rdata)  ; "SetInt"
rva 0x000C46CB0  0x0000000000000000
```

This is a 3-slot record (fn, name, 0) repeated for the whole Lua API.
`GetInt` is at `0xC46CB8`/`0x4ACA60`, `SetFloat` at `0xC46CD0`/`0x4AF640`.
**A native DLL can call `0x4AF710` directly**, but it does not need to.

### 2.2 Call chain (all RVAs, from real disassembly)

```
Lua SetInt(key, value, sync)
  0x4AF710  Lua binding shim: reads args, resolves key, calls 0x49CCC0
  0x49CCC0  Mod-registry key rewrite  (this is where the mod name is applied)
  0x1267C0  SetInt: special-cases game.tool.* ammo keys, then tail-jumps
  0x45AFE0  RegistrySetValue(registry, key, valuestring, sync)
  0x4FFD90  int -> string, snprintf(dst, 0x20, "%i", value)   ; format "%i" @0x9CD68C
  0x45B080  the real write
  0x458960  find-or-create node by dotted path
```

The tail jump, quoted:

```
001268F8  mov      rcx, qword ptr [rip + 0x1b4a7b1]   ; rva 0x1C710B0
001268FF  mov      rcx, qword ptr [rcx + 0x100]       ; the registry object
00126925  jmp      0x14045afe0
```

GetInt is the mirror image, and proves the value is a **string** that gets
parsed on read:

```
00458FC0  ...  call 0x140459370      ; find node, copy node->value out
00458FFB  call 0x140503760          ; -> jmp qword ptr [rip+0x47f310] ; rva 0x982A80
00459000  mov      edi, eax          ; IAT 0x982A80 == atoi
```

`0x982A80` is an import thunk; `atoi` exists as a plain name in `.rdata` at
file offset `0xBEF834`. **There is no int field anywhere in the node.**

---

## 3. The key rewrite — how the mod name selects the store

This is the part that matters for a per-mod bus, and it is **not** what the
brief guessed. `0x49CCC0` does not store per-mod. It **rewrites the key
prefix**, gated on a flag:

```
0049CCE4  cmp      qword ptr [rcx + 0x420], 0     ; the flag; 0 => key is un-namespaced
0049CCF5  lea      rdx, [rip + 0x59aa54]          ; rva 0xA37750  'savegame.mod'
0049CD0F  lea      rdx, [rip + 0x5a3ed2]          ; rva 0xA40BE8  'savegame.mod.'
0049CD29  lea      rdx, [rip + 0x5a3ec8]          ; rva 0xA40BF8  'session.mod'
0049CD43  lea      rdx, [rip + 0x5a3ebe]          ; rva 0xA40C08  'session.mod.'
0049CD62  lea      rdx, [rip + 0x4f2e97]          ; rva 0x98FC00  'savegame'
0049CD78  lea      rdx, [rip + 0x4f3449]          ; rva 0x9901C8  'savegame.'
0049CD8E  lea      rdx, [rip + 0x4f2e7b]          ; rva 0x98FC10  'options'
0049CDA4  lea      rdx, [rip + 0x5a3e6d]          ; rva 0xA40C18  'options.'
0049CDBB  lea      rdx, [rip + 0x4e8bbe]          ; rva 0x985980  ""  (empty string)
```

Consequences, all of which matter for the VR bus:

1. **The mod does not get its own store.** Everything lives under one global
   `savegame` subtree in one process-wide registry. A mod must namespace its
   own keys, and the shipped game does exactly that by hand.
2. **Naming convention is a convention, not enforcement.** 1,488 occurrences of
   `savegame.mod.` appear across the shipped `.lua` files, and **zero**
   occurrences of `session.mod.`. A mod that writes plain `SetInt("x.y", 1)` gets
   the bare key, unmodified. Examples of the real convention, harvested from
   `mods/folkrace` and `dlcs/wildwestheist`:
   `savegame.mod.cash`, `savegame.mod.campaign1.level1.bestTime`,
   `savegame.mod.available_vehicles.garage_vehicle`,
   `savegame.mod.builtin-artvandals.cinematic.complete`.
   Note the last one: the second component is a **mod name**, and it can contain
   a `-`. So for this project: **`savegame.mod.<modname>.<yourkey>`**.
3. `savegame.mod.*` is the **persistent** namespace (it reaches the XML file);
   `session.mod.*` is the **session-only** namespace. Which one a key lands in
   is decided by the flag at `[rcx+0x420]`, read at `0x49CCE4`.
4. Dots are **structure**, not text. `savegame.mod.a.b` is a 4-deep node chain,
   not one key with dots in it. The splitter is at `0x458A60`
   (`cmp byte ptr [rax + r12], 0x2e`).

**Recommendation for the VR bus: use `savegame.mod.` if you want values to
survive a restart, and do not rely on a session flag you cannot see from the
native side.**

---

## 4. THE ANSWER: node layout in process memory

### 4.1 Identity of the tree

```
registry = *( *(uint8**)0x1401C710B0 + 0x100 )
```

- Global slot RVA `0x1C710B0`, so VA `0x1401C710B0` (.dataa, zero-initialised BSS).
- The engine object at that slot has the registry pointer at offset `+0x100`.
- 3,437 `mov reg,[rip+disp]` load sites target `0x1C710B0`; 544 of them follow
  through with `+0x100`. Two stores ever write the slot (`0x6C0FB`, `0xA5A7A`),
  so it is set once at startup and is safe to read from the DLL.

**Reading order for a DLL:**
`uint8* engine = *(uint8**)0x1401C710B0; Registry* reg = *(Registry**)((uint8*)engine + 0x100);`

### 4.2 One entry

`0x458960` allocates nodes inline; the size is proved by the allocation call:

```
00458D57  mov      ecx, 0x60
00458D5C  call     0x1404fd970          ; operator new(0x60)
```

**`sizeof(node) == 0x60` (96 bytes), heap-allocated, one per key component.**

| Offset | Size | Field | Proof |
|---|---|---|---|
| `+0x00` | 8 | `parent` node ptr, NULL at root | `00458D7C mov qword ptr [rax], rsi` |
| `+0x08` | 0x20 | `name` (string32) — the key component | `00458D7F lea rcx,[rax+8]` + `00458D83 call 0x4ff7b0` |
| `+0x28` | 0x20 | `value` (string32) — **the payload, always text** | `00458D89 lea rcx,[rbx+0x28]` + `00458D8D call 0x4ff9b0` (clear), written at `0045B0ED call 0x4ffa10` |
| `+0x48` | 8 | `next` sibling | `00458DA1 mov qword ptr [rbx + 0x48], r13` |
| `+0x50` | 8 | `firstChild` | `00458D92 mov qword ptr [rbx + 0x50], r13`; `00458DAE mov qword ptr [rsi + 0x50], rbx` |
| `+0x58` | 1 | `dirty` (needs saving) | `00458D96 mov word ptr [rbx + 0x58], 0` (zeroes +0x58 and +0x59); set `1` at `0045B0F2` |
| `+0x59` | 1 | `sync-to-clients` | `0045B11B mov byte ptr [rdi + 0x59], 1` |

**Children are an intrusive singly-linked list, not a vector.** Proof: the append
walks `+0x48` to the tail and links there.

```
00458DB4  mov      rax, qword ptr [rcx + 0x48]     ; walk +0x48
00458DC3  mov      rax, qword ptr [rax + 0x48]
00458DCC  mov      qword ptr [rcx + 0x48], rbx     ; link new node at tail
```

The child search compares against `+0x08` (name), independently confirmed in
three places: `00458D21 lea rcx,[rbx+8]`, `00458F04 lea rcx,[rbx+8]`, and the
serializer at `0045AB59 movzx r8d, byte ptr [rsi + 0x27]` (the heap flag of the
name string at `+0x08`).

> Correction to an intermediate note: `+0x28` is the **value**, not the name.
> The name is at `+0x08`. The constructor copies the path component into `+0x08`
> and explicitly *clears* `+0x28`.

### 4.3 The 32-byte string type

Not MSVC `std::string` (that has the heap flag at `+0x18`). This one:

- `+0x00` (8) — heap pointer, valid when the flag below is set
- `+0x08` (4) — capacity, u32
- `+0x0C`..`+0x1E` — inline characters
- `+0x1F` (1) — heap flag, `1` = data in the pointer

Proof of the flag and the select:
```
004FFAB0  movzx    r9d, byte ptr [rcx + 0x1f]     ; read heap flag
004FFAC0  mov      rdx, qword ptr [rcx]           ; select heap pointer
004FFAD3  mov      rcx, qword ptr [rcx]
```
Proof of inline capacity 0x20 (32):
```
00501BC1  mov      eax, dword ptr [rdi + 8]       ; capacity at +0x08
00501BC6  mov      eax, 0x20                      ; inline capacity
00501C4A  mov      byte ptr [rdi + 0x1f], 1       ; set heap flag
```

**Practical read for the DLL:** a registry value under 32 bytes (always true for
`SetInt`, which writes at most 11 chars) is inline, so
`*(char*)(node + 0x28)` is the NUL-terminated text directly. No indirection
needed. Implement the flag check anyway for values set via `SetString`.

### 4.4 Concurrency

`+0x10` on the **registry** is a mutex, not on the node:
```
0045B0A5  lea      rbx, [rcx + 0x10]      ; rcx == registry here
0045B0B1  call     0x1405093b0            ; resolves to IAT 0x982528, _Mtx_lock
0045B140  call     0x140509500            ; resolves to IAT 0x982238-ish, _Mtx_unlock
```
**The DLL must take the same lock, or copy the value out and accept a torn read.**
A 32-byte inline string is not atomic. Safest: lock, read, unlock — or read the
32 bytes twice and compare.

### 4.5 Minimal read algorithm

```
reg  = *(uint64*)((*(uint64*)0x1401C710B0) + 0x100)
// split "savegame.mod.<mod>.<key>" on '.', walk child lists via +0x50 / +0x48,
// comparing each node's name at +0x08 (inline unless +0x1F != 0).
// final node's payload is the string at +0x28.
```

---

## 5. The file shape (also real — shape 1 from the brief)

### 5.1 Path

```
%LOCALAPPDATA%\Teardown\savegame.xml
```

On the test VM, **verified to exist**:
`C:\Users\vm\AppData\Local\Teardown\savegame.xml`, 211,434 bytes, plus a
`savegame-backup-2.0.0.xml` sibling. So `%LOCALAPPDATA%` is right and the
directory is `Teardown`.

The path is assembled by `0x45C6C0`, which is a plain `dir + "/" + name` join:
```
0045C6D0  lea      r8, [rip + 0x530cc1]      ; rva 0x98D398  '/'
0045C6DC  lea      rcx, [rip + 0x19f959d]     ; rva 0x1E55C80  base dir string
0045C6E3  call     0x140500050
0045C6EC  mov      rdx, rdi                  ; the filename
0045C6F2  call     0x1404ffc10              ; string concat
```
The base dir itself is produced by `0x45C780`, which takes the exe directory and
appends `Teardown`:
```
0045C7FD  lea      rax, [rip + 0x5490c4]      ; rva 0x9A58C8  'Teardown'
0045C822  lea      r8, [rip + 0x54909f]      ; rva 0x9A58C8  'Teardown'
```
**UNVERIFIED:** the exact chain that puts `%LOCALAPPDATA%` in the base dir.
`0x1E55C80` and `0x1E55C00` read as empty in the dumped file (they are BSS,
initialised at runtime), and **no `.text` instruction writes them via a simple
`mov [rip+disp]`, reg`** — they are almost certainly filled by a
`GetEnvironmentVariableA`/`SHGetFolderPath` wrapper through a level of
indirection I did not chase. The *observed* location on the VM is
`%LOCALAPPDATA%\Teardown\`, which is what matters, and it is consistent with
Teardown's documented behaviour. I am not going to dress an inference up as a
disassembly result.

### 5.2 Save function

`RegistrySaveToFile` is RVA **`0x45A690`**, and it has exactly **two** callers:

```
00073C67  call 0x14045a690    ; save the "options" subtree  (0x73C4A builds "options")
00073D0C  call 0x14045a690    ; save the "savegame" subtree (0x73CEF builds "savegame")
```

Both live in the same function, which also does the load (`0x6E4A0` references
`savegame.xml` and checks a version string). So **the file is written on quit /
explicit save, not on every `SetInt`.** Consequence for the DLL: **you cannot
reliably poll the XML for live values.** Reading the file gives you state as of
the last save only.

### 5.3 Entry format in the file

One node = one XML element; the payload is a `value=` attribute. Emitted by
`0x45AD20` (per node) and `0x45ABC0` (document root):

```
0045ABF5  lea      rdx, [rip + 0x535574]   ; rva 0x990170  'registry'   <- root element
0045AC24  lea      rdx, [rip + 0x5329f5]   ; rva 0x98D620  'version'
0045AC36  lea      r8,  [rip + 0x5329db]   ; rva 0x98D618  '2.0.3'      <- version literal
0045AE20  ...  call 0x140511d20                                ; <name> as element tag
0045AE44  lea      rdx, [rip + 0x5843c9]   ; rva 0x9DF214  'value'
0045AE61  call     0x140512c50                                ; set attribute
0045AE71  mov      rbx, qword ptr [rsi + 0x50]                 ; recurse into children
0045AE89  call     0x14045ad20
0045AE9A  call     0x1405124d0                                ; close element
```

The `value=` attribute is **omitted when the node's value string is empty** —
that is what `rva 0x985980` (the empty-string literal) is compared against at
`0045AE33 call 0x1404ffab0`. An empty-valued node is written as a bare container.

A live sample from the test VM, trimmed:

```xml
<registry version="2.1.0">
	<savegame>
		<stats>
			<totalplaytime value="0.0"/>
			<startcount value="225"/>
		</stats>
		<player>
			<character value="sven_lagom"/>
		</player>
		<savegame>
			<mod>
				<cash value="12345"/>
			</mod>
		</savegame>
	</savegame>
</registry>
```

Note: **indentation is tab**, values are **unquoted** attribute text, and a
number is written as plain text (`"225"`) while a float keeps its own formatting
(`"0.0"`). The float path is `0x45AF50` -> `0x500C40`, which is a different
formatter from the int path's `0x4FFD90`/`"%i"`.

### 5.4 Name sanitisation (matters if your key has a weird character)

`0x45AD20` validates node names before emitting them. Allowed: `a-z` and
`0-9` and `-` (the bitmap `0x43ffffff01ff9` covers
`0x09`-`0x0C`, `0x14`-`0x2C`, `0x32`, plus `0x00`, `0x03`-`0x08`).
It then rejects names starting with `-` or `_`:
```
0045ADAB  lea      rdx, [rip + 0x53258e]   ; rva 0x98D340  '-'
0045ADC6  lea      rdx, [rip + 0x532a8b]   ; rva 0x98D858  '_'
```
A rejected name makes `0x45AD20` return early (`0045AE9F` epilogue) and **the
node is silently dropped from the file**. It stays in memory, so a memory read
and a file read will disagree for such keys. **Keep the VR bus keys to
lowercase alphanumerics and dots.**

---

## 6. What this means for `hook/teardown_vr.c`

The DLL currently has 0 references to the registry. Concrete plan:

```c
// Add to the DLL. All addresses are RVAs; add the runtime image base.
#define TD_REGISTRY_SLOT_RVA   0x1C710B0ull   // uint8** -> engine
#define TD_REGISTRY_OFF        0x100          // engine + 0x100 -> Registry*

typedef struct TdNode {
    void        *parent;      // +0x00
    /* name string32 */      // +0x08 .. +0x27
    /* value string32 */     // +0x28 .. +0x47   <-- the payload
    struct TdNode *next;     // +0x48
    struct TdNode *child;    // +0x50
    uint8_t      dirty;      // +0x58
    uint8_t      sync;       // +0x59
} TdNode;                    // 0x60 bytes

// string32: if (p[0x1F]) data is at *(void**)p else data is at p
static const char *td_str(const void *p) {
    return ((const uint8_t *)p)[0x1F] ? *(const char *const *)p : (const char *)p;
}
```

Search for `savegame`, then `mod`, then the mod directory name, then the key.
Poll from the render/hot thread; **take the registry lock at `registry + 0x10`**
(or read the 32 value bytes twice and compare) to avoid a torn read while Lua is
mid-`SetInt`.

**Do not** build the haptics bus on the XML file. It is written twice per
lifetime (options + savegame) at `0x73C67`/`0x73D0C`, so a haptics message
written by Lua would not appear on disk until the game exits.

---

## 7. Shapes ruled out, and how

- **Shape 3, "a fixed struct the engine mirrors for the settings UI" — ruled out.**
  There is no typed-int mirror. The node has no int/float field; `GetInt` calls
  `atoi` on the string at `+0x28`. The settings UI is not a separate struct, it
  reads the same registry.
- **Shape 2 as "a string map inside the script VM" — ruled out for persistence.**
  The store is reached through the engine singleton, not the Lua VM, and it
  outlives the VM: it is serialised to disk. But shape 2 is *half* right — the
  per-key container is an intrusive linked list keyed by node name, not a hash
  map. So: right idea, wrong container.
- **Shape 1 as the *only* store — ruled out.** The file exists and is written,
  but only at save time, so it cannot serve a live bus. The in-memory tree is
  the live store.
- **A separate per-mod store — ruled out.** `0x49CCC0` only rewrites the key
  prefix; nothing in the write path allocates a per-mod registry. All mods share
  one `savegame` subtree, which is why 1,488 shipped call sites all hand-prefix
  their own keys.

---

## 8. Build-version caveat — read before hardcoding

The binary analysed here is **not** the one running on `192.168.1.6`:

| | this analysis | test VM |
|---|---|---|
| `teardown.exe` size | 30,333,440 (`0x1CEDA00`) | 30,555,208 |
| SHA256 | `4de0201f…` | `c8228c87…` |
| `version=` written | `2.0.3` (literal at RVA `0x98D618`) | `2.1.0` (present in this binary too, at file offset `0xA70100`) |

Both strings exist in the analysed binary, but the serializer at `0x45ABC0`
references `0x98D618` = `2.0.3`, while the VM's live `savegame.xml` says
`2.1.0`. **So the write path in the shipped retail build is a different function
from `0x45ABC0`.** The *node layout* and the *key convention* are almost
certainly unchanged (they are engine-wide, and the observed XML shape matches
exactly), but the exact RVA of the serializer and save trigger in the **retail**
binary is UNVERIFIED and must be re-measured against
`D:\SteamLibrary\steamapps\common\Teardown\teardown.exe` before any hook is
pointed at it.

**What is build-independent and safe to hardcode:** the `version=` attribute
value is read-only for a DLL; the XML element shape (`<node value="...">`, tab
indent) is stable across both versions; the key convention
`savegame.mod.<modname>.<key>` is enforced by the shipped Lua corpus, not by the
binary.

---

## 9. Tooling written for this

All in `/home/truenas_admin/teardown-analysis/`:

- `tdmap.py` — real PE section table, correct file-offset <-> RVA. **Use this
  instead of the `0x400` delta.**
- `ripdis.py` — disassemble an RVA range with every rip-relative target
  auto-resolved and annotated with the C string there. Take the instruction end
  from the decoded length, never by hand.
- `disb.py` — same, plus raw bytes.
- `tdstr.py` — find a string, list all 8-byte pointers to it.
- `disp.py` — find every instruction with a given disp32, any opcode form.
  `find_riprefs.py` only catches `lea` and **misses `mov reg,[rip+disp]`**,
  which is how the 3,437-reference singleton was nearly missed.
- `NOTES_node_layout.md` — the node-layout derivation, in full.

---

## 10. Open items, honestly

1. **The exact chain that resolves `%LOCALAPPDATA%` into the base dir** is not
   proven. Observed on the VM, not derived. (§5.1)
2. **Retail-build RVAs** for the serializer and save trigger are unmeasured.
   (§8)
3. **The float formatter's exact format string** at `0x500C40` is not confirmed
   — the sample shows `"0.0"`, which is consistent with `%g` and inconsistent
   with `%f`, but the DLL does not need it: read the string and use `atof`.
4. **Bytes `0x5A`-`0x5F` of a node are never touched** by any `.text`
   instruction, and the constructor never writes `+0x10..+0x27` of a node. Treat
   them as reserved padding.
