#pragma once

#include "stl.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

struct SDL_Window;

namespace si {

void check(VkResult, const char* operation);

struct Buffer {
    VmaAllocator allocator {};
    VkBuffer buffer {};
    VmaAllocation allocation {};
    void* mapped {};
    VkDeviceSize bytes {};

    ~Buffer();
};

struct Image {
    VkDevice device {};
    VmaAllocator allocator {};
    VkImage image {};
    VmaAllocation allocation {};
    VkImageView view {};
    VkDescriptorSet descriptor {};
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t ready = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

    ~Image();
};

struct Mesh {
    std::vector<std::shared_ptr<Buffer>> chunks;
    std::vector<uint32_t> counts;
    Metadata metadata;
    uint64_t ready = 0;
};

struct Thumbnail {
    std::shared_ptr<Image> image;
    std::shared_ptr<std::vector<uint8_t>> pixels;
    Metadata metadata;
};

struct Camera {
    float yaw = -0.8f;
    float pitch = 0.6f;
    float zoom = 1.0f;
    glm::vec2 pan { 0 };

    glm::mat4 matrix(float aspect) const;
};

class Renderer {
public:
    Renderer(SDL_Window*, bool validation, bool singleQueue = false);
    ~Renderer();
    Renderer(const Renderer&) = delete;

    void beginFrame();
    bool drawFrame();
    void resize();

    Thumbnail thumbnail(const std::filesystem::path&, Cancel, Progress);
    std::shared_ptr<Image> uploadImage(const std::vector<uint8_t>&, uint32_t, uint32_t, Cancel);

    std::shared_ptr<Mesh> loadMesh(
        const std::filesystem::path&, uint64_t budget, Cancel, Progress, Metadata* metadata = nullptr);

    std::shared_ptr<Image> renderView(
        const std::shared_ptr<Mesh>&, const Camera&, uint32_t, uint32_t, uint64_t cameraVersion);
    void use(const std::shared_ptr<Image>&);
    void resetViewer();
    void idle();

    void requestCapture(const std::filesystem::path& path)
    {
        capturePath = path;
    }

    uint64_t allocatedBytes() const;
    uint64_t availableBytes() const;
    uint64_t completedBackground() const;

    void setBudget(uint64_t bytes)
    {
        deviceBudget = bytes;
    }

    std::string gpuName;
    bool separateQueue = false;
    std::atomic_uint validationErrors { 0 };
    std::atomic_uint64_t deviceBudget { 3500000000ull };
    float viewProgress = 1;
    uint64_t frames = 0;

private:
    struct Frame {
        VkCommandPool pool {};
        VkCommandBuffer command {};
        VkFence fence {};
        VkSemaphore acquired {};
        std::vector<std::shared_ptr<Image>> images;
        std::vector<std::shared_ptr<Mesh>> meshes;
    };

    struct Context;

    SDL_Window* window {};
    VkInstance instance {};
    VkDebugUtilsMessengerEXT debug {};
    VkPhysicalDevice gpu {};
    VkDevice device {};
    VkSurfaceKHR surface {};
    VmaAllocator allocator {};

    VkQueue foreground {};
    VkQueue background {};
    uint32_t family {};
    std::mutex queueMutex;
    std::mutex allocationMutex;
    VkSemaphore backgroundTimeline {};
    uint64_t submittedBackground = 0;
    uint64_t waitBackground = 0;

    VkSwapchainKHR swapchain {};
    VkFormat swapFormat {};
    VkExtent2D extent {};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    std::vector<VkSemaphore> presentSemaphores;
    std::array<Frame, 2> frameData {};
    Frame* current {};
    bool rebuild = false;
    bool imguiReady = false;

    VkDescriptorPool descriptorPool {};
    VkSampler sampler {};
    VkPipeline pipeline {};
    VkPipelineLayout pipelineLayout {};

    std::shared_ptr<Image> viewColor;
    std::shared_ptr<Image> viewDepth;
    std::shared_ptr<Image> viewDisplayed;
    uint64_t viewVersion = ~0ull;
    size_t viewChunk = 0;
    std::shared_ptr<Mesh> viewMesh;
    std::filesystem::path capturePath;

    std::shared_ptr<Buffer> buffer(VkDeviceSize, VkBufferUsageFlags, bool host);
    std::shared_ptr<Image> image(uint32_t, uint32_t, VkFormat, VkImageUsageFlags);

    void createSwapchain();
    void destroySwapchain();
    void createPipeline();
    void initImguiRenderer();

    void renderChunks(VkCommandBuffer, const Image&, const Image&, const glm::mat4&,
        const std::vector<std::shared_ptr<Buffer>>&, const std::vector<uint32_t>&, bool clear, size_t first,
        size_t end);

    friend struct Context;
};

} // namespace si
