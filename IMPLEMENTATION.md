# Implementation Notes

*This document was written by the AI agent that wrote the code, and describes
v0.6.5.*

It covers what the patch changes, why each change is made where it is, and how
the addresses and constants were derived. It is aimed at anyone auditing the
source or picking the work up later. Where something is inferred rather than
measured, it says so.

---

## Contents

- [The problem](#the-problem)
- [How the patch attaches](#how-the-patch-attaches)
- [The XACT audio model](#the-xact-audio-model)
- [Patch 1 — Speech category volume](#patch-1--speech-category-volume)
- [Patch 2 — the speech soundbank](#patch-2--the-speech-soundbank)
- [Patch 3 — subtitles wait for the voice line](#patch-3--subtitles-wait-for-the-voice-line)
- [Patch 4 — ambience and Special Edition sound effects](#patch-4--ambience-and-special-edition-sound-effects)
- [Patch 5 — per-line dialogue skip](#patch-5--per-line-dialogue-skip)
- [Patch 6 — mouse wheel scrolling](#patch-6--mouse-wheel-scrolling)
- [A bug in the game](#a-bug-in-the-game)
- [The soundbank format](#the-soundbank-format)
- [Failing safe](#failing-safe)
- [How things were verified](#how-things-were-verified)
- [Known limitations](#known-limitations)

---

## The problem

Classic mode silences the voice acting, and it does so in **two independent
places**. Undoing either one alone achieves nothing:

1. **In the audio data.** Every cue in `SpeechCues.xsb` carries an XACT runtime
   parameter curve that returns −96 dB whenever the game's `NewVersion` variable
   is 0 — which is exactly what classic mode sets it to.
2. **In the code.** `MISE.exe` re-sets the Speech category volume to
   `(1 − classicBlend) × voiceVolume` every frame, and `classicBlend` is 1 in
   classic mode.

Everything else the patch does was added afterwards, once the voice was audible
and the gaps became obvious.

---

## How the patch attaches

The patch is a **proxy DLL**, not an injector. `MISE.exe` imports exactly one
function from `d3d9.dll` — `Direct3DCreate9` — and `d3d9.dll` is not on the
KnownDLLs list, so a `d3d9.dll` sitting next to the executable is loaded in
preference to the system one. The patch loads the real
`%WINDIR%\System32\d3d9.dll` and forwards that single export to it.

The real library is loaded on **first use** rather than from `DllMain`, because
calling `LoadLibrary` under the loader lock is a documented deadlock risk. The
DLL entry point does only three things: disable thread notifications, build the
log path, and start one worker thread.

### Why the worker thread waits

`MISE.exe` is wrapped in Steam's **CEG** copy protection. The `.text` section is
encrypted on disk (entropy 7.998) and is decrypted at runtime by a stub inside
the executable's entry point — which runs *after* every statically imported
DLL's entry point. So at the moment the patch is loaded, the code it wants to
modify does not exist yet in readable form.

Rather than guess at a delay, the worker polls: for each patch site it checks
that the page is committed and readable, compares the expected byte sequence,
and only writes when it matches. In practice every code patch lands about 14 ms
in — before the game has created its own threads or loaded any soundbank — and
the thread exits as soon as all of them have applied.

This has a useful side effect. Because each site is signature-checked before it
is written, an unexpected build is **left alone rather than corrupted**.

### Why absolute addresses are viable

`MISE.exe` has `DllCharacteristics = 0x0000` — **ASLR is off**, and the binary
carries no relocations. The image always loads at `0x400000`, so the addresses
below are stable across runs and machines. This is unusual for modern software
and is the reason a patch of this shape is practical at all.

---

## The XACT audio model

The game's audio is XACT3. Three things matter here.

**Categories.** Six, defined in `Monkey1Audio.xgs`, and flat — every one is a
child of `Global`:

```
0 Global   1 Default   2 Music   3 Speech   4 SFX   5 Ambience
```

The game resolves them by name at startup and caches the indices in its audio
manager. Reading those calls is how the mapping below was established, rather
than by parsing the `.xgs` name table (whose order is not the index order):

| call | stored at |
|---|---|
| `GetCategory("Ambience")` | `[audioMgr+2A0h]` |
| `GetCategory("Speech")` | `[audioMgr+2A2h]` |
| `GetCategory("Music")` | `[audioMgr+2A4h]` |
| `GetCategory("SFX")` | `[audioMgr+2A6h]` |

**RPC presets.** A runtime parameter curve maps a variable to a property. Each
sound in a bank names the presets that apply to it, by byte offset into the
`.xgs`. The ones that matter:

| offset | preset | effect |
|---|---|---|
| `0x43D` | RPC22 | `NewVersion → Volume`; **−96 dB when NewVersion is 0**, i.e. in classic mode |
| `0x454` | RPC23 | `NewVersion → Volume`; −96 dB when NewVersion is 1, i.e. in Special Edition mode |
| `0x46B` | RPC24 | ducking |
| `0x499` | RPC26 | `CueVolume → Volume` |

RPC22 and RPC23 are a mirrored pair: they are how the game decides whether you
hear the new content or the original content, without unloading either.

**Soundbanks.** `SpeechCues`, `AmbienceCues`, `SFXCuesNew`, `SFXCuesOriginal`,
`MusicCuesNew`, `MusicCuesOriginal`, `RoomSFXCues`.

**Key addresses used throughout:**

| what | where |
|---|---|
| audio manager | `[0x5B9888]` |
| XACT engine pointer | `[audioMgr+70h]` |
| `IXACT3Engine` vtable | CreateSoundBank = index 9 (`+24h`), GetCategory = 17 (`+44h`), Stop = 18 (`+48h`), SetVolume = 19 (`+4Ch`) |

The vtable indices were cross-checked at two independent call sites — the game
calls CreateSoundBank through `[ecx+24h]` and SetVolume through `[ecx+4Ch]`,
which pins the rest of the layout.

---

## Patch 1 — Speech category volume

**Site `0x441E8B`, 3 bytes.**

The audio update computes the Speech category volume every frame:

```
0x441E84  0F B7 93 A2 02 00 00     movzx edx, [ebx+2A2h]   ; Speech category
0x441E8B  0F 5A C8                 cvtps2pd xmm1, xmm0     ; <-- patched
0x441E8E  F2 0F 10 05 70 C9 4E 00  movsd xmm0, [4EC970h]   ; 1.0
0x441E96  F2 0F 5C C1              subsd xmm0, xmm1        ; 1.0 - classicBlend
```

`xmm0` holds `classicBlend`, which is 1.0 in classic mode, so the result is
zero. Replacing the `cvtps2pd` with `xorps xmm1, xmm1` — the same three bytes —
makes the subtraction `1.0 − 0.0` regardless of mode. `xmm1` is reloaded two
instructions later, so nothing downstream depends on the value that was
discarded.

---

## Patch 2 — the speech soundbank

**Site `0x440FCD`, 5 bytes — a call detour.**

Undoing the code-side mute is not enough: the RPC22 curve attached to every
speech cue still returns −96 dB. That has to be dealt with in the bank data,
which means intercepting the bank as it is created:

```
0x440FCD  8B 41 24   mov eax, [ecx+24h]     ; IXACT3Engine::CreateSoundBank
0x440FD0  FF D0      call eax
```

Those five bytes become a `call` to a naked trampoline that forwards the six
`__stdcall` arguments unchanged — the callee's `ret 18h` still balances — and
hands them to a C function that decides what to do.

### What the hook does

Anything that is not a listed bank passes straight through untouched. For a bank
that *is* listed, the hook allocates a private copy, edits the copy, and gives
XACT the copy. **The game's own buffer is never written to.** The copy is
deliberately never freed, because XACT reads sound data out of it on demand for
the lifetime of the bank rather than parsing it up front.

### Why the curve is swapped rather than removed

Each sound carries an 11-byte RPC block: `u16 dataLen`, `u8 numPresets`, then one
`.xgs` offset per preset. Speech sounds ship `{RPC22, RPC27}`.

The obvious edit — drop RPC22 and set `numPresets` to 1 — is rejected by XACT,
which validates that `dataLen == 3 + 4 × numPresets`. Naming the same preset
twice is also rejected. So RPC22 is **replaced** by RPC26 (`CueVolume → Volume`),
which keeps the block the same size and the presets distinct. `CueVolume` is a
cue instance variable whose default maps to exactly 0 dB on that curve, and
`MISE.exe` never writes it, so the substitution is inert.

RPC22 cannot simply be neutralised everywhere: it is also what mutes the new
music and the new sound effects in classic mode. Only named banks are touched.

### The header word

XACT validates a 16-bit field at header offset 8 and rejects any edited bank
whose value does not match. No standard CRC-16 reproduced it, so the value for
each bank was recovered empirically — by sweeping all 65,536 candidates against
`CreateSoundBank` until one was accepted — and then baked in:

| bank | size | shipped word | patched word | edits |
|---|---|---|---|---|
| `SpeechCues.xsb` | 289,474 | `0x85BE` | `0xB1CC` | 4,547 |
| `AmbienceCues.xsb` | 3,899 | `0xF52E` | `0xECDC` | 30 |
| `SFXCuesNew.xsb` | 13,935 | `0x219E` | `0xA5E6` | 114 |

The hardcoded word is only trusted when the bank's size **and** shipped header
word both match. Otherwise the patch falls back to a cached value, and failing
that leaves the bank alone. The sweep still exists as a recalibration path but is
**opt-in**: it runs only if `%TEMP%\MISEVoiceSweep` exists, because it takes long
enough to look like a hang, and the result is cached so it happens at most once.

---

## Patch 3 — subtitles wait for the voice line

**Site `0x442B36`, 2 bytes.**

With the voice restored, subtitles and audio drifted apart: the text ran on
SCUMM's own timer while the voice played independently, so lines were cut off or
overlapped.

The engine already knows how to do this correctly. When speech starts it records
the talk id in the global at `0x5B996C`, and the SCUMM message tick at
`0x498BE0` refuses to expire a subtitle whose talk id matches that global. The
logic exists and works — it is simply never armed in classic mode, because
storing the id is gated on the classic-mode flag:

```
0x442B2F  80 BF 00 03 00 00 00   cmp byte ptr [edi+300h], 0
0x442B36  75 09                  jne 0x442B41        <-- skips the store
0x442B38  0F BF 45 18            movsx eax, word ptr [ebp+18h]
0x442B3C  A3 6C 99 5B 00         mov [5B996Ch], eax
```

NOPping the `jne` stores the talk id in both modes and the existing hold engages.
Lines with no recorded cue never reach this code, so they keep their normal
timing and cannot hang. In Special Edition mode the flag is already zero, so the
branch was never taken and nothing changes.

This is the pattern the rest of the patch follows where it can: find the
behaviour the engine already implements and stop it being switched off, rather
than reimplement it.

---

## Patch 4 — ambience and Special Edition sound effects

**F9** switches the Special Edition's added sound layer on and off. The two
halves need different mechanisms.

### Ambience — a volume gate

Ambience is its own category with no "Original" counterpart, so unmuting
`AmbienceCues.xsb` cannot double against anything. Its ducking curve is kept, so
ambience still drops under dialogue.

Muting it at runtime is done at **site `0x441F2F`, 7 bytes**, on the copy of the
value that becomes `SetVolume`'s argument — *not* on the value the game stored.
The reason is [a bug in the game](#a-bug-in-the-game), and it is the single most
important detail in this document if you intend to change anything here.

Switching presets in the bank would not work for ambience in any case: a preset
is read when a cue *starts*, and room ambience is one long looping cue, so a
toggle would not reach the instance already playing.

### Sound effects — a preset switch

`SFXCuesNew.xsb` holds 146 cues. **62** of them are re-recordings of cues that
also exist in `SFXCuesOriginal`, which classic mode already plays; unmuting those
would play both copies. The other **84** are content the original game never had
— the chef crying, ghost laughs, menu clicks, and the AdLib-era effects the
SoundBlaster set never carried. Only those are touched.

Working out which is which required mapping every cue to the sound entries it can
reach, which meant [decoding the bank format](#the-soundbank-format). The result
is a fixed table of 114 byte offsets whose dword is RPC22. Exactly one sound
(`0x2C4`) is reachable from both groups — shared by `92_LeChuckPunch` and
`22_sound_SBL_whack` — and is deliberately left muted; the cost is one punch in
the LeChuck fight, against reintroducing a doubled effect.

The bank ships with the game and never changes, so the offsets are baked in
rather than recomputed at runtime, which would mean parsing four tables inside
the DLL. Every offset is checked to actually contain RPC22 before it is written,
so a bank they were not derived from is left completely alone.

Because these are one-shot cues, flipping presets in the live buffer **does**
work: XACT re-reads sound data when a cue starts. F9 writes RPC22 or RPC26 into
those 114 slots. Only the low word differs between the two values, so a single
16-bit store does it — a dword store would be unaligned at most of these offsets
and could be torn by a concurrent read from the mixer.

---

## Patch 5 — per-line dialogue skip

The original SCUMM release let you cut a line short and move to the next. The
Special Edition dropped it: Backspace and Delete are wired to a
whole-conversation skip instead, and nothing reads `.` any more. `,`, `.` and the
right mouse button restore it.

A line is described by two engine globals:

| address | meaning |
|---|---|
| `0x5B996C` | dword — talk id of the voice line playing, 0 for none (set by patch 3) |
| `0x5C2012` | word — the subtitle's own countdown, ticked down every frame |

Zeroing both is **exactly the state the engine reaches when a line ends
normally**, so the script advances through its own path. No script state is
faked, and the next line plays in full. Verified live: with a line playing,
clearing both made the following line start about 0.6 s later, the same as an
unskipped one.

The voice is stopped separately, or it would talk over the next line —
`IXACT3Engine::Stop` on the Speech category, immediately, in the same keypress.

An earlier version ramped the category down over 80 ms first, to avoid clicking
on a waveform cut mid-word. That turned out to be a mistake worth recording: the
stop is *category-wide*, so a deferred stop silences whatever happens to be
playing when it fires, and scripts vary enormously in how soon they reach the
next line — an insult swordfight leaves half a second, an ordinary exchange none
at all. Stopping immediately removes the window rather than narrowing it, and
took a code hook, two globals and the only floating-point code in the file with
it.

The right mouse button is only swallowed when a line was actually skipped, so it
keeps its normal meaning everywhere else.

---

## Patch 6 — mouse wheel scrolling

Long conversations show six lines at a time with arrows to page through them,
and classic mode gives you no way to work them but the mouse.

The game *does* handle `WM_MOUSEWHEEL`, but the handler bails out immediately in
classic mode, and what it drives (`0x427820`) is a Special Edition UI widget with
no connection to the classic list — bypassing the branch in a running game
scrolls nothing.

The classic arrows, though, are ordinary SCUMM verbs, and verbs carry a keyboard
shortcut. Reading the verb slots while a swordfight list was on screen:

| slot | rect (game coords) | key |
|---|---|---|
| dialogue lines 1–6 | x18–219, y145…185 | `1`–`6` |
| up arrow | x0–16, y145–169 | `q` |
| down arrow | x0–16, y172–196 | `a` |

So a wheel notch only has to feed the engine `q` or `a`. That is much better than
synthesising a click on the arrow: no screen-coordinate arithmetic, nothing
resolution-dependent, and — most importantly — the engine matches a key against
**visible** verbs only. When the arrows are not on screen the keystroke does
nothing, so the safety check comes for free and a stray notch can never pick a
dialogue line.

The character is written straight to the engine's pending-key global at
`0x5B98C0` rather than posted as a keystroke, because the game's own key path
derives the character with `MapVirtualKey`, which is keyboard-layout dependent —
`VK_Q` yields `a` on AZERTY, which would scroll the wrong way.

Incidentally, this means **`1`–`6` already select dialogue options and `q`/`a`
already scroll** in unmodified MISE. Neither needs a patch.

---

## A bug in the game

Worth its own section, because it cost several wrong fixes and will mislead
anyone who touches the ambience code.

The audio update computes one volume, hands it to `SetVolume` for the Ambience
category, and then hands **the same stack slot** to `SetVolume` for the SFX
category without recomputing it. Writing `E` for the stack pointer before the
sequence begins:

```
E-4    0x441F14  push ecx                  ; creates the argument slot
E-4    0x441F29  movss [esp+20h], xmm0     ; writes E+1Ch  <- shared slot
E-12   0x441F36  push edx / push eax
E      0x441F3B  call SetVolume            ; stdcall, ret 12 -> esp back to E
E      0x441F76  jb 0x441FC8               ; taken: [audioMgr+0CCh] is -1, limit 0
  --             movss [esp+1Ch], xmm0     ; the SFX volume: SKIPPED
E      0x441FCB  fld [esp+1Ch]             ; reads E+1Ch -- the same dword
```

`[esp+20h]` at `0x441F29` and `[esp+1Ch]` at `0x441FCB` are the same four bytes.
The branch that would recompute the SFX volume is always taken in practice, so
SFX receives whatever the ambience path left behind.

Stock MISE never notices, because ambience and SFX are both 100 and the reuse
changes nothing. **Any patch that lowers the ambience volume lowers the SFX
volume with it**, silencing every sound effect in the game. Earlier versions of
this patch did exactly that, and the symptom was blamed on the soundbanks, on
category routing, and on a volume-conversion helper before the actual cause
turned up.

Hence the rule the current code follows: the value the game *stores* is left
untouched, and only the copy that becomes `SetVolume`'s argument is muted.

---

## The soundbank format

Reverse-engineered here to work out which cues could be safely unmuted. Recorded
because it is reusable and was not trivial to derive.

**Header:**

| offset | field |
|---|---|
| `0x00` | `"SDBK"` |
| `0x08` | `u16` validated header word (see [patch 2](#the-header-word)) |
| `0x13` | `u16` simple cue count |
| `0x15` | `u16` complex cue count |
| `0x19` | `u16` total cues |
| `0x1C` | `u16` sound count |
| `0x1E` | `u16` cue name table length |
| `0x22` | `u32` simple cue table offset |
| `0x26` | `u32` complex cue table offset |
| `0x2A` | `u32` cue name table offset |
| `0x46` | `u32` sound table offset |

**Simple cue — 5 bytes:** `u8 flags`, `u32` **file offset** of the sound entry.

**Complex cue — 15 bytes:** `u8 flags`, `u32 value`. If `flags & 0x04` the value
is a direct sound offset; otherwise it points at a variation table.

**Variation table:** 8-byte header (`u16 count`, `u8 flags`, `u8`,
`u32 0xFFFFFFFF`) followed by `count × 6` bytes of `u32 soundOffset`,
`u16 weight`.

**Sound entry:** `u8 flags`, `u16 category`, `u8 volume`, `s16 pitch`,
`u8 priority`, `u16 filter`, then `flags & 0x01 ? u8 clipCount : (u16 track +
u8 waveBank)`, then if `flags & 0x0E` an RPC block (`u16 dataLen`,
`u8 numPresets`, `u32 × n`), then if `flags & 0x10` a DSP block.

**Cue names** are NUL-separated at the name table offset, in cue index order,
simple cues first.

Sanity check that this is right: walking every cue in `SFXCuesNew.xsb` reached
304 of the bank's 305 sound entries.

---

## Failing safe

The patch is built so that a build it does not recognise gets **nothing done to
it**, rather than something half-done:

- Every code site is compared against its expected byte sequence before any write.
  A mismatch means that patch is skipped.
- Bank edits are made to a private copy. If the result is rejected, XACT is handed
  the game's original, untouched buffer.
- Baked-in header words are only trusted when the bank's size *and* shipped header
  word both match.
- Every offset in the sound-effect table is verified to contain RPC22 before it is
  written.
- Bank size is bounded before anything is allocated.
- `%TEMP%\MISERetroVoice-<pid>.log` records which patches applied, with timings,
  and names any that did not.

Page protection is changed with `VirtualProtect`, written, and restored, followed
by `FlushInstructionCache`. Writes are not atomic and no threads are suspended —
acceptable here only because the code patches land about 14 ms in, before the
game has created its own threads. That is a timing argument, not a guarantee.

The keyboard handling is a **process-local window subclass**, deliberately not a
system-wide `SetWindowsHookEx` keyboard hook — which would be unnecessary here
and indistinguishable from a keylogger. Nothing is recorded or persisted; the
keys the patch uses are inspected and swallowed, everything else passes straight
through.

---

## How things were verified

Claims in this document that say "verified" or "measured" mean one of:

- **Loopback capture.** Recording the system audio output through WASAPI and
  comparing waveform energy between builds on identical saves, to establish
  whether audio was actually audible rather than merely un-muted in theory.
- **Live memory inspection.** Reading the engine's globals out of the running
  process while a line played, to confirm what each address holds and when it
  changes.
- **Direct experiment.** Writing a value into the running game with the patch
  uninvolved, to separate the game's behaviour from the patch's. This is how the
  shared-slot bug above was finally pinned down.

Addresses were found by disassembling a memory dump of the decrypted image, since
the on-disk `.text` is encrypted.

---

## Known limitations

- **Steam build only.** Developed and tested against `MISE.exe` version
  `0708.C417756.A384207`, 1,359,872 bytes, built 2009-07-08. GOG and retail are
  untested; the addresses would almost certainly differ.
- **One sound effect stays muted by design.** `92_LeChuckPunch` shares a sound
  entry with a paired cue, so unmuting it would double that effect.
- **Some new effects ignore F9.** Of the sound entries reachable from those 84
  cues, 46 have no RPC block at all, meaning the game never muted them. They play
  regardless of the toggle, and always did.
- **Skipping very rapidly can leave a line unstarted.** Pressing skip again
  advances it. The likely cause is a skip landing inside the window where the
  engine has played the cue but not yet stored the talk id; unverified.
- **The voice is cut without a fade.** A waveform stopped mid-word can click.
  Adding a ramp reintroduces the timing problem described in patch 5.
- **No unhooking on unload.** The patch assumes it lives for the process
  lifetime, which for a proxy DLL it does.
