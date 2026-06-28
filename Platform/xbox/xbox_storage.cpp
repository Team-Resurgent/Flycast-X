// ============================================================================
//  xbox_storage.cpp  --  real Xbox hostfs::storage(). The desktop storage.cpp
//  uses drive-letter enumeration + SHGetFolderPath (no Xbox analogue), so we
//  provide a concrete AllStorage backed by stdio (RXDK CRT) via the StdFile
//  wrapper that already ships in storage.h. Files live under the data dir.
// ============================================================================
#include "types.h"
#include "oslib/storage.h"
#include <cstdio>
#include <string>
#include <vector>

extern "C" void __stdcall OutputDebugStringA(const char*);

namespace hostfs
{

class XboxStorage : public AllStorage
{
public:
    bool isKnownPath(const std::string& /*path*/) override { return true; }

    File* openFile(const std::string& path, const std::string& mode) override
    {
        FILE* f = std::fopen(path.c_str(), mode.c_str());
        OutputDebugStringA(f ? "FLYCAST openFile OK: " : "FLYCAST openFile FAIL: ");
        OutputDebugStringA(path.c_str());
        OutputDebugStringA("\n");
        return f ? new StdFile(f) : nullptr;
    }

    std::vector<FileInfo> listContent(const std::string& /*path*/) override
    {
        return std::vector<FileInfo>();   // directory browsing not needed headless
    }

    std::string getParentPath(const std::string& path) override
    {
        size_t s = path.find_last_of("\\/");
        return s == std::string::npos ? path : path.substr(0, s);
    }

    std::string getSubPath(const std::string& reference, const std::string& subpath) override
    {
        if (!reference.empty() && (reference.back() == '\\' || reference.back() == '/'))
            return reference + subpath;
        return reference + "\\" + subpath;
    }

    FileInfo getFileInfo(const std::string& path) override
    {
        FileInfo fi;
        fi.path = path;
        size_t s = path.find_last_of("\\/");
        fi.name = (s == std::string::npos) ? path : path.substr(s + 1);
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f) { std::fseek(f, 0, SEEK_END); fi.size = (size_t)std::ftell(f); std::fclose(f); }
        return fi;
    }

    bool exists(const std::string& path) override
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f) { std::fclose(f); return true; }
        return false;
    }

    std::string getDefaultDirectory() { return "D:\\Media\\"; }
};

AllStorage& storage()
{
    static XboxStorage inst;
    return inst;
}

} // namespace hostfs
