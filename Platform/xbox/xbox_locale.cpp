// ============================================================================
//  xbox_locale.cpp  --  the XDK-native (C99/ASCII) "C"-locale DATA layer.
//
//  The modern C++ std::locale splits into (1) facet CONTAINER templates
//  (standard library) and (2) a platform DATA layer (xlocinfo) that on desktop
//  queries the Windows locale. The Xbox has no Windows locale, so we provide
//  that data layer ourselves as a fixed ASCII "C" locale -- ctype tables, case
//  mapping, single-byte conversion, byte-wise collation. The std-lib facet
//  templates consume this and Just Work; nothing queries the OS.
// ============================================================================
#include <locale>     // full std::locale machinery (_Locimp/_Locinfo/facets) + xlocinfo
#include <ios>        // std::ios_base (for _Ios_base_dtor / _Addstd)
#include <cstring>
#include <cstdlib>

// ---- ASCII "C" ctype classification table (indexed [-1..255]) --------------
//  table[-1] is the EOF slot; we return &table[1].
static short  g_ctype[257];
static bool   g_ctype_ready = false;

static const short* classic_ctype_table()
{
    if (!g_ctype_ready) {
        for (int c = 0; c < 256; ++c) {
            short m = 0;
            if (c >= 'A' && c <= 'Z') m |= _UPPER | _ALPHA;
            if (c >= 'a' && c <= 'z') m |= _LOWER | _ALPHA;
            if (c >= '0' && c <= '9') m |= _DIGIT | _HEX;
            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) m |= _HEX;
            if (c == ' ' || (c >= '\t' && c <= '\r')) m |= _SPACE;
            if (c == ' ' || c == '\t') m |= _BLANK;
            if (c < 0x20 || c == 0x7f) m |= _CONTROL;
            if (c >= 0x21 && c <= 0x7e && !(m & (_ALPHA | _DIGIT))) m |= _PUNCT;
            g_ctype[c + 1] = m;
        }
        g_ctype[0] = 0;     // EOF slot
        g_ctype_ready = true;
    }
    return g_ctype + 1;
}

extern "C" {

// ---- vectors describing the locale -----------------------------------------
_Ctypevec __cdecl _Getctype() noexcept
{
    _Ctypevec v;
    v._Page       = 0;
    v._Table      = classic_ctype_table();
    v._Delfl      = 0;          // do not free our static table
    v._LocaleName = nullptr;
    return v;
}

_Cvtvec __cdecl _Getcvt() noexcept
{
    _Cvtvec v;
    std::memset(&v, 0, sizeof(v));
    v._Page     = 0;
    v._Mbcurmax = 1;            // single-byte
    v._Isclocale = 1;          // "C" locale
    return v;
}

_Collvec __cdecl _Getcoll() noexcept
{
    _Collvec v;
    std::memset(&v, 0, sizeof(v));   // _Page 0, name null -> byte-wise collation
    return v;
}

int __cdecl _Getdateorder() noexcept { return 0; }   // MDY

// ---- case mapping ----------------------------------------------------------
int __cdecl _Tolower(int c, const _Ctypevec*) noexcept { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int __cdecl _Toupper(int c, const _Ctypevec*) noexcept { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
wchar_t __cdecl _Towlower(wchar_t c, const _Ctypevec*) noexcept { return (c >= L'A' && c <= L'Z') ? c + 32 : c; }
wchar_t __cdecl _Towupper(wchar_t c, const _Ctypevec*) noexcept { return (c >= L'a' && c <= L'z') ? c - 32 : c; }

short __cdecl _Getwctype(wchar_t c, const _Ctypevec*) noexcept
{
    return (c >= 0 && c < 256) ? classic_ctype_table()[(int)c] : (short)0;
}
const wchar_t* __cdecl _Getwctypes(const wchar_t* first, const wchar_t* last, short* out, const _Ctypevec*) noexcept
{
    for (; first != last; ++first, ++out)
        *out = (*first >= 0 && *first < 256) ? classic_ctype_table()[(int)*first] : (short)0;
    return last;
}

// ---- single-byte <-> wide conversion (ASCII passthrough) -------------------
int __cdecl _Mbrtowc(wchar_t* wc, const char* s, size_t n, mbstate_t*, const _Cvtvec*) noexcept
{
    if (!s || n == 0) return -2;            // incomplete
    unsigned char ch = (unsigned char)*s;
    if (wc) *wc = (wchar_t)ch;
    return ch != 0 ? 1 : 0;
}

// ---- collation: byte-wise / code-unit-wise ---------------------------------
int __cdecl _Strcoll(const char* f1, const char* l1, const char* f2, const char* l2, const _Collvec*) noexcept
{
    for (; f1 != l1 && f2 != l2; ++f1, ++f2) {
        unsigned char a = (unsigned char)*f1, b = (unsigned char)*f2;
        if (a != b) return a < b ? -1 : 1;
    }
    if (f1 == l1 && f2 == l2) return 0;
    return f1 == l1 ? -1 : 1;
}
size_t __cdecl _Strxfrm(char* d1, char* dl, const char* s1, const char* sl, const _Collvec*) noexcept
{
    size_t n = (size_t)(sl - s1);
    size_t room = (size_t)(dl - d1);
    for (size_t i = 0; i < n && i < room; ++i) d1[i] = s1[i];
    return n;
}
int __cdecl _Wcscoll(const wchar_t* f1, const wchar_t* l1, const wchar_t* f2, const wchar_t* l2, const _Collvec*) noexcept
{
    for (; f1 != l1 && f2 != l2; ++f1, ++f2)
        if (*f1 != *f2) return *f1 < *f2 ? -1 : 1;
    if (f1 == l1 && f2 == l2) return 0;
    return f1 == l1 ? -1 : 1;
}
size_t __cdecl _Wcsxfrm(wchar_t* d1, wchar_t* dl, const wchar_t* s1, const wchar_t* sl, const _Collvec*) noexcept
{
    size_t n = (size_t)(sl - s1);
    size_t room = (size_t)(dl - d1);
    for (size_t i = 0; i < n && i < room; ++i) d1[i] = s1[i];
    return n;
}

// ---- time names: English "C"-locale, malloc'd (caller frees) ---------------
//  Format expected by the STL: colon-separated, leading colon, abbrev+full.
char* __cdecl _Getdays()   { return _strdup(":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:Saturday"); }
char* __cdecl _Getmonths() { return _strdup(":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:Aug:August:Sep:September:Oct:October:Nov:November:Dec:December"); }
void* __cdecl _Gettnames() { return nullptr; }

size_t __cdecl _Strftime(char* s, size_t n, const char*, const struct tm*, void*) { if (s && n) s[0] = 0; return 0; }

} // extern "C"

// ============================================================================
//  Native std::locale machinery (no VS runtime objects).
//
//  The facet template classes (ctype<char>, num_get<char>, ...) compile from the
//  STL headers into THIS object -> our toolchain, our (working) EH. We provide
//  the out-of-line container/bootstrap functions the headers reference. Because
//  our data layer above is a fixed "C" locale, _Locinfo just records the name.
// ============================================================================
namespace std {

// ---- _Locinfo: record the name; all data comes from the global _Get* funcs ---
void __CLRCALL_PURE_OR_CDECL _Locinfo::_Locinfo_ctor(_Locinfo* p, const char* name)       { p->_Newlocname = name ? name : "C"; }
void __CLRCALL_PURE_OR_CDECL _Locinfo::_Locinfo_ctor(_Locinfo* p, int, const char* name)  { p->_Newlocname = name ? name : "C"; }
void __CLRCALL_PURE_OR_CDECL _Locinfo::_Locinfo_dtor(_Locinfo*)                            {}
_Locinfo& __CLRCALL_PURE_OR_CDECL _Locinfo::_Locinfo_Addcats(_Locinfo* p, int, const char* name) { if (name) p->_Newlocname = name; return *p; }

// ---- _Locimp: a growable facet vector ---------------------------------------
locale::_Locimp* __CLRCALL_PURE_OR_CDECL locale::_Locimp::_New_Locimp(bool _Transparent) { return new _Locimp(_Transparent); }
locale::_Locimp* __CLRCALL_PURE_OR_CDECL locale::_Locimp::_New_Locimp(const _Locimp& _Right) { return new _Locimp(_Right); }

void __CLRCALL_PURE_OR_CDECL locale::_Locimp::_Locimp_Addfac(_Locimp* p, facet* f, size_t id)
{
    if (id >= p->_Facetcount) {
        size_t n = id + 1;
        facet** nv = static_cast<facet**>(::realloc(p->_Facetvec, n * sizeof(facet*)));
        for (size_t i = p->_Facetcount; i < n; ++i) nv[i] = nullptr;
        p->_Facetvec   = nv;
        p->_Facetcount = n;
    }
    if (p->_Facetvec[id]) delete p->_Facetvec[id]->_Decref();
    p->_Facetvec[id] = f;
    f->_Incref();
}

void __CLRCALL_PURE_OR_CDECL locale::_Locimp::_Locimp_dtor(_Locimp* p)
{
    for (size_t i = 0; i < p->_Facetcount; ++i)
        if (p->_Facetvec[i]) delete p->_Facetvec[i]->_Decref();
    ::free(p->_Facetvec);
    p->_Facetvec = nullptr;
    p->_Facetcount = 0;
}

void __CLRCALL_PURE_OR_CDECL locale::_Locimp::_Locimp_ctor(_Locimp* p, const _Locimp& r)
{
    p->_Facetcount = r._Facetcount;
    p->_Facetvec = static_cast<facet**>(::malloc(r._Facetcount * sizeof(facet*)));
    for (size_t i = 0; i < r._Facetcount; ++i) {
        p->_Facetvec[i] = r._Facetvec[i];
        if (p->_Facetvec[i]) p->_Facetvec[i]->_Incref();
    }
}

// ---- _Init / global locale: build the classic "C" locale once ---------------
static locale::_Locimp* g_classic = nullptr;

locale::_Locimp* __CLRCALL_PURE_OR_CDECL locale::_Init(bool)
{
    if (!g_classic) {
        _Locimp* imp = _Locimp::_New_Locimp(false);
        _Locinfo li("C");
        // ::std:: qualify ctype/collate to dodge legacy global ::ctype/::collate.
        imp->_Addfac(new ::std::ctype<char>(reinterpret_cast<const ::std::ctype<char>::mask*>(classic_ctype_table()), false, 1), ::std::ctype<char>::id._Get_index());
        imp->_Addfac(new codecvt<char, char, mbstate_t>(1),                     codecvt<char, char, mbstate_t>::id._Get_index());
        imp->_Addfac(new numpunct<char>(li, 1, true),                           numpunct<char>::id._Get_index());
        imp->_Addfac(new num_get<char>(1),                                      num_get<char>::id._Get_index());
        imp->_Addfac(new num_put<char>(1),                                      num_put<char>::id._Get_index());
        imp->_Addfac(new ::std::collate<char>(1),                               ::std::collate<char>::id._Get_index());
        // The classic locale is ETERNAL. A single +1 isn't enough: streambuf
        // locale teardown over-decrements (a stringbuf lifecycle nets the count
        // down past 0), which frees g_classic and its facets -> the next
        // _Incref calls through a freed/reused vtable -> _purecall. Pin the
        // refcount astronomically high so no realistic boot decref reaches 0.
        // _Myrefs lives at offset +4 (after the vptr); the disasm of _Incref/
        // _Decref confirms `lock xadd [this+4]`.
        ((unsigned*)imp)[1] = 0x40000000u;   // ~1e9 refs: never deleted
        _Locimp::_Clocptr = imp;
        g_classic = imp;
    }
    return g_classic;
}

locale::_Locimp* __CLRCALL_PURE_OR_CDECL locale::_Getgloballocale() { return _Init(false); }

// locale::classic() is _MRTIMP2_PURE (out-of-line in the CRT) -> without this it
// binds to RXDK's old libcpmt, returning a facet-less classic _Locimp that
// crashes use_facet (e.g. std::tolower(c, locale::classic()) in LogManager).
// We never call locale::global(), so the global locale IS the classic one, and
// a default-constructed locale wraps g_classic. Heap-allocate once (eternal).
const locale& __CLRCALL_PURE_OR_CDECL locale::classic()
{
    static locale* c = new locale();
    return *c;
}

// ---- ios_base / facet registration (minimal) --------------------------------
void __CLRCALL_PURE_OR_CDECL _Facet_Register(_Facet_base*) {}

// ios_base teardown: free the per-stream locale + sparse arrays (_Tidy is inline).
void __CLRCALL_PURE_OR_CDECL ios_base::_Ios_base_dtor(ios_base* p)
{
    delete p->_Ploc;
    p->_Ploc = nullptr;
    p->_Tidy();
}
// _Addstd marks cin/cout/... as "standard" to suppress teardown; no-op for boot.
void __CLRCALL_PURE_OR_CDECL ios_base::_Addstd(ios_base*) {}

} // namespace std
