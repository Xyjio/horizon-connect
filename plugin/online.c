// Included by horizonconnect.c before start(). "Horizon Connect": everything a player needs runs from the mod folder.
//
//   mods\Horizon Connect\plugins\HorizonConnect.dll          this DLL
//   mods\Horizon Connect\plugins\HorizonConnect\python\   the embeddable Python (python.org, PSF licence)
//   mods\Horizon Connect\plugins\HorizonConnect\server\   fh2srv (run_server.py and its modules)
//   mods\Horizon Connect\plugins\HorizonConnect\playit\   playitd.exe (playit.gg agent, BSD-2-Clause)
//
// Every player runs his own local server (started here, hidden, killed with the game through a job object): it
// answers the game's web calls (sign-in, profiles, session directory) on 127.0.0.1. Only the game's UDP traffic
// goes to the session's relay: the player's own (127.0.0.1) or the host's, through the host's playit tunnel.
// So joining someone only changes where the UDP goes ([online] Join, or online_set_join() from the menu).
//
// Hosting (from the menu only): the playit agent is set up once (the browser opens playit's claim page;
// a guest account is fine), its secret is kept in %LOCALAPPDATA%\HorizonConnect\playit-secret.txt (not in the mod
// folder, so sharing the mod never shares it), a UDP tunnel to the local relay is created through playit's API
// if there is none, its address is written to [online] HostAddress and put on the clipboard, and playitd runs
// hidden for as long as the game does.

#include <shellapi.h>

static int g_autoServer, g_localPort = 5101;
static wchar_t g_onlineDir[MAX_PATH];        // ...\plugins\HorizonConnect\ (trailing backslash)
static HANDLE g_job;                          // children die with the game
static volatile LONG g_hostState;             // 0 off, 1 setting up, 2 waiting for the browser, 3 online, -1 failed,
                                              // -2 the agent was deleted on playit.gg
static volatile LONG g_agentDeleted;          // the saved agent key is no longer valid (the host page offers a new one)
static wchar_t g_hostAddress[256];            // the tunnel's public address, once known
static wchar_t g_lastJoin[256];              // the last server joined from the list (shown there)
static wchar_t g_joinAddress[256];            // the relay this console uses ("" = its own)

// ---- small helpers ---------------------------------------------------------------------------------------------

static int tcp_port_open(const char *ip, int port, int timeout_ms) {
  WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return 0;
  struct sockaddr_in a = { 0 };
  a.sin_family = AF_INET; a.sin_port = htons((u_short)port); InetPtonA(AF_INET, ip, &a.sin_addr);
  u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
  int up = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
  if (!up && WSAGetLastError() == WSAEWOULDBLOCK) {
    fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    up = select(0, NULL, &w, &e, &tv) == 1 && FD_ISSET(s, &w);
  }
  closesocket(s);
  return up;
}

static HANDLE job(void) {
  if (!g_job) {
    g_job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = { 0 };
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (g_job) SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
  }
  return g_job;
}

// Starts a hidden child that ends with the game. Returns its process handle or NULL.
static HANDLE spawn_hidden(wchar_t *cmdline, const wchar_t *cwd, const wchar_t *what) {
  STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
  si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
  if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, cwd, &si, &pi)) {
    logf("online: %ls didn't start (error %lu)", what, GetLastError());
    return NULL;
  }
  if (job()) AssignProcessToJobObject(g_job, pi.hProcess);
  ResumeThread(pi.hThread); CloseHandle(pi.hThread);
  logf("online: %ls started (pid %lu)", what, pi.dwProcessId);
  return pi.hProcess;
}

// Parses "host:port", "[v6]:port", "http://host:port/" or "host" (port 5101) into host and port.
static int parse_address(const wchar_t *text, wchar_t *host, int host_len, int *port) {
  wchar_t buf[256]; wcsncpy(buf, text, 255); buf[255] = 0;
  wchar_t *j = buf;
  while (*j == L' ' || *j == L'\t' || *j == L'"') j++;
  if (!_wcsnicmp(j, L"http://", 7)) j += 7;
  size_t n = wcslen(j);
  while (n && (j[n - 1] == L' ' || j[n - 1] == L'/' || j[n - 1] == L'\r' || j[n - 1] == L'\n' || j[n - 1] == L'"' || j[n - 1] == L'\t')) j[--n] = 0;
  if (!*j || n > 200) return 0;
  *port = 5101;
  wchar_t *colon = NULL;
  if (*j == L'[') {
    wchar_t *close = wcschr(j, L']');
    if (!close) return 0;
    *close = 0; colon = close[1] == L':' ? close + 1 : NULL; j++;
  } else if (wcschr(j, L':') == wcsrchr(j, L':')) colon = wcsrchr(j, L':');
  if (colon) { *colon = 0; *port = _wtoi(colon + 1); }
  if (*port <= 0 || *port > 65535) return 0;
  for (wchar_t *c = j; *c; c++)   // a host name or address: letters, digits, '.', '-', ':' (IPv6)
    if (!(iswalnum(*c) || *c == L'.' || *c == L'-' || *c == L':' || *c == L'_')) return 0;
  wcsncpy(host, j, host_len - 1); host[host_len - 1] = 0;
  return 1;
}

// host -> numeric address text in g_serverIp6 (IPv4 preferred). Returns 1 on success.
static int resolve_into_relay(const wchar_t *host) {
  IN6_ADDR a6; IN_ADDR a4;
  if (InetPtonW(AF_INET, host, &a4) == 1 || InetPtonW(AF_INET6, host, &a6) == 1) {
    wcsncpy(g_serverIp6, host, 63); g_serverIp6[63] = 0;
    return 1;
  }
  WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
  ADDRINFOW hints = { 0 }, *ai = NULL;
  hints.ai_socktype = SOCK_DGRAM;
  if (GetAddrInfoW(host, NULL, &hints, &ai) != 0 || !ai) { logf("online: %ls not found", host); return 0; }
  ADDRINFOW *pick = ai;
  for (ADDRINFOW *p = ai; p; p = p->ai_next) if (p->ai_family == AF_INET) { pick = p; break; }
  wchar_t s[64];
  if (pick->ai_family == AF_INET) InetNtopW(AF_INET, &((struct sockaddr_in *)pick->ai_addr)->sin_addr, s, 64);
  else InetNtopW(AF_INET6, &((struct sockaddr_in6 *)pick->ai_addr)->sin6_addr, s, 64);
  FreeAddrInfoW(ai);
  wcsncpy(g_serverIp6, s, 63); g_serverIp6[63] = 0;
  logf("online: %ls is %ls", host, g_serverIp6);
  return 1;
}

// Where this console's game traffic goes: someone's address ("host:port"), or "" for its own relay.
// Takes effect at the next session join (the association is made then). Saved to [online] Join.
static int online_set_join(const wchar_t *address) {
  wchar_t host[256]; int port;
  if (!address || !*address) {
    wcscpy(g_serverIp6, L"127.0.0.1"); g_serverPort = g_localPort; g_joinAddress[0] = 0;
    logf("online: game traffic goes to this PC's own relay (127.0.0.1:%d)", g_localPort);
    return 1;
  }
  if (!parse_address(address, host, 256, &port)) { logf("online: \"%ls\" is not an address", address); return 0; }
  if (!resolve_into_relay(host)) return 0;
  g_serverPort = port;
  _snwprintf(g_joinAddress, 256, L"%ls:%d", host, port);
  wcscpy(g_lastJoin, g_joinAddress);
  WritePrivateProfileStringW(L"online", L"LastJoin", g_joinAddress, g_ini);
  logf("online: game traffic goes to %ls ([%ls]:%d)", g_joinAddress, g_serverIp6, port);
  return 1;
}

static void relay_where(wchar_t *out, int len) {
  _snwprintf(out, len, L"[%ls]:%d", g_serverIp6[0] ? g_serverIp6 : L"127.0.0.1", g_serverPort ? g_serverPort : g_localPort);
}

static int clipboard_set(const wchar_t *text) {
  if (!OpenClipboard(NULL)) return 0;
  EmptyClipboard();
  size_t n = (wcslen(text) + 1) * sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
  int ok = 0;
  if (h) { memcpy(GlobalLock(h), text, n); GlobalUnlock(h); ok = SetClipboardData(CF_UNICODETEXT, h) != NULL; if (!ok) GlobalFree(h); }
  CloseClipboard();
  return ok;
}

static int clipboard_get(wchar_t *out, int len) {
  out[0] = 0;
  if (!OpenClipboard(NULL)) return 0;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) { const wchar_t *p = GlobalLock(h); if (p) { wcsncpy(out, p, len - 1); out[len - 1] = 0; GlobalUnlock(h); } }
  CloseClipboard();
  return out[0] != 0;
}

// ---- the local server ---------------------------------------------------------------------------------------------

static void start_local_server(void) {
  if (tcp_port_open("127.0.0.1", g_localPort, 300)) { logf("online: a server already answers on port %d; using it", g_localPort); return; }
  wchar_t az[MAX_PATH], ini[MAX_PATH], title[64], section[80], game[MAX_PATH];
  azimuth_folder(az);
  _snwprintf(ini, MAX_PATH, L"%lsdata\\azimuth.ini", az);
  GetPrivateProfileStringW(L"launcher", L"game", L"1a96545b", title, 64, ini);
  _snwprintf(section, 80, L"title-%ls", title);
  GetPrivateProfileStringW(section, L"folder", L"", game, MAX_PATH, ini);
  if (!game[0]) { logf("online: the game folder isn't in %ls; no local server", ini); return; }
  wchar_t py[MAX_PATH], srv[MAX_PATH], cmd[4 * MAX_PATH];
  _snwprintf(py, MAX_PATH, L"%lspython\\python.exe", g_onlineDir);
  _snwprintf(srv, MAX_PATH, L"%lsserver", g_onlineDir);
  if (GetFileAttributesW(py) == INVALID_FILE_ATTRIBUTES) { logf("online: %ls is missing; no local server", py); return; }
  // --leader-xuid: when this PC hosts, its own player gets the crown (session leader)
  // [online] OpenToLan=1: the relay also takes consoles from the LAN (by this PC's address); otherwise only this PC
  // and the playit agent reach it
  int lan = GetPrivateProfileIntW(L"online", L"OpenToLan", 0, g_ini);
  _snwprintf(cmd, 4 * MAX_PATH, L"\"%ls\" -I -u \"%ls\\run_server.py\" --game \"%ls\" --port %d --game-port %d --game-host %ls --leader-xuid %llu",
             py, srv, game, g_localPort, g_localPort, lan ? L"::" : L"127.0.0.1", wcstoull(g_xuid, NULL, 10));
  HANDLE p = spawn_hidden(cmd, srv, L"local server");
  if (p) CloseHandle(p);
}

// ---- playit.gg -----------------------------------------------------------------------------------------------------

#define PLAYIT_API L"api.playit.gg"
#define TUNNEL_NAME "Horizon Connect"
#define TUNNEL_NAME_OLD "FH2 Online"          // what 0.x named it: an agent set up then keeps its tunnel and address

// POST https://api.playit.gg<path> with a JSON body; returns a malloc'd UTF-8 reply (or NULL).
static char *playit_post(const wchar_t *path, const char *body, const char *secret) {
  HINTERNET s = WinHttpOpen(L"HorizonConnect/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return NULL;
  HINTERNET c = WinHttpConnect(s, PLAYIT_API, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
  char *out = NULL;
  if (r) {
    wchar_t headers[600];
    if (secret) _snwprintf(headers, 600, L"Content-Type: application/json\r\nAuthorization: Agent-Key %hs\r\n", secret);
    else _snwprintf(headers, 600, L"Content-Type: application/json\r\n");
    if (WinHttpSendRequest(r, headers, (DWORD)-1, (void *)body, (DWORD)strlen(body), (DWORD)strlen(body), 0) && WinHttpReceiveResponse(r, NULL)) {
      size_t len = 0, cap = 4096; out = malloc(cap);
      DWORD got;
      while (out && WinHttpReadData(r, out + len, (DWORD)(cap - len - 1), &got) && got) {
        len += got;
        if (cap - len < 1024) { char *grown = realloc(out, cap *= 2); if (!grown) { free(out); out = NULL; break; } out = grown; }
      }
      if (out) out[len] = 0;
    }
    WinHttpCloseHandle(r);
  }
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return out;
}

// The string value after "key":" at or after `from` (unescaped only for \" and \\), or 0.
static int json_string(const char *json, const char *key, char *out, int len) {
  char pat[64]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
  const char *p = json ? strstr(json, pat) : NULL;
  if (!p) return 0;
  p += strlen(pat);
  int n = 0;
  while (*p && *p != '"' && n < len - 1) { if (*p == '\\' && p[1]) p++; out[n++] = *p++; }
  out[n] = 0;
  return n > 0;
}

// ---- update check -------------------------------------------------------------------------------------------------
// [online] CheckUpdates=1 (default): a few seconds after start, ask GitHub for the newest release. When it is newer
// than this version, the server list shows a row for it (A opens the release page in the browser).
// The release list, not /releases/latest: that one leaves out releases marked as pre-release (and answers 404 when
// all are).
#define HC_RELEASES_API L"/repos/Xyjio/horizon-connect/releases?per_page=20"
#define HC_RELEASES_PAGE L"https://github.com/Xyjio/horizon-connect/releases"
static char g_newVersion[32];             // the newest release's version if it is newer than HC_VERSION, else ""
static wchar_t g_newUrl[256] = HC_RELEASES_PAGE;

// "1.0.10" > "1.0.9": compares the dot-separated numbers in order.
static int version_cmp(const char *a, const char *b) {
  while (*a || *b) {
    long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
    if (x != y) return x < y ? -1 : 1;
    if (*a == '.') a++;
    if (*b == '.') b++;
    if ((*a && !isdigit((unsigned char)*a)) || (*b && !isdigit((unsigned char)*b))) break;   // "-beta" etc.: equal so far
  }
  return 0;
}

static char *https_get(const wchar_t *host, const wchar_t *path) {
  HINTERNET s = WinHttpOpen(L"HorizonConnect/" HC_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return NULL;
  HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
  char *out = NULL;
  if (r) {
    DWORD status = 0, size = sizeof status;
    if (WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)
        && WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)
        && status == 200) {
      size_t len = 0, cap = 8192; out = malloc(cap);
      DWORD got;
      while (out && WinHttpReadData(r, out + len, (DWORD)(cap - len - 1), &got) && got) {
        len += got;
        if (cap - len < 1024) { char *grown = realloc(out, cap *= 2); if (!grown) { free(out); out = NULL; break; } out = grown; }
      }
      if (out) out[len] = 0;
    } else logf("update: GitHub answered %lu", status);
    WinHttpCloseHandle(r);
  }
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return out;
}

static DWORD WINAPI update_thread(LPVOID unused) {
  (void)unused;
  Sleep(8000);                            // not while the game is starting up
  char *reply = https_get(L"api.github.com", HC_RELEASES_API), tag[32], best[32] = "";
  for (const char *p = reply; p && (p = strstr(p, "\"tag_name\":\"")); p++) {   // the highest version in the list
    if (!json_string(p, "tag_name", tag, sizeof tag)) continue;
    const char *v = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
    if (isdigit((unsigned char)*v) && (!best[0] || version_cmp(v, best) > 0)) snprintf(best, sizeof best, "%s", v);
  }
  if (!best[0]) logf("update: no answer from GitHub");
  else if (version_cmp(best, HC_VERSION) > 0) {
    snprintf(g_newVersion, sizeof g_newVersion, "%s", best);
    logf("update: Horizon Connect %s is out (this is " HC_VERSION ") - %ls", best, g_newUrl);
  } else logf("update: up to date (newest release %s)", best);
  free(reply);
  return 0;
}

// Installing it (A on the update row): server\update.py downloads the release ZIP and swaps the changed files in
// (see there); the new version runs from the next start. Not in the job object, so it finishes even if the game
// is closed meanwhile.
static volatile LONG g_updateState;          // 0 not started, 1 installing, 2 installed (restart), 3 restarting, -1 failed

static DWORD WINAPI update_install_thread(LPVOID unused) {
  (void)unused;
  wchar_t cmd[4 * MAX_PATH], srv[MAX_PATH];
  _snwprintf(srv, MAX_PATH, L"%lsserver", g_onlineDir);
  _snwprintf(cmd, 4 * MAX_PATH, L"\"%lspython\\python.exe\" -I -u \"%ls\\update.py\" --mod \"%ls..\\..\" --version " HC_VERSION_W,
             g_onlineDir, srv, g_onlineDir);
  STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
  si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, srv, &si, &pi)) {
    logf("update: the updater didn't start (error %lu)", GetLastError());
    InterlockedExchange(&g_updateState, -1);
    return 0;
  }
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 2; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess);
  logf("update: updater finished with %lu (0 updated, 1 up to date, 2 failed; details in HorizonConnect\\update.log)", code);
  InterlockedExchange(&g_updateState, code == 0 ? 2 : -1);
  return 0;
}

static void update_install(void) {
  if (InterlockedCompareExchange(&g_updateState, 1, 0) == 0 || InterlockedCompareExchange(&g_updateState, 1, -1) == -1)
    CloseHandle(CreateThread(NULL, 0, update_install_thread, NULL, 0, NULL));
}

// The files an update replaced (renamed to *.update-old while they were in use) go at the next start.
static void update_cleanup(void) {
  wchar_t list[MAX_PATH];
  _snwprintf(list, MAX_PATH, L"%lsupdate-old.txt", g_onlineDir);
  HANDLE f = CreateFileW(list, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  char buf[32768]; DWORD got = 0; ReadFile(f, buf, sizeof buf - 1, &got, NULL); CloseHandle(f);
  buf[got] = 0;
  int left = 0, gone = 0;
  for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
    if (!strstr(line, ".update-old") || strstr(line, "..") || strchr(line, ':')) continue;   // only our own leftovers
    wchar_t rel[MAX_PATH], path[2 * MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, line, -1, rel, MAX_PATH)) continue;
    _snwprintf(path, 2 * MAX_PATH, L"%ls..\\..\\%ls", g_onlineDir, rel);
    if (DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND) gone++; else left++;
  }
  if (!left) DeleteFileW(list);
  logf("update: removed %d old files of the last update%s", gone, left ? " (some are still in use)" : "");
}

// ---- restarting the game (after an update) -------------------------------------------------------------------------
// server\restart.py waits until this process has ended and starts Azimuth again straight into the game
// (--skip-launcher --game=<title>). The game is asked to close like with its window's X. It is only ended by force
// when its windows are gone but the process hangs on (20 s): while a window is still open (a question, the game
// saving) nothing is forced - the player closes it, and restart.py waits up to 10 minutes.
static BOOL CALLBACK close_own_window(HWND w, LPARAM pid) {
  DWORD p = 0; GetWindowThreadProcessId(w, &p);
  if (p == (DWORD)pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) PostMessageW(w, WM_CLOSE, 0, 0);
  return TRUE;
}

static BOOL CALLBACK count_own_window(HWND w, LPARAM out) {
  DWORD p = 0; GetWindowThreadProcessId(w, &p);
  if (p == GetCurrentProcessId() && IsWindowVisible(w)) ++*(int *)out;
  return TRUE;
}

static DWORD WINAPI restart_thread(LPVOID unused) {
  (void)unused;
  Sleep(800);                                 // the menu shows "Restarting…" first
  EnumWindows(close_own_window, (LPARAM)GetCurrentProcessId());
  for (int gone = 0;; Sleep(1000)) {
    int open = 0; EnumWindows(count_own_window, (LPARAM)&open);
    gone = open ? 0 : gone + 1;
    if (gone >= 20) break;
  }
  logf("restart: the game's windows are closed but it hangs on; ending it");
  TerminateProcess(GetCurrentProcess(), 0);
  return 0;
}

static int game_restart(void) {
  wchar_t exe[MAX_PATH], az[MAX_PATH], ini[MAX_PATH], title[64], srv[MAX_PATH], cmd[4 * MAX_PATH];
  GetModuleFileNameW(NULL, exe, MAX_PATH);
  azimuth_folder(az);
  _snwprintf(ini, MAX_PATH, L"%lsdata\\azimuth.ini", az);
  GetPrivateProfileStringW(L"launcher", L"game", L"1a96545b", title, 64, ini);
  _snwprintf(srv, MAX_PATH, L"%lsserver", g_onlineDir);
  _snwprintf(cmd, 4 * MAX_PATH, L"\"%lspython\\python.exe\" -I \"%ls\\restart.py\" --pid %lu -- \"%ls\" --skip-launcher --game=%ls",
             g_onlineDir, srv, GetCurrentProcessId(), exe, title);
  STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
  si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
  // not in the game's job (if Azimuth has one): the helper has to outlive the game
  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, NULL, srv, &si, &pi)
      && !CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, srv, &si, &pi)) {
    logf("restart: the helper didn't start (error %lu)", GetLastError());
    return 0;
  }
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  logf("restart: closing the game; %ls starts it again", exe);
  CloseHandle(CreateThread(NULL, 0, restart_thread, NULL, 0, NULL));
  return 1;
}

static void update_check_start(void) {
  update_cleanup();
  if (GetPrivateProfileIntW(L"online", L"CheckUpdates", 1, g_ini)) CloseHandle(CreateThread(NULL, 0, update_thread, NULL, 0, NULL));
}

static void secret_path(wchar_t *out) {
  wchar_t base[MAX_PATH], old[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) wcscpy(base, L".");
  _snwprintf(out, MAX_PATH, L"%ls\\HorizonConnect", base);
  CreateDirectoryW(out, NULL);
  // [online] SecretName (testing two copies of the game on one PC): another file name in the same folder, so the
  // second copy sets up its own agent. Only a plain file name is taken, so the secret never lands in the mod folder.
  wchar_t name[64];
  GetPrivateProfileStringW(L"online", L"SecretName", L"", name, 64, g_ini);
  if (name[0] && !wcspbrk(name, L"\\/:") && !wcsstr(name, L"..")) {
    wcscat(out, L"\\"); wcscat(out, name);
    return;
  }
  wcscat(out, L"\\playit-secret.txt");
  // before the rename (FH2 Online 0.x) it was kept in %LOCALAPPDATA%\FH2Online: take it over, so the agent and
  // its tunnel address stay the same
  _snwprintf(old, MAX_PATH, L"%ls\\FH2Online\\playit-secret.txt", base);
  if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES) CopyFileW(old, out, TRUE);
}

static int load_secret(char *out, int len) {
  wchar_t p[MAX_PATH]; secret_path(p);
  HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return 0;
  DWORD got = 0; ReadFile(f, out, len - 1, &got, NULL); CloseHandle(f);
  out[got] = 0;
  while (got && (out[got - 1] == '\r' || out[got - 1] == '\n' || out[got - 1] == ' ')) out[--got] = 0;
  return got > 10;
}

static void save_secret(const char *secret) {
  wchar_t p[MAX_PATH]; secret_path(p);
  HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD put; WriteFile(f, secret, (DWORD)strlen(secret), &put, NULL); CloseHandle(f);
}

// The claim flow of playit's own CLI: a random code, the claim page in the browser, setup polled until the user
// accepts, then the code exchanged for the agent's secret. Up to 10 minutes.
static int playit_claim(char *secret, int len, const char *agent_type) {
  unsigned char rnd[5]; char code[11];
  HCRYPTPROV prov;
  if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return 0;
  CryptGenRandom(prov, sizeof rnd, rnd); CryptReleaseContext(prov, 0);
  for (int i = 0; i < 5; i++) snprintf(code + 2 * i, 3, "%02x", rnd[i]);
  wchar_t url[96]; _snwprintf(url, 96, L"https://playit.gg/claim/%hs", code);
  logf("playit: claim code %s; opening %ls", code, url);
  ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
  g_hostState = 2;
  char body[160]; snprintf(body, sizeof body, "{\"code\":\"%s\",\"agent_type\":\"%s\",\"version\":\"Horizon Connect " HC_VERSION "\"}", code, agent_type);
  int accepted = 0;
  for (int i = 0; i < 300 && !accepted; i++) {     // ~10 minutes at 2 s
    char *reply = playit_post(L"/claim/setup", body, NULL);
    if (reply) {
      if (strstr(reply, "UserAccepted")) accepted = 1;
      else if (strstr(reply, "UserRejected")) { logf("playit: the claim was rejected in the browser"); free(reply); return 0; }
      if (i < 3 || accepted) logf("playit: claim setup -> %.120s", reply);
      free(reply);
    }
    if (!accepted) Sleep(2000);
  }
  if (!accepted) { logf("playit: the claim wasn't accepted in time"); return 0; }
  snprintf(body, sizeof body, "{\"code\":\"%s\"}", code);
  for (int i = 0; i < 30; i++) {
    char *reply = playit_post(L"/claim/exchange", body, NULL);
    int ok = reply && json_string(reply, "secret_key", secret, len);
    if (reply && (i < 3 || ok)) logf("playit: claim exchange -> %s", ok ? "secret received" : reply);
    free(reply);
    if (ok) { save_secret(secret); return 1; }
    Sleep(2000);
  }
  return 0;
}

// Our tunnel in a /v1/agents/rundata reply ({"agent_id":..., "tunnels":[{..., "name":"Horizon Connect",
// "display_address":"x.ply.gg:123", ...}], "pending":[...]}): 1 with its address, -1 when there is none.
static int rundata_address(const char *run, wchar_t *out, int len) {
  const char *pending = run ? strstr(run, "\"pending\":") : NULL;
  const char *t = run ? strstr(run, "\"name\":\"" TUNNEL_NAME "\"") : NULL;
  if (!t && run) t = strstr(run, "\"name\":\"" TUNNEL_NAME_OLD "\"");
  if (!t || (pending && t > pending)) return -1;
  const char *start = t;                                  // the tunnel object's fields come in any order
  while (start > run && *start != '{') start--;
  char addr[256];
  if (!json_string(start, "display_address", addr, sizeof addr)) return 0;
  _snwprintf(out, len, L"%hs", addr);
  return 1;
}

// Tunnel creation. Which request body playit's API takes from an agent key isn't documented outside the agent's
// generated client, so the known shapes are tried in turn (each answer is logged) until one is accepted.
static int playit_create_tunnel(const char *secret, const char *agent_id) {
  // The request the playit website itself sends (its /v1/tunnels/create: name, protocol, origin, endpoint,
  // enabled), with a raw UDP port whose software is described, on playit's free global network.
  char fields[200], bodies[3][900];
  snprintf(fields, sizeof fields, "{\"fields\":[{\"name\":\"local_ip\",\"value\":\"127.0.0.1\"},{\"name\":\"local_port\",\"value\":\"%d\"}]}", g_localPort);
  const char *proto = "{\"type\":\"raw-ports\",\"details\":{\"port_type\":\"udp\",\"port_count\":1,"
                      "\"software_description\":\"Forza Horizon 2 multiplayer for friends (Horizon Connect mod, game traffic relay)\"}}";
  snprintf(bodies[0], 900, "{\"name\":\"" TUNNEL_NAME "\",\"protocol\":%s,\"origin\":{\"type\":\"agent\",\"data\":{\"agent_id\":\"%s\",\"config\":%s}},"
           "\"endpoint\":{\"type\":\"region\",\"details\":{\"region\":\"global\",\"port\":null}},\"enabled\":true}", proto, agent_id, fields);
  snprintf(bodies[1], 900, "{\"name\":\"" TUNNEL_NAME "\",\"protocol\":%s,\"origin\":{\"type\":\"agent\",\"data\":{\"agent_id\":\"%s\",\"config\":%s}},"
           "\"endpoint\":{\"type\":\"region\",\"details\":{\"region\":\"global\"}},\"enabled\":true}", proto, agent_id, fields);
  snprintf(bodies[2], 900, "{\"name\":\"" TUNNEL_NAME "\",\"protocol\":%s,\"origin\":{\"type\":\"agent\",\"data\":{\"agent_id\":\"%s\",\"config\":%s}},"
           "\"enabled\":true}", proto, agent_id, fields);
  for (int i = 0; i < 3; i++) {
    char *reply = playit_post(L"/v1/tunnels/create", bodies[i], secret);
    logf("playit: create tunnel, try %d -> %.400s", i + 1, reply ? reply : "(no answer)");
    int ok = reply && strstr(reply, "\"status\":\"success\"");
    free(reply);
    if (ok) return 1;
  }
  return 0;
}

static int playit_tunnel(const char *secret, wchar_t *address, int len) {
  int created = 0;
  for (int i = 0; i < 90; i++) {                          // ~3 minutes at 2 s
    char *run = playit_post(L"/v1/agents/rundata", "{}", secret);
    if (run && (i == 0 || i == 10)) logf("playit: rundata -> %.700s", run);
    int found = rundata_address(run, address, len);
    char agent_id[64] = "";
    json_string(run, "agent_id", agent_id, sizeof agent_id);
    free(run);
    if (found == 1) return 1;
    // (a free playit account allows one tunnel: a second agent gets "AgentVersionTooOld" here)
    if (found == -1 && !created && agent_id[0]) {
      created = 1;
      if (!playit_create_tunnel(secret, agent_id)) { logf("playit: no request shape was accepted; see the answers above"); return 0; }
    }
    Sleep(2000);
  }
  return 0;
}

static HANDLE g_agent;

static int start_agent(const char *secret) {
  if (g_agent) { TerminateProcess(g_agent, 0); CloseHandle(g_agent); g_agent = NULL; }
  wchar_t exe[MAX_PATH], dir[MAX_PATH], cmd[2 * MAX_PATH];
  _snwprintf(dir, MAX_PATH, L"%lsplayit", g_onlineDir);
  _snwprintf(exe, MAX_PATH, L"%ls\\playitd.exe", dir);
  if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) { logf("online: %ls is missing", exe); return 0; }
  _snwprintf(cmd, 2 * MAX_PATH, L"\"%ls\" --secret %hs --socket-path \\\\.\\pipe\\horizonconnect-playitd --log-path \"%ls\\playitd.log\"", exe, secret, dir);
  g_agent = spawn_hidden(cmd, dir, L"playit agent");
  return g_agent != NULL;
}

// playit's tunnels for an agent key can only be made by a "self-managed" agent (playit's own app claims
// "assignable" agents, whose tunnels are set up on the website). New claims are self-managed; an older
// assignable agent that can't make the tunnel is replaced by a self-managed one (the browser opens once more).
// playit answers {"status":"error","data":{"type":"auth","message":"InvalidAgentKey"}} for an agent key that was
// deleted on playit.gg (or never existed).
static int agent_key_invalid(const char *secret) {
  char *run = playit_post(L"/v1/agents/rundata", "{}", secret);
  int invalid = run && strstr(run, "InvalidAgentKey");
  free(run);
  return invalid;
}

// At start, when hosting was set up: is the agent still there? Then the host page can say so right away.
static DWORD WINAPI agent_check_thread(LPVOID unused) {
  (void)unused;
  char secret[256];
  if (load_secret(secret, sizeof secret) && agent_key_invalid(secret)) {
    InterlockedExchange(&g_agentDeleted, 1);
    logf("playit: the agent was deleted on playit.gg; the host page offers a new one");
  }
  SecureZeroMemory(secret, sizeof secret);
  return 0;
}

static DWORD WINAPI host_thread(LPVOID unused) {
  (void)unused;
  g_hostState = 1;
  char secret[256];
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!load_secret(secret, sizeof secret) && !playit_claim(secret, sizeof secret, "self-managed")) { g_hostState = -1; return 0; }
    if (agent_key_invalid(secret)) {
      logf("playit: the agent was deleted on playit.gg");
      InterlockedExchange(&g_agentDeleted, 1);
      g_hostState = -2;
      return 0;
    }
    g_hostState = 1;
    if (!start_agent(secret)) { g_hostState = -1; return 0; }
    if (playit_tunnel(secret, g_hostAddress, 256)) {
      WritePrivateProfileStringW(L"online", L"HostAddress", g_hostAddress, g_ini);
      clipboard_set(g_hostAddress);
      g_hostState = 3;
      logf("online: hosting at %ls (copied to the clipboard)", g_hostAddress);
      return 0;
    }
    char *run = playit_post(L"/v1/agents/rundata", "{}", secret);
    int self_managed = run && strstr(run, "\"is_self_managed\":true");
    free(run);
    if (self_managed || attempt) break;
    logf("playit: this agent isn't self-managed; claiming a self-managed one");
    wchar_t p[MAX_PATH]; secret_path(p);
    wchar_t old[MAX_PATH]; _snwprintf(old, MAX_PATH, L"%ls.assignable", p);
    MoveFileExW(p, old, MOVEFILE_REPLACE_EXISTING);
  }
  logf("playit: no tunnel address");
  g_hostState = -1;
  return 0;
}

// One setup at a time: the state is taken (set to 1) before the thread starts, so a second A right away does nothing.
static void online_host(void) {
  LONG s = g_hostState;
  if (s > 0 || InterlockedCompareExchange(&g_hostState, 1, s) != s) return;
  HANDLE t = CreateThread(NULL, 0, host_thread, NULL, 0, NULL);
  if (t) CloseHandle(t); else g_hostState = -1;
}

// "Set up a new one" after the agent was deleted: the old key is put aside (playit-secret.txt.deleted), the old
// address forgotten, and hosting set up again like the first time (claim in the browser).
static void online_new_agent(void) {
  if (g_hostState > 0) return;
  wchar_t p[MAX_PATH], old[MAX_PATH];
  secret_path(p);
  _snwprintf(old, MAX_PATH, L"%ls.deleted", p);
  MoveFileExW(p, old, MOVEFILE_REPLACE_EXISTING);
  g_hostAddress[0] = 0;
  WritePrivateProfileStringW(L"online", L"HostAddress", L"", g_ini);
  InterlockedExchange(&g_agentDeleted, 0);
  InterlockedExchange(&g_hostState, 0);
  logf("playit: setting up a new agent");
  online_host();
}

static void online_unhost(void) {
  if (g_hostState != 3) return;
  if (g_agent) { TerminateProcess(g_agent, 0); CloseHandle(g_agent); g_agent = NULL; }
  g_hostState = 0;
  logf("online: hosting stopped (the playit agent was closed)");
}

// Plugin start: settings, the local server, where the game traffic goes. Hosting starts from the menu.
static void online_start(const wchar_t *plugin_dir) {
  _snwprintf(g_onlineDir, MAX_PATH, L"%lsHorizonConnect\\", plugin_dir);
  { // FH2 Online 0.x kept these in plugins\fh2online
    wchar_t old[MAX_PATH]; _snwprintf(old, MAX_PATH, L"%lsfh2online", plugin_dir);
    if (GetFileAttributesW(g_onlineDir) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES && MoveFileW(old, g_onlineDir))
      logf("online: plugins\\fh2online renamed to plugins\\HorizonConnect");
  }
  g_autoServer = GetPrivateProfileIntW(L"online", L"AutoServer", 1, g_ini);
  g_localPort = GetPrivateProfileIntW(L"online", L"LocalPort", 5101, g_ini);
  update_check_start();
  if (!g_autoServer) return;
  start_local_server();
  // web calls: always this PC's server; the server check is left to the game's own retries (the server needs a
  // few seconds to read the game's message formats)
  _snwprintf(g_httpServer, 256, L"http://127.0.0.1:%d", g_localPort);
  g_offlineIfUnreachable = 0;
  // Every start begins on this PC's own server; another server is chosen in the list, right before going online
  // (a remembered remote server that isn't up could otherwise be used by the game at start).
  GetPrivateProfileStringW(L"online", L"LastJoin", L"", g_lastJoin, 256, g_ini);
  GetPrivateProfileStringW(L"online", L"HostAddress", L"", g_hostAddress, 256, g_ini);   // hosting was set up before
  if (g_hostAddress[0]) CloseHandle(CreateThread(NULL, 0, agent_check_thread, NULL, 0, NULL));
  if (!g_lastJoin[0]) GetPrivateProfileStringW(L"online", L"Join", L"", g_lastJoin, 256, g_ini);
  WritePrivateProfileStringW(L"online", L"Join", NULL, g_ini);
  online_set_join(L"");
  logf("online: web %ls, game traffic [%ls]:%d", g_httpServer, g_serverIp6, g_serverPort);
}
