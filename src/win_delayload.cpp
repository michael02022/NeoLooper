// A CLAP plugin loaded by a host has no control over the DLL search path the
// OS uses to resolve its own imports: that search is anchored to the HOST's
// own directory (e.g. REAPER's install folder), not to NeoLooper.clap's --
// so the vcpkg-provided sndfile.dll/rubberband-3.dll sitting right next to
// NeoLooper.clap are invisible to it even though they load fine from a
// same-folder test harness. Delay-loading those two DLLs (see the
// /DELAYLOAD linker flags in CMakeLists.txt) and resolving them here by an
// absolute path -- our own module's directory, found via our own address,
// plus LOAD_WITH_ALTERED_SEARCH_PATH so their own further dependencies
// (ogg/vorbis/FLAC/opus/mp3lame/samplerate/sleef*) resolve the same way --
// makes the plugin self-sufficient regardless of which process loads it.
//
// LOAD_WITH_ALTERED_SEARCH_PATH only prioritizes our folder for the DLL we
// explicitly load by full path -- NOT for THAT DLL's own further imports.
// sndfile.dll itself implicitly imports FLAC/ogg/vorbis/vorbisenc/opus/
// mpg123/libmp3lame by plain name, and REAPER ships its own (differently
// built, ABI-incompatible) libmp3lame.dll under its Plugins\ folder, which
// the standard search order can find instead of ours -- surfacing as a
// bogus "ordinal not found" error blamed on sndfile.dll. Pre-loading all of
// our own copies by full path (also with LOAD_WITH_ALTERED_SEARCH_PATH)
// before sndfile.dll gets loaded means the OS loader's "already loaded,
// matched by base name" fast path picks up ours instead of searching again.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <delayimp.h>

#include <string>

namespace {

const wchar_t* const kSiblingLibs[] = {
    L"FLAC.dll",     L"ogg.dll",     L"vorbis.dll", L"vorbisenc.dll",
    L"vorbisfile.dll",
    L"opus.dll",     L"mpg123.dll",  L"libmp3lame.DLL",
    L"samplerate.dll", L"sleef.dll", L"sleefdft.dll",
};

std::wstring ownModuleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&ownModuleDir, &self))
        return L"";
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring s(path, n);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
}

void primeSiblingLibsOnce(const std::wstring& dir) {
    static bool done = false;
    if (done) return;
    done = true;
    for (const wchar_t* name : kSiblingLibs) {
        std::wstring full = dir + name;
        LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        // Ignore failures: not every sibling applies to every delay-loaded
        // DLL (e.g. rubberband doesn't use FLAC/ogg/vorbis/opus/mp3lame),
        // and any that do get used will still fail normally, visibly, if
        // truly missing.
    }
}

FARPROC WINAPI delayLoadHook(unsigned dliNotify, DelayLoadInfo* pdli) {
    if (dliNotify != dliNotePreLoadLibrary) return nullptr;

    std::wstring dir = ownModuleDir();
    if (dir.empty()) return nullptr;

    primeSiblingLibsOnce(dir);

    int wlen = MultiByteToWideChar(CP_ACP, 0, pdli->szDll, -1, nullptr, 0);
    if (wlen <= 0) return nullptr;
    std::wstring name((size_t)wlen - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, pdli->szDll, -1, name.data(), wlen);

    // "sndfile.dll" is a common, generic name shared by other audio software
    // that might bundle their own (incompatible) build; a host process that
    // loads many plugins together, like a DAW, could end up resolving some
    // OTHER component's plain-named "sndfile.dll" reference to a mismatched
    // module. We ship our copy under a plugin-unique name on disk (see the
    // POST_BUILD copy in CMakeLists.txt) precisely so nothing else in the
    // host process can ever collide with it, and redirect the linker's
    // expected name to that file here.
    if (_wcsicmp(name.c_str(), L"sndfile.dll") == 0)
        name = L"neolooper_sndfile.dll";

    std::wstring full = dir + name;
    HMODULE h = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    return (FARPROC)h;
}

} // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = delayLoadHook;
