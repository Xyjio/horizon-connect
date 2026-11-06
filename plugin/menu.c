// Horizon Connect's pages in the game's own menus. Included by horizonconnect.c after online.c.
//
// The native-UI technique (a standalone VideoOptions page with its own XAML URI served from the plugin's media
// directory, properties registered on the VideoOptions type, hiding the pause page behind it, text lists made in
// the game's own collection type) is adapted from "EO Weather Control" (Set Time & Weather FH2 by Pantsedit,
// Extra Options integration) and Extra Options' native Video settings rows by Arthur (RokkuDayo,
// github.com/RokkuDayo/ForzaHorizon2_ExtraOptions); GPL-3.0. This file, and HorizonConnect.dll built with it, are
// therefore GPL-3.0 as well; see LICENSE-GPL3.txt. Game addresses are for Forza Horizon 2 1.0.0.64.
//
// F12 (or View; [menu] Key= picks another key) in the free-roam pause menu opens HORIZON CONNECT, a page in the style of the game's settings. The
// texts are German or English, following the game's language (TR()).
//
//   list page   each saved server                          online · N players / can't be reached (pinged)
//               (or "No servers yet" when there are none)
//               Add server                                 ›
//               Host a server                              ›
//               A: the row's action (join / open)          X: edit  Y: delete (twice)  B: back
//   edit page   Address (A pastes the clipboard; filled in by itself when the clipboard holds one),
//               Name (optional), Save, Cancel, [Delete server]
//   host page   the first time: Set up hosting (playit.gg's claim page opens in the browser)
//               then: Start the server and join, Address for friends (A copies it), Stop the server
//
// Input: the page's own button handler (VideoOptions vtable +0x578) is hooked so A and X are ours (the game's A
// would accept and close the page); Y arrives through the page's "reset to defaults" slot. The game's list
// doesn't report which row has the focus, so up/down are counted along (g_cursor).
//
// One of our pages is open at a time; moving between them closes the current one and opens the next as soon as
// the pause menu is free again (so nothing goes stale and nothing stacks). Joining closes the page and starts
// the game's own "Private Free Roam Online" (the pause menu's online_private_freeroam transition).

#include <stdint.h>
#include <stdarg.h>

typedef void (*UiAction)(void *);
static unsigned char *const g_game = (unsigned char *)0x140000000ULL;

enum {
  UI_DELETE = 0x257730, UI_LANGUAGE = 0x3662990, UI_PADS = 0x365d7c8, UI_PADS_VTABLE = 0x2e7a2e8,
  UI_MODE_MANAGER = 0x34c0f40, UI_MODE_GET = 0x2129a0, UI_GET_VALUE = 0x15e0f10, UI_MAKE_STRING = 0x13fe8e0,
  UI_SET_VALUE = 0x15f8a70, UI_KEY_VISIBILITY = 0x36a47b0, UI_KEY_OPACITY = 0x36a5a08, UI_BOX_ENUM = 0x336a10,
  UI_BOX_FLOAT = 0x336ac0, UI_FIND_NAME = 0xc63120, UI_VIDEO_FACTORY = 0xb11e30, UI_VIDEO_VTABLE = 0x2e60370,
  UI_WSTRING_ASSIGN = 0x22c570, UI_SCREENS = 0x1412fe0,
  SLOT_PAUSE_INIT = 0x2e14310, SLOT_PAUSE_UPDATE = 0x2e14350, SLOT_VIDEO_RESET = 0x2e60900,
  SLOT_VIDEO_BUTTON = 0x2e608e8,          // VideoOptions vtable +0x578: (page, button, pressed) -> handled
  PROP_REGISTER = 0x155ee30, PROP_REGISTRY = 0x35470d0, PROP_TYPE_INIT = 0xeca710, PROP_OBJECT_INIT = 0x143eb80,
  PROP_INT_TEMPLATE = 0xed81f0, PROP_LIST_TEMPLATE = 0xed77d0, PROP_BOX_INT = 0x4702d0, PROP_CHANGED = 0xcf9c60,
  COLL_TYPE = 0x1476110, COLL_ALLOCATE = 0x14424e0, COLL_INIT = 0xaefd60, COLL_INSERT = 0xb52170,
  PAGE_FIRE_TRANSITION = 0x1422f10,      // (page, transition id): what the pause tiles call
  PRIVATE_FREEROAM_ID = 0x352e2a8,       // the "online_private_freeroam" transition id (+0x10 of its global)
  PRIVATE_FREEROAM_TILE = 0xde7e20,      // lea rdx, [PRIVATE_FREEROAM_ID] in that tile's handler
};

typedef struct {
  void **vtable;
  unsigned char header[0x20];
  void *getter, *setter, *unused;
  const char *name;
  void *type;
  uint64_t flags;
} UiProperty;
typedef struct UiLink { UiProperty *property; struct UiLink *next; } UiLink;
_Static_assert(sizeof(UiProperty) == 0x58, "native property layout");

static int g_menuKey = VK_F12;      // [menu] Key=: F12 by default. Not F5-F9: the Azimuth Mapper's hotkeys (F6 switches
static wchar_t g_menuKeyName[16] = L"F12";   // its mouse pointer mode, which takes W/S away from gas and brake)

// Reads [menu] Key= (F1-F24, A-Z, 0-9, Insert, Delete, Home, End, PageUp, PageDown, a code 0xNN, or None).
static void menu_key_load(void) {
  wchar_t k[16] = L"";
  GetPrivateProfileStringW(L"menu", L"Key", L"F12", k, 16, g_ini);
  static const struct { const wchar_t *n; int vk; } named[] = {
    { L"Insert", VK_INSERT }, { L"Delete", VK_DELETE }, { L"Home", VK_HOME }, { L"End", VK_END },
    { L"PageUp", VK_PRIOR }, { L"PageDown", VK_NEXT }, { L"None", 0 } };
  int vk = -1;
  if ((k[0] == L'F' || k[0] == L'f') && k[1] >= L'1' && k[1] <= L'9') { int n = _wtoi(k + 1); if (n >= 1 && n <= 24) vk = VK_F1 + n - 1; }
  else if (k[0] == L'0' && (k[1] == L'x' || k[1] == L'X')) vk = (int)wcstol(k, NULL, 16) & 0xFF;
  else if (k[0] && !k[1] && iswalnum(k[0])) vk = towupper(k[0]);
  else for (size_t i = 0; i < sizeof named / sizeof *named; i++) if (!_wcsicmp(k, named[i].n)) vk = named[i].vk;
  if (vk < 0) { logf("menu: [menu] Key=%ls isn't a key I know; using F12", k); return; }
  g_menuKey = vk; wcsncpy(g_menuKeyName, k, 15);
}
#define PAD_A 0x1000
#define PAD_X 0x4000
#define PAD_VIEW 0x20
#define PAGE_POOL 100           // page files exist before the game starts (it doesn't see files added later)
#define MAX_SERVERS 16
#define MAX_ROWS 20

enum { PAGE_NONE, PAGE_LIST, PAGE_EDIT, PAGE_HOST, PAGE_JOIN };
enum { ROW_OWN, ROW_SERVER, ROW_ADD, ROW_HOST, ROW_NAME, ROW_ADDRESS, ROW_SAVE, ROW_CANCEL, ROW_DELETE,
       ROW_STATUS, ROW_HOSTADDR, ROW_HOSTTOGGLE, ROW_OWNNAME, ROW_SETUP, ROW_HOSTJOIN, ROW_EMPTY, ROW_UPDATE };

typedef struct { wchar_t name[64], address[128]; } Server;
static Server g_servers[MAX_SERVERS];
static int g_serverCount;
static wchar_t g_ownName[64];

static UiAction g_pauseInitNext, g_pauseUpdateNext, g_videoResetNext;
static void *g_pausePage, *g_padSeen, *g_page;          // g_page: the page of ours that is open (compared only)
static int g_padHeld = 1, g_menuReady, g_canStartOnline, g_pageKind, g_next, g_pageHeld = 1;
static int g_rowKind[MAX_ROWS], g_rowServer[MAX_ROWS], g_rows;
static wchar_t g_value[MAX_ROWS][160];                  // what each row shows on its right
static void *g_items[MAX_ROWS];                         // ... as the game's text list
static UiProperty g_props[2 * MAX_ROWS];                // Fh2RowN (int, always 0) and Fh2ItemsN (that list)
static UiLink g_links[2 * MAX_ROWS];
static char g_propNames[2 * MAX_ROWS][16];
static wchar_t g_mediaRoot[MAX_PATH], g_scenes[MAX_PATH];
static LONG g_pageSeq;
static int g_editIndex = -1, g_deleteArmed = -1, g_startOwn;
static wchar_t g_editName[64], g_editAddress[128], g_flash[MAX_ROWS][96];
static ULONGLONG g_flashUntil[MAX_ROWS];
static void *g_hiddenPause, *g_pauseVisibility, *g_pauseOpacity;
static int g_pauseFade; static ULONGLONG g_pauseFadeAt; static float g_pauseAlpha, g_pauseFadeFrom, g_pauseSavedAlpha;

static void ui_release(void *p) {
  if (p && InterlockedDecrement((volatile LONG *)((unsigned char *)p + 0x10)) == 0) ((UiAction)(g_game + UI_DELETE))(p);
}

static int ui_german(void) {
  unsigned char *manager = *(void **)(g_game + UI_LANGUAGE);
  if (!manager) return 1;
  const unsigned char *name = manager + 0x30;
  size_t count = *(const size_t *)(name + 16), cap = *(const size_t *)(name + 24);
  return cap == 15 && count == 2 && !_strnicmp((const char *)name, "DE", 2);
}
#define TR(de, en) (ui_german() ? (de) : (en))

// ---- the server list ([servers] S1..S16 = name|address, [online] HostName) ------------------------------------------

static void servers_save(void) {
  for (int i = 0; i < MAX_SERVERS; i++) {
    wchar_t key[8], value[200]; _snwprintf(key, 8, L"S%d", i + 1);
    if (i < g_serverCount) _snwprintf(value, 200, L"%ls|%ls", g_servers[i].name, g_servers[i].address);
    WritePrivateProfileStringW(L"servers", key, i < g_serverCount ? value : NULL, g_ini);
  }
  WritePrivateProfileStringW(L"online", L"HostName", g_ownName, g_ini);
}

static void servers_load(void) {
  g_serverCount = 0;
  for (int i = 0; i < 32 && g_serverCount < MAX_SERVERS; i++) {
    wchar_t key[8], value[200]; _snwprintf(key, 8, L"S%d", i + 1);
    GetPrivateProfileStringW(L"servers", key, L"", value, 200, g_ini);
    if (!value[0]) continue;
    Server *s = &g_servers[g_serverCount];
    wchar_t *bar = wcschr(value, L'|');
    if (bar) { *bar = 0; wcsncpy(s->name, value, 63); wcsncpy(s->address, bar + 1, 127); }
    else { wcsncpy(s->address, value, 127); wcsncpy(s->name, value, 63); }
    s->name[63] = 0; s->address[127] = 0;
    if (!_wcsicmp(s->name, s->address)) {   // saved before names were shortened: the address's first part
      wchar_t *cut = wcschr(s->name, L'.'); if (!cut) cut = wcschr(s->name, L':');
      if (cut && cut != s->name) *cut = 0;
    }
    g_serverCount++;
  }
  GetPrivateProfileStringW(L"online", L"HostName", L"", g_ownName, 64, g_ini);
  if (!g_ownName[0]) wcscpy(g_ownName, TR(L"Mein Server", L"My server"));
  if (GetPrivateProfileIntW(L"online", L"ListVersion", 0, g_ini) < 3) {   // the server used before this list
    int known = !g_lastJoin[0] || (g_hostAddress[0] && !_wcsicmp(g_lastJoin, g_hostAddress));
    for (int i = 0; i < g_serverCount; i++) if (!_wcsicmp(g_servers[i].address, g_lastJoin)) known = 1;
    if (!known && g_serverCount < MAX_SERVERS) {
      Server *s = &g_servers[g_serverCount++];
      wcsncpy(s->name, TR(L"Zuletzt gespielt", L"Last played"), 63); wcsncpy(s->address, g_lastJoin, 127);
    }
    WritePrivateProfileStringW(L"online", L"ListVersion", L"3", g_ini);
    servers_save();
  }
}

// Hosting was set up on this PC (playit claimed, the tunnel made): its address is known.
static int host_set_up(void) { return g_hostAddress[0] != 0; }
static int g_hostPageSetUp;

// ---- the game's text lists and our properties ---------------------------------------------------------------------

static void *make_items(const wchar_t *text) {
  void *type = ((void *(*)(void *))(g_game + COLL_TYPE))(NULL);
  void *items = ((void *(*)(size_t, void *))(g_game + COLL_ALLOCATE))(0x58, type);
  if (!items) return NULL;
  ((UiAction)(g_game + COLL_INIT))(items);
  static const unsigned vtables[] = { 0x2db73f8, 0x2db7470, 0x2db74d0, 0x2db74e8, 0x2db7508 };
  static const unsigned offsets[] = { 0, 0x30, 0x38, 0x40, 0x48 };
  for (unsigned i = 0; i < 5; ++i) *(void **)((unsigned char *)items + offsets[i]) = g_game + vtables[i];
  void *label = NULL;
  ((void **(*)(void **, const wchar_t *))(g_game + UI_MAKE_STRING))(&label, text && *text ? text : L" ");
  if (!label) { ui_release(items); return NULL; }
  ((void (*)(void *, int, void *))(g_game + COLL_INSERT))(items, 0, label);
  ui_release(label);
  return items;
}

// Row i shows `text` on its right; the open page is told when it changes.
static void row_value(int i, const wchar_t *text, int notify) {
  if (i < 0 || i >= MAX_ROWS) return;
  if (g_items[i] && !wcscmp(g_value[i], text)) return;
  void *items = make_items(text);
  if (!items) return;
  wcsncpy(g_value[i], text, 159); g_value[i][159] = 0;
  void *old = g_items[i]; g_items[i] = items; ui_release(old);
  if (notify && g_page) ((void (*)(void *, void *))(g_game + PROP_CHANGED))(g_page, &g_props[MAX_ROWS + i]);
}

static int prop_index(UiProperty *p) { return (int)(p - g_props); }
static void **get_row(void **out, UiProperty *p, void *page) { (void)p; (void)page; int zero = 0; *out = NULL; return ((void **(*)(void **, const void *))(g_game + PROP_BOX_INT))(out, &zero); }
static void set_row(UiProperty *p, void *page, void *boxed) { (void)boxed; if (page) ((void (*)(void *, void *))(g_game + PROP_CHANGED))(page, p); }
static void **get_items(void **out, UiProperty *p, void *page) {
  (void)page;
  int i = prop_index(p) - MAX_ROWS;
  *out = i >= 0 && i < MAX_ROWS ? g_items[i] : NULL;
  if (*out) InterlockedIncrement((volatile LONG *)((unsigned char *)*out + 0x10));
  return out;
}

static int code_is(uint32_t rva, const char *bytes, size_t n) { return !memcmp(g_game + rva, bytes, n); }

static int menu_properties(void) {
  if (!code_is(PROP_REGISTER, "\x48\x8b\x41\x40\x48\x89\x42\x08\x48\x89\x51\x40\xc3", 13) ||
      !code_is(PROP_CHANGED, "\x40\x57\x48\x83\xec\x60\x48\xc7\x44\x24\x20\xfe", 12) ||
      !code_is(PROP_BOX_INT, "\x40\x53\x48\x83\xec\x30\x48\x8b\xd9\xc7\x44\x24", 12) ||
      !code_is(PROP_OBJECT_INIT, "\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x20\x48\x8d", 12) ||
      !code_is(PROP_TYPE_INIT, "\x40\x53\x48\x81\xec\xd0\x00\x00\x00\x48\xc7\x44\x24\x20\xfe\xff", 16) ||
      !code_is(PROP_INT_TEMPLATE, "\x40\x57\x48\x83\xec\x30\x48\xc7\x44\x24\x20\xfe\xff\xff\xff\x48", 16) ||
      !code_is(PROP_LIST_TEMPLATE, "\x40\x57\x48\x83\xec\x30\x48\xc7\x44\x24\x20\xfe\xff\xff\xff\x48", 16)) {
    logf("menu: unexpected game code; no Horizon Connect pages");
    return 0;
  }
  ((void (*)(void))(g_game + PROP_TYPE_INIT))();
  UiProperty *ti = ((UiProperty *(*)(void))(g_game + PROP_INT_TEMPLATE))();
  UiProperty *tl = ((UiProperty *(*)(void))(g_game + PROP_LIST_TEMPLATE))();
  if (!ti || !ti->vtable || !ti->name || strcmp(ti->name, "Subtitles") || !ti->type ||
      !tl || !tl->vtable || !tl->name || strcmp(tl->name, "OnOffOptions") || !tl->type) { logf("menu: the property templates looked wrong"); return 0; }
  for (int i = 0; i < 2 * MAX_ROWS; i++) {
    UiProperty *p = &g_props[i], *t = i < MAX_ROWS ? ti : tl;
    ((void (*)(void *))(g_game + PROP_OBJECT_INIT))(p);
    p->vtable = t->vtable;
    memcpy((unsigned char *)p + 0x28, (unsigned char *)t + 0x28, 0x30);
    snprintf(g_propNames[i], sizeof g_propNames[i], i < MAX_ROWS ? "Fh2Row%d" : "Fh2Items%d", i % MAX_ROWS);
    p->name = g_propNames[i];
    p->getter = i < MAX_ROWS ? (void *)get_row : (void *)get_items;
    p->setter = i < MAX_ROWS ? (void *)set_row : NULL;
    g_links[i].property = p; g_links[i].next = NULL;
    ((void (*)(void *, UiLink *))(g_game + PROP_REGISTER))(g_game + PROP_REGISTRY, &g_links[i]);
  }
  for (int i = 0; i < MAX_ROWS; i++) row_value(i, L" ", 0);
  return 1;
}

// ---- page text ----------------------------------------------------------------------------------------------------

static void xml_escape(const wchar_t *in, wchar_t *out, int len) {
  int n = 0;
  for (; *in && n < len - 8; in++) {
    const wchar_t *r = *in == L'&' ? L"&amp;" : *in == L'<' ? L"&lt;" : *in == L'>' ? L"&gt;" : *in == L'"' ? L"&quot;" : *in == L'\n' ? L"&#10;" : NULL;
    if (r) { wcscpy(out + n, r); n += (int)wcslen(r); } else out[n++] = *in;
  }
  out[n] = 0;
}

typedef struct { wchar_t *buf; size_t len, cap; } WBuf;
static void wb(WBuf *b, const wchar_t *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int n = _vsnwprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
  va_end(ap);
  if (n > 0) b->len += (size_t)n;
}

typedef struct { int kind, server; wchar_t label[96], help[600]; } RowSpec;

// Writes Anthem\HorizonConnect_<n>.xaml: a title, the rows, the footer prompts and the description panel.
static int write_page(LONG n, const wchar_t *title, const RowSpec *rows, int count, const wchar_t *a, const wchar_t *x, const wchar_t *y) {
  WBuf b = { malloc(200000 * sizeof(wchar_t)), 0, 200000 };
  if (!b.buf) return 0;
  // EO:StandaloneOptionsPage keeps Extra Options' Video rows out; the TimeLockEnabled text keeps EO Weather Control's out.
  wb(&b, L"<!-- EO:StandaloneOptionsPage -->\r\n<!-- Horizon Connect page; not a Video page: {Binding TimeLockEnabled} -->\r\n");
  wb(&b, L"<Page xmlns:local=\"clr-namespace:ForzaUI\" xmlns:System=\"clr-namespace:System;assembly=mscorlib\" xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\" x:Class=\"ForzaUI.A_VideoOptions\" Grid.IsSharedSizeScope=\"True\" Style=\"{DynamicResource A_Style_Page_Generic}\" DataContext=\"{Binding RelativeSource={RelativeSource Self}}\">\r\n"
             L"<local:UIPageTransitionManager.TransitionManager><local:UIPageTransitionManager DurationSpread=\"{StaticResource A_Duration_Transition_Spread}\" TransitionDelay=\"{StaticResource A_Duration_Transition_Delay}\"/></local:UIPageTransitionManager.TransitionManager>\r\n"
             L"<Page.Resources><ResourceDictionary>\r\n");
  for (int i = 0; i < count; i++) {
    wchar_t h[300], d[1400];
    xml_escape(rows[i].label, h, 300); xml_escape(rows[i].help, d, 1400);
    wb(&b, L"<local:TileMenuDataItem x:Key=\"Fh2Tag%d\" Heading=\"%ls\" Description=\"%ls\"/>\r\n", i, h, d);
  }
  wchar_t te[200], ae[100], xe[100], ye[100];
  xml_escape(title, te, 200); xml_escape(a, ae, 100); xml_escape(x ? x : L"", xe, 100); xml_escape(y ? y : L"", ye, 100);
  wb(&b, L"</ResourceDictionary></Page.Resources>\r\n"
         L"<Grid><ContentControl x:Name=\"TopBar\" Style=\"{StaticResource A_Style_PlayerBar}\"/>\r\n"
         L"<ContentControl x:Name=\"BottomBar\" Style=\"{StaticResource A_Style_ButtonBar}\"><ContentControl x:Name=\"DefaultHelpPanel\" local:ControllerButtonPanel.ADescription=\"%ls\" local:ControllerButtonPanel.BDescription=\"{local:Loc HelpButtons.IDS_Back}\"",
     ae);
  if (x) wb(&b, L" local:ControllerButtonPanel.XDescription=\"%ls\"", xe);
  if (y) wb(&b, L" local:ControllerButtonPanel.YDescription=\"%ls\"", ye);
  int height = 55 * (count < 9 ? count : 9);
  wb(&b, L" Style=\"{StaticResource A_Style_ContentControl_DefaultButtonBar}\"/></ContentControl>\r\n"
         L"<Border Style=\"{StaticResource A_Style_Border_ScenePadding}\" SnapsToDevicePixels=\"True\" UseLayoutRounding=\"True\"><Grid x:Name=\"LayoutRoot\">\r\n"
         L"<ContentControl Style=\"{StaticResource A_Style_ContentControl_ScreenTilt}\" Margin=\"0,-60,0,0\" Focusable=\"False\"><StackPanel Name=\"BasicOptions\" VerticalAlignment=\"Center\" HorizontalAlignment=\"Center\">\r\n"
         L"<StackPanel Orientation=\"Horizontal\" Margin=\"0,0,0,6\" Style=\"{StaticResource A_Style_FrameworkElement_DefaultTransition_0}\"><ContentControl Tag=\"{x:Null}\" Content=\"%ls\" Style=\"{StaticResource A_Style_ContentControl_ScreenTitleBlock}\" Focusable=\"False\"/></StackPanel>\r\n"
         L"<StackPanel Width=\"806\" Style=\"{StaticResource A_Style_FrameworkElement_DefaultTransition_0}\"><Grid local:UIPageTransitionManager.TransitionIndex=\"0\"><local:FZMaterial Color=\"{StaticResource A_Color_TileSelected_Text}\"><local:CommonListBox x:Name=\"OptionsList\" Style=\"{StaticResource A_Style_ListBox_List_Default}\" ItemContainerStyle=\"{StaticResource A_Style_ListBoxItem_List_Default}\" Width=\"806\" Height=\"%d\">\r\n",
     te, height);
  for (int i = 0; i < count; i++) {
    wchar_t l[300]; xml_escape(rows[i].label, l, 300);
    wb(&b, L"<local:SpinnerControl%ls Label=\"%ls\" Tag=\"{StaticResource Fh2Tag%d}\" SelectedIndex=\"{Binding Fh2Row%d}\" ItemsSource=\"{Binding Fh2Items%d}\" Style=\"{StaticResource A_Style_SpinnerControl_Default}\"/>\r\n",
       i ? L"" : L" x:Name=\"FirstItem\"", l, i, i, i);
  }
  wb(&b, L"</local:CommonListBox></local:FZMaterial></Grid><local:FZMaterialGlassCapture/>\r\n"
         L"<ContentControl Content=\"{Binding Tag.Description}\" DataContext=\"{Binding SelectedItem, ElementName=OptionsList}\" Height=\"130\" Width=\"806\" Template=\"{StaticResource A_ControlTemplate_DescriptionPanelContent}\"/>\r\n"
         L"</StackPanel></StackPanel></ContentControl></Grid></Border><local:FZMaterial x:Name=\"Fader\" Color=\"Black\" Opacity=\"0\"/></Grid></Page>\r\n");
  int bytes = WideCharToMultiByte(CP_UTF8, 0, b.buf, (int)b.len, NULL, 0, NULL, NULL);
  char *utf8 = malloc((size_t)bytes + 3);
  int ok = 0;
  if (utf8) {
    utf8[0] = (char)0xEF; utf8[1] = (char)0xBB; utf8[2] = (char)0xBF;   // UTF-8 with a BOM, as the game reads it
    WideCharToMultiByte(CP_UTF8, 0, b.buf, (int)b.len, utf8 + 3, bytes, NULL, NULL);
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%ls\\HorizonConnect_%ld.xaml", g_scenes, n);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) { DWORD put; ok = WriteFile(f, utf8, (DWORD)bytes + 3, &put, NULL); CloseHandle(f); }
    free(utf8);
  }
  free(b.buf);
  if (!ok) logf("menu: the page text couldn't be written to %ls", g_scenes);
  return ok;
}

// ---- is a server up? (its relay answers "FH2O?" with "FH2O!{players, max}"; relay.py since 0.4) ----------

enum { PING_UNKNOWN, PING_CHECKING, PING_UP, PING_DOWN };
typedef struct { wchar_t address[128]; volatile LONG state, players, max; } Ping;
static Ping g_ping[MAX_SERVERS + 1];      // the list's servers, and [MAX_SERVERS] = this PC's own
static volatile LONG g_pingBusy;
static ULONGLONG g_pingAt;

static Ping *ping_for(const wchar_t *address) {
  for (int i = 0; i <= MAX_SERVERS; i++) if (g_ping[i].address[0] && !_wcsicmp(g_ping[i].address, address)) return &g_ping[i];
  return NULL;
}

static DWORD WINAPI ping_thread(LPVOID unused) {
  (void)unused;
  WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in to[MAX_SERVERS + 1]; int have[MAX_SERVERS + 1] = { 0 };
  if (s != INVALID_SOCKET) {
    struct sockaddr_in any = { 0 }; any.sin_family = AF_INET; bind(s, (struct sockaddr *)&any, sizeof any);
    for (int i = 0; i <= MAX_SERVERS; i++) {
      wchar_t host[256]; int port;
      if (!g_ping[i].address[0] || !parse_address(g_ping[i].address, host, 256, &port)) continue;
      ADDRINFOW hints = { 0 }, *ai = NULL; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
      if (GetAddrInfoW(host, NULL, &hints, &ai) || !ai) { InterlockedExchange(&g_ping[i].state, PING_DOWN); continue; }
      to[i] = *(struct sockaddr_in *)ai->ai_addr; to[i].sin_port = htons((u_short)port); FreeAddrInfoW(ai);
      have[i] = 1;
      sendto(s, "FH2O?", 5, 0, (struct sockaddr *)&to[i], sizeof to[i]);
    }
    int answered[MAX_SERVERS + 1] = { 0 };
    ULONGLONG until = GetTickCount64() + 2000;
    for (;;) {
      ULONGLONG now = GetTickCount64(); if (now >= until) break;
      fd_set fs; FD_ZERO(&fs); FD_SET(s, &fs);
      struct timeval tv = { 0, (long)((until - now) * 1000) };
      if (select(0, &fs, NULL, NULL, &tv) <= 0) break;
      char buf[512]; struct sockaddr_in from; int fl = sizeof from;
      int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
      if (n < 5 || memcmp(buf, "FH2O!", 5)) continue;
      buf[n] = 0;
      for (int i = 0; i <= MAX_SERVERS; i++) {
        if (!have[i] || answered[i] || to[i].sin_addr.s_addr != from.sin_addr.s_addr || to[i].sin_port != from.sin_port) continue;
        int players = 0, max = 0; const char *p;
        if ((p = strstr(buf, "\"players\":"))) players = atoi(p + 10);
        if ((p = strstr(buf, "\"max\":"))) max = atoi(p + 6);
        InterlockedExchange(&g_ping[i].players, players); InterlockedExchange(&g_ping[i].max, max);
        InterlockedExchange(&g_ping[i].state, PING_UP); answered[i] = 1;
      }
    }
    for (int i = 0; i <= MAX_SERVERS; i++) if (have[i] && !answered[i]) InterlockedExchange(&g_ping[i].state, PING_DOWN);
    closesocket(s);
  }
  InterlockedExchange(&g_pingBusy, 0);
  return 0;
}

// Asks every listed server (and this PC's own) whether it is up; at most every 5 s, in the background.
static void ping_servers(void) {
  if (GetTickCount64() - g_pingAt < 5000 || InterlockedCompareExchange(&g_pingBusy, 1, 0)) return;
  g_pingAt = GetTickCount64();
  for (int i = 0; i < MAX_SERVERS; i++) {
    const wchar_t *a = i < g_serverCount ? g_servers[i].address : L"";
    if (wcscmp(g_ping[i].address, a)) { wcsncpy(g_ping[i].address, a, 127); InterlockedExchange(&g_ping[i].state, a[0] ? PING_CHECKING : PING_UNKNOWN); }
  }
  _snwprintf(g_ping[MAX_SERVERS].address, 128, L"127.0.0.1:%d", g_localPort);
  HANDLE t = CreateThread(NULL, 0, ping_thread, NULL, 0, NULL);
  if (t) CloseHandle(t); else InterlockedExchange(&g_pingBusy, 0);
}

static void ping_text(const Ping *p, wchar_t *out, int n) {
  if (!p || p->state == PING_UNKNOWN || p->state == PING_CHECKING) _snwprintf(out, n, L"%ls", TR(L"wird geprüft …", L"checking…"));
  else if (p->state == PING_DOWN) _snwprintf(out, n, L"%ls", TR(L"nicht erreichbar", L"can't be reached"));
  else if (p->players == 1) _snwprintf(out, n, L"%ls", TR(L"online · 1 Spieler", L"online · 1 player"));
  else _snwprintf(out, n, TR(L"online · %ld Spieler", L"online · %ld players"), p->players);
}

// ---- what the rows show -------------------------------------------------------------------------------------------

static const wchar_t *flash(int i) { return GetTickCount64() < g_flashUntil[i] ? g_flash[i] : NULL; }
static void say(int i, const wchar_t *text) { wcsncpy(g_flash[i], text, 95); g_flash[i][95] = 0; g_flashUntil[i] = GetTickCount64() + 3000; }

static const wchar_t *host_state_text(void) {
  return g_hostState == 3 ? TR(L"Online", L"Online")
       : g_hostState == 2 ? TR(L"Im Browser bestätigen (Alt+Tab)", L"Confirm in your browser (Alt+Tab)")
       : g_hostState == 1 ? TR(L"Wird gestartet …", L"Starting…")
       : g_hostState < 0 ? TR(L"Fehlgeschlagen", L"Failed") : TR(L"Offline", L"Offline");
}

static void row_text(int i, wchar_t *out, int n) {
  const wchar_t *f = flash(i);
  if (f) { _snwprintf(out, n, L"%ls", f); return; }
  switch (g_rowKind[i]) {
  case ROW_SETUP: _snwprintf(out, n, L"%ls", g_hostState > 0 || g_hostState < 0 ? host_state_text() : L"›"); break;
  case ROW_HOSTJOIN:
    if (g_hostState == 3) {
      const Ping *p = &g_ping[MAX_SERVERS];
      if (p->state == PING_UP) _snwprintf(out, n, TR(L"läuft · %ld Spieler", L"running · %ld players"), p->players);
      else _snwprintf(out, n, L"%ls", TR(L"läuft", L"running"));
    } else _snwprintf(out, n, L"%ls", g_hostState > 0 ? host_state_text() : TR(L"aus", L"off"));
    break;
  case ROW_OWN: _snwprintf(out, n, L"%ls", g_hostState == 3 ? TR(L"Online – A: beitreten", L"Online – A: join")
                                         : g_hostState > 0 ? host_state_text() : TR(L"Offline – A: starten", L"Offline – A: start")); break;
  case ROW_SERVER: ping_text(ping_for(g_servers[g_rowServer[i]].address), out, n); break;
  case ROW_EMPTY: _snwprintf(out, n, L"%ls", TR(L"A: Server hinzufügen", L"A: add a server")); break;
  case ROW_ADD: case ROW_HOST: case ROW_SAVE: case ROW_CANCEL: _snwprintf(out, n, L"›"); break;
  case ROW_DELETE: _snwprintf(out, n, L"%ls", g_deleteArmed == g_editIndex ? TR(L"Nochmal A = löschen", L"A again = delete") : L"›"); break;
  case ROW_NAME: {
    if (g_editName[0]) { _snwprintf(out, n, L"%ls", g_editName); break; }
    wchar_t host[256]; int port;   // what it will be called: the address's first part
    if (g_editAddress[0] && parse_address(g_editAddress, host, 256, &port)) { wchar_t *dot = wcschr(host, L'.'); if (dot && dot != host) *dot = 0; _snwprintf(out, n, L"%ls", host); }
    else _snwprintf(out, n, L"%ls", TR(L"(optional)", L"(optional)"));
    break;
  }
  case ROW_ADDRESS: _snwprintf(out, n, L"%ls", g_editAddress[0] ? g_editAddress : TR(L"A: aus Zwischenablage einfügen", L"A: paste from the clipboard")); break;
  case ROW_STATUS: _snwprintf(out, n, L"%ls", host_state_text()); break;
  case ROW_HOSTADDR: _snwprintf(out, n, L"%ls", g_hostAddress[0] ? g_hostAddress : L"—"); break;
  case ROW_HOSTTOGGLE: _snwprintf(out, n, L"%ls", g_hostState == 3 ? TR(L"A: beenden", L"A: stop") : g_hostState > 0 ? host_state_text() : TR(L"läuft nicht", L"not running")); break;
  case ROW_OWNNAME: _snwprintf(out, n, L"%ls", g_ownName); break;
  case ROW_UPDATE: _snwprintf(out, n, L"%ls", TR(L"A: Download-Seite öffnen", L"A: open the download page")); break;
  default: _snwprintf(out, n, L" ");
  }
}

static void refresh_rows(int notify) {
  for (int i = 0; i < g_rows; i++) { wchar_t t[160]; row_text(i, t, 160); row_value(i, t, notify); }
}

// ---- pause page: availability, hiding it behind ours, input -------------------------------------------------------

static unsigned pad_buttons(void **selected) {
  *selected = NULL;
  unsigned char *manager = *(void **)(g_game + UI_PADS);
  if (!manager || *(void **)manager != g_game + UI_PADS_VTABLE) return 0;
  unsigned char *pad = *(void **)(manager + 0xab28);
  uintptr_t first = (uintptr_t)manager + 0x10, current = (uintptr_t)pad;
  if (current < first || current >= first + 8 * 0x1300 || (current - first) % 0x1300 || pad[4] != 1) return 0;
  *selected = pad;
  return *(unsigned short *)(pad + 0xc0);
}

static int window_focused(void) {
  DWORD id = 0; HWND w = GetForegroundWindow();
  if (w) GetWindowThreadProcessId(w, &id);
  return id == GetCurrentProcessId();
}

static int pause_free(void *page) {   // free roam's pause menu, with nothing on top of it
  unsigned char *p = page;
  void *behavior = p ? *(void **)(p + 0x310) : NULL;
  if (!g_menuReady || !behavior || !*((unsigned char *)behavior + 0x78) || *(void **)(p + 0x2a0)) return 0;
  void *manager = *(void **)(g_game + UI_MODE_MANAGER);
  if (!manager) return 0;
  struct { void *object, *counter; } context = { 0 };
  ((void *(*)(void *, void *))(g_game + UI_MODE_GET))(manager, &context);
  int mode = context.object ? *(int *)((unsigned char *)context.object + 0x60) : -1;
  if (context.counter && InterlockedDecrement((volatile LONG *)((unsigned char *)context.counter + 8)) == 0) {
    ((UiAction)(*(void ***)context.counter)[0])(context.counter);
    if (InterlockedDecrement((volatile LONG *)((unsigned char *)context.counter + 12)) == 0) ((UiAction)(*(void ***)context.counter)[1])(context.counter);
  }
  return mode == 2;
}

static void visual_set(void *visual, unsigned key, void *value) {
  if (value && *(void **)(g_game + key)) ((void (*)(void *, void *, void *))(g_game + UI_SET_VALUE))(visual, *(void **)(g_game + key), value);
}
static void pause_restore(void) {
  if (g_hiddenPause) {
    visual_set(g_hiddenPause, UI_KEY_VISIBILITY, g_pauseVisibility);
    visual_set(g_hiddenPause, UI_KEY_OPACITY, g_pauseOpacity);
    ui_release(g_pauseVisibility); ui_release(g_pauseOpacity); ui_release(g_hiddenPause);
  }
  g_hiddenPause = g_pauseVisibility = g_pauseOpacity = NULL;
  g_pauseFade = 0;
}
static void pause_alpha(float value) {
  void *boxed = NULL;
  ((void **(*)(void **, const float *))(g_game + UI_BOX_FLOAT))(&boxed, &value);
  if (boxed) { visual_set(g_hiddenPause, UI_KEY_OPACITY, boxed); ui_release(boxed); g_pauseAlpha = value; }
}
static void pause_fade(int direction) { g_pauseFade = direction; g_pauseFadeFrom = g_pauseAlpha; g_pauseFadeAt = GetTickCount64(); }
static void pause_return(void) { if (!g_hiddenPause) return; visual_set(g_hiddenPause, UI_KEY_VISIBILITY, g_pauseVisibility); pause_fade(2); }
static void pause_tick(void) {
  if (!g_hiddenPause || !g_pauseFade) return;
  ULONGLONG elapsed = GetTickCount64() - g_pauseFadeAt;
  float progress = elapsed >= 200 ? 1.0f : (float)elapsed / 200.0f;
  float target = g_pauseFade == 1 ? 0.0f : g_pauseSavedAlpha;
  pause_alpha(g_pauseFadeFrom + (target - g_pauseFadeFrom) * progress);
  if (progress < 1.0f) return;
  if (g_pauseFade == 2) { pause_restore(); return; }
  void *boxed = NULL; int collapsed = 2;
  ((void **(*)(void **, const int *))(g_game + UI_BOX_ENUM))(&boxed, &collapsed);
  if (boxed) { visual_set(g_hiddenPause, UI_KEY_VISIBILITY, boxed); ui_release(boxed); }
  g_pauseFade = 0;
}
static void pause_hide(void *page) {
  if (g_hiddenPause) { if (g_pauseFade == 2) pause_fade(1); return; }
  void *name = NULL;
  ((void **(*)(void **, const wchar_t *))(g_game + UI_MAKE_STRING))(&name, L"LayoutRoot");
  if (!name) return;
  void *root = ((void *(*)(void **, void *))(g_game + UI_FIND_NAME))(&name, page);
  void *key = *(void **)(g_game + UI_KEY_VISIBILITY), *alpha = *(void **)(g_game + UI_KEY_OPACITY);
  if (!root || !key || !alpha) return;
  void *before = NULL, *opacity = NULL;
  ((void **(*)(void *, void **, void *))(g_game + UI_GET_VALUE))(root, &before, key);
  ((void **(*)(void *, void **, void *))(g_game + UI_GET_VALUE))(root, &opacity, alpha);
  int visible = 0; float opaque = 1.0f;
  if (!before) ((void **(*)(void **, const int *))(g_game + UI_BOX_ENUM))(&before, &visible);
  if (!opacity) ((void **(*)(void **, const float *))(g_game + UI_BOX_FLOAT))(&opacity, &opaque);
  if (before && opacity) {
    InterlockedIncrement((volatile LONG *)((unsigned char *)root + 0x10));
    g_hiddenPause = root; g_pauseVisibility = before; g_pauseOpacity = opacity;
    g_pauseSavedAlpha = *(float *)((unsigned char *)opacity + 0x28);
    g_pauseAlpha = g_pauseSavedAlpha; pause_fade(1);
  } else { ui_release(before); ui_release(opacity); }
}

// ---- opening and closing our pages ---------------------------------------------------------------------------------

static ULONGLONG g_nextAt;
static int g_closeRequested, g_cursor, g_pendingA, g_pendingX;
// Our page closes the way B closes it (the next pause-page update presses B on it).
static void close_page(int next) {
  g_next = next; g_nextAt = GetTickCount64();
  memset(g_flashUntil, 0, sizeof g_flashUntil);   // messages belong to the page they were shown on
  if (g_page) g_closeRequested = 1;
}

static void add_row(RowSpec *rows, int *n, int kind, int server, const wchar_t *label, const wchar_t *help) {
  if (*n >= MAX_ROWS) return;
  RowSpec *r = &rows[*n];
  r->kind = kind; r->server = server;
  wcsncpy(r->label, label, 95); r->label[95] = 0;
  wcsncpy(r->help, help, 599); r->help[599] = 0;
  g_rowKind[*n] = kind; g_rowServer[*n] = server;
  (*n)++;
}

static int open_page(void *pause, int kind) {
  RowSpec rows[MAX_ROWS]; int n = 0;
  const wchar_t *title = L"HORIZON CONNECT", *a = TR(L"Auswählen", L"Select"), *x = NULL, *y = NULL;
  wchar_t text[700];
  if (kind == PAGE_LIST) {
    servers_load();
    if (g_newVersion[0]) {
      wchar_t label[96];
      _snwprintf(label, 96, TR(L"Neue Version %hs verfügbar", L"New version %hs available"), g_newVersion);
      _snwprintf(text, 700, TR(L"Horizon Connect %hs ist erschienen (du hast " HC_VERSION_W L"). A öffnet die Download-Seite im Browser; zurück ins Spiel mit Alt+Tab. Zum Aktualisieren den Ordner „Horizon Connect“ in deinem mods-Ordner durch den aus der ZIP ersetzen.",
                               L"Horizon Connect %hs is out (you have " HC_VERSION_W L"). A opens the download page in your browser; back to the game with Alt+Tab. To update, replace the \"Horizon Connect\" folder in your mods folder with the one from the ZIP."), g_newVersion);
      add_row(rows, &n, ROW_UPDATE, -1, label, text);
    }
    g_pingAt = 0; ping_servers();
    for (int i = 0; i < g_serverCount; i++) {
      _snwprintf(text, 700, TR(L"Adresse: %ls\nA: beitreten   X: bearbeiten   Y: löschen",
                               L"Address: %ls\nA: join   X: edit   Y: delete"), g_servers[i].address);
      add_row(rows, &n, ROW_SERVER, i, g_servers[i].name, text);
    }
    if (!g_serverCount)
      add_row(rows, &n, ROW_EMPTY, -1, TR(L"Noch keine Server", L"No servers yet"),
              TR(L"Hier stehen die Server deiner Freunde. Lass dir die Adresse vom Host schicken (z. B. über Discord) und füge sie mit „Server hinzufügen“ ein – oder hoste selbst.",
                 L"Your friends' servers appear here. Ask the host for the address (e.g. on Discord) and add it with \"Add server\" – or host one yourself."));
    add_row(rows, &n, ROW_ADD, -1, TR(L"Server hinzufügen", L"Add server"),
            TR(L"Tipp: Kopiere die Adresse vom Host zuerst (Strg+C, z. B. in Discord) – sie wird dann automatisch eingetragen.",
               L"Tip: copy the host's address first (Ctrl+C, e.g. in Discord) – it is then filled in for you."));
    add_row(rows, &n, ROW_HOST, -1, TR(L"Server hosten", L"Host a server"),
            TR(L"Deinen eigenen Server starten und Freunde einladen. Beim ersten Mal wird er einmalig eingerichtet.",
               L"Start your own server and invite friends. The first time it is set up once."));
    x = TR(L"Bearbeiten", L"Edit"); y = TR(L"Löschen", L"Delete");
  } else if (kind == PAGE_EDIT) {
    title = g_editIndex < 0 ? TR(L"SERVER HINZUFÜGEN", L"ADD SERVER") : TR(L"SERVER BEARBEITEN", L"EDIT SERVER");
    add_row(rows, &n, ROW_ADDRESS, -1, TR(L"Adresse", L"Address"),
            TR(L"Die Adresse vom Host, z. B. name.ply.gg:12345. Kopiere sie (Strg+C) und drücke hier A, um sie einzufügen.",
               L"The host's address, e.g. name.ply.gg:12345. Copy it (Ctrl+C) and press A here to paste it."));
    add_row(rows, &n, ROW_NAME, -1, TR(L"Name", L"Name"),
            TR(L"Wie der Server in deiner Liste heißt. Ohne eigenen Namen wird der erste Teil der Adresse genommen. A: kopierten Text als Namen übernehmen.",
               L"What the server is called in your list. Without a name the first part of the address is used. A: use the copied text as the name."));
    add_row(rows, &n, ROW_SAVE, -1, TR(L"Speichern", L"Save"), TR(L"Den Server oben in deine Liste übernehmen.", L"Put the server at the top of your list."));
    add_row(rows, &n, ROW_CANCEL, -1, TR(L"Abbrechen", L"Cancel"), TR(L"Ohne Änderungen zurück zur Liste.", L"Back to the list without changes."));
    if (g_editIndex >= 0) add_row(rows, &n, ROW_DELETE, -1, TR(L"Server löschen", L"Delete server"), TR(L"Den Server aus der Liste entfernen (zweimal A).", L"Remove the server from the list (A twice)."));
  } else if (kind == PAGE_HOST) {
    title = TR(L"SERVER HOSTEN", L"HOST A SERVER");
    g_hostPageSetUp = host_set_up();
    if (!g_hostPageSetUp) {
      add_row(rows, &n, ROW_SETUP, -1, TR(L"Hosten einrichten", L"Set up hosting"),
              TR(L"Einmalig: A drücken. Dein Browser öffnet die Seite von playit.gg (kostenlos, damit Freunde über das Internet zu dir kommen). Dort anmelden oder ein Konto anlegen und bestätigen, dann mit Alt+Tab zurück ins Spiel. Danach erscheinen hier Starten und deine Adresse.",
                 L"Once: press A. Your browser opens playit.gg (free, so friends can reach you over the internet). Log in or sign up there and confirm, then come back with Alt+Tab. Start and your address then appear here."));
    } else {
      g_pingAt = 0; ping_servers();
      add_row(rows, &n, ROW_HOSTJOIN, -1, TR(L"Server starten und beitreten", L"Start the server and join"),
              TR(L"A: deinen Server starten (falls er noch nicht läuft) und gleich beitreten – die Private Freie Fahrt Online beginnt.",
                 L"A: start your server (unless it runs) and join right away – Private Free Roam Online begins."));
      add_row(rows, &n, ROW_HOSTADDR, -1, TR(L"Adresse für Freunde", L"Address for friends"),
              TR(L"Schick diese Adresse deinen Freunden (z. B. über Discord); sie fügen sie bei „Server hinzufügen“ ein. A: kopieren.",
                 L"Send this address to your friends (e.g. on Discord); they add it under \"Add server\". A: copy it."));
      add_row(rows, &n, ROW_HOSTTOGGLE, -1, TR(L"Server beenden", L"Stop the server"),
              TR(L"A: deinen Server beenden – deine Freunde können dann nicht mehr beitreten. Er endet auch, wenn du das Spiel schließt.",
                 L"A: stop your server – your friends can't join any more. It also stops when you close the game."));
    }
  }
  g_rows = n;
  g_deleteArmed = -1;
  LONG id = InterlockedIncrement(&g_pageSeq) % PAGE_POOL + 1;
  if (!write_page(id, title, rows, n, a, x, y)) return 0;
  refresh_rows(0);
  void *dialog = NULL;
  ((void **(*)(void **))(g_game + UI_VIDEO_FACTORY))(&dialog);
  if (!dialog) return 0;
  if (*(void **)dialog != g_game + UI_VIDEO_VTABLE) { ui_release(dialog); return 0; }
  wchar_t uri[64]; _snwprintf(uri, 64, L"Anthem\\HorizonConnect_%ld", id);
  ((void *(*)(void *, const wchar_t *, size_t))(g_game + UI_WSTRING_ASSIGN))((unsigned char *)dialog + 0x250, uri, wcslen(uri));
  void *screens = ((void *(*)(void))(g_game + UI_SCREENS))();
  if (!screens) { ui_release(dialog); return 0; }
  g_page = dialog; g_pageKind = kind; g_pageHeld = 1; g_cursor = 0; g_closeRequested = 0; g_pendingA = g_pendingX = 0;
  ((void (*)(void *, void *, void *))(*(void ***)screens)[0x1d8 / 8])(screens, pause, dialog);
  if (*(void **)((unsigned char *)pause + 0x2a0) == dialog) pause_hide(pause);
  else { g_page = NULL; g_pageKind = PAGE_NONE; }
  ui_release(dialog);
  logf("menu: page %d opened as %ls (%d rows)", kind, uri, n);
  return g_page != NULL;
}

// ---- actions ------------------------------------------------------------------------------------------------------

static int g_joinRequested;

static void clip_text(wchar_t *out, int n) {
  wchar_t c[256];
  if (!clipboard_get(c, 256)) { out[0] = 0; return; }
  wchar_t *s = c; while (*s == L' ' || *s == L'\t' || *s == L'\r' || *s == L'\n') s++;
  int len = (int)wcslen(s); while (len && (s[len - 1] == L' ' || s[len - 1] == L'\r' || s[len - 1] == L'\n' || s[len - 1] == L'\t')) s[--len] = 0;
  for (wchar_t *p = s; *p; p++) if (*p == L'\r' || *p == L'\n' || *p == L'|') *p = L' ';
  wcsncpy(out, s, n - 1); out[n - 1] = 0;
}

// The server joined or saved moves to the top of the list (the list starts on it next time).
static void server_to_front(const wchar_t *address) {
  servers_load();
  for (int i = 1; i < g_serverCount; i++)
    if (!_wcsicmp(g_servers[i].address, address)) {
      Server t = g_servers[i];
      memmove(&g_servers[1], &g_servers[0], sizeof t * (size_t)i);
      g_servers[0] = t; servers_save(); break;
    }
}

static void open_add(void) {
  g_editIndex = -1; g_editName[0] = g_editAddress[0] = 0;
  wchar_t t[128], host[256]; int port;   // an address on the clipboard is filled in
  clip_text(t, 128);
  close_page(PAGE_EDIT);
  if (t[0] && parse_address(t, host, 256, &port)) {
    _snwprintf(g_editAddress, 128, L"%ls:%d", host, port);
    say(0, TR(L"aus der Zwischenablage übernommen", L"taken from the clipboard"));
  }
}

static void join_address(const wchar_t *address) {
  if (address && !online_set_join(address)) return;
  if (!address) online_set_join(L"");
  g_joinRequested = 1;
  close_page(PAGE_JOIN);
}

static void action_a(int row) {
  if (row < 0) return;
  switch (g_rowKind[row]) {
  case ROW_SETUP: if (g_hostState <= 0) online_host(); break;
  case ROW_HOSTJOIN:
  case ROW_OWN:
    if (g_hostState == 3) { join_address(NULL); break; }
    if (g_hostState <= 0) { g_startOwn = 1; online_host(); }
    break;
  case ROW_SERVER: {
    Server *s = &g_servers[g_rowServer[row]];
    if (!online_set_join(s->address)) { say(row, TR(L"Adresse nicht gefunden – prüfen", L"Address not found – check it")); break; }
    wchar_t address[128]; wcscpy(address, s->address);
    server_to_front(address);
    join_address(address);
    break;
  }
  case ROW_ADD: case ROW_EMPTY: open_add(); break;
  case ROW_HOST: close_page(PAGE_HOST); break;
  case ROW_UPDATE: ShellExecuteW(NULL, L"open", g_newUrl, NULL, NULL, SW_SHOWNORMAL); say(row, TR(L"Im Browser geöffnet (Alt+Tab)", L"Opened in your browser (Alt+Tab)")); break;
  case ROW_NAME: { wchar_t t[64]; clip_text(t, 41); if (t[0]) wcscpy(g_editName, t); else say(row, TR(L"Zwischenablage ist leer", L"Clipboard is empty")); break; }
  case ROW_ADDRESS: {
    wchar_t t[128], host[256]; int port;
    clip_text(t, 128);
    if (t[0] && parse_address(t, host, 256, &port)) _snwprintf(g_editAddress, 128, L"%ls:%d", host, port);
    else say(row, TR(L"Keine Adresse in der Zwischenablage", L"No address on the clipboard"));
    break;
  }
  case ROW_SAVE: {
    wchar_t host[256]; int port;
    int at = -1; for (int i = 0; i < g_rows; i++) if (g_rowKind[i] == ROW_ADDRESS) at = i;
    if (!g_editAddress[0] || !parse_address(g_editAddress, host, 256, &port)) { say(at, TR(L"Erst eine Adresse einfügen", L"Paste an address first")); break; }
    servers_load();
    int index = g_editIndex >= 0 && g_editIndex < g_serverCount ? g_editIndex : -1;
    if (index < 0) for (int i = 0; i < g_serverCount; i++) if (!_wcsicmp(g_servers[i].address, g_editAddress)) index = i;   // already listed
    if (index < 0) {
      if (g_serverCount == MAX_SERVERS) { say(row, TR(L"Liste ist voll", L"The list is full")); break; }
      index = g_serverCount++;
    }
    wchar_t name[64];
    if (g_editName[0]) wcscpy(name, g_editName);
    else { wcsncpy(name, host, 63); name[63] = 0; wchar_t *dot = wcschr(name, L'.'); if (dot && dot != name) *dot = 0; }
    wcsncpy(g_servers[index].name, name, 63); g_servers[index].name[63] = 0;
    wcsncpy(g_servers[index].address, g_editAddress, 127); g_servers[index].address[127] = 0;
    servers_save();
    logf("menu: server %d saved (%ls, %ls)", index, g_servers[index].name, g_servers[index].address);
    server_to_front(g_editAddress);
    close_page(PAGE_LIST);
    break;
  }
  case ROW_CANCEL: close_page(PAGE_LIST); break;
  case ROW_DELETE:
    if (g_deleteArmed != g_editIndex) { g_deleteArmed = g_editIndex; break; }
    servers_load();
    if (g_editIndex >= 0 && g_editIndex < g_serverCount) {
      logf("menu: server %d deleted (%ls)", g_editIndex, g_servers[g_editIndex].address);
      memmove(&g_servers[g_editIndex], &g_servers[g_editIndex + 1], sizeof g_servers[0] * (size_t)(g_serverCount - g_editIndex - 1));
      g_serverCount--; servers_save();
    }
    close_page(PAGE_LIST);
    break;
  case ROW_HOSTTOGGLE:
    if (g_hostState == 3) { online_unhost(); say(row, TR(L"beendet", L"stopped")); }
    break;
  case ROW_HOSTADDR:
    if (g_hostAddress[0] && clipboard_set(g_hostAddress)) say(row, TR(L"kopiert – mit Strg+V verschicken", L"copied – send it with Ctrl+V"));
    break;
  case ROW_OWNNAME: { wchar_t t[64]; clip_text(t, 41); if (t[0]) { wcscpy(g_ownName, t); servers_save(); } else say(row, TR(L"Zwischenablage ist leer", L"Clipboard is empty")); break; }
  }
}

static void action_x(int row) {   // edit
  if (row < 0 || g_pageKind != PAGE_LIST) return;
  if (g_rowKind[row] != ROW_SERVER) return;
  g_editIndex = g_rowServer[row];
  wcscpy(g_editName, g_servers[g_editIndex].name); wcscpy(g_editAddress, g_servers[g_editIndex].address);
  close_page(PAGE_EDIT);
}

static void action_y(int row) {   // delete (asks once more)
  if (row < 0 || g_pageKind != PAGE_LIST || g_rowKind[row] != ROW_SERVER) return;
  int i = g_rowServer[row];
  if (g_deleteArmed != i) { g_deleteArmed = i; say(row, TR(L"Nochmal Y = löschen", L"Y again = delete")); return; }
  servers_load();
  if (i < g_serverCount) {
    logf("menu: server %d deleted (%ls)", i, g_servers[i].address);
    memmove(&g_servers[i], &g_servers[i + 1], sizeof g_servers[0] * (size_t)(g_serverCount - i - 1));
    g_serverCount--; servers_save();
  }
  close_page(PAGE_LIST);
}

// ---- hooks --------------------------------------------------------------------------------------------------------

// The page's own button handling: on our pages A and X are ours (the game's A would accept and close the page).
typedef char (*ButtonFn)(void *, int, char);
static ButtonFn g_videoButtonNext;
// Button ids the page gets: A 18, B 45, X 67, up 24 (and 82), down 26 (and 76). The game's list moves its focus
// on up/down (we count along: the list's SelectedIndex doesn't follow the focus); A and X are ours.
enum { BTN_A = 18, BTN_B = 45, BTN_X = 67, BTN_UP = 24, BTN_DOWN = 26 };
static char hook_video_button(void *page, int button, char pressed) {
  if (page && page == g_page) {
    if (button == BTN_UP && pressed && g_cursor > 0) g_cursor--;
    if (button == BTN_DOWN && pressed && g_cursor < g_rows - 1) g_cursor++;
    if (button == BTN_A || button == BTN_X) {
      if (pressed && !g_pageHeld) { if (button == BTN_A) g_pendingA = 1; else g_pendingX = 1; }
      return 1;   // not the game's: its A would accept and close the page
    }
  }
  return g_videoButtonNext(page, button, pressed);
}

// Y on a VideoOptions page is "reset to defaults"; on ours it is our Y (and the video settings stay as they are).
static void hook_video_reset(void *page) {
  if (page && page == g_page) { action_y(g_cursor); refresh_rows(1); return; }
  g_videoResetNext(page);
}

static void page_update(void *pause) {
  void *top = *(void **)((unsigned char *)pause + 0x2a0);
  if (!g_page) return;
  if (top != g_page) {   // closed (by B or by us)
    g_page = NULL; g_closeRequested = 0; g_pendingA = g_pendingX = 0;
    if (g_next == PAGE_NONE) g_startOwn = 0;   // left with B: no joining later on its own
    if (g_next == PAGE_NONE && g_pageKind != PAGE_LIST) { g_next = PAGE_LIST; g_nextAt = GetTickCount64(); }   // B on a sub-page: back to the list
    g_pageKind = PAGE_NONE;
    return;
  }
  if (!g_hiddenPause) pause_hide(pause);
  if (g_pageHeld) {   // what was held when the page opened doesn't count
    void *selected = NULL; unsigned st = pad_buttons(&selected);
    if (!(st & (PAD_A | PAD_X))) g_pageHeld = 0;
  }
  if (g_pendingA) { g_pendingA = 0; logf("menu: A on row %d", g_cursor); action_a(g_cursor); }
  if (g_pendingX) { g_pendingX = 0; logf("menu: X on row %d", g_cursor); action_x(g_cursor); }
  if (g_page && g_startOwn && g_hostState == 3) { g_startOwn = 0; join_address(NULL); }
  // the host page right after the first setup: show Start and the address
  if (g_page && g_pageKind == PAGE_HOST && !g_hostPageSetUp && host_set_up() && !g_closeRequested) close_page(PAGE_HOST);
  if (g_startOwn && g_hostState < 0) g_startOwn = 0;
  if (g_page && (g_pageKind == PAGE_LIST || g_pageKind == PAGE_HOST)) ping_servers();
  if (g_page) refresh_rows(1);
  if (g_page && g_closeRequested == 1 && g_videoButtonNext) {
    g_closeRequested = 2;
    g_videoButtonNext(g_page, BTN_B, 1);
    if (g_page) g_videoButtonNext(g_page, BTN_B, 0);
  }
}

static void sample_pause_input(void *page, int initial) {
  void *selected = NULL; unsigned state = pad_buttons(&selected);
  int focus = window_focused();
  int down = (state & PAD_VIEW) == PAD_VIEW || (g_menuKey && (GetAsyncKeyState(g_menuKey) & 0x8000));
  if (!initial && g_pausePage == page && selected == g_padSeen && !g_padHeld && down && focus && !g_page && pause_free(page)) {
    logf("menu: opening HORIZON CONNECT");
    g_next = PAGE_LIST; g_nextAt = GetTickCount64();
  }
  g_pausePage = page; g_padSeen = selected; g_padHeld = !focus || down;
}

static void hook_pause_init(void *page) {
  pause_restore();
  g_pauseInitNext(page);
  g_page = NULL; g_pageKind = PAGE_NONE;   // a pending page (g_next) still opens, if it is recent
  sample_pause_input(page, 1);
}

static void hook_pause_update(void *page) {
  g_pauseUpdateNext(page);
  page_update(page);
  if (g_next != PAGE_NONE && GetTickCount64() - g_nextAt > 8000) { logf("menu: page %d dropped (the pause menu wasn't free)", g_next); g_next = PAGE_NONE; }
  if (!g_page && g_next != PAGE_NONE && pause_free(page)) {
    int next = g_next; g_next = PAGE_NONE;
    if (next == PAGE_JOIN) {
      pause_restore();
      if (g_canStartOnline) {
        logf("menu: starting Private Free Roam Online (game traffic to %ls)", g_joinAddress[0] ? g_joinAddress : L"this PC's server");
        ((void (*)(void *, void *))(g_game + PAGE_FIRE_TRANSITION))(page, g_game + PRIVATE_FREEROAM_ID);
      }
    } else if (!open_page(page, next)) pause_return();
  } else if (!g_page && g_next == PAGE_NONE && g_hiddenPause && g_pauseFade != 2) pause_return();
  pause_tick();
  sample_pause_input(page, 0);
}

static int swap_slot(unsigned rva, void *expected, void *with) {
  void **slot = (void **)(g_game + rva); DWORD old, unused;
  if (*slot != expected || !VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return 0;
  *slot = with; VirtualProtect(slot, 8, old, &unused); return 1;
}

// Chains onto whatever is in the slots now (the game's functions, or another plugin's, e.g. EO Weather Control).
static int menu_install(const wchar_t *plugin_dir) {
  menu_key_load();
  _snwprintf(g_mediaRoot, MAX_PATH, L"%lsHorizonConnect\\Media", plugin_dir);
  _snwprintf(g_scenes, MAX_PATH, L"%ls\\UI\\Scenes\\Anthem", g_mediaRoot);
  wchar_t p[MAX_PATH];
  CreateDirectoryW(g_mediaRoot, NULL);
  _snwprintf(p, MAX_PATH, L"%ls\\UI", g_mediaRoot); CreateDirectoryW(p, NULL);
  _snwprintf(p, MAX_PATH, L"%ls\\UI\\Scenes", g_mediaRoot); CreateDirectoryW(p, NULL); CreateDirectoryW(g_scenes, NULL);
  // the pool stays from run to run, so its files are there when the game starts; missing ones are made now
  // (seen from the next start on)
  RowSpec blank = { ROW_CANCEL, -1, L" ", L" " };
  for (LONG i = 1; i <= PAGE_POOL; i++) {
    _snwprintf(p, MAX_PATH, L"%ls\\HorizonConnect_%ld.xaml", g_scenes, i);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) write_page(i, L"HORIZON CONNECT", &blank, 1, L" ", NULL, NULL);
  }
  int32_t disp; memcpy(&disp, g_game + PRIVATE_FREEROAM_TILE + 3, 4);
  g_canStartOnline = !memcmp(g_game + PAGE_FIRE_TRANSITION, "\x48\x8b\x89\x10\x03\x00\x00\x80\x79\x78\x00\x74\x09\x48\x83\xc1\x60", 17) &&
                     !memcmp(g_game + PRIVATE_FREEROAM_TILE, "\x48\x8d\x15", 3) && PRIVATE_FREEROAM_TILE + 7 + disp == PRIVATE_FREEROAM_ID;
  if (!g_canStartOnline) logf("menu: unexpected game code; joining won't start the online free roam by itself");
  if (!menu_properties()) return 0;
  g_pauseInitNext = *(UiAction *)(g_game + SLOT_PAUSE_INIT);
  g_pauseUpdateNext = *(UiAction *)(g_game + SLOT_PAUSE_UPDATE);
  g_videoResetNext = *(UiAction *)(g_game + SLOT_VIDEO_RESET);
  g_videoButtonNext = *(ButtonFn *)(g_game + SLOT_VIDEO_BUTTON);
  if (!g_pauseInitNext || !g_pauseUpdateNext || !g_videoResetNext) return 0;
  if (!swap_slot(SLOT_PAUSE_INIT, (void *)g_pauseInitNext, (void *)hook_pause_init)) return 0;
  if (!swap_slot(SLOT_PAUSE_UPDATE, (void *)g_pauseUpdateNext, (void *)hook_pause_update)) {
    swap_slot(SLOT_PAUSE_INIT, (void *)hook_pause_init, (void *)g_pauseInitNext); return 0;
  }
  if (!swap_slot(SLOT_VIDEO_RESET, (void *)g_videoResetNext, (void *)hook_video_reset)) {
    swap_slot(SLOT_PAUSE_UPDATE, (void *)hook_pause_update, (void *)g_pauseUpdateNext);
    swap_slot(SLOT_PAUSE_INIT, (void *)hook_pause_init, (void *)g_pauseInitNext); return 0;
  }
  if (!g_videoButtonNext || !swap_slot(SLOT_VIDEO_BUTTON, (void *)g_videoButtonNext, (void *)hook_video_button)) {
    g_videoButtonNext = NULL; logf("menu: the page's button handler couldn't be hooked");
  }
  g_menuReady = 1;
  logf("menu: ready (%ls / View in the pause menu)", g_menuKey ? g_menuKeyName : L"no key");
  return 1;
}

__declspec(dllexport) const wchar_t *WINAPI FH2PluginMediaDirectory(void) { return g_menuReady ? g_mediaRoot : NULL; }
