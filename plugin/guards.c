// Included by horizonconnect.c after patches.c. Crash guards ported from FH2MP's shim (GPL-3.0,
// https://github.com/Charelene/FH2MP, shim/src: remote_prediction_guard.inc, gameplaylog_guard.inc,
// torque_scale.inc, torque_load_guard.inc). Each one checks the game's code before it changes anything and logs
// when it is applied; [guards] Enabled=0 switches them all off.
//
//  - RemotePrediction: States::SetRemotePrediction (1409224DD) asks each network car's player for its user and
//    reads through it without a NULL check. A player who has just left (or isn't in the member list yet) has no
//    user, so a join or leave at the wrong moment stopped the game. The guard gives such a car a stand-in user
//    that answers "no", the same path as a real user that says no.
//  - GameplayLog: CGameplayLogManager's event handler (1409C88E0) writes through the user's log record; a user
//    with no record (no Logon answer for that id) stopped the game at 1409C898F. Such an event is left out
//    (it is telemetry fh2srv answers with an empty OK anyway).
//  - TorqueScale: the upgrade torque reader (14105E530) releases three rowsets and stopped the game when one of
//    them was missing; a missing one gets an empty rowset.
//  - TorqueLoad: the car preloader (140FCFE70) builds the car's physics from its engine's torque curve; a car whose
//    engine, camshaft or curve is missing or unusable (a modified car another player drives) stopped the game. Such
//    a car is loaded through the game's own unavailable-car path (the stand-in car NUL_Car_00).
//
// Test switches (for checking the guards' other path in-game; leave them off): [guards] TestRejectCar=<car id>
// treats that car as invalid (TorqueLoad), TestForce=1 makes RemotePrediction use the stand-in for every car and
// GameplayLog leave out every event.

static int g_guards = 1, g_guardTestForce;
static unsigned g_guardTestCar;
static int g_guardFirst[4];    // first call seen (logged once each)
static volatile LONG g_rpHits, g_glDropped, g_tsMissing, g_tlRejects, g_tlCancels;

// ---- code page near the game and function detours ----------------------------------------------------------------
static BYTE *g_nearCode;
static SIZE_T g_nearUsed;

// A page just below the game image, within rel32 reach of all of its code.
static BYTE *guard_page(void) {
  if (g_nearCode) return g_nearCode;
  for (ULONG_PTR a = (ULONG_PTR)GAME_BASE - 0x10000; a > (ULONG_PTR)GAME_BASE - 0x60000000ULL; a -= 0x10000) {
    BYTE *p = VirtualAlloc((void *)a, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p) { g_nearCode = p; break; }
  }
  return g_nearCode;
}

static BYTE *guard_take(SIZE_T n) {
  BYTE *page = guard_page();
  if (!page || g_nearUsed + n > 0x1000) return NULL;
  BYTE *p = page + g_nearUsed;
  g_nearUsed = (g_nearUsed + n + 15) & ~(SIZE_T)15;
  return p;
}

static void guard_writable(int on) {
  DWORD old;
  if (g_nearCode) VirtualProtect(g_nearCode, 0x1000, on ? PAGE_READWRITE : PAGE_EXECUTE_READ, &old);
}

static BYTE *put_jmp_abs(BYTE *p, const void *to) {      // jmp qword ptr [rip+0] ; dq to
  static const BYTE j[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
  memcpy(p, j, 6); memcpy(p + 6, &to, 8);
  return p + 14;
}

static int rel32_ok(const BYTE *from_next, const BYTE *to) {
  LONG64 d = (LONG64)(to - from_next);
  return d <= 0x7fffffffLL && d >= -0x80000000LL;
}

// Waits until the game code at `at` is executable (see patch_bytes) and compares `n` bytes with `want`.
static int guard_code_is(BYTE *at, const BYTE *want, SIZE_T n, const char *what) {
  int i;
  for (i = 0; i < 60000 && !(page_protect(at) & EXEC_PROTECT); i++) Sleep(1);
  if (!(page_protect(at) & EXEC_PROTECT) || !readable(at, n) || memcmp(at, want, n)) {
    logf("guard %s at %p: the game code differs, not applied", what, at);
    return 0;
  }
  return 1;
}

// Sends the function at `va` to `hook`. Its first `steal` bytes (whole, position-independent instructions, the
// start of `want`) move to a trampoline that *orig then calls.
static int detour(ULONG64 va, const BYTE *want, SIZE_T wantN, SIZE_T steal, void *hook, void **orig, const char *what) {
  BYTE *site = GAME_BASE + (va - 0x140000000ULL);
  if (!guard_code_is(site, want, wantN, what)) return 0;
  guard_writable(1);
  BYTE *tramp = guard_take(steal + 14), *stub = guard_take(14);
  if (!tramp || !stub || !rel32_ok(site + 5, stub)) { guard_writable(0); logf("guard %s: no code page near the game, not applied", what); return 0; }
  memcpy(tramp, want, steal);
  put_jmp_abs(tramp + steal, site + steal);
  put_jmp_abs(stub, hook);
  guard_writable(0);
  FlushInstructionCache(GetCurrentProcess(), g_nearCode, 0x1000);
  *orig = tramp;
  BYTE jmp[16] = { 0xE9 };
  int d = (int)(stub - (site + 5)); memcpy(jmp + 1, &d, 4);
  for (SIZE_T i = 5; i < steal; i++) jmp[i] = 0x90;
  return patch_bytes(site, want, jmp, steal, what, 1);
}

static int rd(const void *p, void *out, SIZE_T n) {
  if (!p || !readable(p, n)) return 0;
  memcpy(out, p, n);
  return 1;
}

// ---- RemotePrediction ----------------------------------------------------------------------------------------------
//     1409224D7  call [rdx+0x130]   ; rax = the car's user (NULL when its player isn't in the session)
//     1409224DD  mov rdx,[rax] ; mov rcx,rax ; call [rdx+0xC0] ; test al,al ; je 1409224F9
// The first two instructions (6 bytes) become a jmp to a stub that puts the stand-in in rax when it is NULL.
static void guard_remote_prediction(void) {
  static const BYTE want[16] = { 0x48, 0x8B, 0x10, 0x48, 0x8B, 0xC8, 0xFF, 0x92, 0xC0, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x74, 0x0C };
  BYTE *site = GAME_BASE + (0x1409224DDULL - 0x140000000ULL);
  if (!guard_code_is(site, want, sizeof want, "RemotePrediction")) return;
  guard_writable(1);
  BYTE *ret0 = guard_take(4), *vt = guard_take(25 * 8), *standin = guard_take(0x48), *stub = guard_take(64);
  if (!ret0 || !vt || !standin || !stub || !rel32_ok(site + 5, stub)) { guard_writable(0); logf("guard RemotePrediction: no code page near the game, not applied"); return; }
  static const BYTE r0[3] = { 0x31, 0xC0, 0xC3 };          // xor eax,eax ; ret
  memcpy(ret0, r0, 3);
  for (int i = 0; i < 25; i++) memcpy(vt + i * 8, &ret0, 8);
  memset(standin, 0, 0x48); memcpy(standin, &vt, 8);
  BYTE *p = stub;
  void *hits = (void *)&g_rpHits;
  *p++ = 0x48; *p++ = 0x85; *p++ = 0xC0;                              // test rax,rax
  *p++ = 0x75; *p++ = g_guardTestForce ? 0x00 : 0x16;                 // jnz have_user (TestForce: always the stand-in)
  *p++ = 0x51;                                                        // push rcx
  *p++ = 0x48; *p++ = 0xB9; memcpy(p, &hits, 8); p += 8;              // mov rcx, &g_rpHits
  *p++ = 0xF0; *p++ = 0xFF; *p++ = 0x01;                              // lock inc dword [rcx]
  *p++ = 0x59;                                                        // pop rcx
  *p++ = 0x48; *p++ = 0x8D; *p++ = 0x05;                              // lea rax,[rip+standin]
  { int d = (int)(standin - (p + 4)); memcpy(p, &d, 4); p += 4; }
  memcpy(p, want, 6); p += 6;                                         // have_user: mov rdx,[rax] ; mov rcx,rax
  put_jmp_abs(p, site + 6);
  guard_writable(0);
  FlushInstructionCache(GetCurrentProcess(), g_nearCode, 0x1000);
  BYTE jmp[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
  int d = (int)(stub - (site + 5)); memcpy(jmp + 1, &d, 4);
  patch_bytes(site, want, jmp, 6, "guard RemotePrediction", 1);
}

// ---- GameplayLog ---------------------------------------------------------------------------------------------------
typedef void (*gl_fn)(void *, void *);
static gl_fn g_glOrig;

// The manager's std::map (this+0x20 = head node; node: [0] left, [1] parent, [2] right, +0x19 is-nil, key +0x20,
// the record's log pointer at +0x60) -> the user's CGameplayLog, NULL when there is none.
static void *gl_log_of(const BYTE *manager, ULONG64 key) {
  const BYTE *head, *node, *best;
  if (!rd(manager + 0x20, &head, 8) || !head || !rd(head + 8, &node, 8)) return NULL;
  best = head;
  for (int guard = 0; node && guard < 64; guard++) {
    BYTE nil; ULONG64 k;
    if (!rd(node + 0x19, &nil, 1) || nil || !rd(node + 0x20, &k, 8)) break;
    if (k < key) { if (!rd(node + 0x10, &node, 8)) return NULL; }
    else { best = node; if (!rd(node, &node, 8)) return NULL; }
  }
  ULONG64 k; void *log;
  if (best == head || !rd(best + 0x20, &k, 8) || k != key || !rd(best + 0x60, &log, 8)) return NULL;
  return log;
}

static void gl_handler(void *manager, void *event) {
  ULONG64 key = 0;
  if (!g_guardFirst[1]) { g_guardFirst[1] = 1; logf("guard GameplayLog: first event"); }
  if (g_guardTestForce || !rd((BYTE *)event + 0x1300, &key, 8) || !gl_log_of(manager, key)) {
    LONG n = InterlockedIncrement(&g_glDropped);
    if (n <= 3 || n % 100 == 0)
      logf("guard GameplayLog: event for user %llx left out (#%ld): the user has no gameplay log record", key, n);
    return;
  }
  g_glOrig(manager, event);
}

// ---- TorqueScale ---------------------------------------------------------------------------------------------------
// 14105E530 sums TorqueScale from three rowsets and releases each one; the only slots called on an empty one are
// +0x148 (row count) and +0x18 (release).
typedef void (*ts_fn)(float *, void **, void **, void **);
static ts_fn g_tsOrig;
static int ts_zero(void *self) { (void)self; return 0; }
static void *const g_tsEmptyVt[42] = { [3] = (void *)ts_zero, [41] = (void *)ts_zero };
static void *const *const g_tsEmpty = g_tsEmptyVt;

static void ts_hook(float *out, void **a, void **b, void **c) {
  void *empty = (void *)&g_tsEmpty;
  if (!g_guardFirst[2]) { g_guardFirst[2] = 1; logf("guard TorqueScale: first call"); }
  if (!a || !*a || !b || !*b || !c || !*c) {
    LONG n = InterlockedIncrement(&g_tsMissing);
    if (n <= 3 || n % 100 == 0) logf("guard TorqueScale: an upgrade rowset is missing (#%ld), read as empty", n);
  }
  g_tsOrig(out, a && *a ? a : &empty, b && *b ? b : &empty, c && *c ? c : &empty);
}

// ---- TorqueLoad ----------------------------------------------------------------------------------------------------
// Validates the car's resolved engine -> camshaft -> torque curve before the preloader (140FCFE70) builds physics.
// An invalid one sets the car's placeholder flag (info+0x2ED), which sends the preloader down its own
// unavailable-car branch: it swaps in NUL_Car_00 with its stock parts. CParts: two header words and 43 part ids.
typedef void (*tl_preload_fn)(void *, void **);
typedef void *(*tl_ctor_fn)(void *);
typedef void (*tl_stock_fn)(void *, void **, unsigned);
typedef void **(*tl_part_fn)(void **, void **, int, unsigned, unsigned);
static tl_preload_fn g_tlOrig;
static tl_ctor_fn g_tlPartsCtor;
static tl_stock_fn g_tlStock;
static tl_part_fn g_tlCarPart, g_tlEnginePart;
typedef struct { const char *why; unsigned detail; } tl_fault;

// A borrowed MSVC 2012 std::string (the game's query methods don't keep or free it).
typedef struct { union { char small[16]; const char *ptr; } text; size_t size, capacity; } tl_key;
static tl_key tl_key_for(const char *text) {
  tl_key key; memset(&key, 0, sizeof key);
  key.size = strlen(text);
  if (key.size < 16) { memcpy(key.text.small, text, key.size + 1); key.capacity = 15; }
  else { key.text.ptr = text; key.capacity = key.size; }
  return key;
}
static void *tl_method(void *obj, unsigned off) { return (*(void ***)obj)[off / 8]; }
static void tl_addref(void *obj) { ((void (*)(void *))tl_method(obj, 0x10))(obj); }
static void tl_release(void *obj) { if (obj) ((void (*)(void *))tl_method(obj, 0x18))(obj); }
static int tl_rows(void *row) { return row ? ((int (*)(void *))tl_method(row, 0x148))(row) : 0; }
static unsigned tl_uint(void *row, const char *field) {
  tl_key key = tl_key_for(field);
  return ((unsigned (*)(void *, const tl_key *))tl_method(row, 0x110))(row, &key);
}
static void *tl_query(void *db, const char *sql) {
  void *row = NULL; tl_key key = tl_key_for(sql);
  ((void **(*)(void *, void **, const tl_key *))tl_method(db, 0x48))(db, &row, &key);
  return row;
}
static void *tl_part(void *db, tl_part_fn fn, int slot, unsigned owner, unsigned id) {
  void *row = NULL, *ref = db;
  tl_addref(db);                 // the game's helpers consume the reference passed in
  fn(&row, &ref, slot, owner, id);
  return row;
}

// The same ids and ownership checks as the game's physics constructor (141056210). Electric motors have their own
// curve. Covers missing/empty/one-sample/oversized ICE curves.
static int tl_parts_valid(void *db, unsigned car, const unsigned parts[45], tl_fault *f) {
  void *engine = tl_part(db, g_tlCarPart, 0, car, parts[2]), *cam = NULL, *curve = NULL;
  int ok = 0;
  if (tl_rows(engine) > 0) {
    unsigned engineId = tl_uint(engine, "EngineID");
    cam = tl_part(db, g_tlEnginePart, 12, engineId, parts[14]);
    if (tl_rows(cam) > 0) {
      char sql[96];
      unsigned curveId = tl_uint(cam, "TorqueCurveFullThrottleID");
      snprintf(sql, sizeof sql, "SELECT * FROM List_TorqueCurve WHERE TorqueCurveID=%u", curveId);
      f->detail = curveId;
      curve = tl_query(db, sql);
      if (tl_rows(curve) > 0) {
        unsigned count = tl_uint(curve, "NumTorqueValues");
        ok = count >= 2 && count <= 246;
        f->why = "the torque curve has an unusable sample count"; f->detail = count;
      } else f->why = "the camshaft's torque curve doesn't exist";
    } else { f->why = "the engine's camshaft doesn't exist"; f->detail = parts[14]; }
  } else {
    void *motor = tl_part(db, g_tlCarPart, 3, car, parts[5]);
    ok = tl_rows(motor) > 0;
    tl_release(motor);
    f->why = "neither the engine nor an electric motor exists for the car"; f->detail = parts[2];
  }
  tl_release(curve); tl_release(cam); tl_release(engine);
  return ok;
}

static void tl_stock_parts(void *db, unsigned car, unsigned parts[45]) {
  void *ref = db;
  g_tlPartsCtor(parts);
  tl_addref(db);
  g_tlStock(parts, &ref, car);
}

static void tl_hook(void *loader, void **jobRef) {
  void *job = NULL, *vehicle = NULL, *single = NULL, *db = NULL;
  unsigned car = 0, parts[45];
  BYTE stock = 0, placeholder = 0, *info;
  const char *unread = NULL;
  tl_fault f = { "", 0 };
  if (!rd(jobRef, &job, 8) || !job || !rd((BYTE *)job + 0x28, &vehicle, 8) || !vehicle) {
    logf("guard TorqueLoad: a car load without a readable job or vehicle, left out");
    return;
  }
  info = (BYTE *)vehicle + 0x110;
  if (!rd(info + 8, &car, 4)) unread = "the car id";
  else if (!rd(info + 0x10, parts, sizeof parts)) unread = "the part list";
  else if (!rd(info + 0x175, &stock, 1)) unread = "the stock-parts flag";
  else if (!rd(info + 0x2ED, &placeholder, 1)) unread = "the placeholder flag";
  else if (!rd(GAME_BASE + 0x354F4F8, &single, 8) || !single) unread = "the game database";
  else if (!rd((BYTE *)single + 0x40, &db, 8) || !db) unread = "the game database object";
  if (unread) {
    *((BYTE *)job + 0x1C0) = 1;  // the load's cancel flag: the worker completes it in its own cancelled branch
    LONG n = InterlockedIncrement(&g_tlCancels);
    if (n <= 5 || n % 50 == 0) logf("guard TorqueLoad: car load cancelled (#%ld): %s can't be read (car %u)", n, unread, car);
    return;
  }
  if (stock) tl_stock_parts(db, car, parts);
  int reject = !tl_parts_valid(db, car, parts, &f);
  if (g_guardTestCar && car == g_guardTestCar && !reject) { reject = 1; f.why = "TestRejectCar"; f.detail = car; }
  if (g_guardFirst[3] < 12) { g_guardFirst[3]++; logf("guard TorqueLoad: car load checked (car %u: %s)", car, reject ? "invalid" : "ok"); }
  if (reject || placeholder) {
    unsigned fb[45]; tl_fault ff = { "", 0 };
    tl_stock_parts(db, 1215, fb);   // NUL_Car_00
    if (!tl_parts_valid(db, 1215, fb, &ff)) {
      *((BYTE *)job + 0x1C0) = 1;
      logf("guard TorqueLoad: car load cancelled: the stand-in car is unusable too (%s, %u); wanted car %u", ff.why, ff.detail, car);
      return;
    }
  }
  if (reject) {
    LONG n = InterlockedIncrement(&g_tlRejects);
    info[0x2ED] = 1;
    if (n <= 50 || n % 10 == 0)
      logf("guard TorqueLoad: car %u (engine part %u, camshaft part %u%s) has invalid physics data (#%ld): %s (%u); "
           "loaded as the game's stand-in car", car, parts[2], parts[14], stock ? ", stock parts" : "", n, f.why, f.detail);
  }
  g_tlOrig(loader, jobRef);
  if (reject) {
    // The preloader keeps the old car's id in +0x2F0 and would restore its physics; -1 stops that for a rejected car.
    unsigned none = 0xFFFFFFFFu;
    memcpy(info + 0x2F0, &none, 4);
  }
}

static int guard_torque_load(void) {
  static const struct { ULONG64 va; BYTE b[16]; } sig[] = {
    { 0x141042D10, { 0x48,0xC7,0x01,0xFF,0xFF,0xFF,0xFF,0x33,0xD2,0x4C,0x8D,0x41,0x08,0x0F,0x1F,0x00 } },
    { 0x14104D5F0, { 0x44,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41 } },
    { 0x1410482F0, { 0x48,0x8B,0xC4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x57,0x41,0x55,0x41 } },
    { 0x141048990, { 0x48,0x8B,0xC4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x57,0x41,0x55,0x41 } },
  };
  static const BYTE pre[16] = { 0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24,0x80 };
  for (unsigned i = 0; i < sizeof sig / sizeof sig[0]; i++)
    if (!guard_code_is(GAME_BASE + (sig[i].va - 0x140000000ULL), sig[i].b, 16, "TorqueLoad (part resolver)")) return 0;
  g_tlPartsCtor = (tl_ctor_fn)(GAME_BASE + 0x1042D10);
  g_tlStock = (tl_stock_fn)(GAME_BASE + 0x104D5F0);
  g_tlCarPart = (tl_part_fn)(GAME_BASE + 0x10482F0);
  g_tlEnginePart = (tl_part_fn)(GAME_BASE + 0x1048990);
  return detour(0x140FCFE70, pre, 16, 5, (void *)tl_hook, (void **)&g_tlOrig, "guard TorqueLoad");   // push rbp/rbx/rsi/rdi
}

// Logs the RemotePrediction guard's hits (its stub only counts them).
static DWORD WINAPI guard_watch(void *arg) {
  (void)arg;
  LONG seen = 0;
  for (;;) {
    Sleep(2000);
    LONG n = g_rpHits;
    if (n != seen) {
      if (seen == 0 || n - seen >= 1) logf("guard RemotePrediction: %ld car(s) without a user in the session so far (skipped instead of a crash)", n);
      seen = n;
    }
  }
  return 0;
}

static void crash_guards(void) {
  if (!g_guards) { logf("crash guards off ([guards] Enabled=0)"); return; }
  guard_remote_prediction();
  {
    static const BYTE want[8] = { 0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x54, 0x41 };     // mov rax,rsp ; push rsi ; push rdi
    detour(0x1409C88E0, want, 8, 5, (void *)gl_handler, (void **)&g_glOrig, "guard GameplayLog");
  }
  {
    static const BYTE want[10] = { 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18 };   // mov [rsp+20],r9
    detour(0x14105E530, want, 10, 5, (void *)ts_hook, (void **)&g_tsOrig, "guard TorqueScale");
  }
  guard_torque_load();
  if (g_guardTestForce || g_guardTestCar) logf("crash guards: TEST switches on (TestForce=%d, TestRejectCar=%u)", g_guardTestForce, g_guardTestCar);
  CloseHandle(CreateThread(NULL, 0, guard_watch, NULL, 0, NULL));
}
