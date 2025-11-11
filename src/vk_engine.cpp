//> includes
#include "vk_engine.h"

#include "VkBootstrap.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>
#include <vk_mesh.h>

#include <chrono>
#include <thread>
#include <fstream>
#include <iostream>

#include <glm/gtx/transform.hpp>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

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
  init_descriptors();
  init_pipelines();
  load_meshes();

  init_scene();

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

  // memory allocator
  VmaAllocatorCreateInfo allocatorInfo = {};
  allocatorInfo.physicalDevice = _chosenGPU;
  allocatorInfo.device = _device;
  allocatorInfo.instance = _instance;
  vmaCreateAllocator(&allocatorInfo, &_allocator);
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

  // depth image size will match window
  VkExtent3D depthImageExtent = {
    _windowExtent.width,
    _windowExtent.height,
    1
  };

  // hardcoded depth format
  _depthFormat = VK_FORMAT_D32_SFLOAT;
  VkImageCreateInfo dimg_info = vkinit::image_create_info(_depthFormat,
                                                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                                          depthImageExtent);
  
  VmaAllocationCreateInfo dimg_allocinfo = {};
  dimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
  dimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  // create image
  vmaCreateImage(_allocator,
                 &dimg_info,
                 &dimg_allocinfo,
                 &_depthImage._image,
                 &_depthImage._allocation,
                 nullptr);
  
  // create image view
  VkImageViewCreateInfo dview_info = vkinit::imageview_create_info(_depthFormat,
                                                                    _depthImage._image,
                                                                    VK_IMAGE_ASPECT_DEPTH_BIT);

  VK_CHECK(vkCreateImageView(_device, &dview_info, nullptr, &_depthImageView));

  _mainDeletionQueue.push_function([=]() {
    vkDestroyImageView(_device, _depthImageView, nullptr);
    vmaDestroyImage(_allocator, _depthImage._image, _depthImage._allocation);
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);
  });
}

void VulkanEngine::init_commands()
{
  VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(_graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
  for(int i = 0; i < FRAME_OVERLAP; ++i)
  {
    VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_frames[i]._commandPool));

    VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_frames[i]._commandPool, 1);
    VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_frames[i]._mainCommandBuffer));

    _mainDeletionQueue.push_function([=]() {
      vkDestroyCommandPool(_device, _frames[i]._commandPool, nullptr);
    });
  }
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

  VkAttachmentDescription depth_attachment = {};
  depth_attachment.flags = 0;
  depth_attachment.format = _depthFormat;
  depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depth_attachment_ref = {};
  depth_attachment_ref.attachment = 1;
  depth_attachment_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_attachment_ref;
  subpass.pDepthStencilAttachment = &depth_attachment_ref;

  VkSubpassDependency dependency = {};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.srcAccessMask = 0;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkSubpassDependency depth_dependency = {};
  depth_dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  depth_dependency.dstSubpass = 0;
  depth_dependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  depth_dependency.srcAccessMask = 0;
  depth_dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  depth_dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  VkAttachmentDescription attachments[2] = { colorAttachment, depth_attachment };

  VkSubpassDependency dependencies[2] = { dependency, depth_dependency };

  VkRenderPassCreateInfo render_pass_info = {};
  render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  // color and depth attachments
  render_pass_info.attachmentCount = 2;
  render_pass_info.pAttachments = &attachments[0];
  // subpass
  render_pass_info.subpassCount = 1;
  render_pass_info.pSubpasses = &subpass;
  // dependencies
  render_pass_info.dependencyCount = 2;
  render_pass_info.pDependencies = &dependencies[0];

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
    VkImageView attachments[2];
    attachments[0] = _swapchainImageViews[i];
    attachments[1] = _depthImageView;

    fb_info.pAttachments = attachments;
    fb_info.attachmentCount = 2;

    VK_CHECK(vkCreateFramebuffer(_device, &fb_info, nullptr, &_framebuffers[i]));

    _mainDeletionQueue.push_function([=]() {
      vkDestroyFramebuffer(_device, _framebuffers[i], nullptr);
      vkDestroyImageView(_device, _swapchainImageViews[i], nullptr);
    });
  }
}

void VulkanEngine::init_sync_structures()
{
  VkFenceCreateInfo fence_create_info = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
  VkSemaphoreCreateInfo semaphore_create_info = vkinit::semaphore_create_info();

  for(int i = 0; i < FRAME_OVERLAP; ++i)
  {
    VK_CHECK(vkCreateFence(_device, &fence_create_info, nullptr, &_frames[i]._renderFence));

    _mainDeletionQueue.push_function([=]() {
      vkDestroyFence(_device, _frames[i]._renderFence, nullptr);
    });

    VK_CHECK(vkCreateSemaphore(_device, &semaphore_create_info, nullptr, &_frames[i]._presentSemaphore));
    VK_CHECK(vkCreateSemaphore(_device, &semaphore_create_info, nullptr, &_frames[i]._renderSemaphore));

    _mainDeletionQueue.push_function([=]() {
      vkDestroySemaphore(_device, _frames[i]._presentSemaphore, nullptr);
      vkDestroySemaphore(_device, _frames[i]._renderSemaphore, nullptr);
    });
  }
}

void VulkanEngine::init_descriptors()
{
	//create a descriptor pool that will hold 10 uniform buffers
	std::vector<VkDescriptorPoolSize> sizes =
	{
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10 }
	};

	VkDescriptorPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = 0;
	pool_info.maxSets = 10;
	pool_info.poolSizeCount = (uint32_t)sizes.size();
	pool_info.pPoolSizes = sizes.data();

	vkCreateDescriptorPool(_device, &pool_info, nullptr, &_descriptorPool);

	//information about the binding.
	VkDescriptorSetLayoutBinding camBufferBinding = {};
	camBufferBinding.binding = 0;
	camBufferBinding.descriptorCount = 1;
	// it's a uniform buffer binding
	camBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

	// we use it from the vertex shader
	camBufferBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	VkDescriptorSetLayoutCreateInfo setinfo = {};
	setinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setinfo.pNext = nullptr;

	//we are going to have 1 binding
	setinfo.bindingCount = 1;
	//no flags
	setinfo.flags = 0;
	//point to the camera buffer binding
	setinfo.pBindings = &camBufferBinding;

	vkCreateDescriptorSetLayout(_device, &setinfo, nullptr, &_globalSetLayout);

	for (int i = 0; i < FRAME_OVERLAP; i++)
	{
		_frames[i].cameraBuffer = create_buffer(sizeof(GPUCameraData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

		//allocate one descriptor set for each frame
		VkDescriptorSetAllocateInfo allocInfo ={};
		allocInfo.pNext = nullptr;
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		//using the pool we just set
		allocInfo.descriptorPool = _descriptorPool;
		//only 1 descriptor
		allocInfo.descriptorSetCount = 1;
		//using the global data layout
		allocInfo.pSetLayouts = &_globalSetLayout;

		vkAllocateDescriptorSets(_device, &allocInfo, &_frames[i].globalDescriptor);

		//information about the buffer we want to point at in the descriptor
		VkDescriptorBufferInfo binfo;
		//it will be the camera buffer
		binfo.buffer = _frames[i].cameraBuffer._buffer;
		//at 0 offset
		binfo.offset = 0;
		//of the size of a camera data struct
		binfo.range = sizeof(GPUCameraData);

		VkWriteDescriptorSet setWrite = {};
		setWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		setWrite.pNext = nullptr;

		//we are going to write into binding number 0
		setWrite.dstBinding = 0;
		//of the global descriptor
		setWrite.dstSet = _frames[i].globalDescriptor;

		setWrite.descriptorCount = 1;
		//and the type is uniform buffer
		setWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		setWrite.pBufferInfo = &binfo;


		vkUpdateDescriptorSets(_device, 1, &setWrite, 0, nullptr);
	}

	_mainDeletionQueue.push_function([&]() {
		vkDestroyDescriptorSetLayout(_device, _globalSetLayout, nullptr);
    vkDestroyDescriptorPool(_device, _descriptorPool, nullptr);
    for (int i = 0; i < FRAME_OVERLAP; i++)
    {
      vmaDestroyBuffer(_allocator, _frames[i].cameraBuffer._buffer, _frames[i].cameraBuffer._allocation);
    }
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

  // mesh triangle
  // ---
  VkShaderModule meshVertexShader;
  if (!load_shader_module("shaders/tri_mesh.vert.spv", &meshVertexShader))
    std::cout << "Error when building the mesh vertex shader module" << std::endl;
  else
    std::cout << "Mesh vertex shader successfully loaded" << std::endl;
  // ---


  // pipeline builder
  PipelineBuilder pipeline_builder;
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
  // depth stencil
  pipeline_builder._depthStencil = vkinit::depth_stencil_create_info(true, true, VK_COMPARE_OP_LESS_OR_EQUAL);

  // pipeline layout
  // ---
  VkPipelineLayoutCreateInfo default_pipeline_layout_info = vkinit::pipeline_layout_create_info();
  VkPipelineLayout default_pipeline_layout;
  // set push constants
  VkPushConstantRange push_constant;
  push_constant.offset = 0;
  push_constant.size = sizeof(MeshPushConstants);
  push_constant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  default_pipeline_layout_info.pPushConstantRanges = &push_constant;
  default_pipeline_layout_info.pushConstantRangeCount = 1;
  default_pipeline_layout_info.setLayoutCount = 1;
  default_pipeline_layout_info.pSetLayouts = &_globalSetLayout;
  VK_CHECK(vkCreatePipelineLayout(_device, &default_pipeline_layout_info, nullptr, &default_pipeline_layout));
  // --- pipeline layout

  VertexInputDescription vertexDescription = Vertex::get_vertex_description();
  
  pipeline_builder._vertexInputInfo.pVertexAttributeDescriptions = vertexDescription.attributes.data();
  pipeline_builder._vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexDescription.attributes.size());

  pipeline_builder._vertexInputInfo.pVertexBindingDescriptions = vertexDescription.bindings.data();
  pipeline_builder._vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexDescription.bindings.size());

  pipeline_builder._shaderStages.push_back(vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_VERTEX_BIT, meshVertexShader));
  pipeline_builder._shaderStages.push_back(vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_FRAGMENT_BIT, triangle_fragment_shader));
  pipeline_builder._pipelineLayout = default_pipeline_layout;
  VkPipeline default_pipeline;
  default_pipeline = pipeline_builder.build_pipeline(_device, _renderPass);

  create_material(default_pipeline, default_pipeline_layout, "defaultmesh");

  // cleanup shader modules
  vkDestroyShaderModule(_device, meshVertexShader, nullptr);
  vkDestroyShaderModule(_device, triangle_vertex_shader, nullptr);
  vkDestroyShaderModule(_device, triangle_fragment_shader, nullptr);
  vkDestroyShaderModule(_device, redTriangleVertShader, nullptr);
  vkDestroyShaderModule(_device, redTriangleFragShader, nullptr);

  _mainDeletionQueue.push_function([=]() {
    vkDestroyPipeline(_device, default_pipeline, nullptr);
    vkDestroyPipelineLayout(_device, default_pipeline_layout, nullptr);
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

void VulkanEngine::load_meshes()
{
  // make the array 3 vertices long
  Mesh _triangleMesh;
  _triangleMesh._vertices.resize(3);

  // vertex positions
  _triangleMesh._vertices[0].position = {1.f, 1.f, 0.5f};
  _triangleMesh._vertices[1].position = {-1.f, 1.f, 0.5f};
  _triangleMesh._vertices[2].position = {0.f, -1.f, 0.5f};

  // vertex colors, all green
  _triangleMesh._vertices[0].color = {0.f, 1.f, 0.f};
  _triangleMesh._vertices[1].color = {0.f, 1.f, 0.f};
  _triangleMesh._vertices[2].color = {0.f, 1.f, 0.f};

  // no normals yet

  // load the monkey
  Mesh _monkeyMesh;
  _monkeyMesh.load_from_obj("assets/monkey_smooth.obj");

  upload_mesh(_triangleMesh);
  upload_mesh(_monkeyMesh);

  _meshes["triangle"] = _triangleMesh;
  _meshes["monkey"] = _monkeyMesh;
}

void VulkanEngine::upload_mesh(Mesh& mesh)
{
  VkBufferCreateInfo buffer_info = {};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = mesh._vertices.size() * sizeof(Vertex);
  buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  
  // writeable by CPU, readable by GPU
  VmaAllocationCreateInfo vma_alloc_info = {};
  vma_alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
  vma_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

  // allocate the buffer
  VK_CHECK(vmaCreateBuffer(_allocator, &buffer_info, &vma_alloc_info, 
    &mesh._vertexBuffer._buffer, 
    &mesh._vertexBuffer._allocation, 
    nullptr));

  // add cleanup to destruction queue
  _mainDeletionQueue.push_function([=]() {
    vmaDestroyBuffer(_allocator, mesh._vertexBuffer._buffer, mesh._vertexBuffer._allocation);
  });

  // copy vertex data
  void* data;
  vmaMapMemory(_allocator, mesh._vertexBuffer._allocation, &data);
  memcpy(data, mesh._vertices.data(), mesh._vertices.size() * sizeof(Vertex));
  vmaUnmapMemory(_allocator, mesh._vertexBuffer._allocation);
}

void VulkanEngine::cleanup()
{
  if (_isInitialized)
  {
    vkDeviceWaitIdle(_device);
    vkWaitForFences(_device, 1, &get_current_frame()._renderFence, VK_TRUE, 1000000000);
    _mainDeletionQueue.flush();

    vmaDestroyAllocator(_allocator);
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
  VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame()._renderFence, VK_TRUE, 1000000000));
  VK_CHECK(vkResetFences(_device, 1, &get_current_frame()._renderFence));

  // request image from swap chain
  uint32_t swapchain_image_index;
  VK_CHECK(vkAcquireNextImageKHR(_device, _swapchain, 1000000000, get_current_frame()._presentSemaphore, VK_NULL_HANDLE, &swapchain_image_index));

  // begin rendering commands
  VK_CHECK(vkResetCommandBuffer(get_current_frame()._mainCommandBuffer, 0));
  VkCommandBuffer cmd = get_current_frame()._mainCommandBuffer;

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

  VkClearValue depth_clear;
  depth_clear.depthStencil.depth = 1.0f;

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
  rp_begin_info.clearValueCount = 2;
  VkClearValue clear_values[2] = { clear_value, depth_clear };
  rp_begin_info.pClearValues = &clear_values[0];

  // render loop
  // ---
  vkCmdBeginRenderPass(cmd, &rp_begin_info, VK_SUBPASS_CONTENTS_INLINE);

  std::sort(_renderables.begin(), _renderables.end(), [](const RenderObject &a, const RenderObject &b) {
    if(a.material->pipeline == b.material->pipeline)
      return a.mesh < b.mesh;
    return a.material->pipeline < b.material->pipeline;
  });

  draw_objects(cmd, _renderables.data(), static_cast<int>(_renderables.size()));
  
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
  submit.pWaitSemaphores = &get_current_frame()._presentSemaphore;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &get_current_frame()._renderSemaphore;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;

  VK_CHECK(vkQueueSubmit(_graphicsQueue, 1, &submit, get_current_frame()._renderFence));

  // commands submitted; now present the image
  VkPresentInfoKHR present_info = {};
  present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present_info.pNext = nullptr;

  present_info.pSwapchains = &_swapchain;
  present_info.swapchainCount = 1;

  present_info.pWaitSemaphores = &get_current_frame()._renderSemaphore;
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

      float cam_speed = 5.f;
      if(e.type == SDL_KEYDOWN)
      {
        switch(e.key.keysym.sym)
        {
          case SDLK_w:
            _camera.velocity_forward = cam_speed;
            break;
          case SDLK_s:
            _camera.velocity_forward = -cam_speed;
            break;
          case SDLK_a:
            _camera.velocity_right = -cam_speed;
            break;
          case SDLK_d:
            _camera.velocity_right = cam_speed;
            break;
          case SDLK_q:
            _camera.velocity_up = -cam_speed;
            break;
          case SDLK_e:
            _camera.velocity_up = cam_speed;
            break;
          default:
            break;
        }
      }

      if(e.type == SDL_KEYUP)
      {
        switch(e.key.keysym.sym)
        {
          case SDLK_w:
          case SDLK_s:
            if(_camera.velocity_forward != 0.f)
              _camera.velocity_forward = 0.f;
            break;
          case SDLK_a:
          case SDLK_d:
            if(_camera.velocity_right != 0.f)
              _camera.velocity_right = 0.f;
            break;
          case SDLK_q:
          case SDLK_e:
            if(_camera.velocity_up != 0.f)
              _camera.velocity_up = 0.f;
          default:
            break;
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

Material* VulkanEngine::create_material(VkPipeline pipeline, VkPipelineLayout layout, const std::string& name)
{
  Material mat;
  mat.pipeline = pipeline;
  mat.pipelineLayout = layout;
  _materials[name] = mat;
  return &_materials[name];
}

Material* VulkanEngine::get_material(const std::string& name)
{
  auto it = _materials.find(name);
  if (it != _materials.end())
    return &it->second;
  else
    return nullptr;
}

Mesh* VulkanEngine::get_mesh(const std::string& name)
{
  auto it = _meshes.find(name);
  if (it != _meshes.end())
    return &it->second;
  else
    return nullptr;
}

void VulkanEngine::init_scene()
{
  RenderObject monkey;
  monkey.mesh = get_mesh("monkey");
  monkey.material = get_material("defaultmesh");
  monkey.transformMatrix = glm::mat4{ 1.f };
  _renderables.push_back(monkey);

  for(int x = -20; x <= 20; x ++)
  {
    for(int y = -20; y <= 20; y ++)
    {
      RenderObject tri;
      tri.mesh = get_mesh("triangle");
      tri.material = get_material("defaultmesh");
      glm::mat4 translation = glm::translate(glm::mat4{ 1.f }, glm::vec3{ (float)x, 0.f, (float)y });
      glm::mat4 scale = glm::scale(glm::mat4{ 1.f }, glm::vec3{ 0.2f, 0.2f, 0.2f });
      tri.transformMatrix = translation * scale;
      _renderables.push_back(tri);
    }
  }
}

void VulkanEngine::draw_objects(VkCommandBuffer cmd, RenderObject* first, int count)
{
  _camera.position += _camera.forward * _camera.velocity_forward;
  _camera.position += glm::normalize(glm::cross(_camera.forward, _camera.up)) * _camera.velocity_right;
  _camera.position += _camera.up * _camera.velocity_up;
  glm::mat4 view = glm::lookAt(_camera.position, _camera.position + _camera.forward, _camera.up);
  // camera projection
  glm::mat4 projection = glm::perspective(glm::radians(70.f), _windowExtent.width / static_cast<float>(_windowExtent.height), 0.1f, 200.f);
  projection[1][1] *= -1; // flip Y for vulkan

	//fill a GPU camera data struct
	GPUCameraData camData;
	camData.proj = projection;
	camData.view = view;
	camData.viewproj = projection * view;

	//and copy it to the buffer
	void* data;
	vmaMapMemory(_allocator, get_current_frame().cameraBuffer._allocation, &data);

	memcpy(data, &camData, sizeof(GPUCameraData));

	vmaUnmapMemory(_allocator, get_current_frame().cameraBuffer._allocation);

  Mesh* last_mesh = nullptr;
  Material* last_material = nullptr;
  for(int i = 0; i < count; ++i)
  {
    RenderObject& object = first[i];
    // only bind the pipeline if it has changed since the last object
    if(object.material != last_material)
    {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, object.material->pipeline);
      last_material = object.material;
      //bind the descriptor set when changing pipelines
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, object.material->pipelineLayout, 0, 1, &get_current_frame().globalDescriptor, 0, nullptr);
    }
    glm::mat4 model = object.transformMatrix;
    glm::mat4 mesh_matrix = projection * view * model;

    // set push constants
    MeshPushConstants constants;
    constants.render_matrix = object.transformMatrix;

    //upload the mesh to the GPU via push constants
    vkCmdPushConstants(cmd, object.material->pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(MeshPushConstants), &constants);

    // only bind mesh if it has changed since the last object
    if(object.mesh != last_mesh)
    {
      VkDeviceSize offset = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &object.mesh->_vertexBuffer._buffer, &offset);
      last_mesh = object.mesh;
    }

    // draw
    vkCmdDraw(cmd, object.mesh->_vertices.size(), 1, 0, 0);
  }
}

AllocatedBuffer VulkanEngine::create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
	//allocate vertex buffer
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.pNext = nullptr;

	bufferInfo.size = allocSize;
	bufferInfo.usage = usage;


	VmaAllocationCreateInfo vmaallocInfo = {};
	vmaallocInfo.usage = memoryUsage;

	AllocatedBuffer newBuffer;

	//allocate the buffer
	VK_CHECK(vmaCreateBuffer(_allocator, &bufferInfo, &vmaallocInfo,
		&newBuffer._buffer,
		&newBuffer._allocation,
		nullptr));

	return newBuffer;
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
  pipeline_info.pDepthStencilState = &_depthStencil;

  VkPipeline newPipeline;
  if(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &newPipeline) != VK_SUCCESS)
  {
    std::cout << "failed to create graphics pipeline" << std::endl;
    newPipeline = VK_NULL_HANDLE;
  }

  return newPipeline;
}
