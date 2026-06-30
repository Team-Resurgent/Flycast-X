/*==========================================================================
  xbox_filebrowser.cpp  --  Pre-boot file browser for the Flycast Xbox port.

  Lets the user navigate the Xbox drives/folders with the controller and pick a
  Dreamcast disc image (.gdi/.cdi/.chd/.iso/.cue) to launch -- or choose
  "Boot Dreamcast BIOS (no disc)" to go straight to the DC dashboard/BIOS menu.

  xbox_RunFileBrowser() runs a self-contained D3D8 render+input loop and blocks
  until the user selects something, then returns the chosen path (or "" for the
  BIOS). main_xbox.cpp passes that to emu.loadGame().

  Rendering reuses the proven SMW/Zelda approach: an embedded 8x8 bitmap font
  drawn as pre-transformed (XYZRHW) coloured quads via DrawPrimitiveUP. Quads are
  flushed in bounded chunks so a long file list never overruns the push buffer.

  Drives: we probe the usual OG Xbox letters with FindFirstFile and show the ones
  that respond. When launched from a dashboard (UnleashX/XBMC/EvoX), E:/F:/G:
  are already mapped and persist into this title, so the HDD shows up.
==========================================================================*/

#include <xtl.h>
#include <cstdio>
#include <cstring>
#include <vector>

extern IDirect3DDevice8* g_xbox_d3d_dev;
extern "C" bool xbox_ReadRawPad(XINPUT_STATE* out);   // xbox_input.cpp

//--------------------------------------------------------------------------
// Drive mounting.
//   A title only inherits D: (media) and C: (system); the HDD game partitions
//   (E:/F:/G:/...) aren't mapped into our process, so FindFirstFile can't see
//   them. Create the symbolic links ourselves via the Xbox kernel. The raw
//   IoCreateSymbolicLink isn't in the RXDK headers but IS exported by
//   xboxkrnl.lib (already linked) -- declare it with the verified __stdcall
//   decoration. Partition->letter map is the canonical OG Xbox layout.
//   Linking a non-existent partition is harmless: FindFirstFile then just won't
//   list it, so only genuinely available drives appear in the browser.
//--------------------------------------------------------------------------
typedef struct { USHORT Length; USHORT MaximumLength; PCHAR Buffer; } XANSI_STRING;
extern "C" LONG __stdcall IoCreateSymbolicLink(XANSI_STRING* LinkName, XANSI_STRING* DeviceName);
extern "C" LONG __stdcall IoDeleteSymbolicLink(XANSI_STRING* LinkName);

static void xstr(XANSI_STRING* s, const char* str)
{
	s->Length        = (USHORT)strlen(str);
	s->MaximumLength = (USHORT)(s->Length + 1);
	s->Buffer        = (PCHAR)str;
}

static void mapDrive(char letter, const char* device)
{
	char link[16];
	_snprintf(link, sizeof(link), "\\??\\%c:", letter);
	XANSI_STRING l, d;
	xstr(&l, link);
	xstr(&d, device);
	IoDeleteSymbolicLink(&l);          // drop any stale mapping (ignore failure)
	IoCreateSymbolicLink(&l, &d);      // create (ignore failure: partition may not exist)
}

static void mountGameDrives()
{
	// letter -> \Device\Harddisk0\PartitionN (canonical OG Xbox). E=1; F..N=6..14.
	// C/D are left alone (already mapped: system + this title's media).
	static const struct { char letter; const char* dev; } kMaps[] = {
		{ 'E', "\\Device\\Harddisk0\\Partition1"  },
		{ 'F', "\\Device\\Harddisk0\\Partition6"  },
		{ 'G', "\\Device\\Harddisk0\\Partition7"  },
		{ 'H', "\\Device\\Harddisk0\\Partition8"  },
		{ 'I', "\\Device\\Harddisk0\\Partition9"  },
		{ 'J', "\\Device\\Harddisk0\\Partition10" },
		{ 'K', "\\Device\\Harddisk0\\Partition11" },
		{ 'L', "\\Device\\Harddisk0\\Partition12" },
		{ 'M', "\\Device\\Harddisk0\\Partition13" },
		{ 'N', "\\Device\\Harddisk0\\Partition14" },
	};
	for (size_t i = 0; i < sizeof(kMaps) / sizeof(kMaps[0]); i++)
		mapDrive(kMaps[i].letter, kMaps[i].dev);
}

//--------------------------------------------------------------------------
// 8x8 bitmap font (ASCII 32..95: space, digits, punctuation, A-Z). Verbatim
// from the SMW/Zelda Xbox ports. Lowercase is drawn as uppercase.
//--------------------------------------------------------------------------
static const BYTE kFont8x8[][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    {0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00}, {0x66,0x66,0xFF,0x66,0xFF,0x66,0x66,0x00},
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, {0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00},
    {0x3C,0x66,0x3C,0x38,0x67,0x66,0x3F,0x00}, {0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}, {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00},
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00},
    {0x3C,0x66,0x6E,0x6E,0x60,0x62,0x3C,0x00}, {0x18,0x3C,0x66,0x7E,0x66,0x66,0x66,0x00},
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00},
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00},
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00}, {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, {0x3C,0x66,0x66,0x66,0x6E,0x3C,0x06,0x00},
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
};

//--------------------------------------------------------------------------
// Batched pre-transformed 2D quads, flushed in bounded chunks.
//--------------------------------------------------------------------------
struct V { float x, y, z, rhw; DWORD c; };
#define FVF_2D (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
static const size_t FLUSH_AT = 6000;   // verts (~120 KB) -- well under the push buffer

static std::vector<V> g_verts;

static void applyStates()
{
	IDirect3DDevice8* d = g_xbox_d3d_dev;
	d->SetVertexShader(FVF_2D);
	d->SetTexture(0, NULL);
	d->SetRenderState(D3DRS_ZENABLE, FALSE);
	d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
}

static void flush()
{
	IDirect3DDevice8* d = g_xbox_d3d_dev;
	if (!d || g_verts.empty())
		return;
	applyStates();
	d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(g_verts.size() / 3), &g_verts[0], sizeof(V));
	g_verts.clear();
}

static void rect(float x, float y, float w, float h, DWORD c)
{
	V a = {x, y, 0, 1, c}, b = {x + w, y, 0, 1, c};
	V e = {x, y + h, 0, 1, c}, f = {x + w, y + h, 0, 1, c};
	g_verts.push_back(a); g_verts.push_back(b); g_verts.push_back(e);
	g_verts.push_back(b); g_verts.push_back(f); g_verts.push_back(e);
	if (g_verts.size() >= FLUSH_AT)
		flush();
}

static void drawChar(float x, float y, char c, DWORD col, float s)
{
	if (c >= 'a' && c <= 'z') c -= 32;
	if (c < 32 || c > 95) c = 32;
	const BYTE* g = kFont8x8[(unsigned char)c - 32];
	for (int r = 0; r < 8; r++)
	{
		BYTE bits = g[r];
		for (int col2 = 0; col2 < 8; col2++)
			if (bits & (0x80 >> col2))
				rect(x + col2 * s, y + r * s, s, s, col);
	}
}

static void drawStr(float x, float y, const char* s, DWORD col, float sc, int maxChars)
{
	for (int i = 0; *s && (maxChars <= 0 || i < maxChars); s++, i++, x += 8 * sc)
		drawChar(x, y, *s, col, sc);
}

//--------------------------------------------------------------------------
// Directory / drive listing
//--------------------------------------------------------------------------
enum EType { T_BIOS, T_DRIVE, T_UP, T_DIR, T_FILE };
struct Entry { char name[128]; char display[96]; int type; };

static std::vector<Entry> g_entries;
static char g_cwd[300];      // "" = drive list; else "X:\..\"
static int  g_sel = 0;
static int  g_scroll = 0;
static char g_result[300];

static bool isDiscImage(const char* name)
{
	const char* dot = strrchr(name, '.');
	if (!dot)
		return false;
	static const char* exts[] = { ".gdi", ".cdi", ".chd", ".iso", ".cue", ".cdr", ".lst" };
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
		if (_stricmp(dot, exts[i]) == 0)
			return true;
	return false;
}

static void addEntry(const char* name, const char* display, int type)
{
	Entry e;
	memset(&e, 0, sizeof(e));
	if (name)    { strncpy(e.name, name, sizeof(e.name) - 1); }
	strncpy(e.display, display, sizeof(e.display) - 1);
	e.type = type;
	g_entries.push_back(e);
}

static void rebuild()
{
	g_entries.clear();
	g_sel = 0;
	g_scroll = 0;

	if (g_cwd[0] == 0)
	{
		// Top level: BIOS option + available drives.
		addEntry(NULL, "BOOT DREAMCAST BIOS  (NO DISC)", T_BIOS);
		static const char* drives[] = {
			"D:\\", "C:\\", "E:\\", "F:\\", "G:\\",
			"H:\\", "I:\\", "J:\\", "K:\\", "L:\\", "M:\\", "N:\\"
		};
		for (size_t i = 0; i < sizeof(drives) / sizeof(drives[0]); i++)
		{
			char pat[16];
			_snprintf(pat, sizeof(pat), "%s*", drives[i]);
			WIN32_FIND_DATA fd;
			HANDLE h = FindFirstFile(pat, &fd);
			if (h != INVALID_HANDLE_VALUE)
			{
				char disp[32];
				_snprintf(disp, sizeof(disp), "[ DRIVE %c: ]", drives[i][0]);
				addEntry(drives[i], disp, T_DRIVE);
				FindClose(h);
			}
		}
		return;
	}

	// A directory: ".." then sub-folders, then disc images.
	addEntry(NULL, "[ .. ]", T_UP);

	char pat[300];
	_snprintf(pat, sizeof(pat), "%s*", g_cwd);
	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFile(pat, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;

	// Folders first.
	do {
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
			continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			char disp[96];
			_snprintf(disp, sizeof(disp), "[%s]", fd.cFileName);
			addEntry(fd.cFileName, disp, T_DIR);
		}
	} while (FindNextFile(h, &fd));
	FindClose(h);

	// Then disc-image files.
	h = FindFirstFile(pat, &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			if (isDiscImage(fd.cFileName))
				addEntry(fd.cFileName, fd.cFileName, T_FILE);
		} while (FindNextFile(h, &fd));
		FindClose(h);
	}
}

static void goUp()
{
	size_t n = strlen(g_cwd);
	if (n == 0)
		return;
	if (g_cwd[n - 1] == '\\')           // strip trailing slash
		g_cwd[--n] = 0;
	char* slash = strrchr(g_cwd, '\\');
	if (slash)
		*(slash + 1) = 0;               // keep "X:\..\"
	else
		g_cwd[0] = 0;                   // was "X:" -> back to drive list
	rebuild();
}

// Returns true when a final selection was made (fills g_result); false to stay.
static bool activate()
{
	if (g_sel < 0 || g_sel >= (int)g_entries.size())
		return false;
	const Entry& e = g_entries[g_sel];
	switch (e.type)
	{
	case T_BIOS:
		g_result[0] = 0;               // empty path -> boot to DC BIOS
		return true;
	case T_FILE:
		_snprintf(g_result, sizeof(g_result), "%s%s", g_cwd, e.name);
		return true;
	case T_DRIVE:
		strncpy(g_cwd, e.name, sizeof(g_cwd) - 1);
		rebuild();
		return false;
	case T_DIR:
		_snprintf(g_cwd + strlen(g_cwd), sizeof(g_cwd) - strlen(g_cwd), "%s\\", e.name);
		rebuild();
		return false;
	case T_UP:
		goUp();
		return false;
	}
	return false;
}

//--------------------------------------------------------------------------
// Frame
//--------------------------------------------------------------------------
static const int   VISIBLE = 16;
static const float ROW_H   = 22.0f;
static const float LIST_Y  = 64.0f;

static void drawFrame()
{
	IDirect3DDevice8* d = g_xbox_d3d_dev;
	if (!d)
		return;

	d->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(12, 14, 32), 1.0f, 0);
	d->BeginScene();
	g_verts.clear();

	// Header.
	rect(0, 0, 640, 34, 0xFF1E2A66);
	drawStr(10, 8, "FLYCAST  -  SELECT GAME", 0xFFFFFF00, 2.0f, 0);
	// Current path (small).
	drawStr(10, 40, g_cwd[0] ? g_cwd : "DRIVES", 0xFF80C8FF, 1.0f, 70);

	// Keep selection in the visible window.
	if (g_sel < g_scroll)                g_scroll = g_sel;
	if (g_sel >= g_scroll + VISIBLE)     g_scroll = g_sel - VISIBLE + 1;

	for (int i = 0; i < VISIBLE; i++)
	{
		int idx = g_scroll + i;
		if (idx >= (int)g_entries.size())
			break;
		float y = LIST_Y + i * ROW_H;
		bool  cursor = (idx == g_sel);
		if (cursor)
			rect(6, y - 2, 628, ROW_H, 0x553388FF);
		DWORD col = cursor ? 0xFF60FF60 : 0xFFCCCCCC;
		drawStr(16, y, g_entries[idx].display, col, 2.0f, 38);
	}

	// Scroll hint.
	if ((int)g_entries.size() > VISIBLE)
	{
		char hint[32];
		_snprintf(hint, sizeof(hint), "%d / %d", g_sel + 1, (int)g_entries.size());
		drawStr(540, 40, hint, 0xFF80C8FF, 1.0f, 0);
	}

	// Footer.
	rect(0, 446, 640, 34, 0xFF1E2A66);
	drawStr(10, 454, "A  SELECT      B  BACK      DPAD  MOVE", 0xFFB0B0B0, 1.5f, 0);

	flush();
	d->EndScene();
	d->Present(NULL, NULL, NULL, NULL);
}

//--------------------------------------------------------------------------
// Public entry point: blocks until the user picks a game (path) or BIOS ("").
//--------------------------------------------------------------------------
extern "C" const char* xbox_RunFileBrowser()
{
	g_verts.reserve(FLUSH_AT + 64);
	mountGameDrives();        // map HDD partitions so they show up
	g_cwd[0] = 0;
	g_result[0] = 0;
	rebuild();

	WORD prevBtns = 0;
	BYTE prevA = 0, prevB = 0;

	for (;;)
	{
		XINPUT_STATE st;
		bool have = xbox_ReadRawPad(&st);
		WORD btns = have ? st.Gamepad.wButtons : 0;
		BYTE aBtn = have ? st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_A] : 0;
		BYTE bBtn = have ? st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_B] : 0;

		bool up    = (btns & XINPUT_GAMEPAD_DPAD_UP)   && !(prevBtns & XINPUT_GAMEPAD_DPAD_UP);
		bool down  = (btns & XINPUT_GAMEPAD_DPAD_DOWN) && !(prevBtns & XINPUT_GAMEPAD_DPAD_DOWN);
		bool aHit  = (aBtn > 30) && !(prevA > 30);
		bool bHit  = (bBtn > 30) && !(prevB > 30);

		int count = (int)g_entries.size();
		if (up && count > 0)   g_sel = (g_sel - 1 + count) % count;
		if (down && count > 0) g_sel = (g_sel + 1) % count;
		if (bHit)              goUp();
		if (aHit && activate())
			break;             // selection made -> g_result is set

		drawFrame();

		prevBtns = btns;
		prevA = aBtn;
		prevB = bBtn;
		Sleep(16);
	}

	return g_result;
}
