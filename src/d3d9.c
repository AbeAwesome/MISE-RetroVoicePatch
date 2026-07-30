/*
 *  MISE Retro Voice Patch  --  proxy d3d9.dll
 *  The Secret of Monkey Island: Special Edition, MISE.exe 2009-07-08 build.
 *
 *  Keeps the voice acting audible in classic (retro) graphics mode, which the
 *  game normally silences.  Two independent mutes have to be undone: an XACT
 *  RPC preset attached to every speech cue, and a per-frame write to the Speech
 *  category volume.  A third patch lets subtitles wait for the voice line, which
 *  the engine already supports but disables in classic mode.  A fourth restores
 *  the per-line dialogue skip the Special Edition dropped.  Everything is
 *  patched in memory, so no game file changes.
 *
 *  Full write-up, including how the addresses and constants were derived, is in
 *  the project notes.  Comments below cover only what is needed to change this
 *  file safely.
 *
 *  Built for x86 without the CRT (/Zl /NODEFAULTLIB /ENTRY:DllEntry).
 */

#include <windows.h>
#include <stdarg.h>

/* The compiler emits a reference to this the moment a float appears (the fade
 * ramp below); with /NODEFAULTLIB there is no CRT to define it.  Its value is
 * never read -- the symbol just has to exist. */
int _fltused = 1;

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
 * Muting is done by detouring the instruction that loads the ambience volume:
 *
 *   0x441F0E  D9 83 C0 00 00 00   fld dword ptr [ebx+0C0h]   ; 6 bytes
 *
 * replaced with "call ourStub; nop".  The stub pushes either the real value or
 * 0.0 onto the FPU stack, so the game's own volume slider still applies when
 * ambience is on, and nothing else in the audio update has to change.
 */
#define VA_AMBVOL 0x00441F0Eu
static const BYTE SIG_AMBVOL[] = {
    0xD9, 0x83, 0xC0, 0x00, 0x00, 0x00,   /* fld dword ptr [ebx+0C0h] */
    0x51,                                 /* push ecx                 */
    0xD9, 0x1C, 0x24                      /* fstp dword ptr [esp]     */
};

#define VK_TOGGLE_AMBIENCE 0x78           /* VK_F9 */

static volatile LONG g_ambienceOn = 1;

/* Replaces the 6-byte fld above.  ebx is the audio manager throughout that
 * function, so the real value is still one instruction away when enabled. */
static __declspec(naked) void AmbienceVolStub(void)
{
    __asm {
        pushfd
        cmp  dword ptr [g_ambienceOn], 0
        je   silent
        popfd
        fld  dword ptr [ebx+0C0h]         /* the instruction we replaced */
        ret
    silent:
        popfd
        fldz                              /* ambience off */
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
 * line.  Cutting a waveform dead mid-word clicks, so the Speech category is
 * ramped to zero over FADE_MS and only then stopped.  The ramp rides on the
 * game's own per-frame volume write:
 *
 *   0x441EA9  F3 0F 11 04 24   movss dword ptr [esp], xmm0   ; volume argument
 *   0x441EAE  52 50            push edx / push eax           ; category, engine
 *   0x441EB0  8B 41 4C         mov eax, [ecx+4Ch]            ; SetVolume
 *
 * That store is exactly 5 bytes, so it becomes a call to a stub that performs
 * the store and then scales it.  Only the Speech category passes through here;
 * the write just below it at 0x441EDF is a different category.
 */
#define VA_SPEECHVOL_STORE 0x00441EA1u
static const BYTE SIG_SPEECHVOL_STORE[] = {
    0xF2, 0x0F, 0x59, 0xC1,               /* mulsd    xmm0, xmm1        */
    0x66, 0x0F, 0x5A, 0xC0,               /* cvtpd2ps xmm0, xmm0        */
    0xF3, 0x0F, 0x11, 0x04, 0x24,         /* movss    [esp], xmm0  <--  */
    0x52, 0x50,                           /* push edx / push eax        */
    0x8B, 0x41, 0x4C,                     /* mov eax, [ecx+4Ch]         */
    0xFF, 0xD0                            /* call eax                   */
};
#define SPEECHVOL_STORE_OFS 8

/* audio manager -> XACT engine at +70h, Speech category index at +2A2h */
#define VA_AUDIOMGR_PTR 0x005B9888u
#define AUDIOMGR_ENGINE 0x70u
#define AUDIOMGR_SPEECHCAT 0x2A2u

#define VA_TALK_ID    0x005B996Cu     /* dword: voice line id, 0 = none */
#define VA_TALK_DELAY 0x005C2012u     /* word:  subtitle countdown      */

#define VK_SKIP_COMMA  0xBC           /* VK_OEM_COMMA  */
#define VK_SKIP_PERIOD 0xBE           /* VK_OEM_PERIOD */

#define FADE_MS 80u                   /* ramp length before the cue is stopped */

/* IXACT3Engine vtable: CreateSoundBank is index 9 (used at 0x440FCD as
 * [ecx+24h]) and SetVolume is index 19 ([ecx+4Ch] above), which pins Stop at
 * index 18. */
#define XACT_VT_STOP 18
#define XACT_STOP_IMMEDIATE 1u

static volatile LONG g_fadeStart = 0;      /* GetTickCount at skip, 0 = idle */
static float         g_speechGain = 1.0f;  /* read by the stub every frame   */
static BOOL          g_haveFadeHook = FALSE;

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

/* Called once per frame from the stub below, on the game's own thread. */
static void UpdateSpeechGain(void)
{
    LONG start = g_fadeStart;
    DWORD dt;

    if (!start) {
        g_speechGain = 1.0f;
        return;
    }
    dt = GetTickCount() - (DWORD)start;
    if (dt < FADE_MS) {
        g_speechGain = 1.0f - (float)dt / (float)FADE_MS;
        return;
    }
    StopSpeechCategory();
    InterlockedExchange(&g_fadeStart, 0);
    g_speechGain = 1.0f;
}

/* Replaces "movss [esp], xmm0".  Our call pushed a return address, so the
 * argument slot the game is building sits one dword further up.  The C call is
 * free to clobber xmm registers because the value is reloaded from that slot
 * afterwards; eax/ecx/edx must survive, hence pushad. */
static __declspec(naked) void SpeechVolStub(void)
{
    __asm {
        movss dword ptr [esp+4], xmm0     /* the store we replaced */
        pushfd
        pushad
        call UpdateSpeechGain
        /* pushad(32) + pushfd(4) + return address(4) = 40 */
        movss xmm0, dword ptr [esp+40]
        mulss xmm0, dword ptr [g_speechGain]
        movss dword ptr [esp+40], xmm0
        popad
        popfd
        ret
    }
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

    if (tid) {
        if (g_haveFadeHook)
            InterlockedExchange(&g_fadeStart, (LONG)GetTickCount());
        else
            StopSpeechCategory();   /* no ramp available; cut it cleanly */
    }

    *(volatile DWORD *)VA_TALK_ID = 0;
    *(volatile WORD *)VA_TALK_DELAY = 0;
    Log("[voice] %s: line skipped (talkid=%u, subtitle=%u)", how, tid, delay);
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
} BankFix;

static const BankFix BANKS[] = {
    { "SpeechCues",   289474u, 0x85BEu, 0xB1CCu, 4547 },
    { "AmbienceCues",   3899u, 0xF52Eu, 0xECDCu,   30 }
};
#define BANK_COUNT 2

#define MAX_BANK_SIZE (16u * 1024u * 1024u)   /* bound before allocating */

typedef HRESULT (WINAPI *PFN_CreateSoundBank)(void *pEngine, const void *pv,
                                              DWORD size, DWORD f1, DWORD f2,
                                              void **ppSB);

/* Swap RPC22 for RPC26 wherever it appears in a two-preset RPC block.  Both
 * orderings occur, and both banks use it, so this is written generically.
 * Requiring the other preset to be a valid offset too keeps it from matching
 * unrelated data that happens to start with 0B 00 02. */
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

    n = RepointRpc22(work, size);
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
            Log("[voice] F9: room ambience %s", was ? "OFF" : "ON");
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
    Log("[voice] F9 ambience toggle armed (hwnd 0x%08X)", (DWORD)(UINT_PTR)hwnd);
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
    BOOL didAmb = FALSE, didHook = FALSE, didFade = FALSE;

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
        if (!didAmb && Readable(VA_AMBVOL, sizeof(SIG_AMBVOL)) &&
            MemCmp((const void *)VA_AMBVOL, SIG_AMBVOL, sizeof(SIG_AMBVOL)) == 0) {
            BYTE det[6];
            det[0] = 0xE8;                          /* call rel32 */
            *(DWORD *)(det + 1) =
                (DWORD)((BYTE *)AmbienceVolStub - (BYTE *)(VA_AMBVOL + 5));
            det[5] = 0x90;                          /* nop, to fill 6 bytes */
            if (WriteCode(VA_AMBVOL, det, 6)) {
                didAmb = TRUE;
                Log("[voice] ambience volume hooked @0x%08X (t=%d ms)", VA_AMBVOL, i);
            }
        }
        if (!didFade && Readable(VA_SPEECHVOL_STORE, sizeof(SIG_SPEECHVOL_STORE)) &&
            MemCmp((const void *)VA_SPEECHVOL_STORE, SIG_SPEECHVOL_STORE,
                   sizeof(SIG_SPEECHVOL_STORE)) == 0) {
            BYTE call5[5];
            call5[0] = 0xE8;                    /* call rel32 */
            *(DWORD *)(call5 + 1) = (DWORD)((BYTE *)SpeechVolStub -
                (BYTE *)(VA_SPEECHVOL_STORE + SPEECHVOL_STORE_OFS + 5));
            if (WriteCode(VA_SPEECHVOL_STORE + SPEECHVOL_STORE_OFS, call5, 5)) {
                didFade = TRUE;
                g_haveFadeHook = TRUE;
                Log("[voice] speech fade hooked @0x%08X (t=%d ms)",
                    VA_SPEECHVOL_STORE + SPEECHVOL_STORE_OFS, i);
            }
        }
        /* the game window does not exist as early as the code patches */
        if (!didHook)
            didHook = InstallSubclass();

        if (didVol && didBank && didHold && didAmb && didHook && didFade)
            return 0;
        Sleep(1);
    }

    Log("[voice] FAILED to apply patches (vol=%d bank=%d hold=%d amb=%d hook=%d"
        " fade=%d) - unexpected build?",
        didVol, didBank, didHold, didAmb, didHook, didFade);
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
