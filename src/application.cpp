#include "application.hpp"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <iostream>
#include <numeric>
#include <optional>
namespace si {
namespace {
std::string bytes(uint64_t n) {
    char text[64];
    if (n >= 1000000000)
        std::snprintf(text, sizeof(text), "%.2f GB", double(n) / 1e9);
    else if (n >= 1000000)
        std::snprintf(text, sizeof(text), "%.1f MB", double(n) / 1e6);
    else
        std::snprintf(text, sizeof(text), "%.1f KB", double(n) / 1e3);
    return text;
}
void metadataText(const Metadata& m) {
    auto d = m.maximum - m.minimum;
    ImGui::Text("%.3f x %.3f x %.3f mm", d.x, d.y, d.z);
    ImGui::Text("%llu triangles", static_cast<unsigned long long>(m.triangles));
    if (m.degenerate)
        ImGui::TextColored({1, .7f, .3f, 1}, "%llu degenerate triangles skipped",
                           static_cast<unsigned long long>(m.degenerate));
    if (m.trailingBytes)
        ImGui::TextColored({1, .7f, .3f, 1}, "Warning: trailing data after binary STL");
}
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
} // namespace
Application::Application(Options o) : options(std::move(o)) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
        throw std::runtime_error(SDL_GetError());
    window = SDL_CreateWindow("STL Inspector", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1500, 950,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                  (options.selfTest ? SDL_WINDOW_HIDDEN : 0));
    if (!window)
        throw std::runtime_error(SDL_GetError());
    renderer = std::make_unique<Renderer>(window, options.validation, options.singleQueue);
    style();
    if (!options.root.empty() && !options.selfTest)
        openRoot(options.root);
}
Application::~Application() {
    stopping = true;
    ++browseGeneration;
    for (auto& j : active)
        j->cancel = true;
    if (viewerJob)
        viewerJob->cancel = true;
    workers.stop();
    directoryWorkers.stop();
    if (renderer) {
        try {
            renderer->idle();
        } catch (...) {
        }
        active.clear();
        viewerJob.reset();
        model.reset();
        cache.clear();
        renderer.reset();
    }
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
}
void Application::style() {
    ImGui::StyleColorsDark();
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 8;
    s.FrameRounding = 5;
    s.GrabRounding = 5;
    s.PopupRounding = 7;
    s.FramePadding = {9, 6};
    s.ItemSpacing = {9, 8};
    s.WindowPadding = {14, 12};
    s.ScrollbarSize = 13;
    auto* c = s.Colors;
    c[ImGuiCol_WindowBg] = {.055f, .068f, .085f, 1};
    c[ImGuiCol_ChildBg] = {.07f, .085f, .105f, 1};
    c[ImGuiCol_FrameBg] = {.11f, .135f, .17f, 1};
    c[ImGuiCol_Button] = {.13f, .19f, .24f, 1};
    c[ImGuiCol_ButtonHovered] = {.18f, .33f, .40f, 1};
    c[ImGuiCol_ButtonActive] = {.15f, .42f, .49f, 1};
    c[ImGuiCol_Header] = {.12f, .25f, .30f, 1};
    c[ImGuiCol_HeaderHovered] = {.16f, .30f, .36f, 1};
    c[ImGuiCol_CheckMark] = {.35f, .80f, .75f, 1};
    c[ImGuiCol_SliderGrab] = {.35f, .80f, .75f, 1};
    c[ImGuiCol_Text] = {.88f, .92f, .95f, 1};
    c[ImGuiCol_TextDisabled] = {.48f, .57f, .65f, 1};
    ImGui::GetIO().FontGlobalScale = 1.0f;
}
void Application::openRoot(const std::filesystem::path& p) {
    if (p.empty())
        return;
    root = p.lexically_normal();
    ++rootGeneration;
    treeFolders = 0;
    treeDirty = true;
    tree.clear();
    auto key = utf8(root);
    tree.emplace(key, TreeNode{root, true, {}});
    std::snprintf(pathText.data(), pathText.size(), "%s", key.c_str());
    requestTree(tree.at(key));
    selectFolder(root);
}
void Application::requestTree(TreeNode& node) {
    if (node.listing)
        return;
    node.listing = std::make_shared<Listing>();
    auto target = node.listing;
    auto p = node.path;
    auto generation = rootGeneration.load();
    directoryWorkers.submit(0, [this, target, p, generation] {
        try {
            target->result =
                enumerate(p, false, [this, generation] { return stopping || rootGeneration != generation; });
            target->result.files.clear();
            target->result.files.shrink_to_fit();
            auto count = target->result.folders.size();
            auto before = treeFolders.fetch_add(count);
            if (before + count > 100000) {
                auto keep = before < 100000 ? 100000 - before : 0;
                target->result.folders.resize(size_t(keep));
                target->result.partial = true;
            }
        } catch (const std::exception& e) {
            target->result.error = e.what();
        }
        target->done.store(true, std::memory_order_release);
    });
}
void Application::selectFolder(const std::filesystem::path& p) {
    closeViewer();
    selected = p;
    auto generation = ++browseGeneration;
    for (auto& j : active)
        j->cancel = true;
    listing = std::make_shared<Listing>();
    auto target = listing;
    bool descendants = recursive;
    filtered.clear();
    filtering.reset();
    ++filterGeneration;
    listedCount = 0;
    sortApplied = -1;
    directoryWorkers.submit(-1, [this, target, p, generation, descendants] {
        try {
            target->result = enumerate(
                p, descendants, [this, generation] { return stopping || browseGeneration != generation; });
        } catch (const std::exception& e) {
            target->result.error = e.what();
        }
        target->done.store(true, std::memory_order_release);
    });
}
void Application::launchThumbnail(const FileEntry& file, int priority) {
    if (paused || stopping || active.size() >= 12)
        return;
    auto found = cache.find(file.key);
    if (found == cache.end()) {
        if (cache.size() >= 100000) {
            status = "Metadata cache is full. Refresh the root or narrow the scope.";
            return;
        }
        found = cache.emplace(file.key, Cache{}).first;
        found->second.file = file;
    }
    auto& entry = found->second;
    if (entry.file.modified != file.modified || entry.file.size != file.size) {
        entry = Cache{};
        entry.file = file;
    }
    entry.lastUse = tick;
    if (entry.loading || entry.image || !entry.error.empty())
        return;
    entry.loading = true;
    auto j = std::make_shared<Job>();
    j->file = file;
    auto pixels = entry.pixels;
    auto oldMetadata = entry.metadata;
    active.push_back(j);
    workers.submit(priority, [this, j, pixels, oldMetadata] {
        j->state = State::Loading;
        auto cancel = [this, j] { return stopping || j->cancel.load(); };
        try {
            if (cancel())
                throw std::runtime_error("Cancelled");
            auto before = statFile(j->file.path);
            if (before.size != j->file.size || before.modified != j->file.modified)
                throw std::runtime_error("File changed; refresh the folder");
            if (pixels) {
                j->thumbnail.image = renderer->uploadImage(*pixels, 320, 320, cancel);
                j->thumbnail.pixels = pixels;
                j->thumbnail.metadata = oldMetadata;
            } else
                j->thumbnail = renderer->thumbnail(j->file.path, cancel, [j](float p) { j->progress = p; });
            auto after = statFile(j->file.path);
            if (before.modified != after.modified || before.size != after.size)
                throw std::runtime_error("File changed while rendering; refresh and retry");
            j->state.store(cancel() ? State::Cancelled : State::Ready, std::memory_order_release);
        } catch (const std::exception& e) {
            j->error = e.what();
            j->state.store(cancel() ? State::Cancelled : State::Failed, std::memory_order_release);
        }
    });
}
void Application::closeViewer() {
    if (viewerJob)
        viewerJob->cancel = true;
    viewerJob.reset();
    viewerFile.reset();
    model.reset();
    if (renderer)
        renderer->resetViewer();
}
void Application::openViewer(const FileEntry& file) {
    closeViewer();
    viewerFile = file;
    camera = Camera{};
    ++cameraVersion;
    for (auto& j : active)
        j->cancel = true;
    auto j = std::make_shared<Job>();
    j->file = file;
    j->viewer = true;
    viewerJob = j;
    auto budget = uint64_t(viewerBudgetMB * 1e6);
    workers.submit(-100, [this, j, budget] {
        j->state = State::Loading;
        auto cancel = [this, j] { return stopping || j->cancel.load(); };
        try {
            auto before = statFile(j->file.path);
            j->mesh = renderer->loadMesh(
                j->file.path, budget, cancel, [j](float p) { j->progress = p; }, &j->metadata);
            auto after = statFile(j->file.path);
            if (before.modified != after.modified || before.size != after.size)
                throw std::runtime_error("File changed while loading");
            j->state.store(cancel() ? State::Cancelled : State::Ready, std::memory_order_release);
        } catch (const std::exception& e) {
            j->error = e.what();
            j->state.store(cancel() ? State::Cancelled : State::Failed, std::memory_order_release);
        }
    });
}
void Application::revealFile(const std::filesystem::path& path) {
    auto request = std::make_shared<ExplorerRequest>();
    explorerRequest = request;
    status = "Opening in Explorer...";
    directoryWorkers.submit(-100, [this, request, path] {
        if (stopping)
            return;
        try {
            request->error = openInExplorer(path);
        } catch (const std::exception& error) {
            request->error = error.what();
        }
        request->done.store(true, std::memory_order_release);
    });
}
void Application::pumpJobs() {
    if (explorerRequest && explorerRequest->done.load(std::memory_order_acquire)) {
        status = explorerRequest->error.empty() ? "Opened in Explorer" : explorerRequest->error;
        explorerRequest.reset();
    }
    for (auto it = active.begin(); it != active.end();) {
        auto j = *it;
        auto state = j->state.load(std::memory_order_acquire);
        if (state == State::Queued || state == State::Loading) {
            ++it;
            continue;
        }
        auto f = cache.find(j->file.key);
        if (f != cache.end()) {
            auto& e = f->second;
            e.loading = false;
            if (e.file.modified == j->file.modified && e.file.size == j->file.size) {
                if (state == State::Ready) {
                    e.image = std::move(j->thumbnail.image);
                    e.pixels = std::move(j->thumbnail.pixels);
                    e.metadata = j->thumbnail.metadata;
                } else if (state == State::Failed)
                    e.error = j->error;
            }
        }
        it = active.erase(it);
    }
    if (viewerJob) {
        auto state = viewerJob->state.load(std::memory_order_acquire);
        if (state == State::Ready && !model) {
            model = viewerJob->mesh;
            status = "Exact model loaded: " + bytes(model->metadata.geometryBytes());
        }
    }
}
void Application::evict() {
    uint64_t gpu = 0, cpu = 0;
    for (auto& [key, e] : cache) {
        if (e.image)
            gpu += uint64_t(e.image->width) * e.image->height * 4;
        if (e.pixels)
            cpu += e.pixels->size();
    }
    if (gpu <= gpuCacheBudget && cpu <= cpuCacheBudget && processMemory() < 6500000000ull)
        return;
    std::vector<Cache*> candidates;
    for (auto& [key, e] : cache)
        if (e.lastUse + 2 < tick && !e.loading)
            candidates.push_back(&e);
    std::sort(candidates.begin(), candidates.end(), [](auto* a, auto* b) { return a->lastUse < b->lastUse; });
    for (auto* e : candidates) {
        if (gpu > gpuCacheBudget && e->image) {
            gpu -= uint64_t(e->image->width) * e->image->height * 4;
            e->image.reset();
        }
        if ((cpu > cpuCacheBudget || processMemory() > 6500000000ull) && e->pixels) {
            cpu -= e->pixels->size();
            e->pixels.reset();
        }
        if (gpu <= gpuCacheBudget && cpu <= cpuCacheBudget && processMemory() < 6500000000ull)
            break;
    }
}
void Application::drawTree() {
    for (auto& [key, node] : tree)
        if (node.listing && !node.observed && node.listing->done.load(std::memory_order_acquire)) {
            node.observed = true;
            treeDirty = true;
        }
    auto& rows = treeRows;
    std::function<void(const std::string&, int)> add = [&](const std::string& key, int depth) {
        if (depth > 128 || rows.size() >= 100000)
            return;
        auto it = tree.find(key);
        if (it == tree.end())
            return;
        rows.push_back({key, depth});
        auto& n = it->second;
        if (n.expanded && n.listing && n.listing->done.load(std::memory_order_acquire)) {
            auto children = n.listing->result.folders;
            for (auto& p : children) {
                if (rows.size() >= 100000)
                    break;
                auto child = utf8(p);
                tree.try_emplace(child, TreeNode{p, false, {}});
                add(child, depth + 1);
            }
        }
    };
    if (treeDirty) {
        rows.clear();
        if (!root.empty())
            add(utf8(root), 0);
        treeDirty = false;
    }
    ImGui::TextDisabled("FOLDERS");
    ImGui::Separator();
    ImGuiListClipper clipper;
    float h = 30;
    clipper.Begin(int(rows.size()), h);
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            auto row = rows[i];
            auto& n = tree.at(row.key);
            ImGui::PushID(row.key.c_str());
            ImGui::Indent(float(row.depth) * 13);
            auto start = ImGui::GetCursorPosY();
            if (ImGui::SmallButton(n.expanded ? "v" : ">")) {
                n.expanded = !n.expanded;
                treeDirty = true;
                if (n.expanded)
                    requestTree(n);
            }
            ImGui::SameLine();
            auto name = utf8(n.path.filename());
            if (name.empty())
                name = utf8(n.path);
            if (ImGui::Selectable(name.c_str(), n.path == selected, 0, {0, 22}))
                selectFolder(n.path);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(row.key.c_str());
                if (n.listing && n.listing->done.load(std::memory_order_acquire)) {
                    if (n.listing->result.partial)
                        ImGui::TextWrapped(
                            "Partial tree: directory budget reached. Open a smaller subtree as the root.");
                    if (!n.listing->result.error.empty())
                        ImGui::TextWrapped("%s", n.listing->result.error.c_str());
                }
                ImGui::EndTooltip();
            }
            ImGui::Unindent(float(row.depth) * 13);
            ImGui::SetCursorPosY(start + h);
            ImGui::PopID();
        }
    if (!root.empty()) {
        auto& n = tree.at(utf8(root));
        if (n.listing && !n.listing->done)
            ImGui::TextDisabled("Reading folders...");
    }
}
void Application::drawGrid() {
    if (!listing) {
        ImGui::SetCursorPos({40, 65});
        ImGui::Text("Inspect your STL library");
        ImGui::Spacing();
        ImGui::TextDisabled("Open a folder to browse models, dimensions, and load errors.");
        ImGui::TextDisabled("Previews are generated in the background and kept for this session.");
        return;
    }
    if (!listing->done.load(std::memory_order_acquire)) {
        ImGui::TextDisabled("Reading directory...");
        return;
    }
    auto& result = listing->result;
    if (!result.error.empty())
        ImGui::TextWrapped("%s", result.error.c_str());
    if (result.partial)
        ImGui::TextColored({1, .7f, .3f, 1},
                           "Partial listing: 100,000 directory entries reached. Select a smaller subtree.");
    auto& files = result.files;
    if (listedCount != files.size() || filterApplied != search.data() || sortApplied != sort) {
        filtering = std::make_shared<FilteredListing>();
        auto output = filtering;
        auto source = listing;
        auto needle = lower(search.data());
        auto sortMode = sort;
        auto generation = ++filterGeneration;
        directoryWorkers.submit(-2, [this, output, source, needle, sortMode, generation] {
            const auto& entries = source->result.files;
            for (size_t i = 0; i < entries.size(); ++i) {
                if ((i & 1023) == 0 && (stopping || filterGeneration != generation))
                    return;
                if (needle.empty() || lower(entries[i].name).find(needle) != std::string::npos)
                    output->indices.push_back(i);
            }
            std::sort(output->indices.begin(), output->indices.end(), [&](size_t a, size_t b) {
                if (sortMode == 1 && entries[a].size != entries[b].size)
                    return entries[a].size > entries[b].size;
                if (sortMode == 2 && entries[a].modified != entries[b].modified)
                    return entries[a].modified > entries[b].modified;
                return entries[a].name < entries[b].name;
            });
            output->done.store(true, std::memory_order_release);
        });
        listedCount = files.size();
        filterApplied = search.data();
        sortApplied = sort;
    }
    if (filtering && filtering->done.load(std::memory_order_acquire)) {
        filtered = std::move(filtering->indices);
        filtering.reset();
    }
    if (filtering)
        ImGui::TextDisabled("Updating filter and sort...");
    ImGui::TextDisabled("%zu files  |  %s", filtered.size(),
                        recursive ? "Including subfolders" : "Current folder");
    ImGui::Separator();
    if (filtered.empty()) {
        ImGui::TextDisabled("No matching STL files in this folder.");
        return;
    }
    float available = ImGui::GetContentRegionAvail().x;
    int columns = std::max(1, int(available / (cardSize + 12)));
    float width = (available - 12 * (columns - 1)) / columns;
    float height =
        width + 3 * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + 12;
    int rows = int((filtered.size() + columns - 1) / columns);
    ImGuiListClipper clip;
    std::unordered_set<std::string> wanted;
    clip.Begin(rows, height);
    int firstVisible = -1, lastVisible = 0;
    while (clip.Step())
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
            if (firstVisible < 0)
                firstVisible = row;
            lastVisible = row;
            float startY = ImGui::GetCursorPosY();
            for (int col = 0; col < columns; ++col) {
                size_t index = size_t(row) * columns + col;
                if (index >= filtered.size())
                    break;
                const auto& f = files[filtered[index]];
                wanted.insert(f.key);
                if (col)
                    ImGui::SameLine(0, 12);
                ImGui::PushID(f.key.c_str());
                ImGui::BeginGroup();
                ImVec2 start = ImGui::GetCursorScreenPos();
                auto* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(start, {start.x + width, start.y + width}, IM_COL32(19, 24, 31, 255), 7);
                auto found = cache.find(f.key);
                if (found != cache.end())
                    found->second.lastUse = tick;
                bool texture = found != cache.end() && found->second.image;
                if (texture) {
                    renderer->use(found->second.image);
                    ImGui::Image(reinterpret_cast<ImTextureID>(found->second.image->descriptor),
                                 {width, width});
                } else {
                    ImGui::Dummy({width, width});
                    std::string text = "Queued";
                    if (found != cache.end()) {
                        if (!found->second.error.empty())
                            text = "Unable to load";
                        else if (found->second.loading)
                            text = "Rendering...";
                    }
                    auto size = ImGui::CalcTextSize(text.c_str());
                    draw->AddText({start.x + (width - size.x) / 2, start.y + width / 2},
                                  IM_COL32(123, 147, 166, 255), text.c_str());
                }
                if (ImGui::IsItemClicked())
                    openViewer(f);
                if (ImGui::IsItemHovered()) {
                    draw->AddRect(start, {start.x + width, start.y + width}, IM_COL32(91, 190, 179, 255), 7,
                                  0, 2);
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(f.name.c_str());
                    ImGui::TextDisabled("%s", f.key.c_str());
                    if (found != cache.end()) {
                        if (found->second.metadata.triangles)
                            metadataText(found->second.metadata);
                        if (!found->second.error.empty())
                            ImGui::TextWrapped("%s", found->second.error.c_str());
                    }
                    ImGui::EndTooltip();
                }
                if (ImGui::BeginPopupContextItem("card")) {
                    if (ImGui::MenuItem("Open model"))
                        openViewer(f);
                    if (ImGui::MenuItem("Retry thumbnail")) {
                        if (found != cache.end()) {
                            found->second.error.clear();
                            found->second.image.reset();
                            found->second.pixels.reset();
                        }
                    }
                    ImGui::EndPopup();
                }
                auto textPosition = ImGui::GetCursorScreenPos();
                draw->PushClipRect(textPosition,
                                   {textPosition.x + width, textPosition.y + ImGui::GetTextLineHeight()},
                                   true);
                draw->AddText(textPosition, ImGui::GetColorU32(ImGuiCol_Text), f.name.c_str());
                draw->PopClipRect();
                ImGui::Dummy({width, ImGui::GetTextLineHeight()});
                ImGui::TextDisabled("%s", bytes(f.size).c_str());
                if (found != cache.end() && found->second.metadata.triangles) {
                    auto d = found->second.metadata.maximum - found->second.metadata.minimum;
                    ImGui::TextDisabled("%.1f x %.1f x %.1f", d.x, d.y, d.z);
                } else
                    ImGui::TextDisabled(" ");
                if (ImGui::Button("Open in Explorer", {width, 0}))
                    revealFile(f.path);
                ImGui::EndGroup();
                ImGui::PopID();
                if (!viewerFile)
                    launchThumbnail(f, 0);
            }
            ImGui::SetCursorPosY(startY + height);
        }
    if (!viewerFile && firstVisible >= 0)
        for (int r = std::max(0, firstVisible - 1); r <= std::min(rows - 1, lastVisible + 1); ++r)
            for (int c = 0; c < columns; ++c) {
                size_t i = size_t(r) * columns + c;
                if (i < filtered.size()) {
                    wanted.insert(files[filtered[i]].key);
                    launchThumbnail(files[filtered[i]], 10);
                }
            }
    if (!prebuild)
        for (auto& job : active)
            if (!wanted.contains(job->file.key))
                job->cancel = true;
}
void Application::drawViewer() {
    if (ImGui::Button("< Back to grid")) {
        closeViewer();
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Open in Explorer"))
        revealFile(viewerFile->path);
    ImGui::SameLine();
    ImGui::TextUnformatted(viewerFile->name.c_str());
    ImGui::Separator();
    if (!viewerJob)
        return;
    auto state = viewerJob->state.load(std::memory_order_acquire);
    if (state == State::Queued || state == State::Loading) {
        ImGui::Text("Loading exact geometry...");
        ImGui::ProgressBar(viewerJob->progress, {-1, 0});
        ImGui::TextDisabled("Background thumbnails yield to this model.");
        if (ImGui::Button("Cancel loading"))
            closeViewer();
        return;
    }
    if (state == State::Failed) {
        ImGui::TextColored({1, .65f, .35f, 1}, "Model could not be loaded");
        ImGui::TextWrapped("%s", viewerJob->error.c_str());
        if (viewerJob->metadata.triangles)
            metadataText(viewerJob->metadata);
        ImGui::SetNextItemWidth(180);
        ImGui::InputDouble("Viewer geometry budget (MB)", &viewerBudgetMB, 100, 500, "%.0f");
        ImGui::SetNextItemWidth(180);
        ImGui::InputDouble("Total GPU target (MB)", &gpuBudgetMB, 100, 500, "%.0f");
        if (ImGui::Button("Apply limits and retry")) {
            viewerBudgetMB = std::clamp(viewerBudgetMB, 1.0, 64000.0);
            gpuBudgetMB = std::clamp(gpuBudgetMB, 256.0, 128000.0);
            renderer->setBudget(uint64_t(gpuBudgetMB * 0.875 * 1e6));
            auto f = *viewerFile;
            openViewer(f);
        }
        return;
    }
    if (!model)
        return;
    metadataText(model->metadata);
    ImGui::SameLine();
    ImGui::TextDisabled(" | Assumed units: mm");
    if (ImGui::Button("Fit")) {
        camera = Camera{};
        ++cameraVersion;
    }
    ImGui::SameLine();
    if (ImGui::Button("Front")) {
        camera.yaw = -1.5707963f;
        camera.pitch = 0;
        ++cameraVersion;
    }
    ImGui::SameLine();
    if (ImGui::Button("Right")) {
        camera.yaw = 0;
        camera.pitch = 0;
        ++cameraVersion;
    }
    ImGui::SameLine();
    if (ImGui::Button("Top")) {
        camera.pitch = 1.5706f;
        ++cameraVersion;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Drag: orbit  |  Wheel: zoom  |  Middle/right drag: pan");
    auto size = ImGui::GetContentRegionAvail();
    size.y = std::max(size.y - 22, 64.0f);
    auto position = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("viewport", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    bool hover = ImGui::IsItemHovered();
    auto& io = ImGui::GetIO();
    if (hover && io.MouseWheel != 0) {
        camera.zoom = std::clamp(camera.zoom * std::pow(.85f, io.MouseWheel), .01f, 100.0f);
        ++cameraVersion;
    }
    if (ImGui::IsItemActive() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            camera.yaw -= io.MouseDelta.x * .008f;
            camera.pitch = std::clamp(camera.pitch + io.MouseDelta.y * .008f, -1.5706f, 1.5706f);
            ++cameraVersion;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
            ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            camera.pan.x += io.MouseDelta.x / size.y * camera.zoom * 1.84f;
            camera.pan.y -= io.MouseDelta.y / size.y * camera.zoom * 1.84f;
            ++cameraVersion;
        }
    }
    auto image = renderer->renderView(model, camera, uint32_t(std::max(size.x, 64.0f)), uint32_t(size.y),
                                      cameraVersion);
    if (image) {
        renderer->use(image);
        ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(image->descriptor), position,
                                             {position.x + size.x, position.y + size.y});
    }
    if (renderer->viewProgress < 1)
        ImGui::TextDisabled("Drawing exact geometry: %.0f%%", renderer->viewProgress * 100);
    else
        ImGui::TextDisabled("%s resident geometry", bytes(model->metadata.geometryBytes()).c_str());
}
void Application::settings() {
    if (!showSettings)
        return;
    ImGui::SetNextWindowSize({520, 310}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Memory and rendering", &showSettings)) {
        ImGui::TextWrapped("Limits are application targets. A margin is reserved for driver allocations. "
                           "Changes never modify your STL files.");
        ImGui::InputDouble("GPU target (MB)", &gpuBudgetMB, 100, 500, "%.0f");
        ImGui::InputDouble("Viewer geometry (MB)", &viewerBudgetMB, 100, 500, "%.0f");
        if (ImGui::Button("Apply")) {
            gpuBudgetMB = std::clamp(gpuBudgetMB, 256.0, 128000.0);
            viewerBudgetMB = std::clamp(viewerBudgetMB, 1.0, 64000.0);
            renderer->setBudget(uint64_t(gpuBudgetMB * .875 * 1e6));
        }
        ImGui::Text("RAM target: 8 GB; session cache: 1 GB CPU / 400 MB GPU");
        ImGui::Text("Vulkan allocations: %s", bytes(renderer->allocatedBytes()).c_str());
        ImGui::Text("Process private memory: %s", bytes(processMemory()).c_str());
        ImGui::TextWrapped("%s", renderer->gpuName.c_str());
        ImGui::TextDisabled("%s", renderer->separateQueue ? "Separate background graphics queue"
                                                          : "Serialized single-queue fallback");
    }
    ImGui::End();
}
void Application::prebuildStep() {
    if (!prebuild || paused || viewerFile || !prebuildListing ||
        !prebuildListing->done.load(std::memory_order_acquire) || active.size() >= 4)
        return;
    size_t resident = 0;
    for (auto& [k, e] : cache)
        if (e.pixels)
            ++resident;
    if ((resident + 4) * 320ull * 320 * 4 > cpuCacheBudget) {
        prebuild = false;
        status = "Prebuild paused: session cache capacity reached. Earlier thumbnails are retained.";
        return;
    }
    auto& files = prebuildListing->result.files;
    if (prebuildIndex >= files.size()) {
        if (active.empty()) {
            prebuild = false;
            status = prebuildListing->result.partial ? "Prebuild finished a partial listing (100,000 entry "
                                                       "limit). Narrow the scope to continue."
                                                     : "Prebuild complete for this session.";
            if (!prebuildListing->result.error.empty())
                status += " " + prebuildListing->result.error;
        }
        return;
    }
    launchThumbnail(files[prebuildIndex++], 50);
}
void Application::drawUI() {
    auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("STL Inspector", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::TextColored({.40f, .82f, .76f, 1}, "STL INSPECTOR");
    ImGui::SameLine();
    ImGui::TextDisabled(" /  Visual library browser");
    ImGui::SameLine(std::max(400.0f, io.DisplaySize.x - 180));
    if (ImGui::Button("Memory / settings"))
        showSettings = !showSettings;
    if (ImGui::Button("Open folder")) {
        auto p = pickFolder();
        if (!p.empty())
            openRoot(p);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(200.0f, io.DisplaySize.x - 360));
    if (ImGui::InputTextWithHint("##path", "Local folder or UNC network path", pathText.data(),
                                 pathText.size(), ImGuiInputTextFlags_EnterReturnsTrue))
        openRoot(fromUtf8(pathText.data()));
    ImGui::SameLine();
    if (ImGui::Button("Go"))
        openRoot(fromUtf8(pathText.data()));
    ImGui::SameLine();
    if (ImGui::Button("Refresh") && !root.empty()) {
        for (auto& j : active)
            j->cancel = true;
        cache.clear();
        ++rootGeneration;
        treeFolders = 0;
        treeDirty = true;
        tree.clear();
        tree.emplace(utf8(root), TreeNode{root, true, {}});
        requestTree(tree.at(utf8(root)));
        selectFolder(selected);
    }
    ImGui::SetNextItemWidth(240);
    ImGui::InputTextWithHint("##filter", "Filter filenames...", search.data(), search.size());
    ImGui::SameLine();
    if (ImGui::Checkbox("Subfolders", &recursive) && !selected.empty())
        selectFolder(selected);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(125);
    ImGui::Combo("##sort", &sort, "Name\0Largest first\0Newest first\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat("Tile size", &cardSize, 120, 300, "%.0f");
    ImGui::SameLine();
    if (ImGui::Checkbox("Pause previews", &paused) && paused)
        for (auto& job : active)
            job->cancel = true;
    ImGui::SameLine();
    if (ImGui::Button(prebuild ? "Stop prebuild" : "Prebuild tree") && !root.empty()) {
        prebuild = !prebuild;
        if (prebuild) {
            prebuildIndex = 0;
            prebuildListing = std::make_shared<Listing>();
            auto target = prebuildListing;
            auto p = root;
            directoryWorkers.submit(5, [this, target, p] {
                try {
                    target->result = enumerate(p, true, [this] { return stopping.load(); });
                } catch (const std::exception& e) {
                    target->result.error = e.what();
                }
                target->done.store(true, std::memory_order_release);
            });
        }
    }
    ImGui::Separator();
    float area = std::max(100.0f, ImGui::GetContentRegionAvail().y - 35);
    ImGui::BeginChild("tree", {treeWidth, area}, true);
    drawTree();
    ImGui::EndChild();
    ImGui::SameLine(0, 3);
    ImGui::InvisibleButton("splitter", {5, area});
    if (ImGui::IsItemActive())
        treeWidth = std::clamp(treeWidth + io.MouseDelta.x, 160.0f, 500.0f);
    ImGui::SameLine(0, 3);
    ImGui::BeginChild(viewerFile ? "viewer-content" : "grid-content", {0, area}, true,
                      viewerFile ? ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None);
    if (viewerFile)
        drawViewer();
    else
        drawGrid();
    ImGui::EndChild();
    size_t ready = 0;
    for (auto& [key, e] : cache)
        if (e.image || e.pixels)
            ++ready;
    ImGui::TextDisabled("%zu cached  |  %zu jobs  |  RAM %s  |  Vulkan %s  |  %.1f ms", ready, active.size(),
                        bytes(processMemory()).c_str(), bytes(renderer->allocatedBytes()).c_str(),
                        1000.0f / std::max(io.Framerate, 1.0f));
    if (!status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status.c_str());
    }
    ImGui::End();
    settings();
}
int Application::run() {
    if (options.selfTest)
        return selfTest();
    bool running = true, captured = false, viewerCaptured = false;
    int exerciseStage = 0;
    uint64_t peakRAM = 0, peakVulkan = 0;
    std::vector<double> frameTimes;
    auto start = std::chrono::steady_clock::now();
    while (running) {
        auto frameStart = std::chrono::steady_clock::now();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                running = false;
            if (event.type == SDL_WINDOWEVENT && (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                                                  event.window.event == SDL_WINDOWEVENT_RESTORED))
                renderer->resize();
            if (event.type == SDL_DROPFILE) {
                auto p = fromUtf8(event.drop.file);
                SDL_free(event.drop.file);
                openRoot(p);
            }
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE && viewerFile)
                closeViewer();
        }
        if (!running)
            break;
        if (options.smokeSeconds > 0 &&
            std::chrono::steady_clock::now() - start > std::chrono::seconds(options.smokeSeconds))
            break;
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(window, &w, &h);
        if (w <= 0 || h <= 0 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
            SDL_Delay(30);
            continue;
        }
        ++tick;
        pumpJobs();
        evict();
        if (options.exercise) {
            if (exerciseStage == 0 && tick > 180) {
                for (auto& [key, e] : cache)
                    if (e.metadata.triangles) {
                        openViewer(e.file);
                        exerciseStage = 1;
                        break;
                    }
            } else if (exerciseStage == 1 && model) {
                camera.yaw += .4f;
                camera.pitch += .15f;
                ++cameraVersion;
                exerciseStage = 2;
            } else if (exerciseStage == 2 && tick > 300 && renderer->viewProgress == 1) {
                SDL_SetWindowSize(window, 1280, 800);
                renderer->resize();
                exerciseStage = 3;
            } else if (exerciseStage == 3 && tick > 480 && renderer->viewProgress == 1) {
                closeViewer();
                selectFolder(selected);
                exerciseStage = 4;
            } else if (exerciseStage == 4 && tick > 520) {
                recursive = !recursive;
                selectFolder(selected);
                exerciseStage = 5;
            } else if (exerciseStage == 5 && tick > 550) {
                recursive = !recursive;
                selectFolder(selected);
                exerciseStage = 6;
            }
        }
        renderer->beginFrame();
        drawUI();
        if (!captured && !options.screenshot.empty() && tick > 120 && active.empty()) {
            renderer->requestCapture(options.screenshot);
            captured = true;
        }
        if (options.exercise && model && renderer->viewProgress == 1 && !viewerCaptured &&
            !options.screenshot.empty()) {
            auto p = options.screenshot.parent_path() / (options.screenshot.stem().string() + "_viewer.ppm");
            renderer->requestCapture(p);
            viewerCaptured = true;
        }
        renderer->drawFrame();
        prebuildStep();
        peakRAM = std::max(peakRAM, processMemory());
        peakVulkan = std::max(peakVulkan, renderer->allocatedBytes());
        if (frameTimes.size() < 100000)
            frameTimes.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart)
                    .count());
    }
    std::sort(frameTimes.begin(), frameTimes.end());
    std::cout << "Rendered " << renderer->frames
              << " frames; validation errors: " << renderer->validationErrors
              << "; peak process bytes: " << peakRAM << "; peak Vulkan bytes: " << peakVulkan
              << "; frame p95 ms: "
              << (frameTimes.empty() ? 0 : frameTimes[size_t((frameTimes.size() - 1) * .95)])
              << "; exercise stage: " << exerciseStage << '\n';
    return renderer->validationErrors || (options.exercise && exerciseStage != 6) ? 2 : 0;
}
int Application::selfTest() {
    if (options.root.empty())
        throw std::runtime_error("--self-test requires an STL path");
    auto started = std::chrono::steady_clock::now();
    auto thumb = renderer->thumbnail(options.root, {}, {});
    auto thumbnailSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    bool rejected = false;
    try {
        renderer->loadMesh(options.root, thumb.metadata.geometryBytes() - 1, {}, {});
    } catch (const std::exception& e) {
        rejected = std::string(e.what()).find("viewer limit") != std::string::npos;
    }
    if (!rejected)
        throw std::runtime_error("Geometry budget was not enforced");
    bool cancelled = false;
    try {
        renderer->thumbnail(options.root, [] { return true; }, {});
    } catch (const std::exception& e) {
        cancelled = std::string(e.what()) == "Cancelled";
    }
    if (!cancelled)
        throw std::runtime_error("Cancellation was not honored");
    size_t changed = 0;
    auto& pixels = *thumb.pixels;
    for (size_t i = 0; i < pixels.size(); i += 4)
        if (pixels[i] > 40)
            ++changed;
    if (changed < 20)
        throw std::runtime_error("Thumbnail has no visible geometry");
    if (!options.screenshot.empty()) {
        std::ofstream out(options.screenshot, std::ios::binary);
        out << "P6\n320 320\n255\n";
        for (size_t i = 0; i < pixels.size(); i += 4)
            out.write(reinterpret_cast<char*>(pixels.data() + i), 3);
    }
    auto cachedImage = renderer->uploadImage(*thumb.pixels, 320, 320, {});
    auto mesh = renderer->loadMesh(options.root, uint64_t(viewerBudgetMB * 1e6), {}, {});
    uint64_t peakVulkan = renderer->allocatedBytes(), peakRAM = processMemory();
    for (int i = 0; i < 10000; ++i) {
        renderer->beginFrame();
        renderer->use(i % 2 ? thumb.image : cachedImage);
        ImGui::Begin("Integration test");
        ImGui::Image(reinterpret_cast<ImTextureID>((i % 2 ? thumb.image : cachedImage)->descriptor),
                     {320, 320});
        auto view = renderer->renderView(mesh, Camera{}, 640, 480, 0);
        if (view) {
            renderer->use(view);
            ImGui::Image(reinterpret_cast<ImTextureID>(view->descriptor), {640, 480});
        }
        ImGui::End();
        if (i >= 12 && renderer->viewProgress == 1 && !options.screenshot.empty())
            renderer->requestCapture(options.screenshot.parent_path() /
                                     (options.screenshot.stem().string() + "_full.ppm"));
        renderer->drawFrame();
        peakVulkan = std::max(peakVulkan, renderer->allocatedBytes());
        peakRAM = std::max(peakRAM, processMemory());
        if (i >= 12 && renderer->viewProgress == 1)
            break;
    }
    if (renderer->viewProgress != 1)
        throw std::runtime_error("Full viewer did not finish exact geometry");
    renderer->idle();
    // Drop app ownership while frame slots still hold the resources, then retire
    // those slots. This exercises texture descriptors and mesh lifetime together.
    auto geometryBytes = mesh->metadata.geometryBytes();
    renderer->resetViewer();
    mesh.reset();
    cachedImage.reset();
    thumb.image.reset();
    for (int i = 0; i < 3; ++i) {
        renderer->beginFrame();
        renderer->drawFrame();
    }
    renderer->idle();
    auto retiredBytes = renderer->allocatedBytes();
    if (geometryBytes > 100000000 && retiredBytes + geometryBytes / 2 > peakVulkan)
        throw std::runtime_error("Viewer geometry did not retire after completion");
    std::cout << "Integration passed: " << thumb.metadata.triangles << " triangles, " << changed
              << " shaded pixels, " << renderer->validationErrors
              << " validation errors; thumbnail seconds: " << thumbnailSeconds << "; total seconds: "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
              << "; peak RAM: " << peakRAM << "; peak Vulkan: " << peakVulkan
              << "; Vulkan after retirement: " << retiredBytes << "\n";
    return renderer->validationErrors ? 2 : 0;
}
} // namespace si
