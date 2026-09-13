#include "platform.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#endif

namespace si {

std::string utf8(const std::filesystem::path& path)
{
    auto s = path.u8string();

    return { reinterpret_cast<const char*>(s.data()), s.size() };
}

std::filesystem::path fromUtf8(const std::string& s)
{
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::filesystem::path systemFont()
{
#ifdef _WIN32
    wchar_t directory[MAX_PATH] {};
    if (GetWindowsDirectoryW(directory, MAX_PATH)) {
        return std::filesystem::path(directory) / "Fonts" / "segoeui.ttf";
    }
#endif

    return {};
}

std::filesystem::path pickFolder()
{
#ifdef _WIN32
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dialog = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD flags = 0;
        dialog->GetOptions(&flags);
        dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR name = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                    result = name;
                    CoTaskMemFree(name);
                }

                item->Release();
            }
        }

        dialog->Release();
    }

    if (SUCCEEDED(init)) {
        CoUninitialize();
    }

    return result;
#else
    return {};
#endif
}

std::string openInExplorer(const std::filesystem::path& path)
{
#ifdef _WIN32
    auto absolute = std::filesystem::absolute(path).lexically_normal();
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        return "Could not initialize Windows Explorer integration.";
    }

    PIDLIST_ABSOLUTE item = nullptr;
    HRESULT result = SHParseDisplayName(absolute.c_str(), nullptr, &item, 0, nullptr);
    if (SUCCEEDED(result)) {
        result = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    }

    CoTaskMemFree(item);
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }

    if (FAILED(result)) {
        return "Could not open this file in Explorer (Windows error "
            + std::to_string(static_cast<unsigned long>(result)) + "). The file may have moved or become unavailable.";
    }

    return {};
#else
    (void)path;

    return "Open in Explorer is available on Windows.";
#endif
}

uint64_t processMemory()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX m {};
    m.cb = sizeof(m);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m));

    return m.PrivateUsage;
#else
    return 0;
#endif
}

bool isLink(const std::filesystem::directory_entry& e)
{
#ifdef _WIN32
    auto attr = GetFileAttributesW(e.path().c_str());

    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code ec;

    return e.is_symlink(ec);
#endif
}

struct FileReader::Impl {
    Cancel cancel;
    uint64_t offset = 0;
    uint64_t length = 0;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;

    ~Impl()
    {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }

        if (event) {
            CloseHandle(event);
        }
    }
#else
    std::ifstream file;
#endif
};

FileReader::FileReader(const std::filesystem::path& path, Cancel cancel)
    : p(std::make_unique<Impl>())
{
    p->cancel = std::move(cancel);
    if (p->cancel && p->cancel()) {
        throw std::runtime_error("Cancelled");
    }
#ifdef _WIN32
    p->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (p->file == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Cannot open file (Windows error " + std::to_string(GetLastError()) + ")");
    }

    LARGE_INTEGER length {};
    if (!GetFileSizeEx(p->file, &length)) {
        throw std::runtime_error("Cannot get file size");
    }

    p->length = length.QuadPart;
    p->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!p->event) {
        throw std::runtime_error("Cannot create I/O event");
    }
#else
    p->file.open(path, std::ios::binary);
    if (!p->file) {
        throw std::runtime_error("Cannot open file");
    }

    p->length = std::filesystem::file_size(path);
#endif
}

FileReader::~FileReader() = default;

size_t FileReader::read(void* dst, size_t bytes)
{
    if (p->cancel && p->cancel()) {
        throw std::runtime_error("Cancelled");
    }

    bytes = std::min<uint64_t>({ bytes, p->length - p->offset, 4 * 1024 * 1024 });
    if (!bytes) {
        return 0;
    }
#ifdef _WIN32
    OVERLAPPED op {};
    op.Offset = DWORD(p->offset);
    op.OffsetHigh = DWORD(p->offset >> 32);
    op.hEvent = p->event;
    ResetEvent(p->event);
    DWORD got = 0;
    BOOL ok = ReadFile(p->file, dst, DWORD(bytes), &got, &op);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        throw std::runtime_error("File read failed");
    }

    if (!ok) {
        bool cancelled = false;
        while (WaitForSingleObject(p->event, 10) == WAIT_TIMEOUT) {
            if (p->cancel && p->cancel()) {
                CancelIoEx(p->file, &op);
                cancelled = true;
            }
        }

        if (!GetOverlappedResult(p->file, &op, &got, FALSE)) {
            throw std::runtime_error(cancelled ? "Cancelled" : "File read failed or source disconnected");
        }

        if (cancelled) {
            throw std::runtime_error("Cancelled");
        }
    }

    p->offset += got;

    return got;
#else
    p->file.read(static_cast<char*>(dst), std::streamsize(bytes));
    auto got = p->file.gcount();
    if (got == 0 && !p->file.eof()) {
        throw std::runtime_error("File read failed");
    }

    p->offset += got;

    return size_t(got);
#endif
}

void FileReader::seek(uint64_t offset)
{
    if (offset > p->length) {
        throw std::runtime_error("Invalid file offset");
    }

    p->offset = offset;
#ifndef _WIN32
    p->file.clear();
    p->file.seekg(std::streamoff(offset));
#endif
}

uint64_t FileReader::size() const
{
    return p->length;
}

} // namespace si
