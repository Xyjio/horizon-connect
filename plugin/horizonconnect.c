// Horizon Connect - HorizonConnect.dll: brings Forza Horizon 2's online free roam back for players who host it themselves.
//
// How it is loaded: as an Extra Options plugin (FH2PluginInitialize, Azimuth 1.0.4, mods\Horizon Connect\plugins\).
// The older way still works for Azimuth 0.9.x: as data\bin\azimuth-tls.dll in front of the original one (renamed
// azimuth-tls-next.dll), whose exports it forwards (horizonconnect.def).
//
// What it does: hooks the game's imports and a few of Azimuth's WinRT answers so the game's Xbox Live calls get
// usable answers (fake tokens, QoS, party, session association - see the [net]/[xbl] settings below), sends its
// web calls to the local server (server/fh2srv.py) and its game traffic (UDP) to a relay (server/relay.py), starts
// that server and the playit.gg tunnel (online.c), and adds the HORIZON CONNECT pages to the pause menu (menu.c).
//
// Based on FH2Net by conner-mcvay (MIT). Built together with menu.c, this DLL is GPL-3.0 (see LICENSE, NOTICE.md).
// Game addresses are for Forza Horizon 2 1.0.0.64.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#define GAME_BASE ((BYTE *)0x140000000ULL)

static CRITICAL_SECTION g_logLock;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static LARGE_INTEGER g_freq, g_t0;

static void logf(const char *fmt, ...) {
  char buf[2048];
  LARGE_INTEGER t; QueryPerformanceCounter(&t);
  int n = snprintf(buf, sizeof buf, "%9.3f [%5lu] ", (double)(t.QuadPart - g_t0.QuadPart) / g_freq.QuadPart, GetCurrentThreadId());
  va_list ap; va_start(ap, fmt);
  int m = vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
  va_end(ap);
  if (m < 0) m = 0;
  n += m; if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
  buf[n++] = '\r'; buf[n++] = '\n';
  EnterCriticalSection(&g_logLock);
  DWORD w; WriteFile(g_log, buf, n, &w, NULL);
  LeaveCriticalSection(&g_logLock);
}

static void guidstr(const GUID *g, char *out) {
  if (!g) { strcpy(out, "(null)"); return; }
  sprintf(out, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g->Data1, g->Data2, g->Data3,
          g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static void addrstr(const struct sockaddr *sa, char *out, size_t cap) {
  if (!sa) { snprintf(out, cap, "(null)"); return; }
  if (sa->sa_family == AF_INET) {
    const struct sockaddr_in *a = (const void *)sa; const BYTE *b = (const BYTE *)&a->sin_addr;
    snprintf(out, cap, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], ntohs(a->sin_port));
  } else if (sa->sa_family == AF_INET6) {
    const struct sockaddr_in6 *a = (const void *)sa; const BYTE *b = a->sin6_addr.s6_addr;
    snprintf(out, cap, "[%02x%02x:%02x%02x:..:%02x%02x:%02x%02x]:%u", b[0], b[1], b[2], b[3], b[12], b[13], b[14], b[15], ntohs(a->sin6_port));
  } else snprintf(out, cap, "family %d", sa->sa_family);
}

// ---- hooks -------------------------------------------------------------------------------------

typedef HRESULT (WINAPI *CoCreateInstance_t)(REFCLSID, void *, DWORD, REFIID, LPVOID *);
static CoCreateInstance_t o_CoCreateInstance;
#include "xhr.c"

static HRESULT WINAPI h_CoCreateInstance(REFCLSID c, void * o, DWORD ctx, REFIID i, LPVOID *pv) {
  HRESULT hr;
  if (!xhr_create(c, i, pv, &hr)) hr = o_CoCreateInstance(c, o, ctx, i, pv);
  char a[40], b[40]; guidstr(c, a); guidstr(i, b);
  logf("CoCreateInstance clsid {%s} iid {%s} -> 0x%08lx obj %p ret %p", a, b, hr, pv ? *pv : 0, __builtin_return_address(0));
  return hr;
}

// ---- settings (HorizonConnect.ini beside the DLL) -------------------------------------------------------

static wchar_t g_ini[MAX_PATH];
static int g_netLevel;   // [net] ConnectivityLevel: 0 = Azimuth's answer; 3 = internet; 4 = Xbox Live access

// ---- Windows.Networking.Connectivity.NetworkInformation ----------------------------------------
// With ConnectivityLevel set, the statics are wrapped: GetInternetConnectionProfile answers a profile of
// that level; every other call goes to Azimuth's object.

static const GUID IID_INetworkInformationStatics = { 0x5074f851, 0x950d, 0x4165, { 0x9c, 0x15, 0x36, 0x56, 0x19, 0x48, 0x1e, 0xea } };
static const GUID IID_IConnectionProfile = { 0x71ba143c, 0x598e, 0x49d0, { 0x84, 0xeb, 0x8f, 0xeb, 0xae, 0xdc, 0xc1, 0x95 } };
static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IInspectable_ = { 0xaf86e2e0, 0xb12d, 0x11d0, { 0x8a, 0x97, 0x00, 0xa0, 0xc9, 0x0b, 0xb5, 0xde } };

typedef struct { void **vtbl; } Obj;
#define VCALL(o, slot, T) ((T)((Obj *)(o))->vtbl[slot])

// A fake ConnectionProfile: a static object (no refcount) answering the connectivity level.
static HRESULT STDMETHODCALLTYPE prof_QI(void *t, const GUID *iid, void **out) {
  if (IsEqualGUID(iid, &IID_IConnectionProfile) || IsEqualGUID(iid, &IID_IUnknown_) || IsEqualGUID(iid, &IID_IInspectable_)) { *out = t; return S_OK; }
  char b[40]; guidstr(iid, b); logf("fake ConnectionProfile QI {%s}: no", b);
  *out = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE prof_AddRef(void *t) { (void)t; return 2; }
static ULONG STDMETHODCALLTYPE prof_Release(void *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE prof_GetIids(void *t, ULONG *n, GUID **iids) { (void)t; *n = 0; *iids = NULL; return S_OK; }
static HRESULT STDMETHODCALLTYPE prof_GetRuntimeClassName(void *t, void **s) { (void)t; *s = NULL; return S_OK; }
static HRESULT STDMETHODCALLTYPE prof_GetTrustLevel(void *t, int *l) { (void)t; *l = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE prof_ProfileName(void *t, void **s) { (void)t; *s = NULL; return S_OK; }  // empty HSTRING
static HRESULT STDMETHODCALLTYPE prof_Level(void *t, int *l) { (void)t; *l = g_netLevel; logf("fake ConnectionProfile level -> %d", g_netLevel); return S_OK; }
static HRESULT STDMETHODCALLTYPE prof_NotImpl(void *t) { (void)t; logf("fake ConnectionProfile: unimplemented call"); return E_NOTIMPL; }
static void *g_profVtbl[] = { prof_QI, prof_AddRef, prof_Release, prof_GetIids, prof_GetRuntimeClassName, prof_GetTrustLevel,
  prof_ProfileName, prof_Level, prof_NotImpl, prof_NotImpl, prof_NotImpl, prof_NotImpl, prof_NotImpl, prof_NotImpl, prof_NotImpl };
static Obj g_profile = { g_profVtbl };

// The statics wrapper: slots 0-13 of INetworkInformationStatics, forwarded except 7 (GetInternetConnectionProfile).
typedef struct { void **vtbl; Obj *real; LONG refs; } NetStatics;
#define NS(t) ((NetStatics *)(t))
static HRESULT STDMETHODCALLTYPE ns_QI(void *t, const GUID *iid, void **out) {
  if (IsEqualGUID(iid, &IID_INetworkInformationStatics) || IsEqualGUID(iid, &IID_IUnknown_) || IsEqualGUID(iid, &IID_IInspectable_)) {
    InterlockedIncrement(&NS(t)->refs); *out = t; return S_OK;
  }
  return VCALL(NS(t)->real, 0, HRESULT (STDMETHODCALLTYPE *)(void *, const GUID *, void **))(NS(t)->real, iid, out);
}
static ULONG STDMETHODCALLTYPE ns_AddRef(void *t) { return InterlockedIncrement(&NS(t)->refs); }
static ULONG STDMETHODCALLTYPE ns_Release(void *t) { return InterlockedDecrement(&NS(t)->refs); }  // never freed: the game may cache it
#define NS_FWD(slot) static HRESULT STDMETHODCALLTYPE ns_##slot(void *t, void *a, void *b, void *c) { \
  return VCALL(NS(t)->real, slot, HRESULT (STDMETHODCALLTYPE *)(void *, void *, void *, void *))(NS(t)->real, a, b, c); }
NS_FWD(3) NS_FWD(4) NS_FWD(5) NS_FWD(6) NS_FWD(8) NS_FWD(9) NS_FWD(10) NS_FWD(11) NS_FWD(12) NS_FWD(13)
static HRESULT STDMETHODCALLTYPE ns_GetInternetConnectionProfile(void *t, void **out) {
  void *realProf = NULL;
  HRESULT hr = VCALL(NS(t)->real, 7, HRESULT (STDMETHODCALLTYPE *)(void *, void **))(NS(t)->real, &realProf);
  int lvl = -1;
  if (SUCCEEDED(hr) && realProf) {
    VCALL(realProf, 7, HRESULT (STDMETHODCALLTYPE *)(void *, int *))(realProf, &lvl);
    VCALL(realProf, 2, ULONG (STDMETHODCALLTYPE *)(void *))(realProf);
  }
  logf("GetInternetConnectionProfile: Azimuth says hr 0x%08lx profile %p level %d; answering a profile of level %d", hr, realProf, lvl, g_netLevel);
  *out = &g_profile;
  return S_OK;
}
static void *g_nsVtbl[] = { ns_QI, ns_AddRef, ns_Release, ns_3, ns_4, ns_5, ns_6, ns_GetInternetConnectionProfile, ns_8, ns_9, ns_10, ns_11, ns_12, ns_13 };

static int wrap_qos(const wchar_t *cls, GUID *iid, void **out);   // qos.c
static int wrap_json(const wchar_t *cls, GUID *iid, void **out);  // json.c
static int wrap_streams(const wchar_t *cls, GUID *iid, void **out);  // proxy.c
static int fix_streams(const wchar_t *cls, GUID *iid, void **out);   // streams.c
static int fix_party(const wchar_t *cls, GUID *iid, void **out);     // party.c
static int fix_association(const wchar_t *cls, GUID *iid, void **out);  // assoc.c
static void game_patches(void);                                         // patches.c
static void xuid_patch(void);                                           // patches.c

// vccorlib110: ?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z (name, iid, out)
typedef HRESULT (*GetAF_t)(void *, GUID *, void **);
static GetAF_t o_GetAF;
static HRESULT h_GetAF(void *name, GUID *iid, void **out) {
  HRESULT hr = o_GetAF(name, iid, out);
  char b[40]; guidstr(iid, b);
  const wchar_t *cls = name;
  if (wcscmp(cls, L"Windows.Xbox.Management.Deployment.PackageTransferManager"))
    logf("ActivationFactory %ls iid {%s} -> 0x%08lx ret %p", cls, b, hr, __builtin_return_address(0));
  if (g_netLevel && SUCCEEDED(hr) && *out && IsEqualGUID(iid, &IID_INetworkInformationStatics)
      && !wcscmp(cls, L"Windows.Networking.Connectivity.NetworkInformation")) {
    NetStatics *w = HeapAlloc(GetProcessHeap(), 0, sizeof *w);
    w->vtbl = g_nsVtbl; w->real = *out; w->refs = 1;
    *out = w;
    logf("  wrapped NetworkInformation statics (level %d)", g_netLevel);
  }
  if (SUCCEEDED(hr)) { wrap_qos(cls, iid, out); wrap_json(cls, iid, out); if (!fix_party(cls, iid, out) && !fix_association(cls, iid, out) && !fix_streams(cls, iid, out)) wrap_streams(cls, iid, out); }
  return hr;
}

typedef void (WINAPI *ODSA_t)(LPCSTR);
typedef void (WINAPI *ODSW_t)(LPCWSTR);
static ODSA_t o_ODSA; static ODSW_t o_ODSW;
static void WINAPI h_ODSA(LPCSTR s) { if (s) logf("dbg %.*s", (int)strcspn(s, "\r\n"), s); o_ODSA(s); }
static void WINAPI h_ODSW(LPCWSTR s) { if (s) logf("dbgW %.*ls", (int)wcscspn(s, L"\r\n"), s); o_ODSW(s); }

typedef int (WSAAPI *getaddrinfo_t)(PCSTR, PCSTR, const ADDRINFOA *, PADDRINFOA *);
static getaddrinfo_t o_getaddrinfo;
static int WSAAPI h_getaddrinfo(PCSTR n, PCSTR s, const ADDRINFOA *h, PADDRINFOA *r) {
  int rc = o_getaddrinfo(n, s, h, r);
  char a[64] = "-"; if (rc == 0 && r && *r) addrstr((*r)->ai_addr, a, sizeof a);
  logf("getaddrinfo %s %s -> %d (%s) ret %p", n ? n : "(null)", s ? s : "", rc, a, __builtin_return_address(0));
  return rc;
}

typedef struct hostent *(WSAAPI *gethostbyname_t)(const char *);
static gethostbyname_t o_gethostbyname;
static struct hostent *WSAAPI h_gethostbyname(const char *n) {
  struct hostent *r = o_gethostbyname(n);
  logf("gethostbyname %s -> %p", n ? n : "(null)", r);
  return r;
}

typedef SOCKET (WSAAPI *socket_t)(int, int, int);
static socket_t o_socket;
static SOCKET WSAAPI h_socket(int af, int type, int proto) {
  SOCKET s = o_socket(af, type, proto);
  logf("socket af %d type %d proto %d -> %lld", af, type, proto, (long long)s);
  return s;
}

typedef SOCKET (WSAAPI *WSASocketW_t)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
static WSASocketW_t o_WSASocketW;
static SOCKET WSAAPI h_WSASocketW(int af, int type, int proto, LPWSAPROTOCOL_INFOW pi, GROUP g, DWORD f) {
  SOCKET s = o_WSASocketW(af, type, proto, pi, g, f);
  logf("WSASocketW af %d type %d proto %d flags 0x%lx -> %lld (err %d)", af, type, proto, f, (long long)s, s == INVALID_SOCKET ? WSAGetLastError() : 0);
  return s;
}

typedef int (WSAAPI *connect_t)(SOCKET, const struct sockaddr *, int);
static connect_t o_connect;
static int WSAAPI h_connect(SOCKET s, const struct sockaddr *sa, int len) {
  int rc = o_connect(s, sa, len);
  char a[64]; addrstr(sa, a, sizeof a);
  logf("connect %lld %s -> %d", (long long)s, a, rc);
  return rc;
}

typedef int (WSAAPI *bind_t)(SOCKET, const struct sockaddr *, int);
static bind_t o_bind;
// [net] GamePort=<port>: the game's UDP socket (5100, manifest SecureConsoleSocket) binds this port instead, so a
// second game on the same PC (its own Azimuth folder) can run beside the first. The relay answers whatever address
// and port a console sends from. 0 or empty: 5100 as always.
static int g_gamePort;
static int WSAAPI h_bind(SOCKET s, const struct sockaddr *sa, int len) {
  struct sockaddr_in6 moved;
  if (g_gamePort && sa && sa->sa_family == AF_INET6 && len >= (int)sizeof moved
      && ntohs(((const struct sockaddr_in6 *)sa)->sin6_port) == 5100) {
    memcpy(&moved, sa, sizeof moved);
    moved.sin6_port = htons((u_short)g_gamePort);
    sa = (const struct sockaddr *)&moved;
    logf("bind: the game's port 5100 moved to %d ([net] GamePort)", g_gamePort);
  }
  // The game's UDP socket is IPv6 ([::]:5100) and Windows makes those IPv6-only: let it reach IPv4 hosts too
  // (as ::ffff:a.b.c.d), so [net] ServerAddress can be a LAN server's IPv4 address.
  if (sa && sa->sa_family == AF_INET6) {
    DWORD off = 0;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof off);
  }
  int rc = o_bind(s, sa, len);
  char a[64]; addrstr(sa, a, sizeof a);
  logf("bind %lld %s -> %d", (long long)s, a, rc);
  return rc;
}

typedef int (WSAAPI *sendto_t)(SOCKET, const char *, int, int, const struct sockaddr *, int);
static sendto_t o_sendto;
static volatile LONG g_sendtoCount;
static int WSAAPI h_sendto(SOCKET s, const char *b, int n, int f, const struct sockaddr *sa, int len) {
  int rc = o_sendto(s, b, n, f, sa, len);
  if (InterlockedIncrement(&g_sendtoCount) <= 50) { char a[64]; addrstr(sa, a, sizeof a); logf("sendto %lld %d bytes %s -> %d", (long long)s, n, a, rc); }
  return rc;
}

typedef int (WSAAPI *WSAStartup_t)(WORD, LPWSADATA);
static WSAStartup_t o_WSAStartup;
static int WSAAPI h_WSAStartup(WORD v, LPWSADATA d) {
  int rc = o_WSAStartup(v, d);
  logf("WSAStartup 0x%04x -> %d", v, rc);
  return rc;
}

// ---- IAT patching ------------------------------------------------------------------------------

typedef struct { const char *dll, *name; void *hook; void **orig; WORD ord; } Hook;  // ord: also match this ordinal import
static Hook g_hooks[] = {
  { "combase.dll", "CoCreateInstance", h_CoCreateInstance, (void **)&o_CoCreateInstance },
  { "vccorlib110.dll", "?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z", h_GetAF, (void **)&o_GetAF },
  { "kernelx.dll", "OutputDebugStringA", h_ODSA, (void **)&o_ODSA },
  { "kernelx.dll", "OutputDebugStringW", h_ODSW, (void **)&o_ODSW },
  { "ws2_32.dll", "getaddrinfo", h_getaddrinfo, (void **)&o_getaddrinfo },
  { "ws2_32.dll", "gethostbyname", h_gethostbyname, (void **)&o_gethostbyname, 52 },
  { "ws2_32.dll", "socket", h_socket, (void **)&o_socket, 23 },
  { "ws2_32.dll", "WSASocketW", h_WSASocketW, (void **)&o_WSASocketW },
  { "ws2_32.dll", "connect", h_connect, (void **)&o_connect, 4 },
  { "ws2_32.dll", "bind", h_bind, (void **)&o_bind, 2 },
  { "ws2_32.dll", "sendto", h_sendto, (void **)&o_sendto, 20 },
  { "ws2_32.dll", "WSAStartup", h_WSAStartup, (void **)&o_WSAStartup, 115 },
};

static int readable(const void *p, SIZE_T n) {
  MEMORY_BASIC_INFORMATION mi;
  if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
  if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
  return (const BYTE *)p + n <= (const BYTE *)mi.BaseAddress + mi.RegionSize;
}

// Patches one module's IAT; returns the number of entries hooked, or -1 while its imports aren't bound yet.
static int patch_iat(BYTE *base, const char *tag) {
  if (!readable(base, 0x1000)) return -1;
  IMAGE_DOS_HEADER *dos = (void *)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
  IMAGE_NT_HEADERS64 *nt = (void *)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;
  IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dd.VirtualAddress || !readable(base + dd.VirtualAddress, dd.Size)) return -1;
  int hooked = 0;
  for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dd.VirtualAddress); d->Name; d++) {
    const char *dll = (const char *)(base + d->Name);
    ULONGLONG *names = (void *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
    void **iat = (void **)(base + d->FirstThunk);
    for (int k = 0; names[k]; k++) {
      int byOrd = IMAGE_SNAP_BY_ORDINAL64(names[k]);
      WORD ord = byOrd ? (WORD)IMAGE_ORDINAL64(names[k]) : 0;
      const char *fn = byOrd ? NULL : (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names[k]))->Name;
      for (size_t h = 0; h < sizeof g_hooks / sizeof *g_hooks; h++) {
        if (_stricmp(dll, g_hooks[h].dll)) continue;
        if (byOrd ? (!g_hooks[h].ord || ord != g_hooks[h].ord) : strcmp(fn, g_hooks[h].name)) continue;
        fn = g_hooks[h].name;
        void *cur = iat[k];
        if (!cur || cur == (void *)names[k]) return -1;  // not bound yet
        if (cur == g_hooks[h].hook) continue;
        if (!*g_hooks[h].orig) *g_hooks[h].orig = cur;
        else if (*g_hooks[h].orig != cur) { logf("skipped %s %s!%s: bound to %p, not %p", tag, dll, fn, cur, *g_hooks[h].orig); continue; }
        DWORD old; VirtualProtect(&iat[k], 8, PAGE_READWRITE, &old);
        iat[k] = g_hooks[h].hook;
        VirtualProtect(&iat[k], 8, old, &old);
        logf("hooked %s %s!%s (was %p)", tag, dll, fn, cur);
        hooked++;
      }
    }
  }
  return hooked;
}

// The package's Xbox Live library, wherever Azimuth mapped it: an image whose export directory names it.
static BYTE *find_module(const char *exportName) {
  MEMORY_BASIC_INFORMATION mi;
  for (BYTE *p = (BYTE *)0x10000; VirtualQuery(p, &mi, sizeof mi); p = (BYTE *)mi.BaseAddress + mi.RegionSize) {
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mi.BaseAddress != mi.AllocationBase) continue;
    BYTE *b = mi.BaseAddress;
    if (mi.RegionSize < 0x1000 || ((IMAGE_DOS_HEADER *)b)->e_magic != IMAGE_DOS_SIGNATURE) continue;
    LONG lfa = ((IMAGE_DOS_HEADER *)b)->e_lfanew;
    if (lfa <= 0 || lfa > 0xE00) continue;
    IMAGE_NT_HEADERS64 *nt = (void *)(b + lfa);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) continue;
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress || !readable(b + dd.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) continue;
    IMAGE_EXPORT_DIRECTORY *ed = (void *)(b + dd.VirtualAddress);
    if (ed->Name && readable(b + ed->Name, 32) && !_stricmp((const char *)(b + ed->Name), exportName)) return b;
  }
  return NULL;
}

#include "trace.c"

// Logs a crash (where, and the return addresses on the stack) to HorizonConnect.log, so it can be looked at afterwards.
static LONG g_crashLogs;
static void crash_where(DWORD64 a, char *out, size_t n) {
  HMODULE m = NULL; wchar_t name[MAX_PATH] = L"?";
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
    GetModuleFileNameW(m, name, MAX_PATH);
    const wchar_t *base = wcsrchr(name, L'\\'); base = base ? base + 1 : name;
    snprintf(out, n, "%ls+%llx", base, a - (DWORD64)m);
  } else snprintf(out, n, "%llx", a);
}
static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *e) {
  DWORD code = e->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
      code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION && code != 0xC0000409) return EXCEPTION_CONTINUE_SEARCH;
  if (InterlockedIncrement(&g_crashLogs) > 20) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT *c = e->ContextRecord; char where[160];
  crash_where(c->Rip, where, sizeof where);
  logf("EXCEPTION %08lx at %s (rip %llx) info %llx %llx; rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx rsp %llx",
       code, where, c->Rip, e->ExceptionRecord->NumberParameters > 0 ? (DWORD64)e->ExceptionRecord->ExceptionInformation[0] : 0,
       e->ExceptionRecord->NumberParameters > 1 ? (DWORD64)e->ExceptionRecord->ExceptionInformation[1] : 0,
       c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R8, c->R9, c->Rsp);
  if (code != EXCEPTION_STACK_OVERFLOW) {
    int shown = 0;
    for (DWORD64 *sp = (DWORD64 *)c->Rsp; shown < 24 && (DWORD64)sp < c->Rsp + 0x1000; sp++) {
      if (!readable(sp, 8)) break;
      DWORD64 v = *sp; HMODULE m = NULL;
      if (v > 0x140001000ULL && v < 0x143a18000ULL) {   // the game image (mapped by Azimuth, not a module)
        BYTE *r = (BYTE *)v;
        if (readable(r - 6, 6) && (r[-5] == 0xE8 || (r[-6] == 0xFF && r[-5] == 0x15) || (r[-2] == 0xFF && (r[-1] & 0xF8) == 0xD0) || (r[-3] == 0xFF && (r[-2] & 0x38) == 0x10))) {
          logf("    stack +%llx: game+%llx", (DWORD64)sp - c->Rsp, v - 0x140000000ULL); shown++;
        }
        continue;
      }
      if (v < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &m)) continue;
      crash_where(v, where, sizeof where); logf("    stack +%llx: %s", (DWORD64)sp - c->Rsp, where); shown++;
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#include "input.c"

// A module's bound import, by name (to call Azimuth's own combase, whose HSTRINGs the game understands).
static void *iat_lookup(BYTE *base, const char *dllName, const char *fnName) {
  IMAGE_NT_HEADERS64 *nt = (void *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
  IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dd.VirtualAddress); d->Name; d++) {
    if (_stricmp((const char *)(base + d->Name), dllName)) continue;
    ULONGLONG *names = (void *)(base + d->OriginalFirstThunk);
    for (int k = 0; names[k]; k++)
      if (!IMAGE_SNAP_BY_ORDINAL64(names[k]) && !strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + names[k]))->Name, fnName))
        return ((void **)(base + d->FirstThunk))[k];
  }
  return NULL;
}

// ---- real sockets -----------------------------------------------------------------------------------
// Azimuth answers the game's Winsock imports with offline stubs (WSASocketW fails with WSAENETDOWN, and
// bind / connect / sendto share one stub), so the game's network transport never starts. With
// [net] RealSockets=1 every ws2_32 import of the module goes to Windows' own ws2_32; where the DLL logs a
// call, its hook stays in the IAT and forwards to the real function.
static int g_realSockets;
static void real_sockets(BYTE *base, const char *tag) {
  HMODULE ws = LoadLibraryW(L"ws2_32.dll");
  IMAGE_NT_HEADERS64 *nt = (void *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
  IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  int n = 0;
  for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dd.VirtualAddress); d->Name; d++) {
    if (_stricmp((const char *)(base + d->Name), "ws2_32.dll")) continue;
    ULONGLONG *names = (void *)(base + d->OriginalFirstThunk);
    void **iat = (void **)(base + d->FirstThunk);
    for (int k = 0; names[k]; k++) {
      void *real = IMAGE_SNAP_BY_ORDINAL64(names[k])
        ? (void *)GetProcAddress(ws, MAKEINTRESOURCEA(IMAGE_ORDINAL64(names[k])))
        : (void *)GetProcAddress(ws, (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names[k]))->Name);
      if (!real) continue;
      int hooked = 0;
      for (size_t h = 0; h < sizeof g_hooks / sizeof *g_hooks; h++)
        if (iat[k] == g_hooks[h].hook) { *g_hooks[h].orig = real; hooked = 1; }
      if (!hooked) {
        DWORD old; VirtualProtect(&iat[k], 8, PAGE_READWRITE, &old);
        iat[k] = real;
        VirtualProtect(&iat[k], 8, old, &old);
      }
      n++;
    }
  }
  logf("real sockets: %d ws2_32 imports of %s go to Windows' ws2_32", n, tag);
}

// ---- Xbox Live tokens --------------------------------------------------------------------------
// Azimuth's LocalUser answers GetTokenAndSignatureAsync (vtable slots 16-18) and LocalUserStatics
// GetTokenAndSignatureForAllUsersAsync (15-17) with an operation already failed with
// ERROR_NETWORK_UNREACHABLE. With [xbl] FakeToken=1 those slots answer an operation already completed
// with a made-up token, which only our own server will ever see.

typedef HRESULT (WINAPI *WindowsCreateString_t)(const wchar_t *, UINT32, void **);
typedef const wchar_t *(WINAPI *WindowsGetStringRawBuffer_t)(void *, UINT32 *);
static WindowsCreateString_t p_WindowsCreateString;
static WindowsGetStringRawBuffer_t p_WindowsGetStringRawBuffer;
static int g_fakeToken;
static wchar_t g_token[512];

static const wchar_t *hs(void *h) { return h && p_WindowsGetStringRawBuffer ? p_WindowsGetStringRawBuffer(h, NULL) : L""; }

static const GUID IID_IAsyncInfo = { 0x00000036, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IAgileObject_ = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

// The result: Token and Signature (and any further string getter) all answer the token.
static HRESULT STDMETHODCALLTYPE res_QI(void *t, const GUID *iid, void **out) {
  if (IsEqualGUID(iid, &IID_IUnknown_) || IsEqualGUID(iid, &IID_IInspectable_) || IsEqualGUID(iid, &IID_IAgileObject_)) { *out = t; return S_OK; }
  char b[40]; guidstr(iid, b); logf("token result QI {%s}: answered as the result interface", b);
  *out = t; return S_OK;
}
static HRESULT STDMETHODCALLTYPE res_String(void *t, void **out) { (void)t; return p_WindowsCreateString(g_token, (UINT32)wcslen(g_token), out); }
static void *g_resVtbl[] = { res_QI, prof_AddRef, prof_Release, prof_GetIids, prof_GetRuntimeClassName, prof_GetTrustLevel,
  res_String, res_String, res_String, res_String, res_String, res_String };
static Obj g_result = { g_resVtbl };

// The completed operation: IAsyncOperation<GetTokenAndSignatureResult^> at +0, IAsyncInfo at +8.
typedef struct { void **vtbl; void **infoVtbl; LONG refs; Obj *handler; Obj *result; const char *what; } TokenOp;  // any completed operation
static TokenOp *op_of_info(void *i) { return (TokenOp *)((BYTE *)i - 8); }
static HRESULT STDMETHODCALLTYPE op_QI(void *t, const GUID *iid, void **out) {
  TokenOp *o = t;
  if (IsEqualGUID(iid, &IID_IAsyncInfo)) { *out = &o->infoVtbl; InterlockedIncrement(&o->refs); return S_OK; }
  if (iid->Data1 == 0x00000003 || iid->Data1 == 0x00000038 || iid->Data1 == 0x00000037) { *out = NULL; return E_NOINTERFACE; }  // IMarshal, IWeakReferenceSource, IWeakReference
  if (!IsEqualGUID(iid, &IID_IUnknown_) && !IsEqualGUID(iid, &IID_IInspectable_) && !IsEqualGUID(iid, &IID_IAgileObject_)) {
    char b[40]; guidstr(iid, b); logf("token operation QI {%s}: answered as the operation", b);
  }
  *out = t; InterlockedIncrement(&o->refs); return S_OK;
}
static ULONG STDMETHODCALLTYPE op_AddRef(void *t) { return InterlockedIncrement(&((TokenOp *)t)->refs); }
static ULONG STDMETHODCALLTYPE op_Release(void *t) {
  TokenOp *o = t; LONG r = InterlockedDecrement(&o->refs);
  if (!r) { if (o->handler) VCALL(o->handler, 2, ULONG (STDMETHODCALLTYPE *)(void *))(o->handler); HeapFree(GetProcessHeap(), 0, o); }
  return r;
}
static HRESULT STDMETHODCALLTYPE op_PutCompleted(void *t, Obj *h) {
  TokenOp *o = t;
  if (!h) return S_OK;
  VCALL(h, 1, ULONG (STDMETHODCALLTYPE *)(void *))(h);
  o->handler = h;
  logf("%s operation: completing", o->what);
  return VCALL(h, 3, HRESULT (STDMETHODCALLTYPE *)(void *, void *, int))(h, o, 1 /* Completed */);
}
static HRESULT STDMETHODCALLTYPE op_GetCompleted(void *t, Obj **h) {
  TokenOp *o = t; *h = o->handler;
  if (o->handler) VCALL(o->handler, 1, ULONG (STDMETHODCALLTYPE *)(void *))(o->handler);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE op_GetResults(void *t, Obj **r) {
  TokenOp *o = t; *r = o->result;
  VCALL(o->result, 1, ULONG (STDMETHODCALLTYPE *)(void *))(o->result);
  logf("%s operation: results taken", o->what); return S_OK;
}
static void *g_opVtbl[] = { op_QI, op_AddRef, op_Release, prof_GetIids, prof_GetRuntimeClassName, prof_GetTrustLevel,
  op_PutCompleted, op_GetCompleted, op_GetResults };

static HRESULT STDMETHODCALLTYPE inf_QI(void *t, const GUID *iid, void **out) { return op_QI(op_of_info(t), iid, out); }
static ULONG STDMETHODCALLTYPE inf_AddRef(void *t) { return op_AddRef(op_of_info(t)); }
static ULONG STDMETHODCALLTYPE inf_Release(void *t) { return op_Release(op_of_info(t)); }
static HRESULT STDMETHODCALLTYPE inf_Id(void *t, UINT32 *id) { (void)t; *id = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE inf_Status(void *t, int *s) { (void)t; *s = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE inf_Error(void *t, HRESULT *e) { (void)t; *e = S_OK; return S_OK; }
static HRESULT STDMETHODCALLTYPE inf_Nop(void *t) { (void)t; return S_OK; }
static void *g_infVtbl[] = { inf_QI, inf_AddRef, inf_Release, prof_GetIids, prof_GetRuntimeClassName, prof_GetTrustLevel,
  inf_Id, inf_Status, inf_Error, inf_Nop, inf_Nop };

static HRESULT token_op(const char *who, void *method, void *url, void **out) {
  logf("%s %ls %ls -> fake token", who, hs(method), hs(url));
  TokenOp *o = HeapAlloc(GetProcessHeap(), 0, sizeof *o);
  o->vtbl = g_opVtbl; o->infoVtbl = g_infVtbl; o->refs = 1; o->handler = NULL; o->result = &g_result; o->what = "token";
  *out = o;
  return S_OK;
}
// (method, url, headers, out); (method, url, headers, bodySize, body, out); (method, url, headers, IBuffer body, out)
static HRESULT STDMETHODCALLTYPE u_tok(void *t, void *m, void *u, void *h, void **out) { (void)t; (void)h; return token_op("User.GetTokenAndSignatureAsync", m, u, out); }
static HRESULT STDMETHODCALLTYPE u_tokArr(void *t, void *m, void *u, void *h, UINT32 n, void *b, void **out) { (void)t; (void)h; (void)n; (void)b; return token_op("User.GetTokenAndSignatureAsync(body)", m, u, out); }
static HRESULT STDMETHODCALLTYPE u_tokBuf(void *t, void *m, void *u, void *h, void *b, void **out) { (void)t; (void)h; (void)b; return token_op("User.GetTokenAndSignatureAsync(buffer)", m, u, out); }
static HRESULT STDMETHODCALLTYPE s_tok(void *t, void *m, void *u, void *h, void **out) { (void)t; (void)h; return token_op("User.GetTokenAndSignatureForAllUsersAsync", m, u, out); }
static HRESULT STDMETHODCALLTYPE s_tokArr(void *t, void *m, void *u, void *h, UINT32 n, void *b, void **out) { (void)t; (void)h; (void)n; (void)b; return token_op("User.GetTokenAndSignatureForAllUsersAsync(body)", m, u, out); }
static HRESULT STDMETHODCALLTYPE s_tokBuf(void *t, void *m, void *u, void *h, void *b, void **out) { (void)t; (void)h; (void)b; return token_op("User.GetTokenAndSignatureForAllUsersAsync(buffer)", m, u, out); }

// An Azimuth build: its LocalUser / LocalUserStatics vtables and the token functions expected in their slots
// 16-18 / 15-17, and where its user id (0x9000000000000) is formatted into the XboxUserId string.
typedef struct { const char *name; DWORD userTable, staticsTable; DWORD userToken[3], staticsToken[3]; DWORD xuidRva; } AzimuthLayout;
static const AzimuthLayout *azimuth_layout(void);

#include "qos.c"
#include "json.c"
#include "proxy.c"
#include "streams.c"
#include "party.c"
#include "assoc.c"
#include "patches.c"
#include "guards.c"

// The Azimuth builds Horizon Connect knows (RVAs in azimuth.exe). A build is recognized by its LocalUser and
// LocalUserStatics vtables holding the expected token functions in the slots it replaces; nothing is patched
// in a build that matches none.
static const AzimuthLayout g_azimuthLayouts[] = {
  { "0.9.7", 0x24FF10, 0x250218, { 0x239B0, 0x239D0, 0x23A20 }, { 0x23EE0, 0x23F00, 0x23F50 }, 0x23855 },
  { "0.9.9", 0x29D718, 0x29D460, { 0x40BF0, 0x40C10, 0x40C60 }, { 0x41150, 0x41170, 0x411C0 }, 0x40A95 },
  { "0.9.10", 0x2B3F60, 0x2B3C88, { 0x42560, 0x42580, 0x425D0 }, { 0x42AC0, 0x42AE0, 0x42B30 }, 0x42405 },
  // Azimuth 1.0.4 (azimuth.exe SHA-256 56d2c9cb...b3b092, 8613376 bytes): LocalUser vtable 6C0B48 (slot 15 is the
  // XboxUserId getter 42150, which formats the movabs at 42185), LocalUserStatics vtable 6C0898; all six token
  // functions call 1A43F0, which answers Azimuth.OfflineTokenRequest with 0x800704CF (ERROR_NETWORK_UNREACHABLE).
  { "1.0.4", 0x6C0B48, 0x6C0898, { 0x422E0, 0x42300, 0x42350 }, { 0x42840, 0x42860, 0x428B0 }, 0x42185 },
};

// LocalUser slots 16-18 and LocalUserStatics slots 15-17: GetTokenAndSignatureAsync and its ...ForAllUsers
// forms (body as string, as array, as buffer).
static void *const g_userTokenFns[3] = { u_tok, u_tokArr, u_tokBuf };
static void *const g_staticsTokenFns[3] = { s_tok, s_tokArr, s_tokBuf };

static const AzimuthLayout *azimuth_layout(void) {
  static const AzimuthLayout *found; static int looked;
  if (looked) return found;
  looked = 1;
  BYTE *az = (BYTE *)GetModuleHandleW(NULL);
  for (size_t k = 0; k < sizeof g_azimuthLayouts / sizeof *g_azimuthLayouts; k++) {
    const AzimuthLayout *l = &g_azimuthLayouts[k];
    void **user = (void **)(az + l->userTable) + 16, **stat = (void **)(az + l->staticsTable) + 15;
    if (!readable(user, 3 * 8) || !readable(stat, 3 * 8)) continue;
    int ok = 1;
    for (int i = 0; i < 3; i++) ok &= user[i] == az + l->userToken[i] && stat[i] == az + l->staticsToken[i];
    if (ok) { found = l; logf("Azimuth %s recognized", l->name); return found; }
  }
  logf("Azimuth build not recognized: token hooks and Xuid can't be applied");
  return NULL;
}

static void install_token_hooks(void) {
  p_WindowsCreateString = iat_lookup(GAME_BASE, "combase.dll", "WindowsCreateString");
  p_WindowsGetStringRawBuffer = iat_lookup(GAME_BASE, "combase.dll", "WindowsGetStringRawBuffer");
  if (!p_WindowsCreateString || !p_WindowsGetStringRawBuffer) { logf("token hooks: the game's HSTRING functions weren't found"); return; }
  const AzimuthLayout *l = azimuth_layout();
  if (!l) return;
  BYTE *az = (BYTE *)GetModuleHandleW(NULL);
  for (int i = 0; i < 6; i++) {
    void **slot = i < 3 ? (void **)(az + l->userTable) + 16 + i : (void **)(az + l->staticsTable) + 15 + (i - 3);
    DWORD old; VirtualProtect(slot, 8, PAGE_READWRITE, &old);
    *slot = i < 3 ? g_userTokenFns[i] : g_staticsTokenFns[i - 3];
    VirtualProtect(slot, 8, old, &old);
  }
  logf("token hooks installed (token \"%ls\")", g_token);
}

// ---- server check ------------------------------------------------------------------------------
// With [http] Server set, the DLL first checks that the server takes a TCP connection ([http] CheckTimeout ms
// per address, default 2000). When it doesn't, it hooks and patches nothing, so the game starts offline as
// with Azimuth alone instead of hanging at load on a server that isn't there. [http] OfflineIfUnreachable=0
// turns the check off. The game doesn't wait for it; the hooks go in once it answers.
static int g_offlineIfUnreachable, g_checkTimeout;
static HANDLE g_checkThread;
static volatile LONG g_serverUp;

static DWORD WINAPI server_check(LPVOID p) {
  (void)p;
  wchar_t host[128]; URL_COMPONENTS uc = { sizeof uc };
  uc.lpszHostName = host; uc.dwHostNameLength = 128;
  if (!WinHttpCrackUrl(g_httpServer, 0, 0, &uc)) { g_serverUp = 1; return 0; }   // xhr_init reports the bad URL
  WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
  wchar_t port[8]; _snwprintf(port, 8, L"%u", uc.nPort);
  ADDRINFOW hints = { 0 }, *ai = NULL;
  hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
  LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
  int up = 0, rc = GetAddrInfoW(host, port, &hints, &ai);
  for (ADDRINFOW *a = rc ? NULL : ai; a && !up; a = a->ai_next) {
    SOCKET s = socket(a->ai_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) continue;
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) up = 1;
    else if (WSAGetLastError() == WSAEWOULDBLOCK) {
      fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
      struct timeval tv = { g_checkTimeout / 1000, (g_checkTimeout % 1000) * 1000 };
      if (select(0, NULL, &w, &e, &tv) == 1 && FD_ISSET(s, &w)) up = 1;
    }
    closesocket(s);
  }
  if (ai) FreeAddrInfoW(ai);
  QueryPerformanceCounter(&t1);
  logf("server check %ls port %u: %s (%.0f ms)", host, uc.nPort, up ? "answers" : rc ? "name not found" : "no answer",
       (double)(t1.QuadPart - t0.QuadPart) * 1000 / g_freq.QuadPart);
  g_serverUp = up;
  return 0;
}

static DWORD WINAPI worker(LPVOID p) {
  (void)p;
  if (g_checkThread && (WaitForSingleObject(g_checkThread, g_checkTimeout * 4 + 5000) != WAIT_OBJECT_0 || !g_serverUp)) {
    logf("the server at %ls isn't reachable: Horizon Connect changes nothing, the game starts offline", g_httpServer);
    return 0;
  }
  logf("waiting for the game image at %p", GAME_BASE);
  int i;
  for (i = 0; i < 600000; i++) {   // up to ~10 minutes at 1 ms
    int n = patch_iat(GAME_BASE, "game");
    if (n >= 0) { logf("game imports bound; %d hooks installed after %d polls", n, i); break; }
    Sleep(1);
  }
  if (g_realSockets) real_sockets(GAME_BASE, "the game");
  if (g_fakeToken) install_token_hooks();
  game_patches();
  crash_guards();
  trace_install(L"Game", GAME_BASE, 0x140000000ULL, "game");
  xhr_init();
  for (i = 0; i < 6000; i++) {     // up to ~10 minutes at 100 ms
    BYTE *x = find_module("Microsoft.Xbox.Services.dll");
    if (x) {
      int n = patch_iat(x, "xsapi");
      if (n >= 0) { logf("Microsoft.Xbox.Services.dll at %p; %d hooks installed", x, n); trace_install(L"Xsapi", x, 0x180000000ULL, "xsapi"); break; }
    }
    Sleep(100);
  }
  return 0;
}

// ---- start-up ------------------------------------------------------------------------------------------------
// Two ways in:
//  - Azimuth 0.9.x: loaded as data\bin\azimuth-tls.dll (DllMain does everything, as FH2Net always did).
//  - Azimuth 1.0.x: Extra Options plugin (mods\<folder>\plugins\HorizonConnect.dll). Azimuth 1.0 has no single
//    azimuth-tls.dll to stand in for any more (it loads azimuth-tls-1.dll, -2.dll as TLS slot modules), so
//    Extra Options loads this DLL with its other plugins, on its loader thread, at the game's first file lookup.
//    Then DllMain does nothing and FH2PluginInitialize starts it.
#define HC_VERSION "1.2.1"
#define HC_VERSION_W L"1.2.1"
static int g_pluginMode;

// Azimuth's folder (where azimuth.exe is), with a trailing backslash.
static void azimuth_folder(wchar_t *out) {
  GetModuleFileNameW(NULL, out, MAX_PATH);
  wchar_t *slash = wcsrchr(out, L'\\'); if (slash) slash[1] = 0;
}

// ---- each player's own identity ([xbl] Xuid=auto, [net] LocalAddress=auto) ------------------------------------------
// Players playing together each need their own Xbox user id and device address. "auto" (the shipped ini) makes
// them once, from a random number, and writes them into HorizonConnect.ini. Azimuth names the save after the id
// (data\titles\1A96545B\saves\player-1\User_<hex id>), so the progress so far (Azimuth's own id's save) is
// copied to the new id's folder once - the original stays as it was.

static void copy_tree(const wchar_t *from, const wchar_t *to) {
  CreateDirectoryW(to, NULL);
  wchar_t pattern[MAX_PATH]; WIN32_FIND_DATAW fd;
  _snwprintf(pattern, MAX_PATH, L"%ls\\*", from);
  HANDLE h = FindFirstFileW(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
    wchar_t a[MAX_PATH], b[MAX_PATH];
    _snwprintf(a, MAX_PATH, L"%ls\\%ls", from, fd.cFileName); _snwprintf(b, MAX_PATH, L"%ls\\%ls", to, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copy_tree(a, b); else CopyFileW(a, b, TRUE);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

static ULONG64 mix64(ULONG64 z) {   // splitmix64
  z += 0x9E3779B97F4A7C15ULL; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// This PC, as a short hash of Windows' MachineGuid ("" if it can't be read). [xbl] Owner keeps the PC the player id
// was made on: a mod folder copied to a friend's PC (with its HorizonConnect.ini) would otherwise give both the same
// id - and the same id twice in a session mixes the two players up. Moving the folder on the same PC keeps it.
static void this_pc(wchar_t *out, int len) {
  wchar_t guid[64]; DWORD size = sizeof guid;
  out[0] = 0;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid", RRF_RT_REG_SZ,
                   NULL, guid, &size) != ERROR_SUCCESS || !guid[0]) return;
  ULONG64 h = 0x48434F4E4E454354ULL;   // "HCONNECT"
  for (const wchar_t *c = guid; *c; c++) h = mix64(h ^ (ULONG64)towlower(*c));
  _snwprintf(out, len, L"%016llx", h);
}

// The device address made from a player id: base64 of "horizon-<id>".
static void device_address(ULONG64 id, wchar_t *out) {
  char raw[48]; int n = snprintf(raw, sizeof raw, "horizon-%llu", id);
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int o = 0;
  for (int i = 0; i < n; i += 3) {
    unsigned v = (unsigned char)raw[i] << 16 | (i + 1 < n ? (unsigned char)raw[i + 1] << 8 : 0) | (i + 2 < n ? (unsigned char)raw[i + 2] : 0);
    out[o++] = b64[v >> 18 & 63]; out[o++] = b64[v >> 12 & 63];
    out[o++] = i + 1 < n ? b64[v >> 6 & 63] : L'='; out[o++] = i + 2 < n ? b64[v & 63] : L'=';
  }
  out[o] = 0;
}

// HorizonConnect.ini as UTF-16 (with a BOM): Windows then keeps any character the player uses (server names, the
// gamertag) - in the shipped ANSI file, WritePrivateProfileString turns everything outside the code page into "?".
// Converted once, from the system's ANSI code page.
static void ini_unicode(void) {
  HANDLE f = CreateFileW(g_ini, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD size = GetFileSize(f, NULL), got = 0;
  char *raw = size != INVALID_FILE_SIZE && size < (1 << 20) ? malloc(size + 1) : NULL;
  int ok = raw && ReadFile(f, raw, size, &got, NULL) && got == size;
  CloseHandle(f);
  if (!ok || (size >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE)) { free(raw); return; }
  int skip = size >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF ? 3 : 0;
  UINT cp = skip ? CP_UTF8 : CP_ACP;
  int n = MultiByteToWideChar(cp, 0, raw + skip, (int)(size - skip), NULL, 0);
  wchar_t *w = malloc(((size_t)n + 1) * sizeof(wchar_t));
  if (w && (n == 0 || MultiByteToWideChar(cp, 0, raw + skip, (int)(size - skip), w + 1, n))) {
    w[0] = 0xFEFF;
    wchar_t tmp[MAX_PATH]; _snwprintf(tmp, MAX_PATH, L"%ls.new", g_ini);
    HANDLE o = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    if (o != INVALID_HANDLE_VALUE) {
      BOOL wrote = WriteFile(o, w, (DWORD)((n + 1) * sizeof(wchar_t)), &put, NULL);
      CloseHandle(o);
      if (wrote && MoveFileExW(tmp, g_ini, MOVEFILE_REPLACE_EXISTING)) logf("settings: HorizonConnect.ini is now Unicode (UTF-16)");
      else DeleteFileW(tmp);
    }
  }
  free(w); free(raw);
}

static void own_identity(void) {
  const ULONG64 azimuth_id = 0x9000000000000ULL;   // 2533274790395904
  ULONG64 earlier = 0;                              // the id this folder had before it came to this PC
  wchar_t pc[24], owner[24];
  this_pc(pc, 24);
  GetPrivateProfileStringW(L"xbl", L"Owner", L"", owner, 24, g_ini);
  if (pc[0] && owner[0] && _wcsicmp(pc, owner) && g_xuid[0] && _wcsicmp(g_xuid, L"auto")) {
    logf("identity: this mod folder was set up on another PC; this PC gets its own player id");
    earlier = wcstoull(g_xuid, NULL, 10);
    wchar_t addr_of_earlier[64]; device_address(earlier, addr_of_earlier);   // was the device address made from it?
    wcscpy(g_xuid, L"auto");
    if (!wcscmp(g_localAddress, addr_of_earlier)) wcscpy(g_localAddress, L"auto");
  }
  if (pc[0] && _wcsicmp(pc, owner)) WritePrivateProfileStringW(L"xbl", L"Owner", pc, g_ini);
  if (!g_xuid[0] || !_wcsicmp(g_xuid, L"auto")) {
    LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
    ULONG64 seed = (ULONG64)qpc.QuadPart ^ (GetTickCount64() << 20) ^ ((ULONG64)GetCurrentProcessId() << 40) ^ (ULONG64)(uintptr_t)&qpc;
    ULONG64 id = azimuth_id + 0x10000 + mix64(seed) % 0xFFFFFFFULL;
    _snwprintf(g_xuid, 32, L"%llu", id);
    WritePrivateProfileStringW(L"xbl", L"Xuid", g_xuid, g_ini);
    logf("identity: new player id %ls", g_xuid);
  }
  ULONG64 id = wcstoull(g_xuid, NULL, 10);
  if (g_pluginMode && id && id != azimuth_id) {
    wchar_t az[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    azimuth_folder(az);
    _snwprintf(from, MAX_PATH, L"%lsdata\\titles\\1A96545B\\saves\\player-1\\User_%llX", az, azimuth_id);
    _snwprintf(to, MAX_PATH, L"%lsdata\\titles\\1A96545B\\saves\\player-1\\User_%llX", az, id);
    if (earlier && earlier != azimuth_id) {   // Azimuth's folder came along too: the progress under the earlier id
      wchar_t prev[MAX_PATH];
      _snwprintf(prev, MAX_PATH, L"%lsdata\\titles\\1A96545B\\saves\\player-1\\User_%llX", az, earlier);
      if (GetFileAttributesW(prev) != INVALID_FILE_ATTRIBUTES) wcscpy(from, prev);
    }
    if (GetFileAttributesW(to) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(from) != INVALID_FILE_ATTRIBUTES) {
      copy_tree(from, to);
      logf("identity: the save so far copied to %ls", to);
    }
  }
  if (!g_localAddress[0] || !_wcsicmp(g_localAddress, L"auto")) {
    device_address(id, g_localAddress);
    WritePrivateProfileStringW(L"net", L"LocalAddress", g_localAddress, g_ini);
    logf("identity: device address %ls", g_localAddress);
  }
}

// [online] Join=<host>:<port>: one address for everything, as a tunnel (playit.gg TCP+UDP) gives it - the server's
// web calls go to http://<host>:<port> and the game's UDP to the same host and port, so it replaces [http] Server,
// [net] ServerAddress and ServerPort. "http://" and a trailing "/" are ignored; [v6]:port works; no port means 5101.
// A ServerAddress that is a host name (not an IP address) is looked up here, IPv4 first: the game's association
// needs an address.
static void apply_join(void) {
  wchar_t join[256], host[256];
  GetPrivateProfileStringW(L"online", L"Join", L"", join, 256, g_ini);
  wchar_t *j = join;
  while (*j == L' ' || *j == L'\t') j++;
  if (!_wcsnicmp(j, L"http://", 7)) j += 7;
  size_t n = wcslen(j);
  while (n && (j[n - 1] == L' ' || j[n - 1] == L'/' || j[n - 1] == L'\r' || j[n - 1] == L'\n')) j[--n] = 0;
  if (*j) {
    int port = 5101;
    wchar_t *colon = NULL;
    if (*j == L'[') {                                   // [v6 address]:port
      wchar_t *close = wcschr(j, L']');
      if (close) { *close = 0; colon = close[1] == L':' ? close + 1 : NULL; j++; }
    } else if (wcschr(j, L':') == wcsrchr(j, L':')) colon = wcsrchr(j, L':');   // host:port (one colon)
    if (colon) { *colon = 0; port = _wtoi(colon + 1); }
    if (port <= 0 || port > 65535) port = 5101;
    wcsncpy(host, j, 255); host[255] = 0;
    if (wcschr(host, L':')) _snwprintf(g_httpServer, 256, L"http://[%ls]:%d", host, port);
    else _snwprintf(g_httpServer, 256, L"http://%ls:%d", host, port);
    g_serverPort = port;
    logf("join: %ls -> web %ls, game port %d", host, g_httpServer, port);
  } else {
    wcsncpy(host, g_serverIp6, 255); host[255] = 0;
  }
  IN6_ADDR a6; IN_ADDR a4;
  if (InetPtonW(AF_INET, host, &a4) == 1 || InetPtonW(AF_INET6, host, &a6) == 1) {
    wcsncpy(g_serverIp6, host, 63); g_serverIp6[63] = 0;
    return;
  }
  WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
  ADDRINFOW hints = { 0 }, *ai = NULL;
  hints.ai_socktype = SOCK_DGRAM;
  if (GetAddrInfoW(host, NULL, &hints, &ai) != 0 || !ai) { logf("join: %ls not found; the game server address stays %ls", host, g_serverIp6); return; }
  ADDRINFOW *pick = ai;
  for (ADDRINFOW *p = ai; p; p = p->ai_next) if (p->ai_family == AF_INET) { pick = p; break; }
  wchar_t s[64];
  if (pick->ai_family == AF_INET) InetNtopW(AF_INET, &((struct sockaddr_in *)pick->ai_addr)->sin_addr, s, 64);
  else InetNtopW(AF_INET6, &((struct sockaddr_in6 *)pick->ai_addr)->sin6_addr, s, 64);
  FreeAddrInfoW(ai);
  wcsncpy(g_serverIp6, s, 63); g_serverIp6[63] = 0;
  logf("join: %ls is %ls", host, g_serverIp6);
}

#include <wincrypt.h>
#include <wctype.h>
#include <ctype.h>
#include <stdlib.h>
#include "online.c"
#include "menu.c"

static void start(const wchar_t *path) {
  InitializeCriticalSection(&g_logLock);
  QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0);
  wchar_t logp[MAX_PATH], next[MAX_PATH];
  _snwprintf(logp, MAX_PATH, L"%lsHorizonConnect.log", path);
  _snwprintf(next, MAX_PATH, L"%lsazimuth-tls-next.dll", path);
  { // keep the previous run's log as HorizonConnect.prev.log (a problem is often noticed only after a restart)
    wchar_t prev[MAX_PATH]; _snwprintf(prev, MAX_PATH, L"%lsHorizonConnect.prev.log", path);
    MoveFileExW(logp, prev, MOVEFILE_REPLACE_EXISTING);
  }
  g_log = CreateFileW(logp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  SYSTEMTIME st; GetLocalTime(&st);
  logf("Horizon Connect " HC_VERSION " loaded %04d-%02d-%02d %02d:%02d:%02d as %s", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
       g_pluginMode ? "an Extra Options plugin (Azimuth 1.0.x)" : "azimuth-tls.dll (Azimuth 0.9.x)");
  _snwprintf(g_ini, MAX_PATH, L"%lsHorizonConnect.ini", path);
  { // FH2 Online 0.x called it fh2net.ini
    wchar_t old[MAX_PATH]; _snwprintf(old, MAX_PATH, L"%lsfh2net.ini", path);
    if (GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES && MoveFileW(old, g_ini)) logf("settings: fh2net.ini renamed to HorizonConnect.ini");
  }
  ini_unicode();
  g_netLevel = GetPrivateProfileIntW(L"net", L"ConnectivityLevel", 0, g_ini);
  g_fakeToken = GetPrivateProfileIntW(L"xbl", L"FakeToken", 0, g_ini);
  GetPrivateProfileStringW(L"xbl", L"Token", L"XBL3.0 x=1234567890;horizonconnect", g_token, 512, g_ini);
  GetPrivateProfileStringW(L"http", L"Server", L"", g_httpServer, 256, g_ini);
  GetPrivateProfileStringW(L"xbl", L"Xuid", L"2533274790395904", g_xuid, 32, g_ini);
  GetPrivateProfileStringW(L"xbl", L"Gamertag", L"", g_gamertag, 64, g_ini);
  if (!g_gamertag[0]) {   // Azimuth's player name, from data\azimuth.ini
    wchar_t az[MAX_PATH], azdir[MAX_PATH];
    if (g_pluginMode) { azimuth_folder(azdir); _snwprintf(az, MAX_PATH, L"%lsdata\\azimuth.ini", azdir); }
    else _snwprintf(az, MAX_PATH, L"%ls..\\azimuth.ini", path);
    GetPrivateProfileStringW(L"launcher", L"player_name", L"Player 1", g_gamertag, 64, az);
  }
  logf("player: xuid %ls, gamertag %ls", g_xuid, g_gamertag);
  g_fakeQos = GetPrivateProfileIntW(L"net", L"FakeQos", 0, g_ini);
  g_debugJson = GetPrivateProfileIntW(L"debug", L"Json", 0, g_ini);
  g_debugStreams = GetPrivateProfileIntW(L"debug", L"Streams", 0, g_ini);
  g_debugParty = GetPrivateProfileIntW(L"debug", L"Party", 0, g_ini);
  g_fixStreams = GetPrivateProfileIntW(L"net", L"FixStreams", 0, g_ini);
  g_realSockets = GetPrivateProfileIntW(L"net", L"RealSockets", 0, g_ini);
  g_fakeParty = GetPrivateProfileIntW(L"net", L"FakeParty", 0, g_ini);
  g_fakeAssociation = GetPrivateProfileIntW(L"net", L"FakeAssociation", 0, g_ini);
  g_skipWeatherWait = GetPrivateProfileIntW(L"net", L"SkipWeatherWait", 0, g_ini);
  g_skipTrafficWait = GetPrivateProfileIntW(L"net", L"SkipTrafficWait", 0, g_ini);
  g_showLeaderHint = GetPrivateProfileIntW(L"online", L"ShowLeaderHint", 0, g_ini);
  g_guards = GetPrivateProfileIntW(L"guards", L"Enabled", 1, g_ini);
  g_guardTestForce = GetPrivateProfileIntW(L"guards", L"TestForce", 0, g_ini);
  g_guardTestCar = GetPrivateProfileIntW(L"guards", L"TestRejectCar", 0, g_ini);
  GetPrivateProfileStringW(L"net", L"ServerAddress", L"::1", g_serverIp6, 64, g_ini);
  g_serverPort = GetPrivateProfileIntW(L"net", L"ServerPort", 5101, g_ini);
  GetPrivateProfileStringW(L"net", L"LocalAddress", L"", g_localAddress, 256, g_ini);
  own_identity();
  g_gamePort = GetPrivateProfileIntW(L"net", L"GamePort", 0, g_ini);
  if (g_gamePort < 0 || g_gamePort > 65535) g_gamePort = 0;
  AddVectoredExceptionHandler(0, crash_handler);
  if (!g_pluginMode) apply_join();   // the plugin chooses the server in its list (online.c)
  g_offlineIfUnreachable = GetPrivateProfileIntW(L"http", L"OfflineIfUnreachable", 1, g_ini);
  g_checkTimeout = GetPrivateProfileIntW(L"http", L"CheckTimeout", 2000, g_ini);
  if (g_pluginMode) online_start(path);
  logf("settings: ConnectivityLevel=%d FakeToken=%d FakeQos=%d FixStreams=%d Server=%ls", g_netLevel, g_fakeToken, g_fakeQos, g_fixStreams, g_httpServer);
  if (g_httpServer[0] && g_offlineIfUnreachable) g_checkThread = CreateThread(NULL, 0, server_check, NULL, 0, NULL);
  if (!g_pluginMode) {
    // The forwarded exports resolve against this module once it is loaded under its own name.
    HMODULE nx = LoadLibraryW(next);
    logf("azimuth-tls-next.dll -> %p (err %lu)", nx, nx ? 0 : GetLastError());
  }
  xuid_patch();
  input_install(path);
  CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID r) {
  (void)r;
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  DisableThreadLibraryCalls(inst);
  wchar_t path[MAX_PATH]; GetModuleFileNameW(inst, path, MAX_PATH);
  wchar_t *slash = wcsrchr(path, L'\\');
  // Only as azimuth-tls.dll does DllMain start everything; any other name is the plugin, started by Extra Options.
  if (!slash || _wcsicmp(slash + 1, L"azimuth-tls.dll")) return TRUE;
  slash[1] = 0;
  start(path);
  return TRUE;
}

// Extra Options plugin entry (FH2_PLUGIN_API_VERSION 1): runs on Extra Options' loader thread after DllMain.
typedef struct {
  DWORD size, api_version, adapter, game_image_size;
  void *game_image;
  HMODULE plugin_module;
  const wchar_t *plugins_directory, *plugin_path;
  const char *extra_options_version;
  void (*log_utf8)(const char *message);
} Fh2PluginContext;

__declspec(dllexport) BOOL WINAPI FH2PluginInitialize(const Fh2PluginContext *c) {
  if (!c || c->size < sizeof *c || c->api_version < 1) return FALSE;
  static LONG once;
  if (InterlockedExchange(&once, 1)) return TRUE;
  if (c->log_utf8) c->log_utf8("HORIZON CONNECT: " HC_VERSION " starting; log in the plugin folder (HorizonConnect.log).\r\n");
  if (c->game_image != (void *)GAME_BASE) {
    if (c->log_utf8) c->log_utf8("HORIZON CONNECT: the game image is not at 0x140000000; the mod stays off.\r\n");
    return FALSE;
  }
  wchar_t path[MAX_PATH];
  _snwprintf(path, MAX_PATH, L"%ls\\", c->plugins_directory);
  g_pluginMode = 1;
  start(path);
  if (!menu_install(path)) logf("menu: the Horizon Connect page is not available");
  logf("Extra Options %s, adapter %lu, game image %p size 0x%lx", c->extra_options_version ? c->extra_options_version : "?",
       c->adapter, c->game_image, c->game_image_size);
  return TRUE;
}
