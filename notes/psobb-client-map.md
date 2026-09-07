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

## Create-game dialog — CONFIRMED LIVE (2026-09-07)

Everything needed for **remembered game-creation settings**. Found statically; no running client was
required, which is why this landed in one pass rather than the session of live hunting that was budgeted.

**The object.** `0x38` bytes, allocated in the main arena, built by the constructor at **`0x00733F20`**,
and stored in the global pointer **`0x00AAB24C`**. Three construction sites, one per episode entry in the
menu — the third is gated on the Episode 4 capability flag.

| Offset | Field | How it was confirmed **in our binary** |
|---|---|---|
| `+0x1D` | flags — bit0 = confirmed/OK, bit1 = cancelled | `test al, 1` at `0x0079ADCF` |
| `+0x1E` | **mode** — 0 normal, 1 challenge, 2 battle, 3 one person | compared against `2`, `1`, `3` from `0x0079ADFD` on |
| `+0x1F` | **difficulty** — 0..3 | accessor `0x007346EC` |
| `+0x20` | episode (u32) | written from the constructor's 3rd argument at `0x00733F7C` |
| `+0x24` | name (`wchar_t*`) | accessor `0x007346CC` — `mov eax,[ecx+0x24]`, empty-string fallback `0x008F8818` |
| `+0x28` | password (`wchar_t*`) | accessor `0x007346DC` — `mov eax,[ecx+0x28]`, same fallback |

**Accessors** (all three are real prologues at these exact addresses):

```
0x007346CC  mov eax,[ecx+0x24] / test / je -> mov eax,0x8F8818 / ret      ; name
0x007346DC  mov eax,[ecx+0x28] / test / je -> mov eax,0x8F8818 / ret      ; password
0x007346EC  movsx eax,byte [ecx+0x1E] / cmp eax,1 / je -> xor eax,eax     ; difficulty,
            movsx eax,byte [ecx+0x1F] / ret                               ; forced 0 in challenge
```

That last one is the strongest single piece of evidence: it encodes a **game rule** (challenge mode has
no difficulty) that nobody would produce by coincidence from a wrong address.

**The confirm handler is `0x0079ADB8`.** It loads `[0x00AAB24C]`, tests `+0x1D & 1`, then calls the three
accessors and reads `+0x1E` to derive battle/challenge/solo — exactly the shape of
`C_CreateGame_BB_C1`.

**⭐ Why the settings are forgotten — it is two instructions.** The constructor zeroes them:

```
0x00733F98  88 42 1E   mov byte ptr [edx+0x1E], al   ; mode       = 0   (al is 0 here)
0x00733F9B  33 C9      xor ecx, ecx
0x00733F9D  88 42 1F   mov byte ptr [edx+0x1F], al   ; difficulty = 0
0x00733FA0  8B 42 20   mov eax, [edx+0x20]           ; eax is reloaded immediately after
```

**Episode does not need remembering** — it comes from the constructor argument, i.e. from *which* menu
entry was chosen, so it is re-stated by the player every time. The feature is preserving those two bytes.

### How it was found — the anchor worked exactly as written down

`C_CreateGame_BB_C1` is `0x50` bytes, so the framed packet is `0x58` with command `0xC1`. The sender is
identifiable by that fingerprint alone: a local `= 0x58`, a local `= 0xC1`, two `wcsncpy(dst, src, 0x10)`
calls, then five bytes at payload offsets `0x48`..`0x4C`. It has exactly **one** caller, and that caller
is the dialog handler. Anchoring on a known wire format beat searching for the UI, as predicted.

⚠ **Two things that would have cost time.** Searching the binary for the command constant found nothing,
because the header is built from a `ushort` size and a **byte** command — not the imm16 or imm32 stores
that were searched for first (this compiler emits only 27 `66 c7` imm16 stores in all 5 MB of `.text`).
And in the psobb.io decompilation the sender is auto-named **`send_packet0x1c`**, which is simply wrong —
it sends `0xC1`. Both are reminders that the dump is a hypothesis: every address above was re-derived
from our own `psobb.exe`.

### ⭐ THREE display paths, and they do NOT agree — this is what the work actually cost

Restoring the two fields is trivial. Making the dialog *agree with itself* took four rounds, because
three different things show mode and only one of them reads the field:

| What | Source | Behaviour |
|---|---|---|
| Popup list cursor | reads `+0x1E` at `0x007345F6` | follows the field ✅ |
| **Play Mode summary line** | **`xor eax,eax` at `0x00733FE0`** | **hardcoded to "Normal" — never reads the field** ⛔ |
| Difficulty summary line | reads `+0x1F` | follows the field ✅ |

`0x00733FE0` is the one that matters:

```
0x00733FDF  push edi
0x00733FE0  xor eax, eax        ; mode, HARDCODED
0x00733FE2  call 0x00734079     ; get_string(0x139 + eax) -> "Normal"
0x00733FEB  push 2              ; line 2 = Play Mode
0x00733FF1  call 0x00738A40     ; set the summary text
```

In the stock client this is invisible: the field is set to 0 immediately after, so the label happens to
be right. Restore a non-zero mode and the dialog says Normal while creating something else.

⚠ **The lesson, which generalises past this dialog:** a label that looks stale may never have read the
value at all. Two attempts were spent on ordering — writing earlier, then writing the text ourselves —
before checking what actually fed the label. **Find the label's source before assuming it is stale.**
The clue that cracked it was Kevin's: *selecting Battle by hand updates it* — i.e. the commit path
passes a real value where the constructor passes a literal.

Also: the constructor's two zeroing stores are the ONLY initialisation `+0x1E`/`+0x1F` get. The object
comes from a plain allocation that does not zero, so a hook that writes them *conditionally* leaves heap
garbage behind — which the difficulty label renders as a literal `%s`. Replace an unconditional store
only with another unconditional store.

### Four patch sites, all guarded on opcode AND resolved call target

| Address | Original | Why |
|---|---|---|
| `0x00733F7F` | `call 0x00734000` | write the fields before the popup list is built |
| `0x00733F98` | the two zeroing stores | write them again after the constructor clears them |
| `0x00733FE0` | `xor eax,eax` + call | supply the real mode to the summary label |
| `0x0079ADD7` | `call 0x007346CC` | capture the values on confirm |

### Text fields — mapped for the Party Name / Password follow-up

`+0x24`/`+0x28` are **pointers**, not inline buffers, and the dialog does not own them: an edit widget
is created on demand at `0x00734216` / `0x00734330`, stored at `+0x34`, and destroyed after. `get_text`
(`0x0078ED4C`) **mallocs** its return via `0x008581C5` and that pointer is what gets stored.

* **`widget_set_text` = `0x0078ED74`** — `__thiscall(ecx = widget, const wchar_t*)`, callee-cleaned.
  Forwards to `0x007310EC`, which wcslens, **clamps to 127 chars**, allocates `len*2+2` and copies.
* Both creation sites end `mov eax,[ebp-0x14]; mov [eax+0x34], edx` — 6 bytes, enough for a call+nop.
* ⛔ **The destructor FREES both pointers** — `0x00733E64` does `free([+0x28])` then `free([+0x24])` via
  `0x00857E98`. So a restored name/password must be allocated with the client's own allocator
  (**`0x008581C5`**, the same malloc `get_text` uses) and the pointer stored there. Pointing `+0x24`/
  `+0x28` at a static buffer hands `free()` a non-heap pointer — heap corruption that would surface
  somewhere unrelated, long after the cause. (`free(NULL)` is why the constructor's NULL init is safe.)
* Class vtable is at **`0x00B408D0`**: `[0]` destructor `0x00733E40` (body `0x00733E64`),
  `[1]` update `0x007340A4` — which confirms the menu-building/commit function is virtual method 1.
* ✅ **SETTLED in game 2026-09-07** by a temporary probe (`feat/party-name-probe`, not for merge):

  | Creation site | maxlen | Field | Stores to |
  |---|---|---|---|
  | **B** `0x0073432D` | `0x0E` (14) | **Party Name** | `+0x24` |
  | **A** `0x00734213` | `0x10` (16) | **Password** | `+0x28` |

  Address proximity suggested the opposite and was wrong; the maxlen argument was right. **14 + the
  2-char `	E` marker = the 16-wchar wire field**, agreeing with `C_CreateGame_BB_C1.name` and with the
  32-byte Ephinea blob — so the password (16, no marker) is the one WITHOUT room for a marker.
  ⚠ Note the arena reuses the widget slot, so both widgets had the same address: the probe was
  conclusive because of create→read ORDERING, not pointer identity. If it is ever re-run, drive the two
  fields in a known order.
* The stored value must include the **`	E` language marker** — see [[corellia-qol-requests]] for the
  Ephinea observation that established this.

### Calling into the client (conventions read off its own call sites, never assumed)

```
get_string      0x0079317C  __cdecl,    caller cleans 1 arg,  returns string in eax
mode label      0x00734079  arg in AL,  returns get_string(0x139 + al)
difficulty      0x00734091  arg in AL,  returns get_string(0x13D + al)
set_line_text   0x00738A40  __thiscall(ecx = menu widget at +0x2C), args (string, line), CALLEE cleans
widget_set_text 0x0078ED74  __thiscall(ecx = edit widget), arg (wchar_t*),               CALLEE cleans
```

The absence of an `add esp,N` after the game's own calls is what identifies callee cleanup.

### Still unverified

Whether restoring a difficulty the character has not unlocked is re-clamped by the dialog. The level
gate was never located. `RESTORE_DIFFICULTY` in the plugin exists to turn that half off.

## Camera — MAPPED AND HOOKED (2026-09-07)

The client has a complete third-person follow camera. Everything below was **navigated to** with the
psobb.io decompilation and then **re-derived from our own `psobb.exe`** — see the provenance rule at
the end of this file.

### The state struct

Global pointer **`0x00A48A54`** → `0x1D4` bytes (`allocate_in_main_arena(0x1d4)`).

| Offset | Field |
|---|---|
| `+0x94`  | copy of `y_rotation`, written at the end of each update |
| `+0x178` | `camera_source` — current eye |
| `+0x184` | `camera_target` — current look-at |
| `+0x190` / `+0x194` / `+0x198` | x / **y** / z rotation, PSO angle units (`0x10000` = 360°) |
| `+0x19C` | FOV, passed to the projection setup |
| `+0x1A0` | **`camera_desired_source`** |
| `+0x1AC` | **`camera_desired_target`** |
| `+0x1B8` / `+0x1BC` | source / target lerp factors |
| `+0x1C0` / `+0x1C4` | shake frame counter / scaling |
| `+0x1C8` | `desired_source_copy`, the "from" end of the collision ray |

⚠ **`y_rotation` (+0x194) is an OUTPUT.** `apply_camera_angle_rotations` (`0x004D6448`) derives it
from the source→target vector every frame. Writing it does nothing useful — it is a cached yaw that
the rest of the engine *consumes*: every billboarded sprite (`matrix_rotation_y`) and the **minimap
heading**, which is `0x8000 - y_rotation`.

➡ That last one is a free on-screen oracle: **if the minimap needle turns, the real camera moved.**

### Per-frame update, and the one hook site

`UpdateDefaultNPCCameraState` @ `0x004D3ABC` (`__fastcall`, state in `ecx`, kept in `esi`):

```
004D3B10  8B CD              mov ecx, ebp
004D3B12  E8 DD E4 FF FF     call 0x004D1FF4   ; auto-camera writes desired_source/target
004D3B17                     <-- hook point: points are fresh, nothing has read them yet
          ...  0x004D3BA8 collision ray  ->  0x004D3C98 / 0x004D3D14 / 0x004D3E90 lerp or snap
          ->  0x004D3D6C source-vs-geometry  ->  0x004D3DF4 shake  ->  0x0082EBC8 projection
```

`0x004D1FF4` writes `[0x00A48A54]+0x1A0..+0x1B4` directly — which is what pins both the global and the
desired-point offsets, independent of the decompilation.

**Therefore the way to drive this camera is to rotate `camera_desired_source` about
`camera_desired_target` at `0x004D3B17`.** Smoothing, wall collision, `y_rotation`, the minimap,
billboarding and camera-relative movement/lock-on are all *derived* from those two points and follow
for free. Built as `psobb_rightstickcamera`.

⛔ **Do not build a camera by rotating the view matrix in the d3d8 wrapper.** It is visual only:
movement is camera-relative and lock-on is defined in terms of camera direction, so the game would
still believe the camera never moved and the character would walk somewhere other than where the
player is looking. This was the plan of record until the struct above was found; it is now a dead end.

Y is the vertical axis: the degenerate case in the view-matrix path (`0x004D2158`) is "X and Z deltas
are both zero", which it treats as looking straight up or down and nudges out of.

### Input

`g_joyState` — a whole `DIJOYSTATE2` (`0x110` bytes) @ **`0x00ADCC80`**. `PollJoystickState`
(`0x00842460`) calls `GetDeviceState(0x110, buf)` on `g_pDevice[0]` (`0x00ADC9BC`) and `rep movsd`s
the result there, so reading it costs nothing and cannot disagree with what the game itself saw.
The client's own frame-delta threshold is `4096.0f` @ `0x0098B350` (12.5% of a signed-16-bit axis).

⚠ The client calls `SetCooperativeLevel(DISCL_EXCLUSIVE | DISCL_BACKGROUND)` — it owns the pad even
unfocused, which is why other gamepad-aware apps lose it while PSO runs. Read `g_joyState`; do not
open a second device.

### Still unverified

Two things that reading the binary cannot settle, both left configurable rather than compiled in:

1. **Which axes carry the right stick.** The device is Xidi's virtual pad
   (`Mapper Type = StandardGamepad`), not the physical controller. The psobb.io input notes say
   `lZ` (+0x08) is the right stick's vertical and `lRz` (+0x14) its horizontal; that is the default.
2. **Which `g_GenericMenuSubSelection` (`0x00A489FC`) bits mean "leave the camera alone."** `0x0C`
   selects the snap branch inside `0x004D1FF4`; `0x820` is the mask the update itself tests to skip
   collision and smoothing. Suppressing on `0x82C` is the conservative reading, not an observation.

A diagnostic build of the plugin logs all six axes and the live mask, which answers both in one
session.

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
frame, then look for the address whose value tracks it. Never built, and **not needed for the camera** —
the struct was found by reading instead (see "Camera" above). Still the right technique for the next
continuously-changing value that has no name.

**Re-check an old plan against assets acquired after it was written.** The right-stick camera sat on the
board for weeks as "highest risk, may stall", with a bounded memory-scan spike defined to decide whether
to abandon it. The plan predated the psobb.io decompilation. Reading that instead answered the whole
question in one session, and the spike was never run.

**Anchoring on a known wire format.** For the create-game dialog, the C1 packet's `0x50`-byte layout is
known exactly, so the code that *builds* C1 is findable, and the struct holding the dialog's live
selections is upstream of it. Better than searching for the UI directly.

## Open targets

| Goal | Anchor | Notes |
|---|---|---|
| Remembered game-creation settings | **DONE — mode + difficulty, confirmed in game 2026-09-07** (`psobb_gamesettings`). See the create-game dialog section for the four patch sites and the three-display-paths trap. Party Name / Password are mapped but not built. | Persist per install directory rather than per registry leaf — each instance is its own install, so a file beside the client is correctly scoped without having to detect which leaf this build owns. |
| Inventory past 30 | `PlayerInventory` at offset 0 | Protocol side understood; the client-side wall is the 30-slot inventory UI, which has no paging. |
| In-game volume control | `dsound.dll` proxy (single import: ordinal #1 `DirectSoundCreate`) | No game RE needed — wrap `CreateSoundBuffer` and scale per buffer. Verify the streaming-vs-static split before promising separate BGM/SE. See the sound survey above. |
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
