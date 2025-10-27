//> includes
#include "vk_engine.h"

#include "VkBootstrap.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>

#include <chrono>
#include <thread>

VulkanEngine *loadedEngine = nullptr;

VulkanEngine &VulkanEngine::Get() { return *loadedEngine; }
void VulkanEngine::init()
{
  // only one engine initialization is allowed with the application.
  assert(loadedEngine == nullptr);
  loadedEngine = this;

  // We initialize SDL and create a window with it.
  SDL_Init(SDL_INIT_VIDEO);

  SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN);

  _window = SDL_CreateWindow("Vulkan Engine",
                             SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED,
                             _windowExtent.width,
                             _windowExtent.height,
                             window_flags);

  init_vulkan();
  init_swapchain();
  init_commands();
  init_default_renderpass();
  init_framebuffers();

  // everything went fine
  _isInitialized = true;
}

void VulkanEngine::init_vulkan()
{
  vkb::InstanceBuilder builder;

  auto inst_ret = builder.set_app_name("Vulkan Engine")
                         .request_validation_layers()
                         .use_default_debug_messenger()
                         .require_api_version(1,3,0)
                         .build();

  vkb::Instance vkb_inst = inst_ret.value();

  // store the instance and debug messenger
  _instance = vkb_inst.instance;
  _debug_messenger = vkb_inst.debug_messenger;

  SDL_Vulkan_CreateSurface(_window, _instance, &_surface);

  // vulkan 1.3 features
  VkPhysicalDeviceVulkan13Features features{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  features.dynamicRendering = VK_TRUE;
  features.synchronization2 = VK_TRUE;

  // vulkan 1.2 features
  VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
  features12.bufferDeviceAddress = VK_TRUE;
  features12.descriptorIndexing = VK_TRUE;

  //vkboostrap will select gpu
  // find GPU that can write to our SDL window with vulkan 1.3 features
  vkb::PhysicalDeviceSelector selector{ vkb_inst };
  vkb::PhysicalDevice physicalDevice = selector.set_minimum_version(1,1)
                                              //  .set_required_features_13(features)
                                              //  .set_required_features_12(features12)
                                               .set_surface(_surface)
                                               .select()
                                               .value();

  // logical device
  vkb::DeviceBuilder deviceBuilder{ physicalDevice };
  vkb::Device vkbDevice = deviceBuilder.build().value();

  // save device
  _device = vkbDevice.device;
  _chosenGPU = physicalDevice.physical_device;

  // get graphics queue
  _graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
  _graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();
}

void VulkanEngine::init_swapchain()
{
  vkb::SwapchainBuilder swapchainBuilder{ _chosenGPU, _device, _surface };
  vkb::Swapchain vkbSwapchain = swapchainBuilder
                                    .use_default_format_selection()
                                    .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                                    .set_desired_extent(_windowExtent.width, _windowExtent.height)
                                    .build()
                                    .value();

  // store swap chain and its properties
  _swapchain = vkbSwapchain.swapchain;
  _swapchainImages = vkbSwapchain.get_images().value();
  _swapchainImageViews = vkbSwapchain.get_image_views().value();
  _swapchainImageFormat = vkbSwapchain.image_format;
}

void VulkanEngine::init_commands()
{
  VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(_graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
  VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_commandPool));

  VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_commandPool, 1);
  VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_mainCommandBuffer));
}

void VulkanEngine::init_default_renderpass()
{
  VkAttachmentDescription colorAttachment = {};
  colorAttachment.format = _swapchainImageFormat;
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentReference color_attachment_ref = {};
  color_attachment_ref.attachment = 0;
  color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_attachment_ref;

  VkRenderPassCreateInfo render_pass_info = {};
  render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  // color attachment
  render_pass_info.attachmentCount = 1;
  render_pass_info.pAttachments = &colorAttachment;
  // subpass
  render_pass_info.subpassCount = 1;
  render_pass_info.pSubpasses = &subpass;

  VK_CHECK(vkCreateRenderPass(_device, &render_pass_info, nullptr, &_renderPass));
}

void VulkanEngine::init_framebuffers()
{
  VkFramebufferCreateInfo fb_info = {};
  fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fb_info.pNext = nullptr;

  fb_info.renderPass = _renderPass;
  fb_info.attachmentCount = 1;
  fb_info.width = _windowExtent.width;
  fb_info.height = _windowExtent.height;
  fb_info.layers = 1;

  // grab all swap chain images
  const uint32_t swapchain_image_count = static_cast<uint32_t>(_swapchainImageViews.size());
  _framebuffers = std::vector<VkFramebuffer>(swapchain_image_count);

  // framebuffer per swapchain image view
  for(int i = 0; i < swapchain_image_count; ++i)
  {
    fb_info.pAttachments = &_swapchainImageViews[i];
    VK_CHECK(vkCreateFramebuffer(_device, &fb_info, nullptr, &_framebuffers[i]));
  }
}

void VulkanEngine::cleanup()
{
  if (_isInitialized)
  {
    vkDestroyCommandPool(_device, _commandPool, nullptr);
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    vkDestroyRenderPass(_device, _renderPass, nullptr);
    // swap chain resources
    for(int i = 0; i < _framebuffers.size(); i++)
    {
      vkDestroyFramebuffer(_device, _framebuffers[i], nullptr);
      vkDestroyImageView(_device, _swapchainImageViews[i], nullptr);
    }
    vkDestroyDevice(_device, nullptr);
    vkDestroySurfaceKHR(_instance, _surface, nullptr);
    vkb::destroy_debug_utils_messenger(_instance, _debug_messenger);
    vkDestroyInstance(_instance, nullptr);
    SDL_DestroyWindow(_window);
  }
  // clear engine pointer
  loadedEngine = nullptr;
}

void VulkanEngine::draw()
{
  // nothing yet
}

void VulkanEngine::run()
{
  SDL_Event e;
  bool bQuit = false;

  // main loop
  while (!bQuit)
  {
    // Handle events on queue
    while (SDL_PollEvent(&e) != 0)
    {
      // close the window when user alt-f4s or clicks the X button
      if (e.type == SDL_QUIT)
        bQuit = true;

      if (e.type == SDL_WINDOWEVENT)
      {
        if (e.window.event == SDL_WINDOWEVENT_MINIMIZED)
        {
          stop_rendering = true;
        }
        if (e.window.event == SDL_WINDOWEVENT_RESTORED)
        {
          stop_rendering = false;
        }
      }
    }

    // do not draw if we are minimized
    if (stop_rendering)
    {
      // throttle the speed to avoid the endless spinning
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    draw();
  }
}