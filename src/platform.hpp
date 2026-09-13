#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
namespace si {
using Cancel = std::function<bool()>;
std::string utf8(const std::filesystem::path& path);
std::filesystem::path fromUtf8(const std::string& text);
std::filesystem::path pickFolder();
// Returns an empty string on success; shell path resolution belongs on a worker.
std::string openInExplorer(const std::filesystem::path& path);
std::filesystem::path systemFont();
uint64_t processMemory();
bool isLink(const std::filesystem::directory_entry& entry);
class FileReader {
  public:
    explicit FileReader(const std::filesystem::path& path, Cancel cancel = {});
    ~FileReader();
    FileReader(const FileReader&) = delete;
    size_t read(void* destination, size_t bytes);
    void seek(uint64_t offset);
    uint64_t size() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
} // namespace si
