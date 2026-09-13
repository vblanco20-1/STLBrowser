#define VMA_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "renderer.hpp"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <SDL.h>
#include <SDL_vulkan.h>
#include <VkBootstrap.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
#include <iostream>
#include <thread>
namespace si {
void check(VkResult r, const char* op) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(op) + " (Vulkan " + std::to_string(r) + ")");
}
Buffer::~Buffer() {
    if (buffer)
        vmaDestroyBuffer(allocator, buffer, allocation);
}
Image::~Image() {
    if (descriptor)
        ImGui_ImplVulkan_RemoveTexture(descriptor);
    if (view)
        vkDestroyImageView(device, view, nullptr);
    if (image)
        vmaDestroyImage(allocator, image, allocation);
}
glm::mat4 Camera::matrix(float aspect) const {
    glm::vec3 direction{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    auto view = glm::lookAt(direction * 3.0f, glm::vec3(0), glm::vec3(0, 0, 1));
    view[3][0] += pan.x;
    view[3][1] += pan.y;
    float h = 0.92f * zoom, w = h * std::max(aspect, 0.05f);
    if (aspect < 1)
        h /= aspect, w = 0.92f * zoom;
    auto projection = glm::ortho(-w, w, -h, h, 0.01f, 8.0f);
    projection[1][1] *= -1;
    return projection * view;
}
namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        static_cast<Renderer*>(user)->validationErrors++;
    if (severity &
        (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
        std::cerr << "[Vulkan] " << data->pMessage << '\n';
    return VK_FALSE;
}
void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
             VkImageAspectFlags aspect, VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dst;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}
void vertexBarrier(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize bytes) {
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer;
    b.size = bytes;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}
VkShaderModule shader(VkDevice device, const char* name) {
    char* base = SDL_GetBasePath();
    auto path = fromUtf8(base ? base : "") / "shaders" / name;
    SDL_free(base);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("Missing shader: " + utf8(path));
    auto size = f.tellg();
    if (size <= 0 || size % 4)
        throw std::runtime_error("Invalid SPIR-V");
    std::vector<uint32_t> bytes(size_t(size) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!f)
        throw std::runtime_error("Shader read failed");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size_t(size);
    info.pCode = bytes.data();
    VkShaderModule module{};
    check(vkCreateShaderModule(device, &info, nullptr, &module), "Create shader");
    return module;
}
} // namespace
struct Renderer::Context {
    Renderer& r;
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    uint64_t last = 0;
    Context(Renderer& renderer) : r(renderer) {
        VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        p.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        p.queueFamilyIndex = r.family;
        check(vkCreateCommandPool(r.device, &p, nullptr, &pool), "Background command pool");
        VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        a.commandPool = pool;
        a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        a.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(r.device, &a, &command), "Background command buffer");
        VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(r.device, &f, nullptr, &fence), "Background fence");
    }
    ~Context() {
        if (fence)
            vkDestroyFence(r.device, fence, nullptr);
        if (pool)
            vkDestroyCommandPool(r.device, pool, nullptr);
    }
    void begin() {
        check(vkResetCommandPool(r.device, pool, 0), "Reset background pool");
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &b), "Begin background commands");
    }
    uint64_t finish() {
        check(vkEndCommandBuffer(command), "End background commands");
        check(vkResetFences(r.device, 1, &fence), "Reset background fence");
        {
            std::lock_guard lock(r.queueMutex);
            last = ++r.submittedBackground;
            VkCommandBufferSubmitInfo c{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            c.commandBuffer = command;
            VkSemaphoreSubmitInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            s.semaphore = r.backgroundTimeline;
            s.value = last;
            s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &c;
            submit.signalSemaphoreInfoCount = 1;
            submit.pSignalSemaphoreInfos = &s;
            check(vkQueueSubmit2(r.background, 1, &submit, fence), "Background submit");
        }
        // Only the worker waits; the UI never waits for thumbnail or model loading.
        VkResult result;
        do {
            result = vkWaitForFences(r.device, 1, &fence, VK_TRUE, 100000000);
        } while (result == VK_TIMEOUT);
        check(result, "Background completion");
        if (!r.separateQueue)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return last;
    }
};
Renderer::Renderer(SDL_Window* w, bool validation, bool singleQueue) : window(w) {
    unsigned count = 0;
    SDL_Vulkan_GetInstanceExtensions(w, &count, nullptr);
    std::vector<const char*> extensions(count);
    SDL_Vulkan_GetInstanceExtensions(w, &count, extensions.data());
    vkb::InstanceBuilder ib;
    ib.set_app_name("STL Inspector").require_api_version(1, 3).enable_extensions(extensions);
    if (validation)
        ib.request_validation_layers()
            .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT)
            .set_debug_callback(debugMessage)
            .set_debug_callback_user_data_pointer(this);
    auto ir = ib.build();
    if (!ir)
        throw std::runtime_error("Vulkan instance: " + ir.error().message());
    instance = ir.value().instance;
    debug = ir.value().debug_messenger;
    if (!SDL_Vulkan_CreateSurface(w, instance, &surface))
        throw std::runtime_error(SDL_GetError());
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    auto selected = vkb::PhysicalDeviceSelector{ir.value()}
                        .set_surface(surface)
                        .set_minimum_version(1, 3)
                        .set_required_features_13(f13)
                        .set_required_features_12(f12)
                        .add_required_extension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)
                        .select();
    if (!selected)
        throw std::runtime_error(
            "A Vulkan 1.3 GPU with dynamic rendering and timeline semaphores is required: " +
            selected.error().message());
    auto physical = selected.value();
    gpu = physical.physical_device;
    gpuName = physical.name;
    bool memoryBudget = physical.enable_extension_if_present(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, nullptr);
    std::vector<VkQueueFamilyProperties> props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, props.data());
    bool found = false;
    for (uint32_t i = 0; i < families; ++i) {
        VkBool32 present = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present);
        if (present && (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            family = i;
            found = true;
            break;
        }
    }
    if (!found)
        throw std::runtime_error("No graphics queue with presentation support");
    separateQueue = !singleQueue && props[family].queueCount >= 2;
    auto dr = vkb::DeviceBuilder{physical}
                  .custom_queue_setup({vkb::CustomQueueDescription(
                      family, separateQueue ? std::vector<float>{1.0f, 0.25f} : std::vector<float>{1.0f})})
                  .build();
    if (!dr)
        throw std::runtime_error("Vulkan device: " + dr.error().message());
    device = dr.value().device;
    vkGetDeviceQueue(device, family, 0, &foreground);
    vkGetDeviceQueue(device, family, separateQueue ? 1 : 0, &background);
    VmaAllocatorCreateInfo ai{};
    ai.instance = instance;
    ai.physicalDevice = gpu;
    ai.device = device;
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    if (memoryBudget)
        ai.flags = VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    check(vmaCreateAllocator(&ai, &allocator), "VMA allocator");
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphore.pNext = &type;
    check(vkCreateSemaphore(device, &semaphore, nullptr, &backgroundTimeline), "Background timeline");
    for (auto& f : frameData) {
        VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        p.queueFamilyIndex = family;
        p.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device, &p, nullptr, &f.pool), "Frame pool");
        VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        a.commandPool = f.pool;
        a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        a.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &a, &f.command), "Frame commands");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(device, &fence, nullptr, &f.fence), "Frame fence");
        VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device, &s, nullptr, &f.acquired), "Acquire semaphore");
    }
    createSwapchain();
    createPipeline();
    VkSamplerCreateInfo sampling{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampling.magFilter = sampling.minFilter = VK_FILTER_LINEAR;
    sampling.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampling.addressModeU = sampling.addressModeV = sampling.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(device, &sampling, nullptr, &sampler), "Image sampler");
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = 4096;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &poolSize;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &descriptorPool), "ImGui descriptors");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    auto font = systemFont();
    if (!font.empty()) {
        std::ifstream file(font, std::ios::binary | std::ios::ate);
        if (file) {
            auto length = size_t(file.tellg());
            auto* data = ImGui::MemAlloc(length);
            file.seekg(0);
            file.read(static_cast<char*>(data), std::streamsize(length));
            if (file)
                ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, int(length), 17.0f);
            else
                ImGui::MemFree(data);
        }
    }
    ImGui_ImplSDL2_InitForVulkan(w);
    initImguiRenderer();
    imguiReady = true;
    std::cout << "GPU: " << gpuName << "; "
              << (separateQueue ? "dedicated background graphics queue" : "serialized single graphics queue")
              << '\n';
}
void Renderer::initImguiRenderer() {
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &swapFormat;
    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = instance;
    info.PhysicalDevice = gpu;
    info.Device = device;
    info.QueueFamily = family;
    info.Queue = foreground;
    info.DescriptorPool = descriptorPool;
    info.MinImageCount = 2;
    info.ImageCount = uint32_t(swapImages.size());
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = rendering;
    info.CheckVkResultFn = [](VkResult r) { check(r, "ImGui Vulkan"); };
    if (!ImGui_ImplVulkan_Init(&info))
        throw std::runtime_error("ImGui Vulkan initialization failed");
    ImGui_ImplVulkan_CreateFontsTexture();
}
void Renderer::createSwapchain() {
    int width = 0, height = 0;
    SDL_Vulkan_GetDrawableSize(window, &width, &height);
    if (width <= 0 || height <= 0)
        return;
    auto result = vkb::SwapchainBuilder{gpu, device, surface, family, family}
                      .set_desired_extent(width, height)
                      .set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                      .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                      .set_desired_min_image_count(3)
                      .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                      .build();
    if (!result)
        throw std::runtime_error("Swapchain: " + result.error().message());
    auto s = result.value();
    swapchain = s.swapchain;
    swapFormat = s.image_format;
    extent = s.extent;
    swapImages = s.get_images().value();
    swapViews = s.get_image_views().value();
    presentSemaphores.resize(swapImages.size());
    for (auto& semaphore : presentSemaphores) {
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device, &info, nullptr, &semaphore), "Present semaphore");
    }
}
void Renderer::destroySwapchain() {
    for (auto s : presentSemaphores)
        vkDestroySemaphore(device, s, nullptr);
    presentSemaphores.clear();
    for (auto view : swapViews)
        vkDestroyImageView(device, view, nullptr);
    swapViews.clear();
    swapImages.clear();
    if (swapchain)
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = {};
}
void Renderer::idle() {
    if (device) {
        std::lock_guard lock(queueMutex);
        check(vkDeviceWaitIdle(device), "Drain Vulkan device");
    }
}
void Renderer::resize() {
    rebuild = true;
}
uint64_t Renderer::allocatedBytes() const {
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);
    return stats.total.statistics.blockBytes;
}
uint64_t Renderer::availableBytes() const {
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator, budgets);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
    uint64_t free = 0;
    for (uint32_t i = 0; i < properties.memoryHeapCount; ++i)
        if (properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            free += budgets[i].budget > budgets[i].usage ? budgets[i].budget - budgets[i].usage : 0;
    return free;
}
uint64_t Renderer::completedBackground() const {
    uint64_t value = 0;
    check(vkGetSemaphoreCounterValue(device, backgroundTimeline, &value), "Query background timeline");
    return value;
}
std::shared_ptr<Buffer> Renderer::buffer(VkDeviceSize bytes, VkBufferUsageFlags usage, bool host) {
    std::lock_guard lock(allocationMutex);
    if (bytes > deviceBudget || allocatedBytes() + bytes > deviceBudget)
        throw std::runtime_error(
            "GPU allocation budget reached; close the current model or raise the budget");
    if (processMemory() > 7500000000ull)
        throw std::runtime_error("RAM budget reached");
    auto b = std::make_shared<Buffer>();
    b->allocator = allocator;
    b->bytes = bytes;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = usage;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (host)
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo details{};
    check(vmaCreateBuffer(allocator, &info, &allocation, &b->buffer, &b->allocation, &details),
          "Allocate buffer");
    b->mapped = details.pMappedData;
    return b;
}
std::shared_ptr<Image> Renderer::image(uint32_t width, uint32_t height, VkFormat format,
                                       VkImageUsageFlags usage) {
    std::lock_guard lock(allocationMutex);
    if (allocatedBytes() + uint64_t(width) * height * 4 > deviceBudget)
        throw std::runtime_error("GPU image budget reached");
    auto result = std::make_shared<Image>();
    result->device = device;
    result->allocator = allocator;
    result->width = width;
    result->height = height;
    VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    i.imageType = VK_IMAGE_TYPE_2D;
    i.format = format;
    i.extent = {width, height, 1};
    i.mipLevels = 1;
    i.arrayLayers = 1;
    i.samples = VK_SAMPLE_COUNT_1_BIT;
    i.tiling = VK_IMAGE_TILING_OPTIMAL;
    i.usage = usage;
    VmaAllocationCreateInfo a{};
    a.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    check(vmaCreateImage(allocator, &i, &a, &result->image, &result->allocation, nullptr), "Allocate image");
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = result->image;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v.format = format;
    v.subresourceRange = {VkImageAspectFlags(format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                            : VK_IMAGE_ASPECT_COLOR_BIT),
                          0, 1, 0, 1};
    check(vkCreateImageView(device, &v, nullptr, &result->view), "Create image view");
    return result;
}
void Renderer::createPipeline() {
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout), "Mesh pipeline layout");
    VkShaderModule vertex = shader(device, "mesh.vert.spv"), fragment = shader(device, "mesh.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& s : stages) {
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    VkVertexInputBindingDescription binding{0, sizeof(glm::vec3), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = 1;
    input.pVertexAttributeDescriptions = &attribute;
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo p{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    p.pNext = &rendering;
    p.stageCount = 2;
    p.pStages = stages;
    p.pVertexInputState = &input;
    p.pInputAssemblyState = &assembly;
    p.pViewportState = &viewport;
    p.pRasterizationState = &raster;
    p.pMultisampleState = &samples;
    p.pDepthStencilState = &depth;
    p.pColorBlendState = &blend;
    p.pDynamicState = &dynamic;
    p.layout = pipelineLayout;
    auto result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &p, nullptr, &pipeline);
    vkDestroyShaderModule(device, vertex, nullptr);
    vkDestroyShaderModule(device, fragment, nullptr);
    check(result, "Mesh graphics pipeline");
}
void Renderer::renderChunks(VkCommandBuffer cmd, const Image& color, const Image& depth,
                            const glm::mat4& matrix, const std::vector<std::shared_ptr<Buffer>>& chunks,
                            const std::vector<uint32_t>& counts, bool clear, size_t first, size_t end) {
    barrier(cmd, color.image, clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
            clear ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            clear ? 0 : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(cmd, depth.image, clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT,
            clear
                ? VK_PIPELINE_STAGE_2_NONE
                : VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            clear ? 0 : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo c{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    c.imageView = color.view;
    c.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    c.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    c.clearValue.color = {{0.075f, 0.095f, 0.12f, 1}};
    VkRenderingAttachmentInfo d{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    d.imageView = depth.view;
    d.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    d.loadOp = c.loadOp;
    d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    d.clearValue.depthStencil = {1, 0};
    VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
    r.renderArea.extent = {color.width, color.height};
    r.layerCount = 1;
    r.colorAttachmentCount = 1;
    r.pColorAttachments = &c;
    r.pDepthAttachment = &d;
    vkCmdBeginRendering(cmd, &r);
    VkViewport viewport{0, 0, float(color.width), float(color.height), 0, 1};
    VkRect2D scissor{{0, 0}, {color.width, color.height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(matrix), &matrix);
    for (size_t i = first; i < end; ++i) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &chunks[i]->buffer, &offset);
        vkCmdDraw(cmd, counts[i], 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
}
Thumbnail Renderer::thumbnail(const std::filesystem::path& path, Cancel cancel, Progress progress) {
    Thumbnail out;
    out.metadata = inspectStl(path, cancel, [&](float p) {
        if (progress)
            progress(p * 0.45f);
    });
    if (cancel && cancel())
        throw std::runtime_error("Cancelled");
    constexpr uint32_t size = 320;
    out.image = image(size, size, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    auto depth = image(size, size, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    Context context(*this);
    bool first = true;
    auto matrix = Camera{}.matrix(1);
    // Reuse one bounded pair of buffers. No whole CPU or GPU thumbnail mesh exists.
    auto staging = buffer(16384 * 36, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    auto geometry =
        buffer(16384 * 36, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    streamStl(
        path, out.metadata,
        [&](std::span<const glm::vec3> vertices) {
            if (cancel && cancel())
                throw std::runtime_error("Cancelled");
            std::memcpy(staging->mapped, vertices.data(), vertices.size_bytes());
            check(vmaFlushAllocation(allocator, staging->allocation, 0, vertices.size_bytes()),
                  "Flush vertices");
            context.begin();
            VkBufferCopy copy{0, 0, vertices.size_bytes()};
            vkCmdCopyBuffer(context.command, staging->buffer, geometry->buffer, 1, &copy);
            vertexBarrier(context.command, geometry->buffer, vertices.size_bytes());
            renderChunks(context.command, *out.image, *depth, matrix, {geometry}, {uint32_t(vertices.size())},
                         first, 0, 1);
            context.finish();
            first = false;
        },
        cancel,
        [&](float p) {
            if (progress)
                progress(0.45f + p * 0.5f);
        },
        16384);
    auto readback = buffer(size * size * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    context.begin();
    barrier(context.command, out.image->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {size, size, 1};
    vkCmdCopyImageToBuffer(context.command, out.image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback->buffer, 1, &copy);
    barrier(context.command, out.image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    out.image->ready = context.finish();
    out.image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    check(vmaInvalidateAllocation(allocator, readback->allocation, 0, VK_WHOLE_SIZE),
          "Read thumbnail pixels");
    out.pixels = std::make_shared<std::vector<uint8_t>>(size * size * 4);
    std::memcpy(out.pixels->data(), readback->mapped, out.pixels->size());
    if (progress)
        progress(1);
    return out;
}
std::shared_ptr<Image> Renderer::uploadImage(const std::vector<uint8_t>& pixels, uint32_t width,
                                             uint32_t height, Cancel cancel) {
    if (cancel && cancel())
        throw std::runtime_error("Cancelled");
    if (pixels.size() != size_t(width) * height * 4)
        throw std::runtime_error("Invalid cached image size");
    auto out = image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    auto staging = buffer(pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging->mapped, pixels.data(), pixels.size());
    check(vmaFlushAllocation(allocator, staging->allocation, 0, VK_WHOLE_SIZE), "Flush image upload");
    Context context(*this);
    context.begin();
    barrier(context.command, out->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(context.command, staging->buffer, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &copy);
    barrier(context.command, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    out->ready = context.finish();
    out->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return out;
}
std::shared_ptr<Mesh> Renderer::loadMesh(const std::filesystem::path& path, uint64_t budget, Cancel cancel,
                                         Progress progress, Metadata* metadata) {
    auto out = std::make_shared<Mesh>();
    out->metadata = inspectStl(path, cancel, [&](float p) {
        if (progress)
            progress(p * 0.4f);
    });
    if (metadata)
        *metadata = out->metadata;
    if (out->metadata.geometryBytes() > budget)
        throw std::runtime_error(
            "Exact geometry needs " + std::to_string((out->metadata.geometryBytes() + 999999) / 1000000) +
            " MB; viewer limit is " + std::to_string(budget / 1000000) + " MB. Raise the budget and retry.");
    if (out->metadata.geometryBytes() + allocatedBytes() + 64000000 > deviceBudget)
        throw std::runtime_error(
            "Not enough overall GPU budget for this model; raise the total budget and retry");
    if (out->metadata.geometryBytes() > availableBytes())
        throw std::runtime_error("Insufficient currently available GPU memory");
    Context context(*this);
    auto staging = buffer(65536 * 36, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    streamStl(
        path, out->metadata,
        [&](std::span<const glm::vec3> vertices) {
            if (cancel && cancel())
                throw std::runtime_error("Cancelled");
            auto geometry =
                buffer(vertices.size_bytes(),
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
            std::memcpy(staging->mapped, vertices.data(), vertices.size_bytes());
            check(vmaFlushAllocation(allocator, staging->allocation, 0, vertices.size_bytes()),
                  "Flush model vertices");
            context.begin();
            VkBufferCopy copy{0, 0, vertices.size_bytes()};
            vkCmdCopyBuffer(context.command, staging->buffer, geometry->buffer, 1, &copy);
            vertexBarrier(context.command, geometry->buffer, vertices.size_bytes());
            out->ready = context.finish();
            out->chunks.push_back(std::move(geometry));
            out->counts.push_back(uint32_t(vertices.size()));
        },
        cancel,
        [&](float p) {
            if (progress)
                progress(0.4f + p * 0.6f);
        });
    return out;
}
void Renderer::beginFrame() {
    if (rebuild) {
        idle();
        for (auto& f : frameData) {
            f.images.clear();
            f.meshes.clear();
        }
        auto oldCount = swapImages.size();
        auto oldFormat = swapFormat;
        destroySwapchain();
        createSwapchain();
        if (imguiReady && (oldCount != swapImages.size() || oldFormat != swapFormat)) {
            std::lock_guard lock(queueMutex);
            ImGui_ImplVulkan_Shutdown();
            initImguiRenderer();
        }
        rebuild = false;
    }
    current = &frameData[frames % frameData.size()];
    check(vkWaitForFences(device, 1, &current->fence, VK_TRUE, UINT64_MAX), "Wait for foreground frame");
    current->images.clear();
    current->meshes.clear();
    check(vkResetCommandPool(device, current->pool, 0), "Reset foreground commands");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(current->command, &begin), "Begin foreground commands");
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}
void Renderer::use(const std::shared_ptr<Image>& img) {
    if (!img)
        return;
    if (!img->descriptor)
        img->descriptor =
            ImGui_ImplVulkan_AddTexture(sampler, img->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    waitBackground = std::max(waitBackground, img->ready);
    current->images.push_back(img);
}
void Renderer::resetViewer() {
    viewColor.reset();
    viewDepth.reset();
    viewDisplayed.reset();
    viewMesh.reset();
    viewVersion = ~0ull;
    viewChunk = 0;
}
std::shared_ptr<Image> Renderer::renderView(const std::shared_ptr<Mesh>& mesh, const Camera& camera,
                                            uint32_t width, uint32_t height, uint64_t version) {
    if (!mesh)
        return {};
    width = std::clamp(width, 64u, 2560u);
    height = std::clamp(height, 64u, 1600u);
    if (viewVersion != version || viewMesh != mesh || !viewColor || viewColor->width != width ||
        viewColor->height != height) {
        viewColor = image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        viewDepth = image(width, height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        viewVersion = version;
        viewMesh = mesh;
        viewChunk = 0;
    }
    if (viewChunk < mesh->chunks.size()) {
        size_t end = std::min(viewChunk + 8, mesh->chunks.size());
        waitBackground = std::max(waitBackground, mesh->ready);
        renderChunks(current->command, *viewColor, *viewDepth, camera.matrix(float(width) / height),
                     mesh->chunks, mesh->counts, viewChunk == 0, viewChunk, end);
        viewChunk = end;
        current->images.push_back(viewColor);
        current->images.push_back(viewDepth);
        current->meshes.push_back(mesh);
        if (end == mesh->chunks.size()) {
            barrier(current->command, viewColor->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            viewColor->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            viewDisplayed = viewColor;
        }
    }
    viewProgress = float(viewChunk) / std::max(size_t(1), mesh->chunks.size());
    return viewDisplayed;
}
bool Renderer::drawFrame() {
    ImGui::Render();
    uint32_t index = 0;
    auto acquire =
        vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, current->acquired, VK_NULL_HANDLE, &index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        check(vkEndCommandBuffer(current->command), "End abandoned frame");
        resetViewer();
        rebuild = true;
        return false;
    }
    if (acquire != VK_SUBOPTIMAL_KHR)
        check(acquire, "Acquire swapchain image");
    else
        rebuild = true;
    barrier(current->command, swapImages[index], VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = swapViews[index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.05f, 0.06f, 0.08f, 1}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(current->command, &rendering);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), current->command);
    vkCmdEndRendering(current->command);
    std::shared_ptr<Buffer> capture;
    if (!capturePath.empty()) {
        capture = buffer(uint64_t(extent.width) * extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        barrier(current->command, swapImages[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(current->command, swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               capture->buffer, 1, &region);
        barrier(current->command, swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    } else
        barrier(current->command, swapImages[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE, 0);
    check(vkEndCommandBuffer(current->command), "End foreground commands");
    VkSemaphoreSubmitInfo waits[2]{};
    waits[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[0].semaphore = current->acquired;
    waits[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    waits[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[1].semaphore = backgroundTimeline;
    waits[1].value = waitBackground;
    waits[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = presentSemaphores[index];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    command.commandBuffer = current->command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = waitBackground ? 2 : 1;
    submit.pWaitSemaphoreInfos = waits;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    check(vkResetFences(device, 1, &current->fence), "Reset frame fence");
    {
        std::lock_guard lock(queueMutex);
        check(vkQueueSubmit2(foreground, 1, &submit, current->fence), "Foreground submit");
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &presentSemaphores[index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        auto result = vkQueuePresentKHR(foreground, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            rebuild = true;
        else
            check(result, "Present");
    }
    if (capture) {
        check(vkWaitForFences(device, 1, &current->fence, VK_TRUE, UINT64_MAX),
              "Explicit screenshot completion");
        check(vmaInvalidateAllocation(allocator, capture->allocation, 0, VK_WHOLE_SIZE),
              "Screenshot readback");
        std::ofstream file(capturePath, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot write screenshot");
        file << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
        auto* pixels = static_cast<uint8_t*>(capture->mapped);
        bool bgra = swapFormat == VK_FORMAT_B8G8R8A8_UNORM || swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
        for (size_t i = 0; i < size_t(extent.width) * extent.height; ++i) {
            char rgb[3] = {char(pixels[i * 4 + (bgra ? 2 : 0)]), char(pixels[i * 4 + 1]),
                           char(pixels[i * 4 + (bgra ? 0 : 2)])};
            file.write(rgb, 3);
        }
        capturePath.clear();
    }
    ++frames;
    return true;
}
Renderer::~Renderer() {
    if (device)
        vkDeviceWaitIdle(device);
    for (auto& f : frameData) {
        f.images.clear();
        f.meshes.clear();
    }
    resetViewer();
    if (imguiReady) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }
    if (device) {
        if (descriptorPool)
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (sampler)
            vkDestroySampler(device, sampler, nullptr);
        if (pipeline)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout)
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        destroySwapchain();
        for (auto& f : frameData) {
            if (f.fence)
                vkDestroyFence(device, f.fence, nullptr);
            if (f.acquired)
                vkDestroySemaphore(device, f.acquired, nullptr);
            if (f.pool)
                vkDestroyCommandPool(device, f.pool, nullptr);
        }
        if (backgroundTimeline)
            vkDestroySemaphore(device, backgroundTimeline, nullptr);
        if (allocator)
            vmaDestroyAllocator(allocator);
        vkDestroyDevice(device, nullptr);
    }
    if (surface)
        vkDestroySurfaceKHR(instance, surface, nullptr);
    if (debug) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy)
            destroy(instance, debug, nullptr);
    }
    if (instance)
        vkDestroyInstance(instance, nullptr);
}
} // namespace si
