// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone host shell. Guest execution is supplied by a separate runtime.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#include "../rendering/capabilities.h"
#include "menu_frag.h"
#include "menu_vert.h"
#include "nuklear.h"

namespace {
void check(VkResult result, const char *operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed (Vulkan " +
                             std::to_string(result) + ")");
}
struct Vertex {
  float position[2], uv[2];
  uint8_t color[4];
};
struct Buffer {
  VkBuffer handle{};
  VkDeviceMemory memory{};
  void *mapped{};
  VkDeviceSize size{};
};
struct Host {
  SDL_Window *window{};
  VkInstance instance{};
  VkSurfaceKHR surface{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family{};
  VkPhysicalDeviceProperties properties{};
  VkPhysicalDeviceMemoryProperties memoryProperties{};
  VkSwapchainKHR swapchain{};
  VkExtent2D extent{};
  VkSurfaceFormatKHR format{};
  std::vector<VkImage> images;
  std::vector<VkImageView> views;
  std::vector<VkFramebuffer> framebuffers;
  VkRenderPass renderPass{};
  VkCommandPool commandPool{};
  VkCommandBuffer command{};
  VkSemaphore acquired{}, rendered{};
  VkDescriptorSetLayout descriptorLayout{};
  VkDescriptorPool descriptorPool{};
  VkDescriptorSet descriptor{};
  VkPipelineLayout pipelineLayout{};
  VkPipeline pipeline{};
  VkImage fontImage{};
  VkDeviceMemory fontMemory{};
  VkImageView fontView{};
  VkSampler sampler{};
  Buffer vertices{}, indices{}, upload{};
  nk_context context{};
  nk_font_atlas atlas{};
  nk_draw_null_texture nullTexture{};
  nk_buffer commands{};
  bool contextReady{}, atlasReady{}, commandsReady{};
  bool running{true}, resize{true};
  unsigned validationErrors{};
  VkDebugUtilsMessengerEXT messenger{};
  bool validation{};
  bool fullscreen{}, showHostFps{true};
  int page{};
  double hostFps{};
  uint64_t fpsEpoch{};
  unsigned fpsFrames{};
  std::string displayError;

  static VKAPI_ATTR VkBool32 VKAPI_CALL
  debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT *info, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
      ++static_cast<Host *>(user)->validationErrors;
    std::fprintf(stderr, "Vulkan validation: %s\n", info->pMessage);
    return VK_FALSE;
  }
  ~Host() {
    if (device)
      vkDeviceWaitIdle(device);
    if (contextReady)
      nk_free(&context);
    if (commandsReady)
      nk_buffer_free(&commands);
    if (atlasReady)
      nk_font_atlas_clear(&atlas);
    if (device) {
      destroySwapchain();
      destroyBuffer(vertices);
      destroyBuffer(indices);
      destroyBuffer(upload);
      vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
      vkDestroyDescriptorPool(device, descriptorPool, nullptr);
      vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
      vkDestroySampler(device, sampler, nullptr);
      vkDestroyImageView(device, fontView, nullptr);
      vkDestroyImage(device, fontImage, nullptr);
      vkFreeMemory(device, fontMemory, nullptr);
      vkDestroyRenderPass(device, renderPass, nullptr);
      vkDestroySemaphore(device, acquired, nullptr);
      vkDestroySemaphore(device, rendered, nullptr);
      vkDestroyCommandPool(device, commandPool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (surface)
      vkDestroySurfaceKHR(instance, surface, nullptr);
    if (messenger)
      reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"))(
          instance, messenger, nullptr);
    if (instance)
      vkDestroyInstance(instance, nullptr);
    if (window)
      SDL_DestroyWindow(window);
    SDL_Quit();
  }
  uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
      if ((bits & (1u << i)) &&
          (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("No suitable Vulkan memory type");
  }
  void makeBuffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage) {
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    check(vkCreateBuffer(device, &ci, nullptr, &b.handle), "create buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.handle, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device, &alloc, nullptr, &b.memory),
          "allocate buffer");
    check(vkBindBufferMemory(device, b.handle, b.memory, 0), "bind buffer");
    check(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped), "map buffer");
  }
  void destroyBuffer(Buffer &b) {
    if (b.mapped)
      vkUnmapMemory(device, b.memory);
    vkDestroyBuffer(device, b.handle, nullptr);
    vkFreeMemory(device, b.memory, nullptr);
    b = {};
  }
  void beginCommands() {
    check(vkResetCommandPool(device, commandPool, 0), "reset commands");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command, &bi), "begin commands");
  }
  void initialize(bool requestValidation) {
    validation = requestValidation;
    if (!SDL_Init(SDL_INIT_VIDEO))
      throw std::runtime_error(SDL_GetError());
    window = SDL_CreateWindow("Time Crisis 3 Native", 1100, 720,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
      throw std::runtime_error(SDL_GetError());
    uint32_t count{};
    const char *const *required = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!required)
      throw std::runtime_error(SDL_GetError());
    std::vector<const char *> extensions(required, required + count);
    const char *validationLayer = "VK_LAYER_KHRONOS_validation";
    if (validation)
      extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Time Crisis 3 Native host";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = validation ? 1 : 0;
    ci.ppEnabledLayerNames = validation ? &validationLayer : nullptr;
    check(vkCreateInstance(&ci, nullptr, &instance), "create instance");
    if (validation) {
      VkDebugUtilsMessengerCreateInfoEXT di{
          VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
      di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      di.pfnUserCallback = debug;
      di.pUserData = this;
      check(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance,
                                      "vkCreateDebugUtilsMessengerEXT"))(
                instance, &di, nullptr, &messenger),
            "create validation messenger");
    }
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
      throw std::runtime_error(SDL_GetError());
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr),
          "enumerate GPUs");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
          "enumerate GPUs");
    for (auto candidate : devices) {
      uint32_t n;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
      std::vector<VkQueueFamilyProperties> families(n);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, families.data());
      uint32_t extensionCount{};
      check(vkEnumerateDeviceExtensionProperties(candidate, nullptr,
                                                 &extensionCount, nullptr),
            "device extensions");
      std::vector<VkExtensionProperties> available(extensionCount);
      check(vkEnumerateDeviceExtensionProperties(
                candidate, nullptr, &extensionCount, available.data()),
            "device extensions");
      if (std::none_of(available.begin(), available.end(), [](const auto &e) {
            return std::strcmp(e.extensionName,
                               VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
          }))
        continue;
      for (uint32_t i = 0; i < n; ++i) {
        VkBool32 present{};
        check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface,
                                                   &present),
              "surface support");
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
          physical = candidate;
          family = i;
          break;
        }
      }
      if (physical)
        break;
    }
    if (!physical)
      throw std::runtime_error("No Vulkan graphics/present queue available");
    vkGetPhysicalDeviceProperties(physical, &properties);
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    const char *swapExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qi;
    dc.enabledExtensionCount = 1;
    dc.ppEnabledExtensionNames = &swapExtension;
    check(vkCreateDevice(physical, &dc, nullptr, &device), "create device");
    vkGetDeviceQueue(device, family, 0, &queue);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count,
                                               nullptr),
          "surface formats");
    std::vector<VkSurfaceFormatKHR> formats(count);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count,
                                               formats.data()),
          "surface formats");
    if (formats.empty())
      throw std::runtime_error("Surface has no formats");
    format = formats.front();
    for (auto f : formats)
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
          f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        format = f;
    if (format.format == VK_FORMAT_UNDEFINED)
      format.format = VK_FORMAT_B8G8R8A8_UNORM;
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = family;
    check(vkCreateCommandPool(device, &pc, nullptr, &commandPool),
          "create command pool");
    VkCommandBufferAllocateInfo ca{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = commandPool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &ca, &command), "allocate commands");
    VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check(vkCreateSemaphore(device, &sc, nullptr, &acquired),
          "create acquire semaphore");
    check(vkCreateSemaphore(device, &sc, nullptr, &rendered),
          "create render semaphore");
    createRenderPass();
    createFont();
    createPipeline();
    makeBuffer(vertices, 4 * 1024 * 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    makeBuffer(indices, 1024 * 1024, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    nk_buffer_init_default(&commands);
    commandsReady = true;
    std::printf("Host ready: SDL3, Vulkan GPU=%s, Nuklear; guest runtime not "
                "connected\n",
                properties.deviceName);
  }
  void createRenderPass() {
    VkAttachmentDescription a{};
    a.format = format.format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo r{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    r.attachmentCount = 1;
    r.pAttachments = &a;
    r.subpassCount = 1;
    r.pSubpasses = &sub;
    r.dependencyCount = 1;
    r.pDependencies = &dep;
    check(vkCreateRenderPass(device, &r, nullptr, &renderPass),
          "create render pass");
  }
  void createFont() {
    nk_font_atlas_init_default(&atlas);
    atlasReady = true;
    nk_font_atlas_begin(&atlas);
    nk_font *font = nk_font_atlas_add_default(&atlas, 20, nullptr);
    if (!font)
      throw std::runtime_error("Nuklear default font unavailable");
    int width, height;
    const void *pixels =
        nk_font_atlas_bake(&atlas, &width, &height, NK_FONT_ATLAS_RGBA32);
    if (!pixels)
      throw std::runtime_error("Nuklear font baking failed");
    makeBuffer(upload, VkDeviceSize(width) * height * 4,
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(upload.mapped, pixels, size_t(upload.size));
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = VK_FORMAT_R8G8B8A8_UNORM;
    ic.extent = {uint32_t(width), uint32_t(height), 1};
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    check(vkCreateImage(device, &ic, nullptr, &fontImage), "create font image");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, fontImage, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &ai, nullptr, &fontMemory), "allocate font");
    check(vkBindImageMemory(device, fontImage, fontMemory, 0), "bind font");
    beginCommands();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    barrier.image = fontImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = ic.extent;
    vkCmdCopyBufferToImage(command, upload.handle, fontImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    check(vkEndCommandBuffer(command), "end font commands");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "upload font");
    check(vkQueueWaitIdle(queue), "wait font upload");
    destroyBuffer(upload);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = fontImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ic.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(device, &vi, nullptr, &fontView), "font view");
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(device, &si, nullptr, &sampler), "font sampler");
    nk_font_atlas_end(&atlas, nk_handle_id(1), &nullTexture);
    if (!nk_init_default(&context, &font->handle))
      throw std::runtime_error("Nuklear context initialization failed");
    contextReady = true;
  }
  void createPipeline() {
    VkDescriptorSetLayoutBinding binding{
        0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
        VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device, &dl, nullptr, &descriptorLayout),
          "descriptor layout");
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &poolSize;
    check(vkCreateDescriptorPool(device, &dp, nullptr, &descriptorPool),
          "descriptor pool");
    VkDescriptorSetAllocateInfo da{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = descriptorPool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &descriptorLayout;
    check(vkAllocateDescriptorSets(device, &da, &descriptor), "descriptor set");
    VkDescriptorImageInfo image{sampler, fontView,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 4 * sizeof(float)};
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &descriptorLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device, &pl, nullptr, &pipelineLayout),
          "pipeline layout");
    VkShaderModule vertex{}, fragment{};
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(tc3_vert);
    sm.pCode = tc3_vert;
    check(vkCreateShaderModule(device, &sm, nullptr, &vertex), "vertex shader");
    sm.codeSize = sizeof(tc3_frag);
    sm.pCode = tc3_frag;
    VkResult fragmentResult =
        vkCreateShaderModule(device, &sm, nullptr, &fragment);
    if (fragmentResult != VK_SUCCESS) {
      vkDestroyShaderModule(device, vertex, nullptr);
      check(fragmentResult, "fragment shader");
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto &s : stages) {
      s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      s.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    VkVertexInputBindingDescription vb{0, sizeof(Vertex),
                                       VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, color)}};
    VkPipelineVertexInputStateCreateInfo vin{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &vb;
    vin.vertexAttributeDescriptionCount = 3;
    vin.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                 VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo gp{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = pipelineLayout;
    gp.renderPass = renderPass;
    VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp,
                                                nullptr, &pipeline);
    vkDestroyShaderModule(device, vertex, nullptr);
    vkDestroyShaderModule(device, fragment, nullptr);
    check(result, "graphics pipeline");
  }
  void destroySwapchain() {
    for (auto f : framebuffers)
      vkDestroyFramebuffer(device, f, nullptr);
    framebuffers.clear();
    for (auto v : views)
      vkDestroyImageView(device, v, nullptr);
    views.clear();
    images.clear();
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
  }
  bool recreate() {
    int width, height;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    if (width <= 0 || height <= 0 ||
        (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED))
      return false;
    check(vkDeviceWaitIdle(device), "wait resize");
    destroySwapchain();
    VkSurfaceCapabilitiesKHR caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps),
          "surface capabilities");
    extent = caps.currentExtent;
    if (extent.width == UINT32_MAX)
      extent = {std::clamp(uint32_t(width), caps.minImageExtent.width,
                           caps.maxImageExtent.width),
                std::clamp(uint32_t(height), caps.minImageExtent.height,
                           caps.maxImageExtent.height)};
    if (!extent.width || !extent.height)
      return false;
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount)
      count = std::min(count, caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (auto a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                   VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                   VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                   VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
      if (caps.supportedCompositeAlpha & a) {
        alpha = a;
        break;
      }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = count;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    check(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain),
          "create swapchain");
    check(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr),
          "swapchain images");
    images.resize(count);
    check(vkGetSwapchainImagesKHR(device, swapchain, &count, images.data()),
          "swapchain images");
    for (auto image : images) {
      VkImageView view{};
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = format.format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      check(vkCreateImageView(device, &vi, nullptr, &view), "swapchain view");
      views.push_back(view);
      VkFramebuffer framebuffer{};
      VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fi.renderPass = renderPass;
      fi.attachmentCount = 1;
      fi.pAttachments = &views.back();
      fi.width = extent.width;
      fi.height = extent.height;
      fi.layers = 1;
      check(vkCreateFramebuffer(device, &fi, nullptr, &framebuffer),
            "framebuffer");
      framebuffers.push_back(framebuffer);
    }
    resize = false;
    return true;
  }
  void input() {
    nk_input_begin(&context);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_EVENT_QUIT:
        running = false;
        break;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      case SDL_EVENT_WINDOW_RESTORED:
        resize = true;
        break;
      case SDL_EVENT_MOUSE_MOTION:
        nk_input_motion(&context, int(event.motion.x), int(event.motion.y));
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_LEFT)
          nk_input_button(&context, NK_BUTTON_LEFT, int(event.button.x),
                          int(event.button.y), event.button.down);
        break;
      case SDL_EVENT_MOUSE_WHEEL:
        nk_input_scroll(&context, nk_vec2(event.wheel.x, event.wheel.y));
        break;
      case SDL_EVENT_KEY_DOWN:
        if (event.key.key == SDLK_ESCAPE)
          running = false;
        break;
      default:
        break;
      }
    }
    nk_input_end(&context);
  }
  void menu(int width, int height) {
    float panelWidth = std::min(760.f, float(width) - 32),
          panelHeight = float(height) - 32;
    if (nk_begin(&context, "Time Crisis 3 Native",
                 nk_rect(16, 16, std::max(1.f, panelWidth),
                         std::max(1.f, panelHeight)),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
      nk_layout_row_dynamic(&context, 34, 1);
      nk_label(&context, "TIME CRISIS 3", NK_TEXT_LEFT);
      nk_layout_row_dynamic(&context, 30, 4);
      const char *pages[] = {"Overview", "Display", "Rendering", "Performance"};
      for (int i = 0; i < 4; ++i)
        if (nk_button_label(&context, pages[i]))
          page = i;
      nk_layout_row_dynamic(&context, 29, 1);
      if (page == 0) {
        nk_label_colored(&context, "Runtime integration pending", NK_TEXT_LEFT,
                         nk_rgb(255, 190, 90));
        nk_layout_row_dynamic(&context, 55, 1);
        nk_label_wrap(&context, "The Linux window and Vulkan menu are running. "
                                "Game execution is not connected yet.");
        nk_layout_row_dynamic(&context, 29, 1);
        nk_label(&context, "Renderer: Vulkan", NK_TEXT_LEFT);
        nk_label(&context, properties.deviceName, NK_TEXT_LEFT);
        nk_layout_row_dynamic(&context, 70, 1);
        nk_label_wrap(&context,
                      "This launcher will use the PCSX2-derived runtime. "
                      "Gameplay and performance measurements become available "
                      "after that integration.");
        nk_layout_row_dynamic(&context, 38, 1);
        if (nk_button_label(&context, "Quit"))
          running = false;
      } else if (page == 1) {
        nk_bool full = fullscreen, show = showHostFps;
        if (nk_checkbox_label(&context, "Fullscreen", &full)) {
          if (SDL_SetWindowFullscreen(window, full)) {
            fullscreen = full;
            resize = true;
            displayError.clear();
          } else
            displayError = SDL_GetError();
        }
        if (nk_checkbox_label(&context, "Show host presentation rate", &show))
          showHostFps = show;
        nk_label(&context, "Presentation: VSync (Vulkan FIFO)", NK_TEXT_LEFT);
        char size[100];
        std::snprintf(size, sizeof(size), "Window: %d x %d; drawable: %u x %u",
                      width, height, extent.width, extent.height);
        nk_label(&context, size, NK_TEXT_LEFT);
        nk_layout_row_dynamic(&context, 55, 1);
        nk_label_wrap(&context,
                      "Game resolution, frame limit and motion blur controls "
                      "become available when the game renderer is connected.");
        if (!displayError.empty())
          nk_label_wrap(&context, displayError.c_str());
      } else {
        nk_layout_row_dynamic(&context, 55, 1);
        nk_label_wrap(&context,
                      "Unavailable options are listed for parity with the "
                      "DriveClub menu. They cannot be enabled until a working "
                      "backend supplies the required scene data.");
        for (size_t i = 0; i < tc3::rendering::capabilities.size(); ++i) {
          if ((page == 2 && i >= 8) || (page == 3 && i < 8))
            continue;
          const auto &cap = tc3::rendering::capabilities[i];
          nk_layout_row_dynamic(&context, 30, 1);
          nk_widget_disable_begin(&context);
          nk_button_label(&context, cap.name);
          nk_widget_disable_end(&context);
          nk_layout_row_dynamic(&context, 44, 1);
          nk_label_wrap(&context, cap.reason);
        }
        if (page == 3) {
          nk_layout_row_dynamic(&context, 28, 1);
          nk_label(&context, "Game FPS: unavailable", NK_TEXT_LEFT);
          nk_label(&context, "Generated FPS: unavailable", NK_TEXT_LEFT);
        }
      }
      if (showHostFps) {
        char fps[100];
        std::snprintf(fps, sizeof(fps),
                      "Host presentation: %.1f FPS (menu only)", hostFps);
        nk_layout_row_dynamic(&context, 28, 1);
        nk_label(&context, fps, NK_TEXT_LEFT);
      }
      nk_layout_row_dynamic(&context, 26, 1);
      nk_label(&context, "Esc closes the host", NK_TEXT_LEFT);
    }
    nk_end(&context);
  }
  bool frame() {
    if (resize && !recreate()) {
      SDL_Delay(20);
      return false;
    }
    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    if (width <= 0 || height <= 0)
      return false;
    menu(width, height);
    nk_buffer vb, ib;
    nk_buffer_init_fixed(&vb, vertices.mapped, size_t(vertices.size));
    nk_buffer_init_fixed(&ib, indices.mapped, size_t(indices.size));
    const nk_draw_vertex_layout_element layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, offsetof(Vertex, position)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, offsetof(Vertex, uv)},
        {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, offsetof(Vertex, color)},
        {NK_VERTEX_LAYOUT_END}};
    nk_convert_config config{};
    config.vertex_layout = layout;
    config.vertex_size = sizeof(Vertex);
    config.vertex_alignment = alignof(Vertex);
    config.tex_null = nullTexture;
    config.circle_segment_count = config.arc_segment_count =
        config.curve_segment_count = 22;
    config.global_alpha = 1;
    config.shape_AA = config.line_AA = NK_ANTI_ALIASING_ON;
    if (nk_convert(&context, &commands, &vb, &ib, &config) !=
        NK_CONVERT_SUCCESS)
      throw std::runtime_error("Nuklear geometry exceeds host buffer capacity");
    uint32_t index{};
    VkResult acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                             acquired, VK_NULL_HANDLE, &index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
      resize = true;
      nk_clear(&context);
      nk_buffer_clear(&commands);
      return false;
    }
    if (acquire != VK_SUBOPTIMAL_KHR)
      check(acquire, "acquire image");
    else
      resize = true;
    beginCommands();
    VkClearValue clear{};
    clear.color = {{.035f, .045f, .065f, 1}};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = renderPass;
    rp.framebuffer = framebuffers[index];
    rp.renderArea.extent = extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
    vkCmdSetViewport(command, 0, 1, &viewport);
    float transform[] = {2.f / width, 2.f / height, -1, -1};
    vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(transform), transform);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 1, &descriptor, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
    vkCmdBindIndexBuffer(command, indices.handle, 0, VK_INDEX_TYPE_UINT16);
    const nk_draw_command *draw;
    uint32_t first = 0;
    nk_draw_foreach(draw, &context, &commands) {
      float sx = float(extent.width) / width,
            sy = float(extent.height) / height;
      int x = std::clamp(int(draw->clip_rect.x * sx), 0, int(extent.width));
      int y = std::clamp(int(draw->clip_rect.y * sy), 0, int(extent.height));
      int right = std::clamp(int((draw->clip_rect.x + draw->clip_rect.w) * sx),
                             x, int(extent.width));
      int bottom = std::clamp(int((draw->clip_rect.y + draw->clip_rect.h) * sy),
                              y, int(extent.height));
      if (right > x && bottom > y && draw->elem_count) {
        VkRect2D clip{{x, y}, {uint32_t(right - x), uint32_t(bottom - y)}};
        vkCmdSetScissor(command, 0, 1, &clip);
        vkCmdDrawIndexed(command, draw->elem_count, 1, first, 0, 0);
      }
      first += draw->elem_count;
    }
    vkCmdEndRenderPass(command);
    check(vkEndCommandBuffer(command), "end frame");
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired;
    submit.pWaitDstStageMask = &wait;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &rendered;
    check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit frame");
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    VkResult result = vkQueuePresentKHR(queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
      resize = true;
    else
      check(result, "present");
    // The shell has one frame in flight. Guest rendering will own its own
    // frame scheduler; the host deliberately makes no gameplay FPS claim.
    check(vkQueueWaitIdle(queue), "wait frame");
    const uint64_t now = SDL_GetTicks();
    if (!fpsEpoch)
      fpsEpoch = now;
    ++fpsFrames;
    if (now - fpsEpoch >= 500) {
      hostFps = double(fpsFrames) * 1000 / double(now - fpsEpoch);
      fpsFrames = 0;
      fpsEpoch = now;
    }
    nk_clear(&context);
    nk_buffer_clear(&commands);
    return true;
  }
};
} // namespace
int main(int argc, char **argv) {
  try {
    unsigned smokeFrames = 0;
    bool validation = false, smokeResize = false, smokeUi = false;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--help") {
        std::puts("tc3-native-host [--smoke-frames N] [--smoke-resize] "
                  "[--smoke-ui] [--validation]\nLinux SDL3/Vulkan/Nuklear host "
                  "shell; no "
                  "guest runtime connected.");
        return 0;
      }
      if (arg == "--validation")
        validation = true;
      else if (arg == "--smoke-resize")
        smokeResize = true;
      else if (arg == "--smoke-ui")
        smokeUi = true;
      else if (arg == "--smoke-frames" && i + 1 < argc) {
        std::string n = argv[++i];
        size_t end{};
        unsigned long value = std::stoul(n, &end);
        if (end != n.size() || !value || value > 100000)
          throw std::runtime_error("Smoke frame count must be 1..100000");
        smokeFrames = unsigned(value);
      } else
        throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
    if (smokeUi && smokeFrames < 12)
      throw std::runtime_error("--smoke-ui requires at least 12 smoke frames");
    Host host;
    host.initialize(validation);
    unsigned frames = 0;
    const uint64_t started = SDL_GetTicks();
    while (host.running && (!smokeFrames || frames < smokeFrames)) {
      host.input();
      if (!host.running)
        break;
      if (smokeUi)
        host.page = int((frames / 3) % 4);
      if (host.frame())
        ++frames;
      if (smokeResize && frames == 2) {
        SDL_SetWindowSize(host.window, 850, 560);
        host.resize = true;
        smokeResize = false;
      }
      if (smokeUi && (frames == 4 || frames == 8)) {
        host.fullscreen = frames == 4;
        if (!SDL_SetWindowFullscreen(host.window, host.fullscreen))
          throw std::runtime_error(SDL_GetError());
        host.resize = true;
      }
      if (smokeFrames && SDL_GetTicks() - started > 30000)
        throw std::runtime_error("Vulkan smoke test timed out");
    }
    if (smokeFrames) {
      if (frames != smokeFrames)
        throw std::runtime_error("Smoke run closed before requested frames");
      std::printf("Vulkan smoke: %u presented frames, %u validation errors\n",
                  frames, host.validationErrors);
    }
    return host.validationErrors ? 1 : 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "Time Crisis 3 host: %s\n", e.what());
    return 1;
  }
}
