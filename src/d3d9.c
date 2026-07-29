/*
 *  MISE Retro Voice Patch  --  proxy d3d9.dll
 *  The Secret of Monkey Island: Special Edition, MISE.exe 2009-07-08 build.
 *
 *  Keeps the voice acting audible in classic (retro) graphics mode, which the
 *  game normally silences.  Two independent mutes have to be undone: an XACT
 *  RPC preset attached to every speech cue, and a per-frame write to the Speech
 *  category volume.  A third patch lets subtitles wait for the voice line, which
 *  the engine already supports but disables in classic mode.  Everything is
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
static const BYTE RPC_A[]  = { 0x0B,0x00,0x02, 0x3D,0x04,0x00,0x00, 0xB0,0x04,0x00,0x00 };
static const BYTE RPC_A2[] = { 0x0B,0x00,0x02, 0x99,0x04,0x00,0x00, 0xB0,0x04,0x00,0x00 };
static const BYTE RPC_B[]  = { 0x0B,0x00,0x02, 0xB0,0x04,0x00,0x00, 0x3D,0x04,0x00,0x00 };
static const BYTE RPC_B2[] = { 0x0B,0x00,0x02, 0xB0,0x04,0x00,0x00, 0x99,0x04,0x00,0x00 };

/* XACT validates the 16-bit word at header offset 8 and rejects any edited
 * bank without it.  These are the values for the shipped SpeechCues.xsb; the
 * patched one was found by sweeping all 65536 candidates. */
#define SPEECH_BANK_SIZE   289474u
#define SPEECH_HDR_ORIG    0x85BEu
#define SPEECH_HDR_PATCHED 0xB1CCu
#define SPEECH_CUE_COUNT   4547

#define MAX_BANK_SIZE (16u * 1024u * 1024u)   /* bound before allocating */

typedef HRESULT (WINAPI *PFN_CreateSoundBank)(void *pEngine, const void *pv,
                                              DWORD size, DWORD f1, DWORD f2,
                                              void **ppSB);

static int RepointSpeechRpcs(BYTE *buf, unsigned int size)
{
    SIZE_T i;
    int n = 0;
    for (i = 0; i + 11 <= size; i++) {
        if (buf[i] != 0x0B || buf[i + 1] != 0x00 || buf[i + 2] != 0x02)
            continue;
        if (MemCmp(buf + i, RPC_A, 11) == 0) {
            MemCpy(buf + i, RPC_A2, 11);
            n++;
        } else if (MemCmp(buf + i, RPC_B, 11) == 0) {
            MemCpy(buf + i, RPC_B2, 11);
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
    BYTE *work;
    WORD orig, cand = 0;
    HRESULT hr;
    int n;
    BOOL haveCand = FALSE;

    /* SFXCuesNew.xsb contains the same 11-byte pattern 207 times and must keep
     * its mute, hence the bank-name check. */
    if (!src || size < 0x40 || size > MAX_BANK_SIZE ||
        MemCmp(src, "SDBK", 4) != 0 ||
        !MemFind(src, size, (const BYTE *)"SpeechCues", 10))
        return real(pEngine, pv, size, f1, f2, ppSB);

    if (!ppSB)      /* no way to tell success from failure; leave it alone */
        return real(pEngine, pv, size, f1, f2, ppSB);

    orig = (WORD)(src[8] | (src[9] << 8));

    work = (BYTE *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
    if (!work)
        return real(pEngine, pv, size, f1, f2, ppSB);
    MemCpy(work, src, size);

    n = RepointSpeechRpcs(work, size);
    if (n == 0) {
        VirtualFree(work, 0, MEM_RELEASE);
        return real(pEngine, pv, size, f1, f2, ppSB);
    }

    /* Only trust the hard-coded header word for the exact bank it came from. */
    if (size == SPEECH_BANK_SIZE && orig == SPEECH_HDR_ORIG) {
        cand = SPEECH_HDR_PATCHED;
        haveCand = TRUE;
    } else if (CacheLookup(size, orig, &cand)) {
        haveCand = TRUE;
        Log("[voice] unrecognised bank (size=%u orig=0x%04X); using cached "
            "header 0x%04X", size, orig, cand);
    }

    if (haveCand) {
        work[8] = (BYTE)(cand & 0xFF);
        work[9] = (BYTE)((cand >> 8) & 0xFF);
        *ppSB = NULL;
        hr = real(pEngine, work, size, f1, f2, ppSB);
        if (hr >= 0 && *ppSB) {
            Log("[voice] speech bank OK: RPC22 detached from %d/%d cues",
                n, SPEECH_CUE_COUNT);
            return hr;                  /* work intentionally not freed */
        }
    }

    if (SweepEnabled()) {
        DWORD t0 = GetTickCount();
        DWORD c;
        Log("[voice] sweeping header word for bank size=%u orig=0x%04X",
            size, orig);
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
        Log("[voice] unrecognised speech bank (size=%u orig=0x%04X). Running "
            "unpatched. To calibrate, create the file %%TEMP%%\\MISEVoiceSweep "
            "and relaunch once (~45 s, result is cached).", size, orig);
    }

    /* Give up cleanly: hand XACT the game's original, untouched buffer. */
    VirtualFree(work, 0, MEM_RELEASE);
    *ppSB = NULL;
    hr = real(pEngine, pv, size, f1, f2, ppSB);
    Log("[voice] speech bank left unpatched (hr=0x%08X); voice stays muted in "
        "classic mode", (DWORD)hr);
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

/*
 * MISE.exe is wrapped in Steam's CEG DRM: .text is encrypted on disk and
 * decrypted by the stub inside the exe entry point, which runs *after* every
 * statically imported DLL's entry point.  So we cannot patch immediately; we
 * wait for the expected bytes to appear.  Verifying the signature first also
 * means an unexpected build is left alone rather than corrupted.
 *
 * In practice both land ~13 ms in, before the game creates its own threads or
 * loads any soundbank, so neither instruction can be executing while rewritten.
 */
static DWORD WINAPI PatchThread(LPVOID unused)
{
    int i;
    BOOL didVol = FALSE, didBank = FALSE, didHold = FALSE;

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
        if (didVol && didBank && didHold)
            return 0;
        Sleep(1);
    }

    Log("[voice] FAILED to apply patches (vol=%d bank=%d hold=%d) - unexpected build?",
        didVol, didBank, didHold);
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
