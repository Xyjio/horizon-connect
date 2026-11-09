// Included by horizonconnect.c after assoc.c. Code patches in the game and in Azimuth, each checked against the
// original bytes.
//
// [net] SkipWeatherWait=1: the free roam entry (stateflow network.xml initial_entry_common) waits in
// WaitForFullWeatherReady (update 1409641D0) for the session's weather object, which the server made from the
// joining player's FullWeather (msgs 53/54) - fh2srv doesn't make it yet. The patch turns the wait into
// "done" (the jump at 1409641EA to the transition at 140964234); ApplyFullWeatherFromServer, next, skips
// itself when there is no weather object (140865A31).

static int g_skipWeatherWait;

#define EXEC_PROTECT (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)

static DWORD page_protect(const void *at) {
  MEMORY_BASIC_INFORMATION mbi;
  return VirtualQuery(at, &mbi, sizeof mbi) ? mbi.Protect : 0;
}

// code: the bytes are game code. Azimuth 0.9.9 maps the game image writable first and makes its code executable
// a moment later; restoring the protection found before that took the execute right away from the patched page
// (a DEP crash right after start). So code is patched only once its page is executable, and left execute-read.
static int patch_bytes(BYTE *at, const BYTE *orig, const BYTE *repl, SIZE_T n, const char *what, int code) {
  if (code) {
    int i;
    for (i = 0; i < 60000 && !(page_protect(at) & EXEC_PROTECT); i++) Sleep(1);
    if (!(page_protect(at) & EXEC_PROTECT)) { logf("patch %s at %p: page never became executable (0x%lx), not applied", what, at, page_protect(at)); return 0; }
    if (i) logf("patch %s: waited %d ms for the game code to become executable", what, i);
  }
  if (!readable(at, n) || memcmp(at, orig, n)) { logf("patch %s at %p: original bytes differ, not applied", what, at); return 0; }
  DWORD old; VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
  memcpy(at, repl, n);
  VirtualProtect(at, n, code ? PAGE_EXECUTE_READ : old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, n);
  logf("patch %s applied at %p (protection 0x%lx)", what, at, page_protect(at));
  return 1;
}

// [xbl] Xuid=<decimal>: the player's Xbox user id. Azimuth has it as a constant (0x9000000000000 =
// 2533274790395904); consoles playing together each need their own. Only the XboxUserId string (a movabs r8
// at the layout's xuidRva) is patched: Azimuth also names the save container from the id (User_%llX), so a
// console with another id has its own save (copy one into data\...\User_<hex id> to keep progress).
static void xuid_patch(void) {
  wchar_t s[32]; GetPrivateProfileStringW(L"xbl", L"Xuid", L"", s, 32, g_ini);
  if (!s[0]) return;
  const AzimuthLayout *l = azimuth_layout();
  if (!l) return;
  ULONG64 x = wcstoull(s, NULL, 10);
  BYTE *az = (BYTE *)GetModuleHandleW(NULL);
  BYTE orig[10] = { 0x49, 0xB8 }, repl[10] = { 0x49, 0xB8 };
  ULONG64 azx = 0x9000000000000ULL;
  memcpy(orig + 2, &azx, 8); memcpy(repl + 2, &x, 8);
  patch_bytes(az + l->xuidRva, orig, repl, 10, "Xuid", 0);
  logf("xuid %llu", x);
}

// [net] SkipTrafficWait=1: the initial car loading (stateflow network.xml initial_car_loading) waits in
// NetworkWaitForAllPendingTraffic (update 1408E05E0) while the traffic manager's pending flag (+0xAE) is set -
// traffic Turn 10's server ran and fh2srv doesn't - and only after it are the players' cars unghosted
// (NetworkInitialCarLoadingUnghosted), so without this players drive through each other. The patch drops the
// jne at 1408E0658, so the wait is done at once (online traffic stays empty).
static int g_skipTrafficWait;

// [online] ShowLeaderHint=0 (default): the session leader's hint "You are the president - start events everyone can
// join" is shown by NetworkFreeRoamLeaderInstructionComponent and, with no server telling it to go, stays on screen
// for the whole session. It is shown in two places: its update (140810920, made to return at once: ret instead of
// its first instruction, push rdi = 40 57) and its handler for a new leader (140812FA0: the call to the show
// function at 14081301C becomes five nops; the hide call for a player who isn't the leader stays).
static int g_showLeaderHint;

// Placement textures (fallback ported from FH2MP's shim, GPL-3.0, https://github.com/Charelene/FH2MP,
// shim/src/fh2mp_mod.c patch_placement_fallback). The game makes some textures with CreatePlacementTexture2D at
// a memory address of its own; Azimuth supports only some descriptions and stops the whole game for the others
// ("CreatePlacementTexture2D of semantics not carried ... at game+0x10c897b"). A car design with layers (vinyls)
// is drawn into such a texture (1024x1024, 11 mips, CPU read access), so a player whose car wore one stopped at
// every join. The game's one call site, in its wrapper 1410C88E0:
//     1410C8971  mov rdx, r14 ; call qword ptr [r10+0x200] ; nop      49 8B D6 41 FF 92 00 02 00 00 90
// becomes "call [rip+slot]" into placement_hook (same arguments): a supported description goes to Azimuth as it
// was, another one is made as an ordinary texture (device vtable slot 5, CreateTexture2D) without the Xbox misc
// flags. It doesn't share the game's memory at that address, so it may look wrong, but the game goes on.
typedef long (*placement_fn)(void *dev, const unsigned *desc, unsigned tile, unsigned pitch, void *va, void **out);
typedef long (*create2d_fn)(void *dev, const unsigned *desc, const void *init, void **out);
static volatile LONG g_placementLogged;

// desc: width, height, mips, slices, format, samples, quality, usage, bind, cpu access, misc
static int placement_supported(const unsigned *d) {
  if (d[7] == 0 && d[9] == 0 && (d[10] == 0 || ((d[10] - 0x20000u) & 0xfffdffffu) == 0) && d[2] == 1 && d[3] == 1 &&
      (d[8] == 0x28 || d[8] == 0x48 || d[8] == 0xa8))
    return 1;                                  // a GPU-written texture
  return !(d[8] != 8 || d[9] != 0 || d[5] != 1 || d[2] == 0 || d[2] > 15 || d[3] > 0x800 || d[3] == 0 || d[7] > 1);
}

static long placement_hook(void *dev, const unsigned *desc, unsigned tile, unsigned pitch, void *va, void **out) {
  void **vt = *(void ***)dev;
  if (placement_supported(desc)) return ((placement_fn)vt[0x200 / 8])(dev, desc, tile, pitch, va, out);
  unsigned copy[11];
  memcpy(copy, desc, sizeof copy);
  copy[10] &= 0xffffu;                         // keep the standard D3D11 misc flags only
  long hr = ((create2d_fn)vt[5])(dev, copy, NULL, out);
  if (InterlockedIncrement(&g_placementLogged) <= 20)
    logf("placement texture %ux%u, %u mips, format %u, usage %u, bind 0x%x, cpu 0x%x, misc 0x%x: Azimuth would stop the "
         "game, made as an ordinary texture (hr 0x%08lx)", desc[0], desc[1], desc[2], desc[4], desc[7], desc[8], desc[9], desc[10], hr);
  return hr;
}

// A page within rel32 reach of `anchor` (for the pointer the patched call reads).
static BYTE *alloc_near(BYTE *anchor) {
  SYSTEM_INFO si; GetSystemInfo(&si);
  ULONG_PTR lo = (ULONG_PTR)anchor > 0x7FF00000u ? (ULONG_PTR)anchor - 0x7FF00000u : 0;
  ULONG_PTR hi = (ULONG_PTR)anchor + 0x7FF00000u;
  if (lo < (ULONG_PTR)si.lpMinimumApplicationAddress) lo = (ULONG_PTR)si.lpMinimumApplicationAddress;
  if (hi > (ULONG_PTR)si.lpMaximumApplicationAddress) hi = (ULONG_PTR)si.lpMaximumApplicationAddress;
  for (ULONG_PTR a = lo & ~(ULONG_PTR)0xFFFF; a + 0x1000 <= hi; ) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)a, &mbi, sizeof mbi)) break;
    ULONG_PTR base = (ULONG_PTR)mbi.BaseAddress, end = base + mbi.RegionSize;
    ULONG_PTR cand = (base + 0xFFFF) & ~(ULONG_PTR)0xFFFF;
    if (mbi.State == MEM_FREE && cand >= lo && cand + 0x1000 <= end && cand + 0x1000 <= hi) {
      BYTE *p = VirtualAlloc((void *)cand, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (p) return p;
    }
    a = end > a ? end : a + 0x10000;
  }
  return NULL;
}

static void placement_patch(void) {
  static const BYTE want[11] = { 0x49, 0x8B, 0xD6, 0x41, 0xFF, 0x92, 0x00, 0x02, 0x00, 0x00, 0x90 };
  BYTE *site = GAME_BASE + (0x1410C8971 - 0x140000000ULL);
  BYTE *slot = alloc_near(site);
  if (!slot) { logf("patch PlacementFallback: no page near the game code, not applied"); return; }
  *(void **)slot = (void *)placement_hook;
  DWORD old; VirtualProtect(slot, 0x1000, PAGE_READONLY, &old);
  LONG64 disp = (LONG64)(slot - (site + 3 + 6));
  if (disp > 0x7fffffffLL || disp < -0x80000000LL) { logf("patch PlacementFallback: page out of reach, not applied"); return; }
  BYTE call[8] = { 0xFF, 0x15, 0, 0, 0, 0, 0x90, 0x90 };   // call qword ptr [rip+disp32] ; nop ; nop
  int d32 = (int)disp; memcpy(call + 2, &d32, 4);
  if (patch_bytes(site + 3, want + 3, call, 8, "PlacementFallback", 1)) InterlockedExchange(&g_placementFix, 1);
}

// Always: the game's Game DVR clips. At notable moments (140562160, called from several gameplay spots) the game
// records a clip of the last seconds through Windows.Xbox.Media.Capture.ApplicationClipCapture (activated in
// 1402BD580, from the task 140542B70). Azimuth doesn't support that class and stops the game ("activation
// Windows.Xbox.Media.Capture.ApplicationClipCapture"; it names HorizonConnect.dll because the activation passes
// our GetActivationFactory hook). The game has a debug switch for it (disableGameDVR, settings+0x3B7A, checked
// at 14056219D); the patch makes that check always skip the recording (jne 1405621A4 -> jmp 14056233A).
static void game_patches(void) {
  placement_patch();
  {
    static const BYTE orig[] = { 0x0F, 0x85, 0x90, 0x01, 0x00, 0x00 }, jmp[] = { 0x90, 0xE9, 0x90, 0x01, 0x00, 0x00 };
    patch_bytes(GAME_BASE + (0x1405621A4 - 0x140000000ULL), orig, jmp, 6, "NoGameDVR", 1);
  }
  if (!g_showLeaderHint) {
    static const BYTE orig[] = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x40 }, ret[] = { 0xC3, 0x57, 0x48, 0x83, 0xEC, 0x40 };
    patch_bytes(GAME_BASE + (0x140810920 - 0x140000000ULL), orig, ret, 6, "HideLeaderHint", 1);
    static const BYTE call[] = { 0xE8, 0x8F, 0xDC, 0xF3, 0xFF }, nops[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    patch_bytes(GAME_BASE + (0x14081301C - 0x140000000ULL), call, nops, 5, "HideLeaderHint (new leader)", 1);
  }
  if (g_skipTrafficWait) {
    static const BYTE orig[] = { 0x75, 0x0D }, nops[] = { 0x90, 0x90 };
    patch_bytes(GAME_BASE + (0x1408E0658 - 0x140000000ULL), orig, nops, 2, "SkipTrafficWait", 1);
  }
  if (g_skipWeatherWait) {
    static const BYTE orig[] = { 0x48, 0x8B, 0x0D }, jmp[] = { 0xEB, 0x48 };   // mov rcx, [rip+..] -> jmp 140964234
    patch_bytes(GAME_BASE + (0x1409641EA - 0x140000000ULL), orig, jmp, 2, "SkipWeatherWait", 1);
  }
}
