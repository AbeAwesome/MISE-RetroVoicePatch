/*
 *  MISE Retro Voice Patch  --  proxy d3d9.dll
 *  The Secret of Monkey Island: Special Edition, MISE.exe 2009-07-08 build.
 *
 *  Keeps the voice acting audible in classic (retro) graphics mode, which the
 *  game normally silences.  Two independent mutes have to be undone: an XACT
 *  RPC preset attached to every speech cue, and a per-frame write to the Speech
 *  category volume.  A third patch lets subtitles wait for the voice line, which
 *  the engine already supports but disables in classic mode.  A fourth restores
 *  the per-line dialogue skip the Special Edition dropped, and a fifth lets the
 *  mouse wheel page through long dialogue lists.  F9 adds the Special
 *  Edition's ambience and its sound effects that have no original
 *  counterpart.  Everything is patched in memory, so no game file changes.
 *
 *  Full write-up, including how the addresses and constants were derived, is in
 *  the project notes.  Comments below cover only what is needed to change this
 *  file safely.
 *
 *  Built for x86 without the CRT (/Zl /NODEFAULTLIB /ENTRY:DllEntry).
 */

#include <windows.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* helpers (no CRT)                                                    */
/* ------------------------------------------------------------------ */

static int MemCmp(const void *a, const void *b, SIZE_T n)
{
    const BYTE *p = (const BYTE *)a, *q = (const BYTE *)b;
    SIZE_T i;
    for (i = 0; i < n; i++) {
        if (p[i] != q[i])
            return (int)p[i] - (int)q[i];
    }
    return 0;
}

static void MemCpy(void *d, const void *s, SIZE_T n)
{
    BYTE *p = (BYTE *)d;
    const BYTE *q = (const BYTE *)s;
    SIZE_T i;
    for (i = 0; i < n; i++)
        p[i] = q[i];
}

static const BYTE *MemFind(const BYTE *hay, SIZE_T hayLen,
                           const BYTE *needle, SIZE_T nLen)
{
    SIZE_T i;
    if (nLen == 0 || hayLen < nLen)
        return NULL;
    for (i = 0; i + nLen <= hayLen; i++) {
        if (hay[i] == needle[0] && MemCmp(hay + i, needle, nLen) == 0)
            return hay + i;
    }
    return NULL;
}

static BOOL TempFile(char *out, const char *leaf)
{
    UINT n = GetTempPathA(MAX_PATH, out);
    if (n == 0 || n >= MAX_PATH)
        return FALSE;
    if (n + (UINT)lstrlenA(leaf) + 1 >= MAX_PATH)
        return FALSE;
    lstrcatA(out, leaf);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* logging -> %TEMP%\MISERetroVoice-<pid>.log                          */
/* ------------------------------------------------------------------ */

static char g_logPath[MAX_PATH];

static void Log(const char *fmt, ...)
{
    /* Must stay above wvsprintfA's documented 1024-char maximum: it takes no
     * destination size, so the only real guard is a big enough buffer. */
    char msg[1100];
    va_list ap;
    int len;
    HANDLE h;
    DWORD written;

    if (!g_logPath[0])
        return;
    va_start(ap, fmt);
    len = wvsprintfA(msg, fmt, ap);
    va_end(ap);
    if (len < 0 || len > (int)sizeof(msg) - 3)
        return;
    msg[len++] = '\r';
    msg[len++] = '\n';

    h = CreateFileA(g_logPath, FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, msg, (DWORD)len, &written, NULL);
    CloseHandle(h);
}

/* ------------------------------------------------------------------ */
/* patch 1 -- Speech category volume                                   */
/* ------------------------------------------------------------------ */

/* The game sets the Speech category volume to (1.0 - classicBlend) * voiceVol
 * every frame.  Turning the cvtps2pd at +7 into "xorps xmm1,xmm1" (same 3
 * bytes) makes that 1.0 - 0.0.  xmm1 is reloaded two instructions later, so
 * nothing else depends on it.
 *
 *   0x441E84  0F B7 93 A2 02 00 00     movzx edx, [ebx+2A2h]   ; Speech category
 *   0x441E8B  0F 5A C8                 cvtps2pd xmm1, xmm0     ; <-- patched
 *   0x441E8E  F2 0F 10 05 70 C9 4E 00  movsd xmm0, [4EC970h]   ; 1.0
 */
#define VA_SPEECHVOL 0x00441E84u
static const BYTE SIG_SPEECHVOL[] = {
    0x0F, 0xB7, 0x93, 0xA2, 0x02, 0x00, 0x00,
    0x0F, 0x5A, 0xC8,
    0xF2, 0x0F, 0x10, 0x05, 0x70, 0xC9, 0x4E, 0x00
};
#define SPEECHVOL_PATCH_OFS 7
static const BYTE PATCH_SPEECHVOL[] = { 0x0F, 0x57, 0xC9 };  /* xorps xmm1, xmm1 */

/* ------------------------------------------------------------------ */
/* patch 1b -- let subtitles wait for the voice line in classic mode   */
/* ------------------------------------------------------------------ */

/* The engine already knows how to hold a subtitle until its voice line
 * finishes.  When speech starts it stores the line's duration (milliseconds,
 * straight out of speech.info) in [audioMgr+2BCh] and the talk id in the global
 * at 5B996Ch; the audio update counts the duration down and clears the global
 * when it expires; and the SCUMM message tick at 0x498BE0 refuses to decrement
 * a message's timer while its talk id matches that global.
 *
 * Classic mode never benefits, because storing the talk id is gated on the
 * classic-mode flag at [audioMgr+300h]:
 *
 *   0x442B2F  80 BF 00 03 00 00 00   cmp byte ptr [edi+300h], 0
 *   0x442B36  75 09                  jne  0x442B41        <-- skips the store
 *   0x442B38  0F BF 45 18            movsx eax, word ptr [ebp+18h]
 *   0x442B3C  A3 6C 99 5B 00         mov  [5B996Ch], eax
 *
 * NOPping that jne stores the talk id in both modes, so the existing hold logic
 * engages and a subtitle stays up for as long as its voice line plays.  Lines
 * with no recorded cue never reach here, so they keep their normal timing and
 * cannot hang.  In Special Edition mode the flag is already zero, so the branch
 * was never taken and behaviour is unchanged.
 */
#define VA_SPEECHHOLD 0x00442B2Fu
static const BYTE SIG_SPEECHHOLD[] = {
    0x80, 0xBF, 0x00, 0x03, 0x00, 0x00, 0x00,   /* cmp byte ptr [edi+300h], 0 */
    0x75, 0x09,                                 /* jne  +9                    */
    0x0F, 0xBF, 0x45, 0x18,                     /* movsx eax, [ebp+18h]       */
    0xA3, 0x6C, 0x99, 0x5B, 0x00                /* mov  [5B996Ch], eax        */
};
#define SPEECHHOLD_PATCH_OFS 7
static const BYTE PATCH_SPEECHHOLD[] = { 0x90, 0x90 };       /* nop; nop */

/* ------------------------------------------------------------------ */
/* patch 1c -- F9 toggles room ambience                                */
/* ------------------------------------------------------------------ */

/* Ambience is unmuted in classic mode by the AmbienceCues fix below, which some
 * people will want and some won't, so it gets a runtime toggle.
 *
 * F9 is safe.  The game's keyboard handler at 0x4020E0 dispatches F1 (menu) and
 * F10 (version hotswap) and then swallows the whole F1..F12 range:
 *
 *   0x402661  cmp edi, 70h      ; F1
 *   0x402664  jb  ...
 *   0x402666  cmp edi, 7Bh      ; F12
 *   0x402669  jbe 0x402B5E      ; -> "unhandled", returns 0
 *
 * so F2..F9 and F11 do nothing in-game.  F12 is Steam's screenshot key, so F9 it
 * is -- unused, and next to F10 where it is easy to remember.  We swallow the
 * key in our window proc, so the game never sees it either way.
 *
 * Where the muting happens matters, because of a latent bug in the game.  The
 * audio update computes one volume, hands it to SetVolume for the Ambience
 * category, and then -- when the branch at 0x441F76 is taken, which it always is
 * in practice -- hands the *same stack slot* to SetVolume for the SFX category
 * without recomputing it:
 *
 *   E-4   0x441F14  push ecx                    ; makes the argument slot
 *   E-4   0x441F29  movss [esp+20h], xmm0       ; writes E+1Ch  <- shared slot
 *   E-12  0x441F36  push edx / push eax
 *   E     0x441F3B  call SetVolume              ; stdcall, ret 12 -> esp back to E
 *   E     0x441F76  jb 0x441FC8                 ; taken: [mgr+0CCh] is -1, limit 0
 *   --              movss [esp+1Ch], xmm0       ; the SFX volume: SKIPPED
 *   E     0x441FCB  fld [esp+1Ch]               ; reads E+1Ch -- the same dword
 *
 * Stock MISE never notices: ambience and SFX are both 100, so reusing the slot
 * changes nothing.  Any patch that lowers the ambience volume, though, lowers
 * the SFX volume with it and silences every sound effect in the game.  Earlier
 * versions of this patch did exactly that by replacing the "fld [ebx+0C0h]" at
 * 0x441F0E, and the symptom was blamed on everything except the real cause.
 *
 * So the value the game stores must be left alone, and only the copy that
 * becomes SetVolume's argument is muted.  That copy is these 7 bytes:
 *
 *   0x441F2F  D9 44 24 20   fld  dword ptr [esp+20h]   ; read the shared slot
 *   0x441F33  D9 1C 24      fstp dword ptr [esp]       ; into the argument slot
 *
 * replaced with "call ourStub; nop; nop".  xmm0 is dead here (0x441F49 reloads
 * it) and an fld/fstp pair is swapped for code using no x87 at all, so the FPU
 * stack is left exactly as it was.
 */
#define VA_AMBARG 0x00441F1Du
static const BYTE SIG_AMBARG[] = {
    0x0F, 0xB7, 0x93, 0xA0, 0x02, 0x00, 0x00,   /* movzx edx,[ebx+2A0h] Ambience */
    0x8B, 0x43, 0x70,                           /* mov eax,[ebx+70h]  engine     */
    0x8B, 0x08,                                 /* mov ecx,[eax]      vtable     */
    0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20,         /* movss [esp+20h],xmm0          */
    0xD9, 0x44, 0x24, 0x20,                     /* fld  [esp+20h]        <-- patched */
    0xD9, 0x1C, 0x24,                           /* fstp [esp]            <-- patched */
    0x52, 0x50,                                 /* push edx / push eax           */
    0x8B, 0x41, 0x4C,                           /* mov eax,[ecx+4Ch]  SetVolume  */
    0xFF, 0xD0                                  /* call eax                      */
};
#define AMBARG_PATCH_OFS 18

#define VK_TOGGLE_AMBIENCE 0x78           /* VK_F9 */

static volatile LONG g_ambienceOn = 1;

/* Our call pushed a return address, so the shared slot the game wrote at
 * [esp+20h] is now at [esp+24h], and the argument slot it is copying into is at
 * [esp+4].  Only the latter is ever written here. */
static __declspec(naked) void AmbienceArgStub(void)
{
    __asm {
        movss xmm0, dword ptr [esp+24h]   /* the value the game computed */
        pushfd
        cmp   dword ptr [g_ambienceOn], 0
        jne   keep
        xorps xmm0, xmm0                  /* ambience off */
    keep:
        popfd
        movss dword ptr [esp+4], xmm0     /* argument only; slot left intact */
        ret
    }
}

/* ------------------------------------------------------------------ */
/* patch 1d -- per-line dialogue skip (comma / period / right mouse)   */
/* ------------------------------------------------------------------ */

/* The original SCUMM release let you cut a line short and move straight to the
 * next one.  The Special Edition dropped it: Backspace and Delete are wired to
 * a whole-conversation skip instead, and nothing reads '.' any more.
 *
 * A line is described by two engine globals:
 *
 *   5B996Ch  dword, talk id of the voice line playing (0 = none).  Set by
 *            patch 1b above; the message tick at 0x498BE0 will not expire a
 *            subtitle whose talk id matches it.
 *   5C2012h  word, the subtitle's own countdown, ticked down every frame.
 *
 * Zeroing both is exactly the state the engine reaches when a line ends
 * normally, so the script advances through its own path -- no faking of script
 * state, and the next line plays in full.  Verified live: with a line playing,
 * clearing both made the following line start ~0.6 s later, same as an
 * unskipped line.
 *
 * The voice itself has to be silenced separately or it would talk over the next
 * line, and that is done the moment the key is pressed rather than on a timer.
 * An earlier version ramped the Speech category down over 80 ms first, to avoid
 * clicking on a waveform cut mid-word.  It was not worth it: the stop is
 * category-wide, so a deferred stop silences whatever happens to be playing when
 * it fires, and scripts differ wildly in how soon they reach the next line --
 * an insult swordfight leaves half a second, an ordinary exchange none at all.
 * Stopping immediately removes the window entirely, and with it a code hook, two
 * globals and the only floating point in the file.
 */
/* audio manager -> XACT engine at +70h, Speech category index at +2A2h */
#define VA_AUDIOMGR_PTR 0x005B9888u
#define AUDIOMGR_ENGINE 0x70u
#define AUDIOMGR_SPEECHCAT 0x2A2u

#define VA_TALK_ID    0x005B996Cu     /* dword: voice line id, 0 = none */
#define VA_TALK_DELAY 0x005C2012u     /* word:  subtitle countdown      */

#define VK_SKIP_COMMA  0xBC           /* VK_OEM_COMMA  */
#define VK_SKIP_PERIOD 0xBE           /* VK_OEM_PERIOD */

/* IXACT3Engine vtable: CreateSoundBank is index 9 (used at 0x440FCD as
 * [ecx+24h]) and SetVolume is index 19 ([ecx+4Ch] above), which pins Stop at
 * index 18. */
#define XACT_VT_STOP 18
#define XACT_STOP_IMMEDIATE 1u

static BOOL Readable(DWORD va, SIZE_T len);    /* defined with the plumbing */

typedef HRESULT (__stdcall *PFN_XactStop)(void *self, DWORD cat, DWORD flags);

static void StopSpeechCategory(void)
{
    BYTE *mgr = *(BYTE **)VA_AUDIOMGR_PTR;
    void *eng;
    void **vt;

    if (!mgr)
        return;
    eng = *(void **)(mgr + AUDIOMGR_ENGINE);
    if (!eng)
        return;
    vt = *(void ***)eng;
    if (!vt || !vt[XACT_VT_STOP])
        return;
    ((PFN_XactStop)vt[XACT_VT_STOP])(eng,
        (DWORD)*(WORD *)(mgr + AUDIOMGR_SPEECHCAT), XACT_STOP_IMMEDIATE);
}

/*
 * Returns TRUE if a line was actually skipped, so the caller can swallow the
 * key or click; anything else is passed on to the game untouched.  That matters
 * for the right mouse button, which the game tracks in its own button mask.
 */
static BOOL TrySkipLine(const char *how)
{
    DWORD tid;
    WORD delay;

    if (!Readable(VA_TALK_ID, 4) || !Readable(VA_TALK_DELAY, 2))
        return FALSE;

    tid = *(volatile DWORD *)VA_TALK_ID;
    delay = *(volatile WORD *)VA_TALK_DELAY;
    if (!tid && !delay)
        return FALSE;               /* nothing is being said right now */

    if (tid)
        StopSpeechCategory();

    *(volatile DWORD *)VA_TALK_ID = 0;
    *(volatile WORD *)VA_TALK_DELAY = 0;
    Log("[voice] %s: line skipped (talkid=%u, subtitle=%u)", how, tid, delay);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* patch 1e -- mouse wheel scrolls the classic dialogue list           */
/* ------------------------------------------------------------------ */

/* Long conversations (the insult swordfights above all) show six lines at a
 * time with arrows to page through them, and classic mode gives you no way to
 * work them but the mouse.  The game does handle WM_MOUSEWHEEL, but the handler
 * bails out immediately in classic mode:
 *
 *   0x402AEB  cmp byte ptr [esi+718h], 0    ; classic input active?
 *   0x402AF2  jne 0x402B5E                  ; -> unhandled
 *
 * and what it drives (0x427820) is a Special Edition UI widget, not the classic
 * list -- bypassing the branch in a running game scrolls nothing.
 *
 * The classic arrows, though, are ordinary SCUMM verbs, and verbs carry a
 * keyboard shortcut.  Reading the verb slots while a swordfight list was up:
 *
 *   slot  rect (game coords)        key
 *   ...   x18..219  y145..153       '1'   dialogue line 1
 *          ..                       ..    lines 2-6 are '2'..'6'
 *   +9    x0..16    y145..169       'q'   scroll up
 *   +10   x0..16    y172..196       'a'   scroll down
 *
 * So a wheel notch only has to feed the engine 'q' or 'a'.  That is much better
 * than synthesising a click on the arrow: no screen-coordinate maths, nothing
 * resolution dependent, and -- most importantly -- the engine matches a key
 * against *visible* verbs only, so when the arrows are not on screen the
 * keystroke does nothing.  The gate we would otherwise have to build ourselves
 * comes for free, and a stray notch can never pick a dialogue line.
 *
 * The character is written straight to the engine's "last key" global rather
 * than posted as a keystroke.  The game's own key path derives the character
 * with MapVirtualKey, which is keyboard-layout dependent -- VK_Q yields 'a' on
 * AZERTY -- and that would scroll the wrong way.  0x48C010 reads this global
 * and clears it, once per frame.
 */
#define VA_GAMEOBJ_PTR   0x004F1070u   /* -> main game object            */
#define GAMEOBJ_CLASSIC  0x718u        /* byte: classic input active     */
#define VA_SCUMM_KEY     0x005B98C0u   /* dword: pending key character   */

#define SCUMM_KEY_SCROLL_UP   'q'
#define SCUMM_KEY_SCROLL_DOWN 'a'

static BOOL ClassicInputActive(void)
{
    BYTE *go;
    if (!Readable(VA_GAMEOBJ_PTR, 4))
        return FALSE;
    go = *(BYTE **)VA_GAMEOBJ_PTR;
    if (!go || !Readable((DWORD)(go + GAMEOBJ_CLASSIC), 1))
        return FALSE;
    return *(volatile BYTE *)(go + GAMEOBJ_CLASSIC) != 0;
}

/* One notch = one line.  The global holds a single character and the engine
 * consumes it once a frame, so there is nothing to gain by writing it twice. */
static BOOL WheelScroll(WPARAM wp)
{
    short delta;
    if (!ClassicInputActive() || !Readable(VA_SCUMM_KEY, 4))
        return FALSE;                  /* SE mode keeps its own wheel use */
    delta = (short)HIWORD(wp);
    if (!delta)
        return FALSE;
    *(volatile DWORD *)VA_SCUMM_KEY =
        (delta > 0) ? SCUMM_KEY_SCROLL_UP : SCUMM_KEY_SCROLL_DOWN;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* patch 2 -- detach the muting RPC preset from the speech soundbank   */
/* ------------------------------------------------------------------ */

/* IXACT3Engine::CreateSoundBank call site.  These 5 bytes become "call
 * <trampoline>"; our call pushes the same return address the original did, so
 * the callee's "ret 18h" still lands correctly.
 *
 *   0x440FCD  8B 41 24   mov eax, [ecx+24h]
 *   0x440FD0  FF D0      call eax
 */
#define VA_CSB_CALL 0x00440FCDu
static const BYTE SIG_CSB_CALL[] = { 0x8B, 0x41, 0x24, 0xFF, 0xD0 };

/* Each speech sound carries an 11-byte RPC block: u16 dataLen, u8 numPresets,
 * then one .xgs offset per preset.  Speech ships {RPC22, RPC27}; RPC22 is the
 * "NewVersion -> Volume" curve that returns -96 dB in classic mode.
 *
 * It is swapped for RPC26, not removed: XACT rejects a bank whose
 * dataLen != 3 + 4*numPresets, and also rejects one naming a preset twice.
 * RPC26 is "CueVolume -> Volume", and CueVolume's initial value maps to exactly
 * 0 dB on that curve while MISE.exe never writes it, so the swap is inert.
 *
 * RPC22 must stay attached elsewhere -- it is also what mutes the new music,
 * SFX and ambience banks in classic mode, which is why only the speech bank is
 * touched.
 */
#define RPC22 0x0000043Du    /* NewVersion -> Volume, -96 dB in classic mode */
#define RPC26 0x00000499u    /* CueVolume  -> Volume, always 0 dB            */
#define RPC_LO 0x243u        /* valid preset offsets live in [0x243,0x4B0]   */
#define RPC_HI 0x4B0u

/* Two banks are silenced in classic mode by RPC22 and want it removed:
 *
 *   SpeechCues.xsb    - the voice lines
 *   AmbienceCues.xsb  - room ambience (the Melee town clock, surf, and so on).
 *                       Safe to unmute because Ambience is its own XACT
 *                       category, so it cannot disturb the original music, and
 *                       there is no "Original" ambience bank to double up
 *                       against.  Its RPC24 (ducking) reference is kept, so
 *                       ambience still drops under dialogue.
 *
 * SFXCuesNew.xsb and MusicCuesNew.xsb also carry RPC22 and must KEEP it -- they
 * are the new-content halves of paired banks whose original counterparts play
 * in classic mode.  Hence the per-bank name gate.
 *
 * XACT validates the 16-bit word at header offset 8 and rejects any edited bank
 * without the right value; each patched bank's word was recovered by sweeping
 * all 65536 candidates against CreateSoundBank.
 */
typedef struct {
    const char *tag;        /* identifying string inside the bank */
    DWORD       size;       /* size as shipped                    */
    WORD        hdrOrig;    /* header word as shipped             */
    WORD        hdrPatched; /* header word once RPC22 is removed  */
    int         cues;       /* expected number of edits           */
    BOOL        listed;     /* edit only the offsets in SFXNEW_FIX */
} BankFix;

/* SFXCuesNew is the one bank that cannot be treated wholesale.  It holds 146
 * cues, but 62 of them are re-recordings of cues that also exist in
 * SFXCuesOriginal -- which classic mode already plays -- so unmuting the bank
 * would play both copies of those.  The other 84 are content the original game
 * never had: the chef crying, ghost laughs, menu clicks, and the AdLib-era
 * effects the SoundBlaster set never carried.  Only those are touched.
 *
 * The cue -> sound mapping was resolved offline (simple cues are 5-byte entries
 * holding a file offset, complex cues 15 bytes pointing either at a sound or at
 * a variation table).  Exactly one sound is reachable from both groups -- 0x2C4,
 * shared by 92_LeChuckPunch and 22_sound_SBL_whack -- so it is deliberately left
 * muted; the cost is one punch in the LeChuck fight, against reintroducing a
 * doubled effect.  Format notes are in HANDOFF.md.
 *
 * The bank ships with the game and never changes, so the resulting offsets are
 * baked in rather than recomputed at runtime, which would mean parsing four
 * tables in the DLL.  Every one is checked to actually hold RPC22 before it is
 * written, so a bank these were not derived from is left alone. */
#define SFXNEW_FIX_COUNT 114
static const WORD SFXNEW_FIX[SFXNEW_FIX_COUNT] = {
    0x04FB, 0x0585, 0x05F8, 0x0768, 0x07DB, 0x084E, 0x09ED, 0x0A04, 0x0A1B, 0x0A32, 0x0A60, 0x0A77,
    0x0A8E, 0x0AA5, 0x0BFE, 0x0C88, 0x0C9F, 0x0CB6, 0x0CCD, 0x0D57, 0x0D6E, 0x0D85, 0x0F96, 0x0FC4,
    0x0FDB, 0x0FF2, 0x1009, 0x1020, 0x1037, 0x104E, 0x1065, 0x107C, 0x1093, 0x10AA, 0x10C1, 0x10D8,
    0x10EF, 0x1106, 0x111D, 0x1134, 0x114B, 0x1162, 0x1179, 0x1190, 0x11A7, 0x11BE, 0x11D5, 0x11EC,
    0x1203, 0x121A, 0x1231, 0x1248, 0x125F, 0x1276, 0x128D, 0x12A4, 0x12BB, 0x12D2, 0x12E9, 0x1300,
    0x1317, 0x132E, 0x1345, 0x135C, 0x13A1, 0x13D6, 0x13ED, 0x1404, 0x141B, 0x1432, 0x1449, 0x1460,
    0x1477, 0x148E, 0x14A5, 0x14BC, 0x14D3, 0x14EA, 0x1501, 0x1518, 0x152F, 0x1546, 0x155D, 0x1574,
    0x158B, 0x15A2, 0x15B9, 0x15D0, 0x15E7, 0x15FE, 0x1615, 0x162C, 0x1643, 0x165A, 0x1671, 0x1688,
    0x169F, 0x16B6, 0x16CD, 0x16E4, 0x16FB, 0x1712, 0x1729, 0x1740, 0x1757, 0x176E, 0x1785, 0x179C,
    0x17B3, 0x17CA, 0x1826, 0x183D, 0x1854, 0x188F
};

/* The live SFXCuesNew buffer, kept so F9 can flip those offsets back and forth.
 * XACT reads sound data out of it when a cue starts rather than copying it up
 * front, which is what makes a runtime flip work at all -- and also why the
 * buffer is never freed. */
static BYTE * volatile g_sfxBank = NULL;

static const BankFix BANKS[] = {
    { "SpeechCues",   289474u, 0x85BEu, 0xB1CCu, 4547, FALSE },
    { "AmbienceCues",   3899u, 0xF52Eu, 0xECDCu,   30, FALSE },
    { "SFXCuesNew",    13935u, 0x219Eu, 0xA5E6u,  114, TRUE  }
};
#define BANK_COUNT (sizeof(BANKS) / sizeof(BANKS[0]))

#define MAX_BANK_SIZE (16u * 1024u * 1024u)   /* bound before allocating */

typedef HRESULT (WINAPI *PFN_CreateSoundBank)(void *pEngine, const void *pv,
                                              DWORD size, DWORD f1, DWORD f2,
                                              void **ppSB);

/* Swap RPC22 for RPC26 wherever it appears in a two-preset RPC block.  Both
 * orderings occur, and both banks use it, so this is written generically.
 * Requiring the other preset to be a valid offset too keeps it from matching
 * unrelated data that happens to start with 0B 00 02. */
/* Selective form, for SFXCuesNew: only the listed offsets, each verified first
 * so a bank these were not derived from is left completely alone. */
static int RepointListed(BYTE *buf, unsigned int size)
{
    int i, n = 0;

    for (i = 0; i < SFXNEW_FIX_COUNT; i++) {
        WORD o = SFXNEW_FIX[i];
        if ((unsigned int)o + 4 > size || *(DWORD *)(buf + o) != RPC22)
            return 0;
    }
    for (i = 0; i < SFXNEW_FIX_COUNT; i++, n++)
        *(DWORD *)(buf + SFXNEW_FIX[i]) = RPC26;
    return n;
}

/* F9 flips the new effects between audible and muted in the live bank.  Only
 * the low word differs between the two preset offsets, so a single 16-bit store
 * does it -- a dword store would be unaligned at most of these offsets and could
 * be torn by a concurrent read from the mixer. */
static void ApplySfxState(BOOL on)
{
    WORD want = (WORD)(on ? (RPC26 & 0xFFFF) : (RPC22 & 0xFFFF));
    BYTE *b = g_sfxBank;
    int i;

    if (!b)
        return;
    for (i = 0; i < SFXNEW_FIX_COUNT; i++)
        *(volatile WORD *)(b + SFXNEW_FIX[i]) = want;
}

static int RepointRpc22(BYTE *buf, unsigned int size)
{
    SIZE_T i;
    int n = 0;
    for (i = 0; i + 11 <= size; i++) {
        DWORD a, b;
        if (buf[i] != 0x0B || buf[i + 1] != 0x00 || buf[i + 2] != 0x02)
            continue;
        a = *(DWORD *)(buf + i + 3);
        b = *(DWORD *)(buf + i + 7);
        if (a < RPC_LO || a > RPC_HI || b < RPC_LO || b > RPC_HI)
            continue;
        if (a == RPC22) {
            *(DWORD *)(buf + i + 3) = RPC26;
            n++;
        } else if (b == RPC22) {
            *(DWORD *)(buf + i + 7) = RPC26;
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* header-word cache, so an unknown bank is swept at most once         */
/* ------------------------------------------------------------------ */

#define CACHE_MAGIC 0x31435654u        /* 'TVC1' */

typedef struct {
    DWORD magic;
    DWORD size;
    WORD  orig;
    WORD  found;
} CacheRec;

static BOOL CacheLookup(DWORD size, WORD orig, WORD *found)
{
    char path[MAX_PATH];
    HANDLE h;
    CacheRec r;
    DWORD got = 0;

    if (!TempFile(path, "MISERetroVoice.cache"))
        return FALSE;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    ReadFile(h, &r, sizeof(r), &got, NULL);
    CloseHandle(h);
    if (got != sizeof(r) || r.magic != CACHE_MAGIC)
        return FALSE;
    if (r.size != size || r.orig != orig)
        return FALSE;
    *found = r.found;
    return TRUE;
}

static void CacheStore(DWORD size, WORD orig, WORD found)
{
    char path[MAX_PATH];
    HANDLE h;
    CacheRec r;
    DWORD wrote = 0;

    if (!TempFile(path, "MISERetroVoice.cache"))
        return;
    r.magic = CACHE_MAGIC;
    r.size = size;
    r.orig = orig;
    r.found = found;
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, &r, sizeof(r), &wrote, NULL);
    CloseHandle(h);
}

/* Opt-in: the sweep takes ~45 s and would look like the game had hung. */
static BOOL SweepEnabled(void)
{
    char path[MAX_PATH];
    if (!TempFile(path, "MISEVoiceSweep"))
        return FALSE;
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* ------------------------------------------------------------------ */

/*
 * Stands in for IXACT3Engine::CreateSoundBank.  Anything that is not the speech
 * bank passes straight through.
 *
 * The game's own buffer is never written to; XACT is handed a private copy,
 * which on success is deliberately never freed because XACT keeps a pointer
 * into it for the lifetime of the sound bank.
 */
HRESULT __stdcall HookedCreateSoundBank(PFN_CreateSoundBank real, void *pEngine,
                                        void *pv, DWORD size, DWORD f1,
                                        DWORD f2, void **ppSB)
{
    const BYTE *src = (const BYTE *)pv;
    const BankFix *fix = NULL;
    BYTE *work;
    WORD orig, cand = 0;
    HRESULT hr;
    int n, bi;
    BOOL haveCand = FALSE;

    if (!src || size < 0x40 || size > MAX_BANK_SIZE ||
        MemCmp(src, "SDBK", 4) != 0)
        return real(pEngine, pv, size, f1, f2, ppSB);

    /* Identify the bank by name.  SFXCuesNew.xsb and MusicCuesNew.xsb carry the
     * same RPC22 blocks but must keep them, so only listed banks are touched. */
    for (bi = 0; bi < BANK_COUNT; bi++) {
        SIZE_T tl = 0;
        while (BANKS[bi].tag[tl])
            tl++;
        if (MemFind(src, size, (const BYTE *)BANKS[bi].tag, tl)) {
            fix = &BANKS[bi];
            break;
        }
    }
    if (!fix)
        return real(pEngine, pv, size, f1, f2, ppSB);

    if (!ppSB)      /* no way to tell success from failure; leave it alone */
        return real(pEngine, pv, size, f1, f2, ppSB);

    orig = (WORD)(src[8] | (src[9] << 8));

    work = (BYTE *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
    if (!work)
        return real(pEngine, pv, size, f1, f2, ppSB);
    MemCpy(work, src, size);

    n = fix->listed ? RepointListed(work, size) : RepointRpc22(work, size);
    if (n == 0) {
        VirtualFree(work, 0, MEM_RELEASE);
        return real(pEngine, pv, size, f1, f2, ppSB);
    }

    /* Only trust the hard-coded header word for the exact bank it came from. */
    if (size == fix->size && orig == fix->hdrOrig && fix->hdrPatched) {
        cand = fix->hdrPatched;
        haveCand = TRUE;
    } else if (CacheLookup(size, orig, &cand)) {
        haveCand = TRUE;
        Log("[voice] %s: unrecognised (size=%u orig=0x%04X); cached header 0x%04X",
            fix->tag, size, orig, cand);
    }

    if (haveCand) {
        work[8] = (BYTE)(cand & 0xFF);
        work[9] = (BYTE)((cand >> 8) & 0xFF);
        *ppSB = NULL;
        hr = real(pEngine, work, size, f1, f2, ppSB);
        if (hr >= 0 && *ppSB) {
            if (fix->listed) {
                g_sfxBank = work;
                ApplySfxState(g_ambienceOn != 0);
            }
            Log("[voice] %s OK: RPC22 detached from %d/%d cues",
                fix->tag, n, fix->cues);
            return hr;                  /* work intentionally not freed */
        }
    }

    if (SweepEnabled()) {
        DWORD t0 = GetTickCount();
        DWORD c;
        Log("[voice] sweeping header word for %s (size=%u orig=0x%04X)",
            fix->tag, size, orig);
        for (c = 0; c <= 0xFFFF; c++) {
            work[8] = (BYTE)(c & 0xFF);
            work[9] = (BYTE)((c >> 8) & 0xFF);
            *ppSB = NULL;
            hr = real(pEngine, work, size, f1, f2, ppSB);
            if (hr >= 0 && *ppSB) {
                CacheStore(size, orig, (WORD)c);
                if (fix->listed) {
                    g_sfxBank = work;
                    ApplySfxState(g_ambienceOn != 0);
                }
                Log("[voice] header word = 0x%04X after %u ms; cached. "
                    "RPC22 detached from %d cues", c, GetTickCount() - t0, n);
                return hr;              /* work intentionally not freed */
            }
        }
        Log("[voice] sweep exhausted after %u ms - bank not patchable",
            GetTickCount() - t0);
    } else if (!haveCand) {
        Log("[voice] %s unrecognised (size=%u orig=0x%04X). Running "
            "unpatched. To calibrate, create the file %%TEMP%%\\MISEVoiceSweep "
            "and relaunch once (result is cached).", fix->tag, size, orig);
    }

    /* Give up cleanly: hand XACT the game's original, untouched buffer. */
    VirtualFree(work, 0, MEM_RELEASE);
    *ppSB = NULL;
    hr = real(pEngine, pv, size, f1, f2, ppSB);
    Log("[voice] %s left unpatched (hr=0x%08X); it stays muted in classic mode",
        fix->tag, (DWORD)hr);
    return hr;
}

/*
 * On entry the six __stdcall arguments sit above our return address exactly as
 * the original "call eax" left them, so they are forwarded as-is along with the
 * real function pointer, and cleaned up with the same "ret 18h".
 */
static __declspec(naked) void CsbTrampoline(void)
{
    __asm {
        /* [esp]=retaddr [esp+4]=pEngine [esp+8]=pv [esp+12]=size
           [esp+16]=f1 [esp+20]=f2 [esp+24]=ppSB */
        push ebx
        push esi
        push edi
        mov  eax, dword ptr [ecx+24h]   /* the real CreateSoundBank */
        push dword ptr [esp+36]         /* ppSB    */
        push dword ptr [esp+36]         /* f2      */
        push dword ptr [esp+36]         /* f1      */
        push dword ptr [esp+36]         /* size    */
        push dword ptr [esp+36]         /* pv      */
        push dword ptr [esp+36]         /* pEngine */
        push eax                        /* real    */
        call HookedCreateSoundBank      /* __stdcall, cleans its 28 bytes */
        pop  edi
        pop  esi
        pop  ebx
        ret  18h
    }
}

/* ------------------------------------------------------------------ */
/* patch plumbing                                                      */
/* ------------------------------------------------------------------ */

static BOOL WriteCode(DWORD va, const BYTE *data, SIZE_T len)
{
    DWORD old, tmp;
    if (!VirtualProtect((LPVOID)va, len, PAGE_EXECUTE_READWRITE, &old))
        return FALSE;
    MemCpy((void *)va, data, len);
    if (!VirtualProtect((LPVOID)va, len, old, &tmp))
        Log("[voice] note: could not restore page protection at 0x%08X", va);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)va, len);
    return TRUE;
}

static BOOL Readable(DWORD va, SIZE_T len)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)va, &mbi, sizeof(mbi)) == 0)
        return FALSE;
    if (mbi.State != MEM_COMMIT)
        return FALSE;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return FALSE;
    return (DWORD)((BYTE *)mbi.BaseAddress + mbi.RegionSize) >= va + len;
}

/* ------------------------------------------------------------------ */
/* window subclass, purely to receive the F9 toggle                    */
/* ------------------------------------------------------------------ */

/* A process-local subclass of the game's own window -- deliberately NOT a
 * system-wide SetWindowsHookEx keyboard hook, which would be unnecessary here
 * and indistinguishable from a keylogger.  Nothing is recorded or persisted:
 * one key is inspected and swallowed, everything else passes straight through. */

static WNDPROC g_origWndProc = NULL;

static LRESULT CALLBACK OurWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (wp == VK_TOGGLE_AMBIENCE) {
            LONG was = InterlockedExchange(&g_ambienceOn, g_ambienceOn ? 0 : 1);
            ApplySfxState(!was);
            Log("[voice] F9: Special Edition sounds %s (ambience + %d new "
                "effects)", was ? "OFF" : "ON", SFXNEW_FIX_COUNT);
            return 0;                   /* swallow it */
        }
        /* The game maps unhandled keys through MapVirtualKey into its SCUMM
         * key global, but nothing there reads ',' or '.', so swallowing them
         * costs nothing. */
        if (wp == VK_SKIP_COMMA || wp == VK_SKIP_PERIOD) {
            if (TrySkipLine(wp == VK_SKIP_COMMA ? "," : "."))
                return 0;
            return 0;                   /* still inert; do not pass it on */
        }
    }
    /* Only swallowed when it actually skipped, so the button keeps its normal
     * meaning everywhere else. */
    if (msg == WM_RBUTTONDOWN && TrySkipLine("right click"))
        return 0;

    if (msg == WM_MOUSEWHEEL && WheelScroll(wp))
        return 0;

    return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

static BOOL InstallSubclass(void)
{
    HWND hwnd;
    if (g_origWndProc)
        return TRUE;
    hwnd = FindWindowA("RemonkeyedMainWindow", NULL);
    if (!hwnd)
        return FALSE;
    g_origWndProc = (WNDPROC)(LONG_PTR)
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)OurWndProc);
    if (!g_origWndProc)
        return FALSE;
    Log("[voice] input hooks armed: F9 = Special Edition sounds, "
        ", / . / right click = skip line, wheel = scroll dialogue "
        "(hwnd 0x%08X)", (DWORD)(UINT_PTR)hwnd);
    return TRUE;
}

/*
 * MISE.exe is wrapped in Steam's CEG DRM: .text is encrypted on disk and
 * decrypted by the stub inside the exe entry point, which runs *after* every
 * statically imported DLL's entry point.  So we cannot patch immediately; we
 * wait for the expected bytes to appear.  Verifying the signature first also
 * means an unexpected build is left alone rather than corrupted.
 *
 * The code patches land ~13 ms in, before the game creates its own threads or
 * loads any soundbank.  The window does not exist that early, so the subclass is
 * installed later in the same loop.
 */
static DWORD WINAPI PatchThread(LPVOID unused)
{
    int i;
    BOOL didVol = FALSE, didBank = FALSE, didHold = FALSE;
    BOOL didAmb = FALSE, didHook = FALSE;

    (void)unused;

    for (i = 0; i < 60000; i++) {
        if (!didVol && Readable(VA_SPEECHVOL, sizeof(SIG_SPEECHVOL)) &&
            MemCmp((const void *)VA_SPEECHVOL, SIG_SPEECHVOL,
                   sizeof(SIG_SPEECHVOL)) == 0) {
            if (WriteCode(VA_SPEECHVOL + SPEECHVOL_PATCH_OFS,
                          PATCH_SPEECHVOL, sizeof(PATCH_SPEECHVOL))) {
                didVol = TRUE;
                Log("[voice] speech category volume unmuted @0x%08X (t=%d ms)",
                    VA_SPEECHVOL + SPEECHVOL_PATCH_OFS, i);
            }
        }
        if (!didHold && Readable(VA_SPEECHHOLD, sizeof(SIG_SPEECHHOLD)) &&
            MemCmp((const void *)VA_SPEECHHOLD, SIG_SPEECHHOLD,
                   sizeof(SIG_SPEECHHOLD)) == 0) {
            if (WriteCode(VA_SPEECHHOLD + SPEECHHOLD_PATCH_OFS,
                          PATCH_SPEECHHOLD, sizeof(PATCH_SPEECHHOLD))) {
                didHold = TRUE;
                Log("[voice] subtitles now wait for speech @0x%08X (t=%d ms)",
                    VA_SPEECHHOLD + SPEECHHOLD_PATCH_OFS, i);
            }
        }
        if (!didBank && Readable(VA_CSB_CALL, sizeof(SIG_CSB_CALL)) &&
            MemCmp((const void *)VA_CSB_CALL, SIG_CSB_CALL,
                   sizeof(SIG_CSB_CALL)) == 0) {
            BYTE call5[5];
            call5[0] = 0xE8;                    /* call rel32 */
            *(DWORD *)(call5 + 1) =
                (DWORD)((BYTE *)CsbTrampoline - (BYTE *)(VA_CSB_CALL + 5));
            if (WriteCode(VA_CSB_CALL, call5, 5)) {
                didBank = TRUE;
                Log("[voice] CreateSoundBank hooked @0x%08X (t=%d ms)",
                    VA_CSB_CALL, i);
            }
        }
        if (!didAmb && Readable(VA_AMBARG, sizeof(SIG_AMBARG)) &&
            MemCmp((const void *)VA_AMBARG, SIG_AMBARG, sizeof(SIG_AMBARG)) == 0) {
            BYTE det[7];
            det[0] = 0xE8;                          /* call rel32 */
            *(DWORD *)(det + 1) = (DWORD)((BYTE *)AmbienceArgStub -
                (BYTE *)(VA_AMBARG + AMBARG_PATCH_OFS + 5));
            det[5] = 0x90;                          /* nop, to fill 7 bytes */
            det[6] = 0x90;
            if (WriteCode(VA_AMBARG + AMBARG_PATCH_OFS, det, 7)) {
                didAmb = TRUE;
                Log("[voice] ambience argument hooked @0x%08X (t=%d ms)",
                    VA_AMBARG + AMBARG_PATCH_OFS, i);
            }
        }
        /* the game window does not exist as early as the code patches */
        if (!didHook)
            didHook = InstallSubclass();

        if (didVol && didBank && didHold && didAmb && didHook)
            return 0;
        Sleep(1);
    }

    Log("[voice] FAILED to apply patches (vol=%d bank=%d hold=%d amb=%d"
        " hook=%d) - unexpected build?",
        didVol, didBank, didHold, didAmb, didHook);
    return 0;
}

/* ------------------------------------------------------------------ */
/* real d3d9.dll forwarding                                            */
/* ------------------------------------------------------------------ */

/* Only Direct3DCreate9 is exported, which is all MISE.exe imports from d3d9.
 * Overlays injected into the process may look for others (Direct3DCreate9Ex,
 * the D3DPERF_* family); adding runtime forwarding stubs for them is the fix if
 * that ever matters.  They cannot be .def forwarders, since "d3d9.<name>" would
 * resolve back to this DLL. */

typedef IUnknown *(WINAPI *PFN_Direct3DCreate9)(UINT);
static HMODULE             g_realD3D9 = NULL;
static PFN_Direct3DCreate9 g_realCreate9 = NULL;

/* Loaded on first use, not from the entry point: calling LoadLibrary under the
 * loader lock is a documented deadlock risk. */
static void EnsureRealD3D9(void)
{
    static volatile LONG state = 0;     /* 0 = idle, 1 = loading, 2 = done */
    char path[MAX_PATH];
    UINT n;
    LONG prev;

    for (;;) {
        prev = InterlockedCompareExchange(&state, 1, 0);
        if (prev == 2)
            return;
        if (prev == 0)
            break;
        Sleep(0);
    }

    n = GetSystemDirectoryA(path, MAX_PATH);
    if (n > 0 && n < MAX_PATH - 12) {
        lstrcatA(path, "\\d3d9.dll");
        g_realD3D9 = LoadLibraryA(path);
    }
    if (g_realD3D9)
        g_realCreate9 = (PFN_Direct3DCreate9)
            GetProcAddress(g_realD3D9, "Direct3DCreate9");
    if (!g_realCreate9)
        Log("[voice] WARNING: could not load the system d3d9.dll");

    InterlockedExchange(&state, 2);
}

IUnknown * WINAPI Direct3DCreate9(UINT SDKVersion)
{
    EnsureRealD3D9();
    if (!g_realCreate9)
        return NULL;
    return g_realCreate9(SDKVersion);
}

BOOL WINAPI DllEntry(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    char tmp[MAX_PATH];
    HANDLE th;
    UINT n;

    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(hInst);

    /* pid in the name so two instances never interleave their output */
    n = GetTempPathA(MAX_PATH, tmp);
    if (n > 0 && n < MAX_PATH - 40)
        wsprintfA(g_logPath, "%sMISERetroVoice-%u.log", tmp,
                  GetCurrentProcessId());
    else
        g_logPath[0] = 0;

    th = CreateThread(NULL, 0, PatchThread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    return TRUE;
}
