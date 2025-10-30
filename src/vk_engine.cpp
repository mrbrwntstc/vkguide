//> includes
#include "vk_engine.h"

#include "VkBootstrap.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>

#include <chrono>
#include <thread>
#include <fstream>
#include <iostream>

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
  init_sync_structures();
  init_pipelines();

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

  _mainDeletionQueue.push_function([=]() {
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);
  });
}

void VulkanEngine::init_commands()
{
  VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(_graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
  VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_commandPool));

  VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_commandPool, 1);
  VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_mainCommandBuffer));

  _mainDeletionQueue.push_function([=]() {
    vkDestroyCommandPool(_device, _commandPool, nullptr);
  });
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

  _mainDeletionQueue.push_function([=]() {
    vkDestroyRenderPass(_device, _renderPass, nullptr);
  });
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

    _mainDeletionQueue.push_function([=]() {
      vkDestroyFramebuffer(_device, _framebuffers[i], nullptr);
      vkDestroyImageView(_device, _swapchainImageViews[i], nullptr);
    });
  }
}

void VulkanEngine::init_sync_structures()
{
  VkFenceCreateInfo fence_info = {};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_info.pNext = nullptr;
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VK_CHECK(vkCreateFence(_device, &fence_info, nullptr, &_renderFence));

  _mainDeletionQueue.push_function([=]() {
    vkDestroyFence(_device, _renderFence, nullptr);
  });

  VkSemaphoreCreateInfo semaphore_info = {};
  semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphore_info.pNext = nullptr;
  semaphore_info.flags = 0;

  VK_CHECK(vkCreateSemaphore(_device, &semaphore_info, nullptr, &_presentSemaphore));
  VK_CHECK(vkCreateSemaphore(_device, &semaphore_info, nullptr, &_renderSemaphore));

  _mainDeletionQueue.push_function([=]() {
    vkDestroySemaphore(_device, _presentSemaphore, nullptr);
    vkDestroySemaphore(_device, _renderSemaphore, nullptr);
  });
}

void VulkanEngine::init_pipelines()
{
  // colored triangle
  // ---
  VkShaderModule triangle_fragment_shader;
  if(!load_shader_module("shaders/colored_triangle.frag.spv", &triangle_fragment_shader))
    printf("Failed to load triangle fragment shader\n");
  else
    printf("Successfully loaded triangle fragment shader\n");
  
  VkShaderModule triangle_vertex_shader;
  if(!load_shader_module("shaders/colored_triangle.vert.spv", &triangle_vertex_shader))
    printf("Failed to load triangle vertex shader\n");
  else
    printf("Successfully loaded triangle vertex shader\n");
  // ---

  // red triangle
  // ---
	VkShaderModule redTriangleFragShader;
	if (!load_shader_module("shaders/triangle.frag.spv", &redTriangleFragShader))
	{
		std::cout << "Error when building the triangle fragment shader module" << std::endl;
	}
	else {
		std::cout << "Red Triangle fragment shader successfully loaded" << std::endl;
	}

	VkShaderModule redTriangleVertShader;
	if (!load_shader_module("shaders/triangle.vert.spv", &redTriangleVertShader))
	{
		std::cout << "Error when building the triangle vertex shader module" << std::endl;
	}
	else {
		std::cout << "Red Triangle vertex shader successfully loaded" << std::endl;
	}
  // ---
  
  // pipeline layout
  VkPipelineLayoutCreateInfo pipeline_layout_info = vkinit::pipeline_layout_create_info();
  VK_CHECK(vkCreatePipelineLayout(_device, &pipeline_layout_info, nullptr, &_trianglePipelineLayout));

  // pipeline builder
  PipelineBuilder pipeline_builder;
  // shader stages
  pipeline_builder._shaderStages.push_back(vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_VERTEX_BIT, triangle_vertex_shader));
  pipeline_builder._shaderStages.push_back(vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_FRAGMENT_BIT, triangle_fragment_shader));
  // vertex input state
  pipeline_builder._vertexInputInfo = vkinit::vertex_input_state_create_info();
  // input assembly
  pipeline_builder._inputAssembly = vkinit::input_assembly_create_info(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  // viewport and scissor
  pipeline_builder._viewport.x = 0.0f;
  pipeline_builder._viewport.y = 0.0f;
  pipeline_builder._viewport.width = static_cast<float>(_windowExtent.width);
  pipeline_builder._viewport.height = static_cast<float>(_windowExtent.height);
  pipeline_builder._viewport.minDepth = 0.0f;
  pipeline_builder._viewport.maxDepth = 1.0f;

  pipeline_builder._scissor.offset = {0, 0};
  pipeline_builder._scissor.extent = _windowExtent;
  // rasterizer
  pipeline_builder._rasterizer = vkinit::rasterization_state_create_info(VK_POLYGON_MODE_FILL);
  // multisampling
  pipeline_builder._multisampling = vkinit::multisampling_state_create_info();
  // color blending
  pipeline_builder._colorBlendAttachment = vkinit::color_blend_attachment_state();
  // pipeline layout
  pipeline_builder._pipelineLayout = _trianglePipelineLayout;
  // build the pipeline
  _trianglePipeline = pipeline_builder.build_pipeline(_device, _renderPass);

  // red triangle pipeline
  pipeline_builder._shaderStages.clear();
  pipeline_builder._shaderStages.push_back(vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_VERTEX_BIT, redTriangleVertShader));
  pipeline_builder._shaderStages.push_back(vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_FRAGMENT_BIT, redTriangleFragShader));
  _redTrianglePipeline = pipeline_builder.build_pipeline(_device, _renderPass);

  // cleanup shader modules
  vkDestroyShaderModule(_device, triangle_vertex_shader, nullptr);
  vkDestroyShaderModule(_device, triangle_fragment_shader, nullptr);
  vkDestroyShaderModule(_device, redTriangleVertShader, nullptr);
  vkDestroyShaderModule(_device, redTriangleFragShader, nullptr);

  _mainDeletionQueue.push_function([=]() {
    vkDestroyPipeline(_device, _trianglePipeline, nullptr);
    vkDestroyPipeline(_device, _redTrianglePipeline, nullptr);
    vkDestroyPipelineLayout(_device, _trianglePipelineLayout, nullptr);
  });
}

bool VulkanEngine::load_shader_module(const char* filePath, VkShaderModule* outShaderModule)
{
  // load shader file into buffer variable
  std::ifstream file(filePath, std::ios::binary | std::ios::ate);
  if (!file.is_open())
  {
    printf("Failed to open shader file: %s\n", filePath);
    return false;
  }
  size_t file_size = (size_t)file.tellg();
  std::vector<uint32_t> buffer(file_size / sizeof(uint32_t));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(buffer.data()), file_size);
  file.close();

  // create shader module from buffer
  VkShaderModuleCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  create_info.pNext = nullptr;
  create_info.codeSize = buffer.size() * sizeof(uint32_t);
  create_info.pCode = buffer.data();
  VkShaderModule shader_module;
  if (vkCreateShaderModule(_device, &create_info, nullptr, &shader_module) != VK_SUCCESS)
  {
    printf("Failed to create shader module from file: %s\n", filePath);
    return false;
  }
  *outShaderModule = shader_module;
  return true;
}

void VulkanEngine::cleanup()
{
  if (_isInitialized)
  {
    vkWaitForFences(_device, 1, &_renderFence, VK_TRUE, 1000000000);
    _mainDeletionQueue.flush();

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
  // wait until GPU finishes rendering the last frame
  VK_CHECK(vkWaitForFences(_device, 1, &_renderFence, VK_TRUE, 1000000000));
  VK_CHECK(vkResetFences(_device, 1, &_renderFence));

  // request image from swap chain
  uint32_t swapchain_image_index;
  VK_CHECK(vkAcquireNextImageKHR(_device, _swapchain, 1000000000, _presentSemaphore, VK_NULL_HANDLE, &swapchain_image_index));

  // begin rendering commands
  VK_CHECK(vkResetCommandBuffer(_mainCommandBuffer, 0));
  VkCommandBuffer cmd = _mainCommandBuffer;

  // command buffer recording
  VkCommandBufferBeginInfo cmd_begin_info = {};
  cmd_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  cmd_begin_info.pNext = nullptr;

  cmd_begin_info.pInheritanceInfo = nullptr;
  cmd_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  VK_CHECK(vkBeginCommandBuffer(cmd, &cmd_begin_info));

  // clear color frame number that flashes every 120*pi frame period
  VkClearValue clear_value;
  float flash = abs(sin(_frameNumber / 120.0f));
  clear_value.color = { {0.0f, 0.0f, flash, 1.0f} };

  // main render pass
  VkRenderPassBeginInfo rp_begin_info = {};
  rp_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rp_begin_info.pNext = nullptr;

  rp_begin_info.renderPass = _renderPass;
  rp_begin_info.renderArea.offset.x = 0;
  rp_begin_info.renderArea.offset.y = 0;
  rp_begin_info.renderArea.extent = _windowExtent;
  rp_begin_info.framebuffer = _framebuffers[swapchain_image_index];

  // connect clear values
  rp_begin_info.clearValueCount = 1;
  rp_begin_info.pClearValues = &clear_value;

  // render loop
  // ---
  vkCmdBeginRenderPass(cmd, &rp_begin_info, VK_SUBPASS_CONTENTS_INLINE);

  if(_selected_shader == 0)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _trianglePipeline);
  else
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _redTrianglePipeline);
  vkCmdDraw(cmd, 3, 1, 0, 0);

  // finalize render pass
  vkCmdEndRenderPass(cmd);
  // finalize command buffer
  VK_CHECK(vkEndCommandBuffer(cmd));
  // --- render loop

  // send command buffer to the queue
  VkSubmitInfo submit = {};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.pNext = nullptr;

  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  submit.pWaitDstStageMask = &wait_stage;
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &_presentSemaphore;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &_renderSemaphore;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;

  VK_CHECK(vkQueueSubmit(_graphicsQueue, 1, &submit, _renderFence));

  // commands submitted; now present the image
  VkPresentInfoKHR present_info = {};
  present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present_info.pNext = nullptr;

  present_info.pSwapchains = &_swapchain;
  present_info.swapchainCount = 1;

  present_info.pWaitSemaphores = &_renderSemaphore;
  present_info.waitSemaphoreCount = 1;

  present_info.pImageIndices = &swapchain_image_index;

  VK_CHECK(vkQueuePresentKHR(_graphicsQueue, &present_info));

  // increase number of frames drawn
  _frameNumber++;
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
      else if(e.type == SDL_KEYDOWN)
        if(e.key.keysym.sym == SDLK_SPACE)
          _selected_shader = (_selected_shader + 1) % 2;
      
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

VkPipeline PipelineBuilder::build_pipeline(VkDevice device, VkRenderPass pass)
{
  VkPipelineViewportStateCreateInfo viewport_state = {};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = &_viewport;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = &_scissor;

  VkPipelineColorBlendStateCreateInfo color_blend = {};
  color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend.pNext = nullptr;
  color_blend.logicOpEnable = VK_FALSE;
  color_blend.logicOp = VK_LOGIC_OP_COPY;
  color_blend.attachmentCount = 1;
  color_blend.pAttachments = &_colorBlendAttachment;

  VkGraphicsPipelineCreateInfo pipeline_info = {};
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.pNext = nullptr;
  pipeline_info.stageCount = static_cast<uint32_t>(_shaderStages.size());
  pipeline_info.pStages = _shaderStages.data();
  pipeline_info.pVertexInputState = &_vertexInputInfo;
  pipeline_info.pInputAssemblyState = &_inputAssembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &_rasterizer;
  pipeline_info.pMultisampleState = &_multisampling;
  pipeline_info.pColorBlendState = &color_blend;
  pipeline_info.layout = _pipelineLayout;
  pipeline_info.renderPass = pass;
  pipeline_info.subpass = 0;
  pipeline_info.basePipelineHandle = VK_NULL_HANDLE;

  VkPipeline newPipeline;
  if(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &newPipeline) != VK_SUCCESS)
  {
    std::cout << "failed to create graphics pipeline" << std::endl;
    newPipeline = VK_NULL_HANDLE;
  }

  return newPipeline;
}
