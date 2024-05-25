#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <xdg-shell-protocol.h>

#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_PROTOTYPES
#include <vulkan/vulkan.h>

#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX_NUM_IMAGES 4

struct window_buffer {
    VkImage image;
    VkImageView view;
    VkFramebuffer framebuffer;
    VkFence cmd_fence;
    VkCommandBuffer cmd_buffer;
};

struct buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    void *map;
};

struct vk {
    VkSwapchainKHR swap_chain;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkDevice device;
    VkRenderPass render_pass;
    VkQueue queue;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkDescriptorSetLayout desc_set_layout;
    VkDescriptorSet desc_set;
    VkSemaphore image_semaphore;
    VkSemaphore render_semaphore;
    VkCommandPool cmd_pool;
    VkSurfaceKHR surface;
    VkFormat image_format;
    uint32_t image_count;
    struct buffer vert_buffer, uniform_buffer;
    VkDescriptorPool desc_pool;
    struct window_buffer win_buffers[MAX_NUM_IMAGES];
};

struct window {
    struct display *display;
    struct wl_surface *wl_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    int width, height;
    bool wait_for_configure;
    struct vk vk;
};

struct display {
    struct wl_display *wl_display;
    struct wl_registry *wl_registory;
    struct wl_compositor *wl_compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct window window;
};

static uint32_t vs_spirv_source[] = {
#include "triangle.vert.spv"
};

static uint32_t fs_spirv_source[] = {
#include "triangle.frag.spv"
};

static void nop() {}

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *shell,
                             uint32_t serial) {
    xdg_wm_base_pong(shell, serial);
}
static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    xdg_wm_base_ping,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
    struct display *display = data;

    if (strcmp(interface, "wl_compositor") == 0) {
        display->wl_compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        display->xdg_wm_base =
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(display->xdg_wm_base, &xdg_wm_base_listener,
                                 display);
    }
}
static const struct wl_registry_listener wl_registry_listener = {
    registry_handle_global,
    nop,
};

static void xdg_surface_handle_configure(void *data,
                                         struct xdg_surface *xdg_surface,
                                         uint32_t serial) {
    struct window *window = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    window->wait_for_configure = false;
}
static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_handle_configure,
};

struct buffer create_buffer(struct vk *vk, VkDeviceSize size,
                            VkBufferUsageFlags usage_flags,
                            VkMemoryPropertyFlagBits properties, bool map) {
    struct buffer buffer;

    buffer.size = size;
    vkCreateBuffer(vk->device,
                   &(VkBufferCreateInfo){
                       .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                       .size = size,
                       .usage = usage_flags,
                   },
                   NULL, &buffer.buffer);
    VkMemoryRequirements reqs;
    vkGetBufferMemoryRequirements(vk->device, buffer.buffer, &reqs);

    unsigned int mem_type = UINT_MAX;
    for (unsigned int i = 0; i < vk->memory_properties.memoryTypeCount; i++) {
        // iterate over possible memory types for the buffer
        if (!(reqs.memoryTypeBits & (1u << i)))
            continue;
        // search for memory type which matches the demanded propeties
        if (vk->memory_properties.memoryTypes[i].propertyFlags & properties) {
            mem_type = i;
            break;
        }
    }
    assert(mem_type != UINT_MAX);

    vkAllocateMemory(vk->device,
                     &(VkMemoryAllocateInfo){
                         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                         .allocationSize = MAX(size, reqs.size),
                         .memoryTypeIndex = mem_type,
                     },
                     NULL, &buffer.memory);

    vkBindBufferMemory(vk->device, buffer.buffer, buffer.memory, 0);

    if (map)
        vkMapMemory(vk->device, buffer.memory, 0, size, 0, &buffer.map);

    return buffer;
}

static void init_vulkan(struct window *window) {
    uint32_t count;

    struct vk *vk = &window->vk;
    vkCreateInstance(
        &(VkInstanceCreateInfo){
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo =
                &(VkApplicationInfo){
                    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                    .pApplicationName = "window",
                    .apiVersion = VK_MAKE_VERSION(1, 1, 0),
                },
            .enabledExtensionCount = 2,
            .ppEnabledExtensionNames =
                (const char *[]){
                    VK_KHR_SURFACE_EXTENSION_NAME,
                    VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
                },
            .enabledLayerCount = 1,
            .ppEnabledLayerNames =
                (const char *[]){
                    "VK_LAYER_KHRONOS_validation",
                },
        },
        NULL, &vk->instance);

    vkEnumeratePhysicalDevices(vk->instance, &count, NULL);
    assert(count);
    VkPhysicalDevice physical_devices[count];
    vkEnumeratePhysicalDevices(vk->instance, &(uint32_t){1}, physical_devices);
    vk->physical_device = physical_devices[0];

    vkGetPhysicalDeviceMemoryProperties(vk->physical_device,
                                        &vk->memory_properties);

    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical_device, &count, NULL);
    assert(count);
    VkQueueFamilyProperties props[count];
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical_device, &count,
                                             props);
    assert(props[0].queueFlags & VK_QUEUE_GRAPHICS_BIT);

    vkCreateDevice(
        vk->physical_device,
        &(VkDeviceCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos =
                &(VkDeviceQueueCreateInfo){
                    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                    .queueFamilyIndex = 0,
                    .queueCount = 1,
                    .pQueuePriorities = (float[]){1.0f},
                },
            .enabledExtensionCount = 1,
            .ppEnabledExtensionNames =
                (const char *const[]){
                    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                },
        },
        NULL, &vk->device);

    vkGetDeviceQueue(vk->device, 0, 0, &vk->queue);

    PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR
        get_wayland_presentation_support =
            (PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)
                vkGetInstanceProcAddr(
                    vk->instance,
                    "vkGetPhysicalDeviceWaylandPresentationSupportKHR");
    assert(get_wayland_presentation_support(vk->physical_device, 0,
                                            window->display->wl_display));

    PFN_vkCreateWaylandSurfaceKHR create_wayland_surface =
        (PFN_vkCreateWaylandSurfaceKHR)vkGetInstanceProcAddr(
            vk->instance, "vkCreateWaylandSurfaceKHR");

    create_wayland_surface(
        vk->instance,
        &(VkWaylandSurfaceCreateInfoKHR){
            .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
            .display = window->display->wl_display,
            .surface = window->wl_surface,
        },
        NULL, &vk->surface);

    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical_device, vk->surface,
                                         &count, NULL);
    VkSurfaceFormatKHR formats[count];
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical_device, vk->surface,
                                         &count, formats);
    for (int i = 0; i < (int)count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            vk->image_format = formats[i].format;
            break;
        }
    }
    assert(vk->image_format);

    vkCreateRenderPass(
        vk->device,
        &(VkRenderPassCreateInfo){
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = (VkAttachmentDescription[]){{
                .format = vk->image_format,
                .samples = 1,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            }},
            .subpassCount = 1,
            .pSubpasses = (VkSubpassDescription[]){{
                .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                .colorAttachmentCount = 1,
                .pColorAttachments =
                    (VkAttachmentReference[]){
                        {.attachment = 0,
                         .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}},
                .pResolveAttachments =
                    (VkAttachmentReference[]){
                        {.attachment = VK_ATTACHMENT_UNUSED,
                         .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}},
            }}},
        NULL, &vk->render_pass);

    vkCreateDescriptorSetLayout(
        vk->device,
        &(VkDescriptorSetLayoutCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = (VkDescriptorSetLayoutBinding[]){{
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            }}},
        NULL, &vk->desc_set_layout);

    vkCreatePipelineLayout(
        vk->device,
        &(VkPipelineLayoutCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &vk->desc_set_layout,
        },
        NULL, &vk->pipeline_layout);

    VkPipelineVertexInputStateCreateInfo vi_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]){{
            .binding = 0,
            .stride = (3 + 3) * sizeof(float), // 3D + RGB
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        }},
        .vertexAttributeDescriptionCount = 2,
        .pVertexAttributeDescriptions =
            (VkVertexInputAttributeDescription[]){
                {
                    .location = 0,
                    .binding = 0,
                    .format = VK_FORMAT_R32G32B32_SFLOAT,
                    .offset = 0,
                },
                {
                    .location = 1,
                    .binding = 0,
                    .format = VK_FORMAT_R32G32B32_SFLOAT,
                    .offset = 3 * sizeof(float),
                },
            },
    };

    VkShaderModule vs_module;
    vkCreateShaderModule(
        vk->device,
        &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(vs_spirv_source),
            .pCode = (uint32_t *)vs_spirv_source,
        },
        NULL, &vs_module);

    VkShaderModule fs_module;
    vkCreateShaderModule(
        vk->device,
        &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(fs_spirv_source),
            .pCode = (uint32_t *)fs_spirv_source,
        },
        NULL, &fs_module);

    vkCreateGraphicsPipelines(
        vk->device, (VkPipelineCache){VK_NULL_HANDLE}, 1,
        &(VkGraphicsPipelineCreateInfo){
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2,
            .pStages =
                (VkPipelineShaderStageCreateInfo[]){
                    {
                        .sType =
                            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_VERTEX_BIT,
                        .module = vs_module,
                        .pName = "main",
                    },
                    {
                        .sType =
                            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                        .module = fs_module,
                        .pName = "main",
                    },
                },
            .pVertexInputState = &vi_create_info,
            .pInputAssemblyState =
                &(VkPipelineInputAssemblyStateCreateInfo){
                    .sType =
                        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                    .primitiveRestartEnable = false,
                },

            .pViewportState =
                &(VkPipelineViewportStateCreateInfo){
                    .sType =
                        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                    .viewportCount = 1,
                    .scissorCount = 1,
                },

            .pRasterizationState =
                &(VkPipelineRasterizationStateCreateInfo){
                    .sType =
                        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                    .polygonMode = VK_POLYGON_MODE_FILL,
                    .cullMode = VK_CULL_MODE_NONE,
                    .frontFace = VK_FRONT_FACE_CLOCKWISE,
                    .depthBiasEnable = VK_FALSE,
                    .depthClampEnable = VK_FALSE,
                    .lineWidth = 1.0f,
                },

            .pMultisampleState =
                &(VkPipelineMultisampleStateCreateInfo){
                    .sType =
                        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                    .rasterizationSamples = 1,
                },
            .pDepthStencilState =
                &(VkPipelineDepthStencilStateCreateInfo){
                    .sType =
                        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                },

            .pColorBlendState =
                &(VkPipelineColorBlendStateCreateInfo){
                    .sType =
                        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                    .attachmentCount = 1,
                    .pAttachments =
                        (VkPipelineColorBlendAttachmentState[]){
                            {.colorWriteMask = VK_COLOR_COMPONENT_A_BIT |
                                               VK_COLOR_COMPONENT_R_BIT |
                                               VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT},
                        }},

            .pDynamicState =
                &(VkPipelineDynamicStateCreateInfo){
                    .sType =
                        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                    .dynamicStateCount = 2,
                    .pDynamicStates =
                        (VkDynamicState[]){
                            VK_DYNAMIC_STATE_VIEWPORT,
                            VK_DYNAMIC_STATE_SCISSOR,
                        },
                },

            .layout = vk->pipeline_layout,
            .renderPass = vk->render_pass,
            .subpass = 0,
        },
        NULL, &vk->pipeline);

    // clang-format off
	static const float vVertices[] = {
        //  X      Y     Z      R      G      B
		 0.0f, -0.5f,  0.0,  1.0f,  0.0f,  0.0f,
		-0.5f,  0.5f,  0.0,  0.0f,  1.0f,  0.0f,
		 0.5f,  0.5f,  0.0,  0.0f,  0.0f,  1.0f,
	};
    // clang-format on

    vk->vert_buffer =
        create_buffer(vk, sizeof(vVertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      true);
    memcpy(vk->vert_buffer.map, vVertices, sizeof(vVertices));
    vkUnmapMemory(vk->device, vk->vert_buffer.memory);
    vk->vert_buffer.map = NULL;

    vk->uniform_buffer =
        create_buffer(vk, sizeof(float[16]), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      true);

    vkCreateDescriptorPool(
        vk->device,
        &(VkDescriptorPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .pNext = NULL,
            .flags = 0,
            .maxSets = 1,
            .poolSizeCount = 1,
            .pPoolSizes = (VkDescriptorPoolSize[]){{
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
            }},
        },
        NULL, &vk->desc_pool);

    vkAllocateDescriptorSets(
        vk->device,
        &(VkDescriptorSetAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = vk->desc_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &vk->desc_set_layout,
        },
        &vk->desc_set);

    vkUpdateDescriptorSets(
        vk->device, 1,
        (VkWriteDescriptorSet[]){{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = vk->desc_set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo =
                &(VkDescriptorBufferInfo){
                    .buffer = vk->uniform_buffer.buffer,
                    .offset = 0,
                    .range = sizeof(float[16]),
                },
        }},
        0, NULL);

    vkCreateCommandPool(
        vk->device,
        &(const VkCommandPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = 0,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        },
        NULL, &vk->cmd_pool);
}

static void create_swapchain(struct window *window) {
    struct vk *vk = &window->vk;

    VkBool32 surface_supported;
    vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device, 0, vk->surface,
                                         &surface_supported);
    assert(surface_supported);

    VkSurfaceCapabilitiesKHR surface_caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physical_device, vk->surface,
                                              &surface_caps);
    assert(surface_caps.supportedCompositeAlpha &
           VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
    assert(surface_caps.minImageCount >= 2 &&
           surface_caps.minImageCount <= MAX_NUM_IMAGES);

    vkCreateSwapchainKHR(
        vk->device,
        &(VkSwapchainCreateInfoKHR){
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .flags = 0,
            .surface = vk->surface,
            .minImageCount = surface_caps.minImageCount,
            .imageFormat = vk->image_format,
            .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            .imageExtent = {window->width, window->height},
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = (uint32_t[]){0},
            .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        },
        NULL, &vk->swap_chain);

    vkGetSwapchainImagesKHR(vk->device, vk->swap_chain, &vk->image_count, NULL);
    assert(vk->image_count > 0 && vk->image_count <= MAX_NUM_IMAGES);

    VkImage swap_chain_images[vk->image_count];
    vkGetSwapchainImagesKHR(vk->device, vk->swap_chain, &vk->image_count,
                            swap_chain_images);

    for (uint32_t i = 0; i < vk->image_count; i++) {
        struct window_buffer *win_buffer = &vk->win_buffers[i];

        vk->win_buffers[i].image = swap_chain_images[i];
        vkCreateImageView(vk->device,
                          &(VkImageViewCreateInfo){
                              .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                              .image = win_buffer->image,
                              .viewType = VK_IMAGE_VIEW_TYPE_2D,
                              .format = vk->image_format,
                              .components =
                                  {
                                      .r = VK_COMPONENT_SWIZZLE_R,
                                      .g = VK_COMPONENT_SWIZZLE_G,
                                      .b = VK_COMPONENT_SWIZZLE_B,
                                      .a = VK_COMPONENT_SWIZZLE_A,
                                  },
                              .subresourceRange =
                                  {
                                      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                      .baseMipLevel = 0,
                                      .levelCount = 1,
                                      .baseArrayLayer = 0,
                                      .layerCount = 1,
                                  },
                          },
                          NULL, &win_buffer->view);

        vkCreateFramebuffer(
            vk->device,
            &(VkFramebufferCreateInfo){
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = vk->render_pass,
                .attachmentCount = 1,
                .pAttachments = &win_buffer->view,
                .width = window->width,
                .height = window->height,
                .layers = 1,
            },
            NULL, &win_buffer->framebuffer);

        vkCreateFence(vk->device,
                      &(VkFenceCreateInfo){
                          .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                          .flags = VK_FENCE_CREATE_SIGNALED_BIT,
                      },
                      NULL, &win_buffer->cmd_fence);

        vkAllocateCommandBuffers(
            vk->device,
            &(VkCommandBufferAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = vk->cmd_pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            },
            &win_buffer->cmd_buffer);
    }

    vkCreateSemaphore(vk->device,
                      &(VkSemaphoreCreateInfo){
                          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                      },
                      NULL, &vk->image_semaphore);
    vkCreateSemaphore(vk->device,
                      &(VkSemaphoreCreateInfo){
                          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                      },
                      NULL, &vk->render_semaphore);
}

void redraw(struct window *window) {
    VkResult r;
    struct vk *vk = &window->vk;
    uint32_t index;
    r = vkAcquireNextImageKHR(vk->device, vk->swap_chain, UINT64_MAX,
                              vk->image_semaphore, VK_NULL_HANDLE, &index);
    assert(r == VK_SUCCESS);

    struct window_buffer *win_buffer = &vk->win_buffers[index];

    vkWaitForFences(vk->device, 1, &win_buffer->cmd_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(vk->device, 1, &win_buffer->cmd_fence);

    vkBeginCommandBuffer(
        win_buffer->cmd_buffer,
        &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = 0,
        });

    vkCmdBeginRenderPass(
        win_buffer->cmd_buffer,
        &(VkRenderPassBeginInfo){
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = vk->render_pass,
            .framebuffer = win_buffer->framebuffer,
            .renderArea = {{0, 0}, {window->width, window->height}},
            .clearValueCount = 1,
            .pClearValues =
                (VkClearValue[]){
                    {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.5f}}},
                },
        },
        VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindVertexBuffers(win_buffer->cmd_buffer, 0, 1,
                           (VkBuffer[]){vk->vert_buffer.buffer},
                           (VkDeviceSize[]){0u});
    vkCmdBindPipeline(win_buffer->cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      vk->pipeline);

    // clang-format off
    memcpy(vk->uniform_buffer.map,
           (float[16]){
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
           },
           sizeof(float[16]));
    // clang-format on
    vkCmdBindDescriptorSets(win_buffer->cmd_buffer,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk->pipeline_layout, 0, 1, &vk->desc_set, 0, NULL);

    vkCmdSetViewport(win_buffer->cmd_buffer, 0, 1,
                     &(VkViewport){
                         .x = 0,
                         .y = 0,
                         .width = window->width,
                         .height = window->height,
                         .minDepth = 0,
                         .maxDepth = 1,
                     });
    vkCmdSetScissor(win_buffer->cmd_buffer, 0, 1,
                    &(VkRect2D){
                        .offset = {0, 0},
                        .extent = {window->width, window->height},
                    });

    vkCmdDraw(win_buffer->cmd_buffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(win_buffer->cmd_buffer);
    vkEndCommandBuffer(win_buffer->cmd_buffer);

    vkQueueSubmit(vk->queue, 1,
                  &(VkSubmitInfo){
                      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .pNext =
                          &(VkProtectedSubmitInfo){
                              .sType = VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO,
                          },
                      .waitSemaphoreCount = 1,
                      .pWaitSemaphores = &vk->image_semaphore,
                      .signalSemaphoreCount = 1,
                      .pSignalSemaphores = &vk->render_semaphore,
                      .pWaitDstStageMask =
                          (VkPipelineStageFlags[]){
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          },
                      .commandBufferCount = 1,
                      .pCommandBuffers = &win_buffer->cmd_buffer,
                  },
                  win_buffer->cmd_fence);

    vkQueuePresentKHR(
        vk->queue, &(VkPresentInfoKHR){
                       .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                       .waitSemaphoreCount = 1,
                       .pWaitSemaphores = (VkSemaphore[]){vk->render_semaphore},
                       .swapchainCount = 1,
                       .pSwapchains = (VkSwapchainKHR[]){vk->swap_chain},
                       .pImageIndices = (uint32_t[]){index},
                       .pResults = &r,
                   });
    assert(r == VK_SUCCESS);

    vkQueueWaitIdle(vk->queue);
}

int main() {
    struct display display = {0};
    struct window *window = &display.window;
    window->display = &display;
    window->width = 250;
    window->height = 250;

    display.wl_display = wl_display_connect(NULL);
    assert(display.wl_display);

    display.wl_registory = wl_display_get_registry(display.wl_display);
    wl_registry_add_listener(display.wl_registory, &wl_registry_listener,
                             &display);
    wl_display_roundtrip(display.wl_display);
    assert(display.xdg_wm_base && display.wl_compositor);

    window->wl_surface = wl_compositor_create_surface(display.wl_compositor);
    window->xdg_surface =
        xdg_wm_base_get_xdg_surface(display.xdg_wm_base, window->wl_surface);
    xdg_surface_add_listener(window->xdg_surface, &xdg_surface_listener,
                             window);
    window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
    window->wait_for_configure = true;
    wl_surface_commit(window->wl_surface);

    while (window->wait_for_configure)
        wl_display_dispatch(display.wl_display);

    init_vulkan(window);
    create_swapchain(window);

    while (wl_display_dispatch_pending(display.wl_display) != -1)
        redraw(window);

    return 0;
}
