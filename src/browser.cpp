#include "browser.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>
namespace si {
namespace {
bool ignoredDirectory(const std::filesystem::path& path) {
    auto name = utf8(path.filename());
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return name == "__macosx";
}
} // namespace
FileEntry statFile(const std::filesystem::path& p) {
    FileEntry f;
    f.path = p;
    f.name = utf8(p.filename());
    f.key = utf8(p.lexically_normal());
    f.size = std::filesystem::file_size(p);
    f.modified = std::filesystem::last_write_time(p).time_since_epoch().count();
    return f;
}
DirectoryResult enumerate(const std::filesystem::path& root, bool recursive, Cancel cancel, size_t limit) {
    DirectoryResult r;
    std::vector<std::filesystem::path> pending{root};
    size_t visited = 0;
    while (!pending.empty()) {
        if (cancel && cancel())
            return {};
        auto folder = std::move(pending.back());
        pending.pop_back();
        if (ignoredDirectory(folder))
            continue;
        std::error_code ec;
        std::filesystem::directory_iterator it(folder, ec), end;
        if (ec) {
            r.error = "Some directories could not be read: " + ec.message();
            continue;
        }
        while (it != end) {
            if (cancel && cancel())
                return {};
            if (++visited > limit) {
                r.partial = true;
                return r;
            }
            auto entry = *it;
            std::error_code typeError;
            if (!isLink(entry) && !ignoredDirectory(entry.path())) {
                if (entry.is_directory(typeError)) {
                    if (folder == root)
                        r.folders.push_back(entry.path());
                    if (recursive)
                        pending.push_back(entry.path());
                } else if (entry.is_regular_file(typeError)) {
                    auto ext = utf8(entry.path().extension());
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return char(std::tolower(c)); });
                    if (ext == ".stl")
                        try {
                            r.files.push_back(statFile(entry.path()));
                        } catch (const std::exception& e) {
                            r.error = e.what();
                        }
                }
            }
            it.increment(ec);
            if (ec) {
                r.error = ec.message();
                break;
            }
        }
    }
    auto less = [](const auto& a, const auto& b) { return a < b; };
    std::sort(r.folders.begin(), r.folders.end(), less);
    std::sort(r.files.begin(), r.files.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return r;
}
Workers::Workers(size_t count) {
    for (size_t i = 0; i < count; ++i)
        threads.emplace_back([this] {
            for (;;) {
                Task task;
                {
                    std::unique_lock lock(mutex);
                    condition.wait(lock, [this] { return stopping || !tasks.empty(); });
                    if (stopping)
                        return;
                    auto best =
                        std::min_element(tasks.begin(), tasks.end(), [](const Task& a, const Task& b) {
                            return a.priority != b.priority ? a.priority < b.priority
                                                            : a.sequence < b.sequence;
                        });
                    task = std::move(*best);
                    tasks.erase(best);
                }
                try {
                    task.work();
                } catch (...) { /* Jobs publish their own errors; keep the executor alive. */
                }
            }
        });
}
Workers::~Workers() {
    stop();
}
void Workers::submit(int priority, std::function<void()> work) {
    {
        std::lock_guard lock(mutex);
        if (stopping)
            return;
        if (tasks.size() >= 1024)
            throw std::runtime_error("Background work queue is full");
        tasks.push_back({priority, sequence++, std::move(work)});
    }
    condition.notify_one();
}
void Workers::clear() {
    std::lock_guard lock(mutex);
    tasks.clear();
}
size_t Workers::pending() {
    std::lock_guard lock(mutex);
    return tasks.size();
}
void Workers::stop() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
        tasks.clear();
    }
    condition.notify_all();
    for (auto& t : threads)
        if (t.joinable())
            t.join();
    threads.clear();
}
} // namespace si
