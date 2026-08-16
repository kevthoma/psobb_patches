# PSOBB client map

Running record of what we know about the *inside* of `PsoBB.exe` — addresses, structures, and the
techniques that found them. The three big open features (remembered game-creation settings, right-stick
camera, inventory past 30) all need the same thing: structures located in a binary with no symbols.
Without a shared map each attempt starts from zero, so **anything learned goes in here, including dead
ends** — knowing where something *isn't* is worth recording.

## The client we target

| | |
|---|---|
| Build | **59NL** (newserv `specific_version = 0x35394E4C`) |
| Also compatible | `50YJ`, `59NJ` — most published patches list all three |
| Architecture | x86, 32-bit |
| Ours to modify | `d3d8.dll` (this repo's wrapper — full source **and CI symbols**), the `.asi` plugins |
| Not ours | `PsoBB.exe` — no symbols and no map, but **not packed**: fully statically analyzable |

**`PsoBB.exe` is NOT packed** (confirmed 2026-08-16). The ASProtect warning in circulation applies to
Sega's original distribution; the anzz1 Multi client we ship is already unpacked. Evidence: `.text` is
5,024,768 bytes at entropy 6.14 and disassembles cleanly at arbitrary offsets deep inside; the import
table is a full 256 functions across 13 DLLs; `.idata` is intact. The one oddity that trips naive packer
heuristics is the entry point living in a 5,120-byte `.pseudo` section — that section is 99% zeros and
holds a 33-byte **patch-loader trampoline**, not a packer stub:

```
0x00B60000  push 0xB60028            ; "patch.dll"
            call [0x008F8130]        ; LoadLibraryA
            push 0xB60032            ; "patch"
            push eax
            call [0x008F812C]        ; GetProcAddress
            call eax                 ; run patch.dll!patch
            jmp  0x0085AB3C          ; original entry point, in .text
```

So: static analysis works, the whole address space is readable, and `patch.dll` gets control before the
game does.

**Column order matters.** Published patches write addresses as `<VERS a b c>` positional against their
`.versions` line. Where that line reads `50YJ 59NJ 59NL`, **the third column is ours**. Always check the
`.versions` order in the file you're reading — it is not consistent between files.

## Confidence

- **Confirmed** — verified by us against the running client or a byte-matched binary.
- **Published** — taken from a credible external patch that targets our build; not independently checked.
- **Inferred** — reasoned from structure or protocol; treat as a hypothesis.

## Known addresses (59NL unless noted)

| What | Address | Confidence | Source |
|---|---|---|---|
| `GetProcAddress` import thunk | `0x008F812C` | **Confirmed** | Read directly out of the entry-point trampoline; also matches the published value exactly |
| `LoadLibraryA` import thunk | `0x008F8130` | **Confirmed** | Same trampoline, adjacent slot |
| Original entry point | `0x0085AB3C` | **Confirmed** | `jmp` target at the end of the trampoline |
| Patch-loader trampoline | `0x00B60000` | **Confirmed** | The `.pseudo` section; loads `patch.dll` and calls its `patch` export |
| `GetModuleHandleA` import thunk | `0x008F81F0` | Published | newserv `client-functions/ItemPickup.s`, 3rd VERS column |
| Item-pickup flag check (hook site) | `0x0068933D` | Published | same — the patch hooks here to gate pickup on a key |

Resolved Win32 import thunks are a useful foothold: a patch of ours can reach the whole Win32 API through
them without doing its own resolution.

**The published addresses check out.** `0x008F812C` was independently confirmed by reading it out of the
trampoline, which also validates the positional `<VERS>` column rule — the third column really is ours.
That raises confidence in the rest of `ItemPickup.s`'s 59NL column, and in other patches read the same way.

## Structures (protocol side, from newserv — reliable)

- `PlayerInventory` = `{u8 num_items, u8 hp_from_materials, u8 tp_from_materials, Language, item[30]}`,
  exactly `0x034C` bytes, each item `0x1C`. It sits at **offset 0** of the character-data struct that
  commands `0x61`/`0x98` exchange, with `disp` at `0x034C` and records/choice-search/info-board after —
  so the array cannot grow without moving every following field. `num_items` is a `u8`, so the count is
  not the limit; the array is.
- `C_CreateGame_BB_C1` is `0x50` bytes: name, password, difficulty, episode, battle/challenge/solo flags.

## Techniques that work here

**Symbolizing a crash in our own DLL.** Take the `Fault offset` from the Windows Error Reporting event,
`gh run download <run>` the CI artifacts for the matching commit, confirm the artifact `d3d8.dll` md5
matches the installed one *and* the `.map` timestamp matches WER's Fault Module Timestamp, then resolve
the RVA against the `.map` and disassemble around it (capstone + pefile in a throwaway venv). This
identified `SetRenderTarget+0x68` reading `[NULL+8]` precisely.

**Using the wrapper as an oracle.** We compile the d3d8 wrapper, and every `SetTransform` the game issues
passes through our code — including the view and projection matrices. For anything camera-related this
turns a blind memory scan into a targeted one: compute the true camera yaw from the view matrix each
frame, then look for the address whose value tracks it. Not yet built.

**Anchoring on a known wire format.** For the create-game dialog, the C1 packet's `0x50`-byte layout is
known exactly, so the code that *builds* C1 is findable, and the struct holding the dialog's live
selections is upstream of it. Better than searching for the UI directly.

## Open targets

| Goal | Anchor | Notes |
|---|---|---|
| Remembered game-creation settings | C1 packet builder (`0x50` bytes) | Best first target — concrete anchor. Needs the struct feeding C1, then persist to `HKCU\Software\SonicTeam\PSOBB`. |
| Right-stick camera | View matrix via our `SetTransform` | Easier to *find* than the above (a continuously-changing value can be correlated) but far more work after: the auto-camera overwrites each frame, plus collision and lock-on. |
| Inventory past 30 | `PlayerInventory` at offset 0 | Protocol side understood; the client-side wall is the 30-slot inventory UI, which has no paging. |
| Post-quest exit in One Person | — | **No anchor yet.** Confirmed the client sends `0x98` Leave game ~2s after the quest's success handler `ret`s; the trigger is in the client and unlocated. Quest scripts and server are ruled out. |

## Dead ends (do not re-walk)

- **Quest scripts do not cause the One Person exit.** `qexit` (`0xF8C6`) is used by only 5 of ~53 solo
  quests, and removing `p_return_guild` from Battle Training changed nothing — verified by prototype.
- **HUD/text sharpening.** A CAS pass was tried in the scene block, at `EndScene`, and at `Present` on the
  real swap-chain backbuffer. None affected HUD text; PSO composites it somewhere our full-frame passes
  don't reach. The scene-only pass is the one that works.
- **dgVoodoo2** cannot be combined with this wrapper — only one `d3d8.dll` can be loaded, and ours is also
  the ASI loader.

## Sources worth reading before starting anything

- **[Solybum/Blue-Burst-Patch-Project](https://github.com/Solybum/Blue-Burst-Patch-Project)** — MIT, targets
  `59NL`. No camera work, but a real corpus of working hooks into this exact binary.
- **newserv `system/client-functions/*.s`** — patches with real addresses. Mind the `.versions` gate: the
  directory mixes Dreamcast, GameCube, Xbox and BB, and most files are *not* BB.
- **newserv `notes/ar-codes.txt`** — pointers to GC-Forever threads and the project above. No camera entries.

**Rule: we do not disassemble Ephinea's `ephinea.dll`.** Their addresses are for their build, what we need
is a fact about the base client, and this stack's clean-room provenance is worth more than a head start.
Black-box observation of their behavior is fine and useful — it tells us what "good" looks like.
