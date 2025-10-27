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
  init_sync_structures();

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
}

void VulkanEngine::init_swapchain()
{
}

void VulkanEngine::init_commands()
{
}

void VulkanEngine::init_sync_structures()
{
}

void VulkanEngine::cleanup()
{
  if (_isInitialized)
  {

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