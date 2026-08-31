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

## Addresses are stable — no ASLR

`PsoBB.exe` has `DYNAMICBASE` off and its relocation directory **stripped**, so it can only ever load at
`0x00400000`. Absolute addresses are therefore valid across runs, across machines, and across players —
which is why published patches hardcode them, and why anything confirmed here stays confirmed. (Our own
`d3d8.dll` is relocatable and prefers `0x10000000`; treat its addresses as RVAs, not absolutes.)

## Tooling: `tools/psobb_inspect.py`

Live read-only inspection of the running client — this is what turns a static-analysis guess into a
confirmed address. It opens the process with `PROCESS_VM_READ` only and never writes, injects, or
allocates, so it cannot corrupt a running game.

```
python tools/psobb_inspect.py modules                  # attach, list modules + bases
python tools/psobb_inspect.py read 0x008F812C 32       # hex dump + i32/u32/f32 interpretations
python tools/psobb_inspect.py find 8B0D????????85C9    # byte pattern, ?? = wildcard
python tools/psobb_inspect.py watch 0x00ABCDEF f32     # poll and print on change
```

The differential hunt, for anything you can make change in-game:

```
python tools/psobb_inspect.py scan f32 0.25    # value as it is now
   ... change it in-game ...
python tools/psobb_inspect.py narrow f32 0.5   # or: narrow changed / narrow unchanged
```

Candidates persist in `tools/.psobb_inspect_state.json` between invocations, so a hunt can span several
sessions. `PSOBB_PROCESS` overrides the target process name.

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

## Player stats structure — CONFIRMED LIVE (2026-08-16)

Located by differential scan on the local player's meseta and cross-checked against newserv's
`PlayerStatsT` (`src/LevelTable.hh:58`). **The client's in-memory layout matches the protocol
definition exactly**, which is a useful general result: newserv's structs are a reliable predictor of
what the client holds in RAM, so we can plan against them before touching a debugger.

Offsets relative to the on-hand meseta field. **Confidence differs per field** — verified against the
in-game stat screen and `$li` output for a Lv.178 FOmar:

| Offset from meseta | Field | Type | Observed | Verified? |
|---|---|---|---|---|
| `-0x20` | `atp` | u16 | 839 | ✗ in-game base is **842** |
| `-0x1E` | `mst` | u16 | 1340 | **✓ exact** |
| `-0x1C` | `evp` | u16 | 627 | **✓ exact** |
| `-0x1A` | `hp` | u16 | 534 | ✗ in-game max is **1044** |
| `-0x18` | `dfp` | u16 | 418 | **✓ exact** |
| `-0x16` | `ata` | u16 | 928 | ✗ in-game base is **154** — unexplained |
| `-0x14` | `lck` | u16 | 96 | **✓ exact** |
| `-0x12` | `esp` | u16 | 0 | — |
| `-0x10` | `attack_range` | f32 | 20.5 | — |
| `-0x0C` | `knockback_range` | f32 | 10.0 | — |
| `-0x08` | `level` | u32 | 177 | **✓** 0-based; displays as Lv.178 |
| `-0x04` | `experience` | u32 | 36,696,684 | **✓ exact** (`$li`: 36696684pt) |
| `+0x00` | **`meseta`** | u32 | 99,261 | **✓ exact** (`$li`: 99261Meseta) |

**`level`, `experience` and `meseta` are certain** — they match the `$li` screen to the digit, and meseta
was located by differential scan in the first place.

**The stat block is located but is NOT the character's effective stats.** Four of seven match the in-game
*base* figures exactly; three do not, and searching ±2 KB around meseta finds **no** copy of the displayed
values — not the totals (ATP 1250, DFP 943, ATA 337, EVP 974) and not HP 1044 or TP 2275. So the effective
stats the UI renders are computed or cached somewhere else entirely, and this block is closer to the
level-table baseline: ATP 839 vs 842 is consistent with 3 Power Materials, and HP 534 vs 1044 with armour
plus HP materials. **ATA 928 vs 154 fits neither reading and is unexplained** — do not trust `-0x16` until
someone works out what it is. Finding where the displayed stats live is an open question, and a good
follow-up hunt (scan for 1044 while healing, or for 1250 after an equipment change).

So `PlayerStats` begins at `meseta - 0x20`. The character name sits at about `meseta-0x4C8`, stored
UTF-16LE with PSO's "marked" prefix (`\tE` — tab plus the language letter), and appears **twice** in the
neighbourhood, so there are at least two copies of the player data live at once.

## Effective (displayed) stat block — CONFIRMED LIVE (2026-08-16)

A second, separate block holds the stats the UI actually shows. Found by sweeping memory once for all
seven displayed values at the same time and looking for a **cluster** — far more effective than
narrowing a single value, and it needs no in-game changes. Only one cluster in 618 MB carried four or
more distinct stats.

Offsets relative to the block start (HP max), for the same Lv.178 FOmar:

| Offset | Field | Observed | In-game |
|---|---|---|---|
| `-0x08` | **pointer to the baseline `PlayerStats`** | `0x10EDFCAC` | — |
| `+0x00` | HP max (u16) | 1044 | **✓** |
| `+0x02` | TP max (u16) | 2275 | **✓** |
| `+0x08` | `839` \| `42` | — | 839 is the *baseline* atp, not the total |
| `+0x0C` | EVP total \| DFP total | 974 \| 943 | **✓ both** |
| `+0x10` | duplicate of `+0x08` | | |
| `+0x14` | duplicate of `+0x0C` | | |
| `+0x18` | ATA total \| LCK | 337 \| 96 | **✓ both** |
| `+0x78` | HP max \| TP max again | 1044 \| 2275 | second copy |

**ATP total (1250) and MST (1340) are NOT in this block** — absent within ±0x800, as are the
parenthesised "base" figures (ATP 842, ATA 154). So those are computed at display time rather than
stored, which fits: ATP total depends on the equipped weapon, and MST had no equipment bonus at all
(base == total == 1340, and 1340 does live in the baseline block).

**The two blocks are linked.** The pointer at `-0x08` is the single pointer to the baseline
`PlayerStats` found earlier, so one object holds both the effective stats and a reference to the
baseline. That object is the natural target for a static anchor — and its start is somewhat before this
block (everything from `-0x40` to `-0x0C` reads as zeros).

### ⚠ These are heap addresses — the layout is durable, the addresses are not

The player data is **heap-allocated**, not a static global (an early guess that it would be in
`PsoBB.exe`'s image was wrong — the one in-image hit for the old value turned out to be ASCII text that
happened to match). So a raw address like `0x10EDFCCC` is valid only for one process instance. What is
reusable is the **layout above**, and what is still missing is a **static anchor**: a pointer in
`PsoBB.exe`'s image that leads to the structure.

Anchor hunt so far: exactly one pointer to the `PlayerStats` base exists (itself on the heap), and
nothing points at *that* — so the chain is walked by arithmetic, not stored pointers. Value-scanning has
hit its limit here. Next techniques, in order of expected yield:
1. **Range pointer scan** — look for any u32 falling inside the object rather than equal to one exact
   address; the object base is probably near `meseta-0x4D0`, before the name.
2. **Code reference hunt** — find the instructions that read meseta. `.text` is fully readable and
   ASLR is off, so the static global holding the player pointer will appear as a literal in a
   `mov reg, [0x00XXXXXX]` near an access at one of the offsets above.

### Technique notes from this hunt

- **Pick a distinctive value.** Scanning 2926 gave 57 candidates; 99761 gave 3. Small round-ish numbers
  are everywhere in a game's memory.
- **Bank operations reallocate the structure.** A narrow across a bank withdrawal lost every candidate,
  because the value moved rather than changing in place. A shop purchase updated in place and narrowed
  57 → 3 → 1 cleanly. Prefer in-place changes when narrowing.

## Asset format: `.pae` opening movie — DECODED (2026-08-18)

`data/openning_e.pae` (English; `openning_j.pae` is Japanese) is the character-creation attract reel —
the thing that looks "barely animated" next to the GameCube CG intro. It is **not a video**. Fully
decoded; tool is `tools/pae_extract.py` (self-contained: PRS + XVM/XVR parse + DXT1 + PNG/contact sheet).

Layout: `[0x20 header][PRS-compressed body]`. PRS is Sega's LZ (same codec as newserv `prs.cc`).

```
header  0x00 u32 magic 0x00010001   0x04 u32 decompressed size   0x08/0x0c 0
        0x10 u32 section-A offset    0x1c u32 XVM-archive offset   0x20.. PRS stream
```

The blob decompresses to a **~200 KB timeline/script section** (frame order, positions, pans, fades,
timing — *not yet reversed*) followed by an **XVM texture archive** of still frames:

```
XVRT entry  0x00 "XVRT"  0x04 u32 datasize  0x0c u32 fmt (6 = DXT1)  0x14 u16 w  0x16 u16 h  0x40.. pixels
```

`openning_e.pae`: 5,960,374 B → 9,774,300 B decompressed → **379 DXT1 stills** (257×256², 121×128², 1×32²;
fmt 6 ×375, fmt 7 ×4 — fmt 7 unconfirmed, likely an alpha S3TC variant, decodes fine as DXT1 for preview).
Content = panned Ragol background vistas, 2×2 atlases of pre-rendered in-game scene shots, class-description
text slides, and the Blue Burst logo. **That is the entire "movie"** — 2D stills the client pans/fades over,
capped at 256×256, which is exactly why it reads as static. This is inherent to the BB asset; nothing to do
with the server or the HD/widescreen wrapper.

Verify anytime: `python tools/pae_extract.py info <path>` / `sheet <path> out.png`.

**No community replacement exists** (checked 2026-08-18): the only documented swap is stock JP↔EN, and we
already ship EN. GameBanana has zero PSOBB opening mods; no public `.pae` editing tools — this decode is the
first. So a nicer intro means *authoring* one, not downloading one.

**BACKLOGGED — texture-refresh prototype.** The tractable improvement is re-skinning the 379 stills in place
with sharper art (same count/timing) and repacking. Needs a PRS *compressor* + XVM *packer* (we only have the
decoders so far) and answers to two unknowns: (1) does the timeline section reference frames by index only, so
same-count/same-dims swaps are safe? (2) will the client accept dims > 256² (does the timeline use UV coords
or fixed pixel rects)? Purely cosmetic attract-mode polish — low priority. Full-motion FMV (GC-style) would
need hooking the client's movie player to bypass `.pae` entirely; not worth it.

## Sound and volume — PHASE 1 BUILT (2026-08-25), survey below from 2026-08-19

**`psobb_dsound/` is the proxy `dsound.dll`.** Phase 1 is pure pass-through: twelve naked `jmp` stubs
forwarding to `%WINDIR%\SysWOW64\dsound.dll`, resolved lazily on the first export call (never in
`DllMain` — `LoadLibrary` under the loader lock is how proxy DLLs deadlock) and never by the bare name
`dsound.dll`, which would load the proxy again. A `jmp` keeps the caller's stack and return address
intact, so the real function does its own stdcall cleanup and returns straight to the game; that is why
phase 1 can be behaviour-preserving for all twelve exports without declaring twelve signatures.

**The ordinals are load-bearing and CI checks them.** The game resolves `DirectSoundCreate` by ordinal,
so a `.def` that drifts from the system layout leaves the import unresolved and the client will not
start, with no compile error anywhere. `tools/pe_exports.py check` diffs the built export table against
the runner's own `SysWOW64\dsound.dll` on every build. Real layout (not what you would guess —
`DllCanUnloadNow`/`DllGetClassObject` are @4/@5, not at the end): `DirectSoundCreate` @1,
`DirectSoundEnumerateA/W` @2/@3, `DllCanUnloadNow` @4, `DllGetClassObject` @5,
`DirectSoundCaptureCreate` @6, `DirectSoundCaptureEnumerateA/W` @7/@8, `GetDeviceID` @9,
`DirectSoundFullDuplexCreate` @10, `DirectSoundCreate8` @11, `DirectSoundCaptureCreate8` @12.

**Logging:** silent by default — creating an empty `dsound_proxy.log` next to the game executable opts
in to a one-line "proxy active" record; failures are always logged. Same philosophy as the wrapper's
`RecoveryLog` (a normal session must leave no file), and for the same reason: this ships to everyone.

### Phase 2 census — RUN 2026-08-27. Music and effects ARE cleanly separable.

26-minute session, **6,363 buffers**. Two independent signals agree on every single one:

| class | count | rate | flags | duration |
|---|---|---|---|---|
| **streams (BGM)** | 24 | 44100 Hz, 2ch (3 were 1ch) | `0x18188`, **`GETCURRENTPOSITION2` set** | exactly 2.000 s or 4.000 s |
| **effects** | 6,338 | 22050 Hz, 1ch | `0x8188` / `0x8198`, no `GETCURRENTPOSITION2` | 94 distinct irregular lengths |

**`DSBCAPS_GETCURRENTPOSITION2` (`0x10000`) is the discriminator** — set on all 24 streams and none of
the 6,338 effects; sample rate agrees perfectly. Exact round durations are the giveaway that the
44.1 kHz buffers are rolling windows refilled by ADX, while effect buffers hold whole sounds. Streams
appear in 2.000 s + 4.000 s pairs seconds apart, at plausible BGM-change moments.

⚠ **Two things the pre-build plan got wrong, both now measured:**
- **`DSBCAPS_STATIC` is never used — not on one buffer.** The "streaming vs static flags will separate
  them" guess was right in conclusion, wrong in mechanism. Use `GETCURRENTPOSITION2`.
- **The game already requests `DSBCAPS_CTRLVOLUME` on every buffer** (6,362 of 6,362; only the primary
  buffer lacks it, and it has no format either). The planned "the proxy must ADD `CTRLVOLUME` at
  creation or `SetVolume` fails" step is **unnecessary** — no descriptor rewriting in phase 3.

**Phase 3 sizing, from the same data:** the client creates a **new buffer per playback** rather than
reusing — 1,878 separate buffers for one 0.239 s sound — averaging ~4 per second. So volume has to be
applied at creation, not only when a slider moves, and the per-creation path must stay cheap.
`CTRL3D` further splits effects into 2,412 positional / 3,926 non-positional, if a third slider is ever
wanted (probably not worth it).

Still live from the plan: **DirectSound volume is hundredths of a dB, not linear** — a 0–100% slider
needs `2000 * log10(fraction)` or 50% sounds nearly silent — and the game's own `SetVolume` calls must
be **combined** with our scalar, not overwritten, or in-game fades break.

### ⛔ The volume hotkey dead end — built, never worked, SHELVED 2026-08-27

An in-game hotkey to change master volume was built and abandoned after three failed in-game tests.
The launcher sliders shipped instead and are considered sufficient. **Code preserved on the
`spike/volume-hotkey` branch** — start there, not from scratch.

**What is known, and it is worth knowing before trying again:**

- **Never bind a function key.** F1–F12 are already bound: Blue Burst has a per-player *"Function key
  setting"* choosing whether they drive **menu shortcuts** or **chat shortcuts** (newserv documents
  the bit at `src/SaveFileFormats.hh:586`). They are in use either way. This was the first attempt's
  default and it was wrong.
- **Polling cannot work in principle.** `GetAsyncKeyState` observes but does not consume, so the game
  receives the key too and does both things at once. Any polled binding collides with whatever the
  client already does with that key.
- **A `WH_KEYBOARD_LL` hook installs fine and appears to receive nothing.** `SetWindowsHookExW`
  succeeds (logged), the thread pumps messages, the bindings are correct in the log — and no press
  ever produced a volume change, with a Windows menu beep indicating the key reached `DefWindowProc`
  instead of being swallowed.
- ⚠ **The instrumentation had a blind spot, and this is the single most useful thing to fix first.**
  The diagnostic only logged keys that were *not* bound, so pressing the **bound** key produced no
  line whether or not the callback ran. That leaves two very different hypotheses untested and
  indistinguishable: the callback never fires at all, versus it fires and then fails its own
  `GameHasFocus()` / `ModifierHeld()` checks. **Instrument the bound-key path before changing
  anything else.**
- Untested throughout: the **controller chord** (`BACK` + D-pad) shares none of the keyboard
  machinery, so trying it alone would isolate hook problems from everything else in one step.

The ask: **a volume control for players.** There is none anywhere today — not in game, not in the setup
tool. What the client actually has:

- **`option.exe` → "PSOBB SOUND" page** (`TSoundOption` / `PsoSoundOptionFrm`, Delphi VCL form) offers only
  switches: `Sound ON/OFF`, `3D Sound`, `Sound Quality`, `Global Focus Sound`. It persists to
  `HKCU\Software\SonicTeam\PSOBB`: **`SOUNDCTRL`** = 3 dwords (ours: `01 01 01`) + **`FOCUS_SOUND`** dword.
  **No level value exists in the registry**, so there is nothing to write from outside the process.
- **BGM = CRI ADX, statically linked.** Only `adx_logo` survives as a string — no exported `adxt_*` names to
  hook by symbol. Music streams from `slbgm_*.afs` / `.ogg`; failure path is `can't create ADXT-BGM #%d`.
- **Everything reaches the OS through DirectSound**, and `psobb.exe` imports **exactly one** function from
  `dsound.dll`: **ordinal #1 = `DirectSoundCreate`**. Both the ADX music stream and the effect buffers are
  created off that one device.

| What | Address | Confidence | Source |
|---|---|---|---|
| `dsound.dll` **IAT slot** — the pointer the game calls `DirectSoundCreate` through | `0x008F8068` (`.data`) | **Confirmed** | `tools/pe_exports.py imports PsoBB.exe dsound.dll` |
| `dsound.dll` import descriptor / OFT | `0x00B5E028` / `0x00B5E180` (`.idata`) | **Confirmed** | Same parse — the import directory itself |
| `"dsound.dll"` module-name string | `0x00B5E734` (`.idata`) | **Confirmed** | ⚠ CORRECTED 2026-08-25. This address was previously listed here as "the dsound import"; it is only the NAME STRING. Patching it does nothing at runtime — an IAT hook must target `0x008F8068`. |
| `"Vol=Opt"` (ADX volume parameter string) | `0x0097A400` (`.data`) | **Confirmed** | String scan; the reference site is NOT yet located |
| `"can't create ADXT-BGM #%d"` | `0x009893D4` (`.data`) | **Confirmed** | String scan — anchor into the BGM creation path |
| `"SOUNDCTRL"` / `"FOCUS_SOUND"` registry key names | `0x009007E0` / `0x009007EC` (`.data`) | **Confirmed** | String scan — anchor into the settings load |

**Best route if we build it — proxy `dsound.dll`, no game RE at all.** The single `DirectSoundCreate`
import means a proxy DLL in the game folder owns the whole audio path, and this client already establishes
the pattern three times over (`d3d8.dll` = our wrapper + ASI loader, `dinput.dll`/`dinput8.dll` = Xidi).
Wrap `IDirectSound::CreateSoundBuffer` and scale each buffer's volume. **Music vs effects may separate for
free**: ADX streams its BGM into a streaming buffer while effects are static one-shots, so the buffer flags
plausibly tell them apart — that is the one thing to verify before promising BGM/SE sliders, and it is
cheap to check by logging buffer descriptors for one session.

**Cheaper fallback — master volume only, ~150 lines:** an ASI that calls WASAPI `ISimpleAudioVolume` on its
own process session, driven by a hotkey plus an XInput chord (pad-only players cannot type, the same trap
that makes `$bank` keyboard-only), persisted to the registry. Zero RE. Unknown: whether Wine/Proton honours
per-session volume for the Steam Deck build — check before shipping it there.

**Not worth it: sliders inside the game's own options menu.** The in-game menu is not extensible without
serious RE, and the win over a hotkey is small.

**Free workaround available right now:** the Windows Volume Mixer sets a per-app level for `psobb.exe` and
remembers it; the client runs windowed, so it is one alt-tab away. Deck players have hardware volume keys.

## Device loss is FATAL by design — CONFIRMED 2026-08-20

The client has **no device-recovery path at all**. It checks the HRESULT from `Present` and, on
`D3DERR_DEVICELOST`, puts up a Japanese message box and quits. Found while testing an exclusive-
fullscreen mode, where alt-tab loses the device every time; it killed the game on the first alt-tab.

The message (Shift-JIS at `0x0098AB80`, caption at `0x0098AC00`) reads:

> 画面のプロパティが変更された為、PsoBB.exeを終了します。ゲーム中は、画面のプロパティを変更しないでください。
> *"The display properties were changed, so PsoBB.exe will exit. Do not change the display properties while in-game."*

The check, inside the present path:

```
0x0083AE29  ff 51 3c            call dword ptr [ecx+0x3c]   ; IDirect3DDevice8::Present (vtable +0x3C)
0x0083AE2C  8b 15 f0 be ac 00   mov edx, [0x00ACBEF0]       ; gate: only check once rendering is up
0x0083AE32  85 d2 / 74 11       test edx,edx / je 0x0083AE47
0x0083AE36  8b 15 c4 96 ad 00   mov edx, [0x00AD96C4]       ; latch: has the dialog already shown?
0x0083AE3C  85 d2 / 75 07       test edx,edx / jne 0x0083AE47
0x0083AE40  3d 68 08 76 88      cmp eax, 0x88760868         ; D3DERR_DEVICELOST
0x0083AE45  74 27               je  0x0083AE6E              ; -> latch, MessageBoxA, quit

0x0083AE6E  c7 05 c4 96 ad 00 01 00 00 00   mov [0x00AD96C4], 1
            push 0 / push 0x0098AC00 / push 0x0098AB80 / push [0x00ACBED8]  ; hWnd
            call [0x008F8348]           ; MessageBoxA
            call 0x007A62DC             ; teardown
```

**Consequences.** Exclusive fullscreen cannot work as a plain present-parameters change: the mode
switch on alt-tab loses the device and the client kills itself before anything can recover. It also
means *any* genuine device loss ends the session today — a driver TDR, an RDP connect, a resolution
change on the desktop — even in borderless, which is presumably why this message exists at all.

**RESOLVED 2026-08-21: exclusive fullscreen is not achievable on this client. Do not re-walk it.**
The wrapper CAN hide the loss -- `Present` returns `D3D_OK` and the client keeps running, music and
all -- but hiding it is not enough, because the device then has to be reset and it never becomes
resettable. Measured on real hardware over 960 polls across 40 seconds:

```
poll   1: coop=DEVICELOST foreground=0 active=0 iconic=1   <- Windows minimised it, expected
poll 121: coop=DEVICELOST foreground=1 active=1 iconic=0   <- restored AND foreground...
poll 241: coop=DEVICELOST foreground=0 active=1 iconic=0   <- ...lost foreground, never regained
  ... unchanged to poll 961
```

`TestCooperativeLevel` never returned `D3DERR_DEVICENOTRESET`, including at poll 121 when the window
was restored, active and foreground -- so the reset that recovery depends on could never run. The
symptom is a black screen with working audio, not a crash. Behind that wall sits a second one: the
client had made **607 `D3DPOOL_DEFAULT` creations**, and a reset fails while any are alive. Only the
client could release them, and it has no code that does.

Aggravating factor worth noting: the test ran at 3840x2160 on a 2560x1440 panel, i.e. a driver
virtual-resolution mode, which are fragile. That does not change the conclusion -- alt-tab loses an
exclusive device regardless -- but the diagnosis was not proven at native resolution.

**What shipped instead.** `DisplayMode=fullscreen` switches the DISPLAY with
`ChangeDisplaySettingsEx` (enumerating the adapter's modes at that size, highest refresh, so the
driver is never handed a mode it lacks) *before* the device is created, then runs the ordinary
borderless popup and windowed device over it. The monitor really changes resolution and the game
fills it; the device is windowed, so there is nothing to lose on alt-tab. Confirmed working
2026-08-21. `CDS_FULLSCREEN` makes Windows restore the desktop even if the client dies.

**Device-loss absorption was kept**, bounded to 10 s: it rides out a driver TDR or an RDP connect,
and if the device does not come back the loss is handed to the client so it exits cleanly instead of
hanging on a black screen. `d3d8_recovery.log` appears next to the game only when a loss happens.

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
| In-game volume control | `dsound.dll` proxy (single import: ordinal #1 `DirectSoundCreate`) | No game RE needed — wrap `CreateSoundBuffer` and scale per buffer. Verify the streaming-vs-static split before promising separate BGM/SE. See the sound survey above. |
| Post-quest exit in One Person | — | **No anchor yet.** Confirmed the client sends `0x98` Leave game ~2s after the quest's success handler `ret`s; the trigger is in the client and unlocated. Quest scripts and server are ruled out. |
| Per-class technique power boosts | — | **No anchor yet**, and static scanning is exhausted — see below. Needs a live debugger breakpoint on damage application. |

## Per-class technique boosts — searched statically, NOT found (2026-08-30)

The question: Force classes are reported to boost particular technique families (FOnewm and the
Ra-line). Where does that rule live?

**Settled first, so it is not re-argued:** technique *level caps* are server-side, in the ItemPMT
(`MaxTechLevels`, 19 techs × 12 classes, stored as level−1). **All four Forces are identical on all 19
techniques**, so caps are NOT the mechanic. Power is separate data, and it was not found.

**Eliminated — do not re-run these:**

| Hypothesis | Result |
|---|---|
| In a server table | No. Every ItemPMT key checked; `TechBoosts` (44 entries) is *item* data reached by `tech_boost_entry_index`. Entry 14 is `Rafoie/Rabarta/Razonde +20%` — looks like class data, is not. |
| In a client data file | No. The client ships no ItemPMT (BB receives it from the server), and none of the 46 `.prs`/`.rel` files is a class/tech table. |
| 12×19 or 19×12 byte matrix in `psobb.exe` | No. Both orientations, *without* assuming class order, requiring exactly four all-zero rows (the androids). Only sparse-bitmap noise. |
| 12-byte per-class index into `TechBoosts` | No. 6 candidates, all incoherent; none puts FOnewm on the Ra entry. |
| 12 consecutive float32, one per class | No. Only denormals and zeros. |
| Boost-shaped float constant referenced near a class compare | 208 sites, none convincing. |
| Region dispatching on ≥3 distinct Force ids | 56 regions. Best was `0x0049B812` (compares 6, 7, 8, 0xA within 32 bytes) — but it groups **(6,9)** against **(7,8,10,11)**, which is gender/model-shaped, not Force-shaped. A false positive from class-sized constants. |

**Conclusion: almost certainly hardcoded in client *code*, not stored as data.** Direct precedent —
newserv's `Items.cc` on mag evolution: *"Sega really did just hardcode all these rules into the client.
There is no data file describing these evolutions."*

**What would actually crack it:** a debugger breakpoint on technique damage application — cast the same
technique at the same level from two different Forces and walk back to the divergence. Static scanning
cannot substitute here, because there is no anchor into the technique damage path: the only technique
strings in the binary are sound-effect names (`SE_TECH_*`), reached by sound *id*, not by string address.

**Tooling built, reusable:** full linear disassembly of `.text` with resync — 1,570,200 instructions.
⚠ **capstone's `disasm()` stops dead at the first undecodable byte.** A plain call covered only ~8% of
`.text` (128k instructions) and silently looked like it had worked — the failure mode is a *quiet
partial answer*, not an error. Always resync:

```python
def disasm_all(buf, base):
    pos = 0
    while pos < len(buf):
        last = pos
        for ins in md.disasm(buf[pos:], base + pos):
            yield ins
            last = ins.address - base + ins.size
        pos = last + 1 if last <= pos else last
```

**Incidental find, real and possibly useful:** something class-shaped is read from `[esi+0x37C]` on the
object handled around `0x0049B7D0`, with `[esi+0x43C]` and `[esi+0x484]` acting as two booleans that
select between ids 7/10 and 8/11. Unidentified, but it is a genuine class-dependent site if a future
hunt needs a starting point.

### ⭐ The hunt reopened — an external address map, VALIDATED against our binary (2026-08-30)

`https://gitlab.psobb.io/psobb/original-psobb-client-source` — despite the name it is **not leaked Sega
source**, it is a community **Ghidra decompilation of the same `psobb.exe` we target**. Its
`notes/_wiki_combat_formulas.md` gave two things the static hunt never had.

**1. The formula shape — and it says my search was looking for the wrong thing.** Attack-technique damage
carries four boost terms: `CB/WB/FB/MB` = Class/Weapon/Frame/Mag boost percentages, **added, not
multiplied**. Every scan above hunted a *multiplier*. A summed percentage is a different search.
(The doc gives no CB values, class mapping or address — it defers those as open work. Map, not answer.)

**2. An address map that CHECKS OUT on our build.** Do not take it on trust — it was verified:

| Check | Result |
|---|---|
| `freeze_entity` @ `0x00778CC0` writes `0x96` (150 frames) | ✅ confirmed — `mov [esi+0x270], 0x96` at `0x00778E18` |
| …and to entity offset `+0x270`, which the doc independently claims is `freeze_duration` | ✅ same instruction |
| 10 claimed function addresses land on real prologues | ✅ 9/10 |

⚠ **A naive check nearly rejected a correct map.** I first looked for the literal `0x1e` in
`get_base_beat_time_in_frames` and for `0x96` in only the first 18 instructions of `freeze_entity`, and
scored both as failures. The compiler strength-reduces `×30` into a shift/add chain so `0x1e` never
appears, and the `0x96` write is ~90 instructions in. Design the falsification test around what a
compiler actually emits.

**Entity struct offsets now known** (doc's, confirmed where checked): `+0x270` freeze_duration,
`+0x2BE` MST (also MaxTP), **`+0x336` current TP** (found here: `0x00777368` clamps `+0x336` to `+0x2BE`),
`+0x2D0` EVP, `+0x2F6` EFR, `+0x2FA` EIC, `+0x2FE` ELT, `+0xBF` Megid resist, `+0x8A4` attack-type enum.

**Useful named functions:** `entity_hit_by_tech` `0x00773AFC` (takes damage as a float *argument* — no MST
read anywhere inside it, so the magnitude is computed caster-side, not here), `EntityHitEntity`
`0x00773350`, `ProcessWeaponHitboxDamage` `0x005E641C`.

**⭐ WHERE TO RESUME.** At `0x0077661C`–`0x00776655` the client computes, in effect,
`(X × MST) × 0.01` — `movsx edx, [ebx+0x2BE]` (MST), `imul esi, edx`, `fild`, `fmul [0x978214]` where
`[0x978214] = 0.01`. **The `× 0.01` is the tell for an added-percentage term**, and `X` arrives from
`[esp+0x2c]`. **Trace where `[esp+0x2c]` is filled** — that is the accumulator CB/WB/FB/MB sum into, and
the class term should be visible at its source. That is the next move, and it is a well-posed one.

⚖ **Provenance:** use that repo to navigate and to form hypotheses only. **No licence is stated, so copy
nothing from it into this repo** — confirm every claim against our own binary (as above) or in game.

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
