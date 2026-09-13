#pragma once

#include "platform.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace si {

struct FileEntry {
    std::filesystem::path path;
    std::string name;
    std::string key;
    uint64_t size = 0;
    int64_t modified = 0;
};

struct DirectoryResult {
    std::vector<FileEntry> files;
    std::vector<std::filesystem::path> folders;
    std::string error;
    bool partial = false;
};

DirectoryResult enumerate(const std::filesystem::path&, bool recursive, Cancel = {}, size_t limit = 100000);
FileEntry statFile(const std::filesystem::path&);

class Workers {
public:
    explicit Workers(size_t count = 2);
    ~Workers();

    void submit(int priority, std::function<void()> work);
    void clear();
    void stop();
    size_t pending();

private:
    struct Task {
        int priority;
        uint64_t sequence;
        std::function<void()> work;
    };

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<Task> tasks;
    std::vector<std::thread> threads;
    bool stopping = false;
    uint64_t sequence = 0;
};

} // namespace si
