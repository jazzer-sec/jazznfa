#include "runtime_bundle.h"
#include <windows.h>
#include <setupapi.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>
#include "bundle_version.h"

namespace
{
    struct file_entry { std::filesystem::path relative; uintmax_t size; };
    struct extraction { std::filesystem::path directory; const std::vector<file_entry>* files; };

    std::wstring wide(const std::string& value)
    {
        int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (!size) throw std::runtime_error("Invalid runtime manifest encoding.");
        std::wstring result(size, 0);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
        return result;
    }

    std::pair<const char*, DWORD> resource(int id)
    {
        HMODULE module = GetModuleHandleW(nullptr);
        HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!info) throw std::runtime_error("Embedded NFA runtime is missing. Rebuild using Build.ps1.");
        HGLOBAL loaded = LoadResource(module, info);
        auto bytes = static_cast<const char*>(LockResource(loaded));
        if (!bytes) throw std::runtime_error("Could not read the embedded NFA runtime.");
        return { bytes, SizeofResource(module, info) };
    }

    std::vector<file_entry> read_manifest()
    {
        auto [bytes, length] = resource(102);
        std::istringstream input(std::string(bytes, length));
        std::vector<file_entry> result;
        std::string line;
        while (std::getline(input, line))
        {
            auto split = line.find('\t');
            if (split == std::string::npos) throw std::runtime_error("Invalid runtime manifest.");
            auto relative = std::filesystem::path(wide(line.substr(0, split)));
            if (relative.is_absolute() || relative.has_root_path()) throw std::runtime_error("Invalid runtime path.");
            for (const auto& part : relative) if (part == L".." || part == L".") throw std::runtime_error("Invalid runtime path.");
            result.push_back({ relative, std::stoull(line.substr(split + 1)) });
        }
        if (result.empty()) throw std::runtime_error("Empty runtime manifest.");
        return result;
    }

    bool complete(const std::filesystem::path& root, const std::vector<file_entry>& files)
    {
        if (!std::filesystem::is_regular_file(root / L".complete")) return false;
        std::error_code error;
        for (const auto& file : files)
        {
            auto path = root / file.relative;
            if (!std::filesystem::is_regular_file(path, error) || std::filesystem::file_size(path, error) != file.size || error) return false;
        }
        return true;
    }

    UINT CALLBACK extract_callback(PVOID context, UINT notification, UINT_PTR param1, UINT_PTR)
    {
        auto& extraction = *static_cast<struct extraction*>(context);
        if (notification == SPFILENOTIFY_FILEINCABINET)
        {
            auto info = reinterpret_cast<FILE_IN_CABINET_INFO_W*>(param1);
            try
            {
                // Cabinet members are numbered; only our manifest decides paths.
                wchar_t* end = nullptr;
                unsigned long index = wcstoul(info->NameInCabinet, &end, 10);
                if (!end || *end || index >= extraction.files->size()) return FILEOP_ABORT;
                auto target = extraction.directory / (*extraction.files)[index].relative;
                auto path = target.wstring();
                if (path.size() >= MAX_PATH) return FILEOP_ABORT;
                std::filesystem::create_directories(target.parent_path());
                wcscpy_s(info->FullTargetName, path.c_str());
                return FILEOP_DOIT;
            }
            catch (...) { return FILEOP_ABORT; }
        }
        if (notification == SPFILENOTIFY_FILEEXTRACTED)
            return reinterpret_cast<FILEPATHS_W*>(param1)->Win32Error;
        if (notification == SPFILENOTIFY_NEEDNEWCABINET) return ERROR_CANCELLED;
        return NO_ERROR;
    }
}

std::filesystem::path runtime_bundle::backend()
{
    auto files = read_manifest();
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)))
        throw std::runtime_error("Could not locate your local application data folder.");
    auto cache = std::filesystem::path(local) / L"JazzNfa" / L"runtime";
    CoTaskMemFree(local);
    auto root = cache / BUNDLE_VERSION;
    if (complete(root, files)) return root / L"NfaLoader.exe";

    auto stage = cache / (std::wstring(BUNDLE_VERSION) + L"-unpacking-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(stage);
    auto cabinet = stage / L"payload.cab";
    auto [bytes, length] = resource(101);
    {
        std::ofstream output(cabinet, std::ios::binary | std::ios::trunc);
        output.write(bytes, length);
        if (!output) throw std::runtime_error("Could not unpack the runtime. Check available disk space.");
    }
    extraction context{ stage, &files };
    if (!SetupIterateCabinetW(cabinet.c_str(), 0, extract_callback, &context))
        throw std::runtime_error("Runtime extraction failed (Windows error " + std::to_string(GetLastError()) + ").");
    DeleteFileW(cabinet.c_str());
    { std::ofstream marker(stage / L".complete"); marker << BUNDLE_VERSION_NARROW; }
    if (!complete(stage, files)) throw std::runtime_error("Runtime extraction is incomplete. Check available disk space.");
    // Rename only after every required file is present. Interrupted extraction
    // never leaves a cache that appears complete to the next launch.
    if (std::filesystem::exists(root))
        std::filesystem::rename(root, cache / (std::wstring(BUNDLE_VERSION) + L"-incomplete-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::rename(stage, root);
    return root / L"NfaLoader.exe";
}
