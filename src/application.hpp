#pragma once
#include "browser.hpp"
#include "renderer.hpp"
#include <optional>
#include <unordered_map>
#include <unordered_set>
namespace si {
struct Options {
    std::filesystem::path root;
    bool validation = false, singleQueue = false, selfTest = false, exercise = false;
    int smokeSeconds = 0;
    std::filesystem::path screenshot;
};
class Application {
  public:
    explicit Application(Options);
    ~Application();
    int run();

  private:
    enum class State { Queued, Loading, Ready, Failed, Cancelled };
    struct Job {
        std::atomic<State> state{State::Queued};
        std::atomic_bool cancel{false};
        std::atomic<float> progress{0};
        FileEntry file;
        Thumbnail thumbnail;
        std::shared_ptr<Mesh> mesh;
        Metadata metadata;
        std::string error;
        bool viewer = false;
    };
    struct Cache {
        FileEntry file;
        std::shared_ptr<Image> image;
        std::shared_ptr<std::vector<uint8_t>> pixels;
        Metadata metadata;
        std::string error;
        uint64_t lastUse = 0;
        bool loading = false;
    };
    struct Listing {
        std::atomic_bool done{false};
        DirectoryResult result;
    };
    struct FilteredListing {
        std::atomic_bool done{false};
        std::vector<size_t> indices;
    };
    struct ExplorerRequest {
        std::atomic_bool done{false};
        std::string error;
    };
    std::shared_ptr<ExplorerRequest> explorerRequest;
    struct TreeNode {
        std::filesystem::path path;
        bool expanded = false;
        std::shared_ptr<Listing> listing;
        bool observed = false;
    };
    struct TreeRow {
        std::string key;
        int depth;
    };
    Options options;
    SDL_Window* window{};
    std::unique_ptr<Renderer> renderer;
    Workers workers{2}, directoryWorkers{2};
    std::atomic_bool stopping{false};
    std::atomic_uint64_t browseGeneration{0};
    std::atomic_uint64_t rootGeneration{0}, treeFolders{0};
    std::filesystem::path root, selected;
    std::shared_ptr<Listing> listing;
    std::unordered_map<std::string, TreeNode> tree;
    std::vector<TreeRow> treeRows;
    bool treeDirty = true;
    std::unordered_map<std::string, Cache> cache;
    std::vector<std::shared_ptr<Job>> active;
    std::shared_ptr<Job> viewerJob;
    std::shared_ptr<Mesh> model;
    std::optional<FileEntry> viewerFile;
    Camera camera;
    uint64_t cameraVersion = 0;
    std::vector<size_t> filtered;
    std::shared_ptr<FilteredListing> filtering;
    std::atomic_uint64_t filterGeneration{0};
    std::string filterApplied;
    int sortApplied = -1;
    size_t listedCount = 0;
    std::array<char, 4096> pathText{};
    std::array<char, 256> search{};
    int sort = 0;
    bool recursive = false, paused = false, showSettings = false;
    float cardSize = 180, treeWidth = 260;
    uint64_t tick = 0;
    double gpuBudgetMB = 4000, viewerBudgetMB = 2300;
    uint64_t cpuCacheBudget = 1000000000, gpuCacheBudget = 400000000;
    std::string status;
    bool prebuild = false;
    std::shared_ptr<Listing> prebuildListing;
    size_t prebuildIndex = 0;
    void openRoot(const std::filesystem::path&);
    void selectFolder(const std::filesystem::path&);
    void requestTree(TreeNode&);
    void launchThumbnail(const FileEntry&, int priority);
    void openViewer(const FileEntry&);
    void revealFile(const std::filesystem::path&);
    void closeViewer();
    void pumpJobs();
    void evict();
    void drawUI();
    void drawTree();
    void drawGrid();
    void drawViewer();
    void settings();
    void style();
    void prebuildStep();
    int selfTest();
};
} // namespace si
