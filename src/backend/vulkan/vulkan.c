#include <shaderc/shaderc.h>
#include <sys/shm.h>
#include <vulkan/vulkan.h>
#include <xcb/dri3.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "backend/vulkan/shaders.h"
#include "log.h"
#include "picom.h"

PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHRProc;
PFN_vkGetMemoryHostPointerPropertiesEXT vkGetMemoryHostPointerPropertiesEXTProc;

enum bind_pixmap_strategy {
	BIND_PIXMAP_STRATEGY_DRI3,
	BIND_PIXMAP_STRATEGY_SHM
};

struct vulkan_data {
	backend_t base;
	VkInstance instance;
	xcb_connection_t *surface_connection;
	VkSurfaceKHR surface;
	enum bind_pixmap_strategy bind_pixmap_strategy;
	VkPhysicalDevice physical_device;
	VkDeviceSize min_imported_host_pointer_alignment;
	uint32_t queue_family_index;
	VkDevice device;
	VkQueue queue;
	VkFence acquire_next_image_fence;
	VkFence queue_submit_fence;
	VkSemaphore semaphore;
	uint32_t width;
	uint32_t height;
	VkSwapchainKHR swapchain;
	uint32_t swapchain_image_count;
	VkImage *swapchain_images;
	VkImageLayout *swapchain_image_layouts;
	VkImageView *swapchain_image_views;
	int32_t *buffer_ages;
	uint32_t swapchain_image_index;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSetLayout descriptor_set_layout;
	VkSampler sampler;
	VkPipelineLayout compose_pipeline_layout;
	VkPipeline compose_pipeline;
	VkPipelineLayout fill_pipeline_layout;
	VkPipeline fill_pipeline;
	VkCommandPool command_pool;
	VkCommandBuffer command_buffer;
};

struct vulkan_image {
	int32_t refcount;
	bool has_alpha;
	xcb_pixmap_t pixmap;
	bool owned;
	uint16_t width;
	uint16_t height;
	VkImage image;
	VkDeviceMemory memory;
	int32_t shm_id;
	void *shm_address;
	xcb_shm_seg_t shm_segment;
	VkBuffer staging_buffer;
	VkDeviceMemory staging_memory;
	VkImageView image_view;
	VkDescriptorSet descriptor_set;
};

static bool vk_has_extension(uint32_t property_count, VkExtensionProperties *properties,
	const char *extension) {
	for (uint32_t i = 0; i < property_count; i++) {
		if (strcmp(properties[i].extensionName, extension) == 0) {
			return true;
		}
	}

	return false;
}

#define to_string_case(x) case x: return #x;
#define to_string_default default: unreachable();
static char *vk_physical_device_type_to_string(VkPhysicalDeviceType physical_device_type) {
	switch (physical_device_type) {
		to_string_case(VK_PHYSICAL_DEVICE_TYPE_OTHER)
		to_string_case(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
		to_string_case(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		to_string_case(VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU)
		to_string_case(VK_PHYSICAL_DEVICE_TYPE_CPU)
		to_string_default
	}
}
#undef to_string_default
#undef to_string_case

static bool vk_create_instance(struct vulkan_data *vd) {
	const char *enabled_extension_names[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_XCB_SURFACE_EXTENSION_NAME
	};

	uint8_t enabled_extension_count = ARR_SIZE(enabled_extension_names);

	uint32_t property_count;
	if (vkEnumerateInstanceExtensionProperties(NULL, &property_count, NULL) != VK_SUCCESS) {
		log_error("Failed to enumerate instance extension properties.");

		return false;
	}

	VkExtensionProperties *properties = ccalloc(property_count, VkExtensionProperties);
	if (vkEnumerateInstanceExtensionProperties(NULL, &property_count, properties) != VK_SUCCESS) {
		log_error("Failed to enumerate instance extension properties.");

		free(properties);

		return false;
	}

	for (uint8_t i = 0; i < enabled_extension_count; i++) {
		if (!vk_has_extension(property_count, properties, enabled_extension_names[i])) {
			log_error("No %s instance extension.", enabled_extension_names[i]);

			free(properties);

			return false;
		}
	}

	free(properties);

	VkApplicationInfo application_info = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pNext = NULL,
		.pApplicationName = NULL,
		.applicationVersion = 0,
		.pEngineName = NULL,
		.engineVersion = 0,
		.apiVersion = VK_API_VERSION_1_3
	};

	VkInstanceCreateInfo instance_create_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.pApplicationInfo = &application_info,
		.enabledLayerCount = 0,
		.ppEnabledLayerNames = NULL,
		.enabledExtensionCount = enabled_extension_count,
		.ppEnabledExtensionNames = enabled_extension_names
	};

	if (vkCreateInstance(&instance_create_info, NULL, &vd->instance) != VK_SUCCESS) {
		log_error("Failed to create instance.");

		return false;
	}

	return true;
}

static bool vk_create_surface(struct vulkan_data *vd, xcb_window_t window) {
	vd->surface_connection = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(vd->surface_connection)) {
		log_error("Failed to connect to the X server.");

		return false;
	}

	VkXcbSurfaceCreateInfoKHR xcb_surface_create_info = {
		.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
		.pNext = NULL,
		.flags = 0,
		.connection = vd->surface_connection,
		.window = window
	};

	if (vkCreateXcbSurfaceKHR(vd->instance, &xcb_surface_create_info, NULL, &vd->surface)
		!= VK_SUCCESS) {
		log_error("Failed to create surface.");

		return false;
	}

	return true;
}

static void vk_select_physical_device(struct vulkan_data *vd, uint32_t physical_device_count,
	VkPhysicalDevice *physical_devices, uint8_t enabled_extension_count,
	const char **enabled_extension_names) {
	for (uint32_t i = 0; i < physical_device_count; i++) {
		VkPhysicalDeviceProperties physical_device_properties;
		vkGetPhysicalDeviceProperties(physical_devices[i], &physical_device_properties);

		if (physical_device_properties.apiVersion < VK_API_VERSION_1_3) {
			continue;
		}

		uint32_t property_count;
		if (vkEnumerateDeviceExtensionProperties(physical_devices[i], NULL, &property_count, NULL)
			!= VK_SUCCESS) {
			log_error("Failed to enumerate device extension properties.");

			continue;
		}

		VkExtensionProperties *properties = ccalloc(property_count, VkExtensionProperties);
		if (vkEnumerateDeviceExtensionProperties(physical_devices[i], NULL, &property_count,
			properties) != VK_SUCCESS) {
			log_error("Failed to enumerate device extension properties.");

			free(properties);

			continue;
		}

		bool has_enabled_extensions = true;
		for (uint8_t j = 0; j < enabled_extension_count; j++) {
			if (!vk_has_extension(property_count, properties, enabled_extension_names[j])) {
				has_enabled_extensions = false;

				break;
			}
		}

		free(properties);

		if (!has_enabled_extensions) {
			continue;
		}

		vd->physical_device = physical_devices[i];

		log_info("Selected physical device %u: %s (%s).", i, physical_device_properties.deviceName,
			vk_physical_device_type_to_string(physical_device_properties.deviceType));

		if (vd->bind_pixmap_strategy == BIND_PIXMAP_STRATEGY_SHM) {
			VkPhysicalDeviceExternalMemoryHostPropertiesEXT external_memory_host_properties = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,
				.pNext = NULL
			};

			VkPhysicalDeviceProperties2 properties_2 = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
				.pNext = &external_memory_host_properties
			};

			vkGetPhysicalDeviceProperties2(vd->physical_device, &properties_2);

			vd->min_imported_host_pointer_alignment
				= external_memory_host_properties.minImportedHostPointerAlignment;
		}

		break;
	}
}

static bool vk_create_device(struct vulkan_data *vd, session_t *session) {
	uint32_t physical_device_count;
	if (vkEnumeratePhysicalDevices(vd->instance, &physical_device_count, NULL) != VK_SUCCESS) {
		log_error("Failed to enumerate physical devices.");

		return false;
	}

	VkPhysicalDevice *physical_devices = ccalloc(physical_device_count, VkPhysicalDevice);
	if (vkEnumeratePhysicalDevices(vd->instance, &physical_device_count, physical_devices)
		!= VK_SUCCESS) {
		log_error("Failed to enumerate physical devices.");

		free(physical_devices);

		return false;
	}

	const char **enabled_extension_names = NULL;
	uint8_t enabled_extension_count = 0;

	char *common_extension_names[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME
	};

	uint8_t common_extension_count = ARR_SIZE(common_extension_names);

	if (session->dri3_exists) {
		vd->bind_pixmap_strategy = BIND_PIXMAP_STRATEGY_DRI3;

		char *dri3_extension_names[] = {
			VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
			VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
			VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME
		};

		uint8_t dri3_extension_count = ARR_SIZE(dri3_extension_names);

		enabled_extension_names = crealloc(enabled_extension_names, common_extension_count
			+ dri3_extension_count);
		memcpy(enabled_extension_names, common_extension_names, sizeof(common_extension_names));
		memcpy(enabled_extension_names + common_extension_count, dri3_extension_names, sizeof(
			dri3_extension_names));
		enabled_extension_count = common_extension_count + dri3_extension_count;

		vk_select_physical_device(vd, physical_device_count, physical_devices,
			enabled_extension_count, enabled_extension_names);
	}

	if (vd->physical_device == VK_NULL_HANDLE && session->shm_exists) {
		vd->bind_pixmap_strategy = BIND_PIXMAP_STRATEGY_SHM;

		char *shm_extension_names[] = {
			VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME
		};

		uint8_t shm_extension_count = ARR_SIZE(shm_extension_names);

		enabled_extension_names = crealloc(enabled_extension_names, common_extension_count
			+ shm_extension_count);
		memcpy(enabled_extension_names, common_extension_names, sizeof(common_extension_names));
		memcpy(enabled_extension_names + common_extension_count, shm_extension_names, sizeof(
			shm_extension_names));
		enabled_extension_count = common_extension_count + shm_extension_count;

		vk_select_physical_device(vd, physical_device_count, physical_devices,
			enabled_extension_count, enabled_extension_names);
	}

	free(physical_devices);

	if (vd->physical_device == VK_NULL_HANDLE) {
		log_error("Failed to find suitable physical device.");

		return false;
	}

	log_info("Binding pixmaps using the X %s extension.", vd->bind_pixmap_strategy
		== BIND_PIXMAP_STRATEGY_DRI3 ? "DRI3" : "SHM");

	uint32_t queue_family_property_count;
	vkGetPhysicalDeviceQueueFamilyProperties(vd->physical_device, &queue_family_property_count,
		NULL);

	VkQueueFamilyProperties *queue_family_properties = ccalloc(queue_family_property_count,
		VkQueueFamilyProperties);
	vkGetPhysicalDeviceQueueFamilyProperties(vd->physical_device, &queue_family_property_count,
		queue_family_properties);

	vd->queue_family_index = UINT32_MAX;
	for (uint32_t i = 0; i < queue_family_property_count; i++) {
		bool has_graphics_bit_set = queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
		VkBool32 supports_xcb_presentation = vkGetPhysicalDeviceXcbPresentationSupportKHR(
			vd->physical_device, i, vd->base.c->c, vd->base.c->screen_info->root_visual);
		VkBool32 supports_surface;
		if (vkGetPhysicalDeviceSurfaceSupportKHR(vd->physical_device, i, vd->surface,
			&supports_surface) != VK_SUCCESS) {
			log_error("Failed to get physical device surface support.");

			free(queue_family_properties);

			return false;
		}

		if (has_graphics_bit_set && supports_xcb_presentation == VK_TRUE && supports_surface
			== VK_TRUE) {
			vd->queue_family_index = i;

			break;
		}
	}

	free(queue_family_properties);

	if (vd->queue_family_index == UINT32_MAX) {
		log_error("Failed to find suitable queue family.");

		return false;
	}

	float queue_priority = 1.0F;
	VkDeviceQueueCreateInfo device_queue_create_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.queueFamilyIndex = vd->queue_family_index,
		.queueCount = 1,
		.pQueuePriorities = &queue_priority
	};

	VkPhysicalDeviceDynamicRenderingFeatures physical_device_dynamic_rendering_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
		.pNext = NULL,
		.dynamicRendering = VK_TRUE
	};

	VkDeviceCreateInfo device_create_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &physical_device_dynamic_rendering_features,
		.flags = 0,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &device_queue_create_info,
		.enabledLayerCount = 0,
		.ppEnabledLayerNames = NULL,
		.enabledExtensionCount = enabled_extension_count,
		.ppEnabledExtensionNames = enabled_extension_names,
		.pEnabledFeatures = NULL
	};

	if (vkCreateDevice(vd->physical_device, &device_create_info, NULL, &vd->device) != VK_SUCCESS) {
		log_error("Failed to create device.");

		free(enabled_extension_names);

		return false;
	}

	free(enabled_extension_names);

#define get_device_procedure_address(procedure) \
procedure##Proc = (PFN_##procedure)vkGetDeviceProcAddr(vd->device, #procedure); \
if (!procedure##Proc) { \
	log_error("Failed to get " #procedure " device procedure address."); \
	return false; \
}
	if (vd->bind_pixmap_strategy == BIND_PIXMAP_STRATEGY_DRI3) {
		get_device_procedure_address(vkGetMemoryFdPropertiesKHR);
	}

	if (vd->bind_pixmap_strategy == BIND_PIXMAP_STRATEGY_SHM) {
		get_device_procedure_address(vkGetMemoryHostPointerPropertiesEXT);
	}
#undef get_device_procedure_address

	vkGetDeviceQueue(vd->device, vd->queue_family_index, 0, &vd->queue);

	return true;
}

static bool vk_create_fences_and_semaphore(struct vulkan_data *vd) {
	VkFenceCreateInfo acquire_next_image_fence_create_info = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0
	};

	if (vkCreateFence(vd->device, &acquire_next_image_fence_create_info, NULL,
		&vd->acquire_next_image_fence) != VK_SUCCESS) {
		log_error("Failed to create fence.");

		return false;
	}

	VkFenceCreateInfo queue_submit_fence_create_info = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.pNext = NULL,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT
	};

	if (vkCreateFence(vd->device, &queue_submit_fence_create_info, NULL, &vd->queue_submit_fence)
		!= VK_SUCCESS) {
		log_error("Failed to create fence.");

		return false;
	}

	VkSemaphoreCreateInfo semaphore_create_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0
	};

	if (vkCreateSemaphore(vd->device, &semaphore_create_info, NULL, &vd->semaphore) != VK_SUCCESS) {
		log_error("Failed to create semaphore.");

		return false;
	}

	return true;
}

static void vk_destroy_swapchain(struct vulkan_data *vd) {
	if (vd->buffer_ages) {
		free(vd->buffer_ages);
	}

	if (vd->swapchain_image_views) {
		for (uint32_t i = 0; i < vd->swapchain_image_count; i++) {
			if (vd->swapchain_image_views[i] != VK_NULL_HANDLE) {
				vkDestroyImageView(vd->device, vd->swapchain_image_views[i], NULL);
			}
		}

		free(vd->swapchain_image_views);
	}

	if (vd->swapchain_image_layouts) {
		free(vd->swapchain_image_layouts);
	}

	if (vd->swapchain_images) {
		free(vd->swapchain_images);
	}

	if (vd->swapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(vd->device, vd->swapchain, NULL);
	}
}

static bool vk_create_swapchain(struct vulkan_data *vd, bool is_vsync_enabled) {
	VkSurfaceCapabilitiesKHR surface_capabilities;
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vd->physical_device, vd->surface,
		&surface_capabilities) != VK_SUCCESS) {
		log_error("Failed to get physical device surface capabilities.");

		return false;
	}

	vd->width = surface_capabilities.currentExtent.width;
	vd->height = surface_capabilities.currentExtent.height;

	uint32_t surface_format_count;
	if (vkGetPhysicalDeviceSurfaceFormatsKHR(vd->physical_device, vd->surface,
		&surface_format_count, NULL) != VK_SUCCESS) {
		log_error("Failed to get physical device surface formats.");

		return false;
	}

	VkSurfaceFormatKHR *surface_formats = ccalloc(surface_format_count, VkSurfaceFormatKHR);
	vkGetPhysicalDeviceSurfaceFormatsKHR(vd->physical_device, vd->surface, &surface_format_count,
		surface_formats);

	VkSurfaceFormatKHR surface_format;
	for (uint32_t i = 0; i < surface_format_count; i++) {
		if (surface_formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
			surface_format = surface_formats[i];

			break;
		}
	}

	free(surface_formats);

	if (surface_format.format == VK_FORMAT_UNDEFINED) {
		log_error("Failed to find suitable surface format.");

		return false;
	}

	VkSwapchainCreateInfoKHR swapchain_create_info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.pNext = NULL,
		.flags = 0,
		.surface = vd->surface,
		.minImageCount = surface_capabilities.minImageCount,
		.imageFormat = surface_format.format,
		.imageColorSpace = surface_format.colorSpace,
		.imageExtent = surface_capabilities.currentExtent,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = NULL,
		.preTransform = surface_capabilities.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = is_vsync_enabled ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR,
		.clipped = VK_TRUE,
		.oldSwapchain = NULL
	};

	if (vkCreateSwapchainKHR(vd->device, &swapchain_create_info, NULL, &vd->swapchain)
		!= VK_SUCCESS) {
		log_error("Failed to create swapchain.");

		return false;
	}

	if (vkGetSwapchainImagesKHR(vd->device, vd->swapchain, &vd->swapchain_image_count, NULL)
		!= VK_SUCCESS) {
		log_error("Failed to get swapchain images.");

		return false;
	}

	vd->swapchain_images = ccalloc(vd->swapchain_image_count, VkImage);
	vkGetSwapchainImagesKHR(vd->device, vd->swapchain, &vd->swapchain_image_count,
		vd->swapchain_images);

	vd->swapchain_image_layouts = ccalloc(vd->swapchain_image_count, VkImageLayout);
	vd->swapchain_image_views = ccalloc(vd->swapchain_image_count, VkImageView);
	vd->buffer_ages = ccalloc(vd->swapchain_image_count, int32_t);
	for (uint32_t i = 0; i < vd->swapchain_image_count; i++) {
		vd->swapchain_image_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;

		VkImageViewCreateInfo image_view_create_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.pNext = NULL,
			.flags = 0,
			.image = vd->swapchain_images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = VK_FORMAT_B8G8R8A8_UNORM,
			.components = {
				.r = VK_COMPONENT_SWIZZLE_R,
				.g = VK_COMPONENT_SWIZZLE_G,
				.b = VK_COMPONENT_SWIZZLE_B,
				.a = VK_COMPONENT_SWIZZLE_A
			},
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			}
		};

		if (vkCreateImageView(vd->device, &image_view_create_info, NULL,
			&vd->swapchain_image_views[i]) != VK_SUCCESS) {
			log_error("Failed to create image view.");

			return false;
		}

		vd->buffer_ages[i] = -1;
	}

	if (vkAcquireNextImageKHR(vd->device, vd->swapchain, UINT64_MAX, vd->semaphore,
		vd->acquire_next_image_fence, &vd->swapchain_image_index) != VK_SUCCESS) {
		log_error("Failed to acquire next image.");

		return false;
	}

	if (vkWaitForFences(vd->device, 1, &vd->acquire_next_image_fence, VK_TRUE, UINT64_MAX)
		!= VK_SUCCESS) {
		log_error("Failed to wait for fences.");

		return false;
	}

	if (vkResetFences(vd->device, 1, &vd->acquire_next_image_fence) != VK_SUCCESS) {
		log_error("Failed to reset fences.");

		return false;
	}

	return true;
}

static bool vk_create_descriptor_pool(struct vulkan_data *vd) {
	VkDescriptorPoolSize descriptor_pool_size = {
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 32
	};

	VkDescriptorPoolCreateInfo descriptor_pool_create_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.pNext = NULL,
		.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		.maxSets = 32,
		.poolSizeCount = 1,
		.pPoolSizes = &descriptor_pool_size
	};

	if (vkCreateDescriptorPool(vd->device, &descriptor_pool_create_info, NULL, &vd->descriptor_pool)
		!= VK_SUCCESS) {
		log_error("Failed to create descriptor pool.");

		return false;
	}

	VkDescriptorSetLayoutBinding descriptor_set_layout_binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.pImmutableSamplers = NULL
	};

	VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.bindingCount = 1,
		.pBindings = &descriptor_set_layout_binding
	};

	if (vkCreateDescriptorSetLayout(vd->device, &descriptor_set_layout_create_info, NULL,
		&vd->descriptor_set_layout) != VK_SUCCESS) {
		log_error("Failed to create descriptor set layout.");

		return false;
	}

	VkSamplerCreateInfo sampler_create_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.mipLodBias = 0.0F,
		.anisotropyEnable = VK_FALSE,
		.maxAnisotropy = 0.0F,
		.compareEnable = VK_FALSE,
		.compareOp = VK_COMPARE_OP_NEVER,
		.minLod = 0.0F,
		.maxLod = VK_LOD_CLAMP_NONE,
		.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
		.unnormalizedCoordinates = VK_FALSE
	};

	if (vkCreateSampler(vd->device, &sampler_create_info, NULL, &vd->sampler) != VK_SUCCESS) {
		log_error("Failed to create sampler.");

		return false;
	}

	return true;
}

static void vk_destroy_pipeline_shader_stage_create_infos(struct vulkan_data *vd,
	VkPipelineShaderStageCreateInfo *pipeline_shader_stage_create_infos) {
	for (uint8_t i = 0; i < 2; i++) {
		if (pipeline_shader_stage_create_infos[i].module != VK_NULL_HANDLE) {
			vkDestroyShaderModule(vd->device, pipeline_shader_stage_create_infos[i].module, NULL);
		}
	}

	free(pipeline_shader_stage_create_infos);
}

static bool vk_create_pipeline_shader_stage_create_infos(struct vulkan_data *vd,
	char *vertex_shader, char *vertex_shader_name, char *fragment_shader,
	char *fragment_shader_name,
	VkPipelineShaderStageCreateInfo *pipeline_shader_stage_create_infos) {
	shaderc_compiler_t compiler = shaderc_compiler_initialize();
	if (!compiler) {
		log_error("Failed to initialize compiler.");

		return false;
	}

	for (uint8_t i = 0; i < 2; i++) {
		char *shader = i == 0 ? vertex_shader : fragment_shader;
		shaderc_compilation_result_t compilation_result = shaderc_compile_into_spv(compiler, shader,
			strlen(shader), i == 0 ? shaderc_glsl_vertex_shader : shaderc_glsl_fragment_shader, i
			== 0 ? vertex_shader_name : fragment_shader_name, "main", NULL);
		if (shaderc_result_get_compilation_status(compilation_result)
			!= shaderc_compilation_status_success) {
			log_error("Failed to compile into SPIR-V: %s", shaderc_result_get_error_message(
				compilation_result));

			shaderc_compiler_release(compiler);

			return false;
		}

		VkShaderModule shader_module;

		VkShaderModuleCreateInfo shader_module_create_info = {
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.pNext = NULL,
			.flags = 0,
			.codeSize = shaderc_result_get_length(compilation_result),
			.pCode = (uint32_t *)shaderc_result_get_bytes(compilation_result)
		};

		if (vkCreateShaderModule(vd->device, &shader_module_create_info, NULL, &shader_module)
			!= VK_SUCCESS) {
			log_error("Failed to create shader module.");

			shaderc_result_release(compilation_result);
			shaderc_compiler_release(compiler);

			return false;
		}

		pipeline_shader_stage_create_infos[i] = (VkPipelineShaderStageCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.pNext = NULL,
			.flags = 0,
			.stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = shader_module,
			.pName = "main",
			.pSpecializationInfo = NULL
		};

		shaderc_result_release(compilation_result);
	}

	shaderc_compiler_release(compiler);

	return true;
}

static bool vk_create_pipelines(struct vulkan_data *vd) {
	VkFormat color_attachment_format = VK_FORMAT_B8G8R8A8_UNORM;

	VkPipelineRenderingCreateInfo compose_pipeline_rendering_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.pNext = NULL,
		.viewMask = 0,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &color_attachment_format,
		.depthAttachmentFormat = VK_FORMAT_UNDEFINED,
		.stencilAttachmentFormat = VK_FORMAT_UNDEFINED
	};

	VkPipelineRenderingCreateInfo fill_pipeline_rendering_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.pNext = NULL,
		.viewMask = 0,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &color_attachment_format,
		.depthAttachmentFormat = VK_FORMAT_UNDEFINED,
		.stencilAttachmentFormat = VK_FORMAT_UNDEFINED
	};

	VkPipelineShaderStageCreateInfo *compose_pipeline_shader_stage_create_infos = ccalloc(2,
		VkPipelineShaderStageCreateInfo);
	VkPipelineShaderStageCreateInfo *fill_pipeline_shader_stage_create_infos = ccalloc(2,
		VkPipelineShaderStageCreateInfo);

	if (!vk_create_pipeline_shader_stage_create_infos(vd, compose_vertex_shader,
		"compose_vertex_shader", compose_fragment_shader, "compose_fragment_shader",
		compose_pipeline_shader_stage_create_infos)) {
		goto err;
	}

	if (!vk_create_pipeline_shader_stage_create_infos(vd, fill_vertex_shader, "fill_vertex_shader",
		fill_fragment_shader, "fill_fragment_shader", fill_pipeline_shader_stage_create_infos)) {
		goto err;
	}

	VkPipelineVertexInputStateCreateInfo pipeline_vertex_input_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.vertexBindingDescriptionCount = 0,
		.pVertexBindingDescriptions = NULL,
		.vertexAttributeDescriptionCount = 0,
		.pVertexAttributeDescriptions = NULL
	};

	VkPipelineInputAssemblyStateCreateInfo pipeline_input_assembly_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
		.primitiveRestartEnable = VK_FALSE
	};

	VkViewport viewport = {
		.x = 0.0F,
		.y = 0.0F,
		.width = (float)vd->width,
		.height = (float)vd->height,
		.minDepth = 0.0F,
		.maxDepth = 0.0F
	};

	VkPipelineViewportStateCreateInfo pipeline_viewport_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.viewportCount = 1,
		.pViewports = &viewport,
		.scissorCount = 1,
		.pScissors = NULL
	};

	VkPipelineRasterizationStateCreateInfo pipeline_rasterization_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.depthClampEnable = VK_FALSE,
		.rasterizerDiscardEnable = VK_FALSE,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.depthBiasEnable = VK_FALSE,
		.depthBiasConstantFactor = 0.0F,
		.depthBiasClamp = 0.0F,
		.depthBiasSlopeFactor = 0.0F,
		.lineWidth = 1.0F
	};

	VkPipelineMultisampleStateCreateInfo pipeline_multisample_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		.sampleShadingEnable = VK_FALSE,
		.minSampleShading = 0.0F,
		.pSampleMask = NULL,
		.alphaToCoverageEnable = VK_FALSE,
		.alphaToOneEnable = VK_FALSE
	};

	VkPipelineColorBlendAttachmentState pipeline_color_blend_attachment_state = {
		.blendEnable = VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
			| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
	};

	VkPipelineColorBlendStateCreateInfo pipeline_color_blend_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.logicOpEnable = VK_FALSE,
		.logicOp = VK_LOGIC_OP_CLEAR,
		.attachmentCount = 1,
		.pAttachments = &pipeline_color_blend_attachment_state,
		.blendConstants = {
			0.0F,
			0.0F,
			0.0F,
			0.0F
		}
	};

	VkDynamicState dynamic_state = VK_DYNAMIC_STATE_SCISSOR;
	VkPipelineDynamicStateCreateInfo pipeline_dynamic_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.dynamicStateCount = 1,
		.pDynamicStates = &dynamic_state
	};

	VkPushConstantRange compose_push_constant_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.offset = 0,
		.size = 32
	};

	VkPipelineLayoutCreateInfo compose_pipeline_layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.setLayoutCount = 1,
		.pSetLayouts = &vd->descriptor_set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &compose_push_constant_range
	};

	if (vkCreatePipelineLayout(vd->device, &compose_pipeline_layout_create_info, NULL,
		&vd->compose_pipeline_layout) != VK_SUCCESS) {
		log_error("Failed to create pipeline layout.");

		goto err;
	}

	VkGraphicsPipelineCreateInfo compose_graphics_pipeline_create_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &compose_pipeline_rendering_create_info,
		.stageCount = 2,
		.pStages = compose_pipeline_shader_stage_create_infos,
		.pVertexInputState = &pipeline_vertex_input_state_create_info,
		.pInputAssemblyState = &pipeline_input_assembly_state_create_info,
		.pTessellationState = NULL,
		.pViewportState = &pipeline_viewport_state_create_info,
		.pRasterizationState = &pipeline_rasterization_state_create_info,
		.pMultisampleState = &pipeline_multisample_state_create_info,
		.pDepthStencilState = NULL,
		.pColorBlendState = &pipeline_color_blend_state_create_info,
		.pDynamicState = &pipeline_dynamic_state_create_info,
		.layout = vd->compose_pipeline_layout,
		.renderPass = NULL,
		.subpass = 0,
		.basePipelineHandle = NULL,
		.basePipelineIndex = 0
	};

	if (vkCreateGraphicsPipelines(vd->device, NULL, 1, &compose_graphics_pipeline_create_info, NULL,
		&vd->compose_pipeline) != VK_SUCCESS) {
		log_error("Failed to create graphics pipelines.");

		goto err;
	}

	vk_destroy_pipeline_shader_stage_create_infos(vd, compose_pipeline_shader_stage_create_infos);

	VkPushConstantRange fill_push_constant_ranges[] = {
		{
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
			.offset = 0,
			.size = 24
		},
		{
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			.offset = 32,
			.size = 16
		}
	};

	VkPipelineLayoutCreateInfo fill_pipeline_layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.setLayoutCount = 0,
		.pSetLayouts = NULL,
		.pushConstantRangeCount = ARR_SIZE(fill_push_constant_ranges),
		.pPushConstantRanges = fill_push_constant_ranges
	};

	if (vkCreatePipelineLayout(vd->device, &fill_pipeline_layout_create_info, NULL,
		&vd->fill_pipeline_layout) != VK_SUCCESS) {
		log_error("Failed to create pipeline layout.");

		goto err;
	}

	VkGraphicsPipelineCreateInfo fill_graphics_pipeline_create_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &fill_pipeline_rendering_create_info,
		.stageCount = 2,
		.pStages = fill_pipeline_shader_stage_create_infos,
		.pVertexInputState = &pipeline_vertex_input_state_create_info,
		.pInputAssemblyState = &pipeline_input_assembly_state_create_info,
		.pTessellationState = NULL,
		.pViewportState = &pipeline_viewport_state_create_info,
		.pRasterizationState = &pipeline_rasterization_state_create_info,
		.pMultisampleState = &pipeline_multisample_state_create_info,
		.pDepthStencilState = NULL,
		.pColorBlendState = &pipeline_color_blend_state_create_info,
		.pDynamicState = &pipeline_dynamic_state_create_info,
		.layout = vd->fill_pipeline_layout,
		.renderPass = NULL,
		.subpass = 0,
		.basePipelineHandle = NULL,
		.basePipelineIndex = 0
	};

	if (vkCreateGraphicsPipelines(vd->device, NULL, 1, &fill_graphics_pipeline_create_info, NULL,
		&vd->fill_pipeline) != VK_SUCCESS) {
		log_error("Failed to create graphics pipelines.");

		goto err;
	}

	vk_destroy_pipeline_shader_stage_create_infos(vd, fill_pipeline_shader_stage_create_infos);

	return true;

err:
	vk_destroy_pipeline_shader_stage_create_infos(vd, fill_pipeline_shader_stage_create_infos);
	vk_destroy_pipeline_shader_stage_create_infos(vd, compose_pipeline_shader_stage_create_infos);

	return false;
}

static bool vk_create_command_pool(struct vulkan_data *vd) {
	VkCommandPoolCreateInfo command_pool_create_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.pNext = NULL,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = vd->queue_family_index
	};

	if (vkCreateCommandPool(vd->device, &command_pool_create_info, NULL, &vd->command_pool)
		!= VK_SUCCESS) {
		log_error("Failed to create command pool.");

		return false;
	}

	VkCommandBufferAllocateInfo command_buffer_allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.pNext = NULL,
		.commandPool = vd->command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1
	};

	if (vkAllocateCommandBuffers(vd->device, &command_buffer_allocate_info, &vd->command_buffer)
		!= VK_SUCCESS) {
		log_error("Failed to allocate command buffers.");

		return false;
	}

	return true;
}

static void vk_deinit(backend_t *base) {
	auto vd = (struct vulkan_data *)base;

	if (vd->device != VK_NULL_HANDLE && vkDeviceWaitIdle(vd->device) != VK_SUCCESS) {
		log_error("Failed to wait for device idle.");
	}

	if (vd->command_buffer != VK_NULL_HANDLE) {
		vkFreeCommandBuffers(vd->device, vd->command_pool, 1, &vd->command_buffer);
	}

	if (vd->command_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(vd->device, vd->command_pool, NULL);
	}

	if (vd->fill_pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(vd->device, vd->fill_pipeline, NULL);
	}

	if (vd->fill_pipeline_layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(vd->device, vd->fill_pipeline_layout, NULL);
	}

	if (vd->compose_pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(vd->device, vd->compose_pipeline, NULL);
	}

	if (vd->compose_pipeline_layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(vd->device, vd->compose_pipeline_layout, NULL);
	}

	if (vd->sampler != VK_NULL_HANDLE) {
		vkDestroySampler(vd->device, vd->sampler, NULL);
	}

	if (vd->descriptor_set_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(vd->device, vd->descriptor_set_layout, NULL);
	}

	if (vd->descriptor_pool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(vd->device, vd->descriptor_pool, NULL);
	}

	vk_destroy_swapchain(vd);

	if (vd->semaphore != VK_NULL_HANDLE) {
		vkDestroySemaphore(vd->device, vd->semaphore, NULL);
	}

	if (vd->queue_submit_fence != VK_NULL_HANDLE) {
		vkDestroyFence(vd->device, vd->queue_submit_fence, NULL);
	}

	if (vd->acquire_next_image_fence != VK_NULL_HANDLE) {
		vkDestroyFence(vd->device, vd->acquire_next_image_fence, NULL);
	}

	if (vd->device != VK_NULL_HANDLE) {
		vkDestroyDevice(vd->device, NULL);
	}

	if (vd->surface != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(vd->instance, vd->surface, NULL);
	}

	xcb_disconnect(vd->surface_connection);

	if (vd->instance != VK_NULL_HANDLE) {
		vkDestroyInstance(vd->instance, NULL);
	}

	free(vd);
}

static backend_t *vk_init(session_t *session, xcb_window_t window) {
	struct vulkan_data *vd = ccalloc(1, struct vulkan_data);
	init_backend_base(&vd->base, session);

	if (!vk_create_instance(vd)) {
		goto err;
	}

	if (!vk_create_surface(vd, window)) {
		goto err;
	}

	if (!vk_create_device(vd, session)) {
		goto err;
	}

	if (!vk_create_fences_and_semaphore(vd)) {
		goto err;
	}

	if (!vk_create_swapchain(vd, session->o.vsync)) {
		goto err;
	}

	if (!vk_create_descriptor_pool(vd)) {
		goto err;
	}

	if (!vk_create_pipelines(vd)) {
		goto err;
	}

	if (!vk_create_command_pool(vd)) {
		goto err;
	}

	return &vd->base;

err:
	vk_deinit(&vd->base);

	return NULL;
}

static void vk_prepare(backend_t *base, const region_t *region) {
	auto vd = (struct vulkan_data *)base;

	if (vkWaitForFences(vd->device, 1, &vd->queue_submit_fence, VK_TRUE, UINT64_MAX)
		!= VK_SUCCESS) {
		log_error("Failed to wait for fences.");
	}

	if (vkResetFences(vd->device, 1, &vd->queue_submit_fence) != VK_SUCCESS) {
		log_error("Failed to reset fences.");
	}

	if (vkResetCommandBuffer(vd->command_buffer, 0) != VK_SUCCESS) {
		log_error("Failed to reset command buffer.");
	}

	VkCommandBufferBeginInfo command_buffer_begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.pNext = NULL,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		.pInheritanceInfo = NULL
	};

	if (vkBeginCommandBuffer(vd->command_buffer, &command_buffer_begin_info) != VK_SUCCESS) {
		log_error("Failed to begin command buffer.");
	}

	VkImageMemoryBarrier image_memory_barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.pNext = NULL,
		.srcAccessMask = VK_ACCESS_NONE,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = vd->swapchain_image_layouts[vd->swapchain_image_index],
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = vd->swapchain_images[vd->swapchain_image_index],
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1
		}
	};

	vkCmdPipelineBarrier(vd->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1,
		&image_memory_barrier);

	vd->swapchain_image_layouts[vd->swapchain_image_index]
		= VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	(void)region;
}

static void vk_compose(backend_t *base, image_handle image, coord_t image_coords, image_handle mask,
	coord_t mask_coords, const region_t *reg_paint, const region_t *reg_visible) {
	auto vd = (struct vulkan_data *)base;

	int32_t n_rects;
	pixman_box32_t *rects = pixman_region32_rectangles(reg_paint, &n_rects);
	if (n_rects < 1) {
		assert(n_rects >= 0);

		return;
	}

	auto bi = (struct backend_image *)image;
	auto vi = (struct vulkan_image *)bi->inner;

	pixman_box32_t *extents = pixman_region32_extents(reg_paint);

	if (vd->bind_pixmap_strategy == BIND_PIXMAP_STRATEGY_SHM) {
		int16_t x = (int16_t)(extents->x1 - image_coords.x);
		int16_t y = (int16_t)(extents->y1 - image_coords.y);
		uint16_t width = (uint16_t)(extents->x2 - extents->x1);
		uint16_t height = (uint16_t)(extents->y2 - extents->y1);
		xcb_shm_get_image_reply_t *r = xcb_shm_get_image_reply(base->c->c, xcb_shm_get_image(
			base->c->c, vi->pixmap, x, y, width, height, UINT32_MAX, XCB_IMAGE_FORMAT_Z_PIXMAP,
			vi->shm_segment, 0), NULL);
		if (!r) {
			log_error("Failed to read image data into shared memory image.");
		} else {
			free(r);
		}

		VkImageMemoryBarrier image_memory_barrier_a = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.pNext = NULL,
			.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = vi->image,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			}
		};

		vkCmdPipelineBarrier(vd->command_buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &image_memory_barrier_a);

		VkBufferImageCopy buffer_image_copy = {
			.bufferOffset = 0,
			.bufferRowLength = 0,
			.bufferImageHeight = 0,
			.imageSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = 0,
				.baseArrayLayer = 0,
				.layerCount = 1
			},
			.imageOffset = {
				.x = x,
				.y = y,
				.z = 0
			},
			.imageExtent = {
				.width = width,
				.height = height,
				.depth = 1
			}
		};

		vkCmdCopyBufferToImage(vd->command_buffer, vi->staging_buffer, vi->image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &buffer_image_copy);

		VkImageMemoryBarrier image_memory_barrier_b = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.pNext = NULL,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = vi->image,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			}
		};

		vkCmdPipelineBarrier(vd->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &image_memory_barrier_b);
	}

	VkRect2D render_area = {
		.offset = {
			.x = extents->x1,
			.y = extents->y1
		},
		.extent = {
			.width = (uint32_t)(extents->x2 - extents->x1),
			.height = (uint32_t)(extents->y2 - extents->y1)
		}
	};

	VkRenderingAttachmentInfo rendering_attachment_info = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.pNext = NULL,
		.imageView = vd->swapchain_image_views[vd->swapchain_image_index],
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.resolveMode = VK_RESOLVE_MODE_NONE,
		.resolveImageView = NULL,
		.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = {
			0.0F,
			0.0F,
			0.0F,
			0.0F
		}
	};

	VkRenderingInfo rendering_info = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.pNext = NULL,
		.flags = 0,
		.renderArea = render_area,
		.layerCount = 1,
		.viewMask = 0,
		.colorAttachmentCount = 1,
		.pColorAttachments = &rendering_attachment_info,
		.pDepthAttachment = NULL,
		.pStencilAttachment = NULL
	};

	vkCmdBeginRendering(vd->command_buffer, &rendering_info);

	vkCmdBindPipeline(vd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vd->compose_pipeline);
	vkCmdSetScissor(vd->command_buffer, 0, 1, &render_area);
	vkCmdBindDescriptorSets(vd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
		vd->compose_pipeline_layout, 0, 1, &vi->descriptor_set, 0, NULL);
	vkCmdPushConstants(vd->command_buffer, vd->compose_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
		0, 8, (uint32_t[]){vd->width, vd->height});
	vkCmdPushConstants(vd->command_buffer, vd->compose_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
		24, 8, (int32_t[]){image_coords.x, image_coords.y});
	for (uint32_t i = 0; i < (uint32_t)n_rects; i++) {
		vkCmdPushConstants(vd->command_buffer, vd->compose_pipeline_layout,
			VK_SHADER_STAGE_VERTEX_BIT, 8, 16, (int32_t[]){rects[i].x1, rects[i].y1, rects[i].x2,
			rects[i].y2});
		vkCmdDraw(vd->command_buffer, 4, 1, 0, 0);
	}

	vkCmdEndRendering(vd->command_buffer);

	(void)mask;
	(void)mask_coords;
	(void)reg_visible;
}

static void vk_fill(backend_t *base, struct color color, const region_t *region) {
	auto vd = (struct vulkan_data *)base;

	int32_t n_rects;
	pixman_box32_t *rects = pixman_region32_rectangles(region, &n_rects);
	if (n_rects < 1) {
		assert(n_rects >= 0);

		return;
	}

	pixman_box32_t *extents = pixman_region32_extents(region);

	VkRect2D render_area = {
		.offset = {
			.x = extents->x1,
			.y = extents->y1
		},
		.extent = {
			.width = (uint32_t)(extents->x2 - extents->x1),
			.height = (uint32_t)(extents->y2 - extents->y1)
		}
	};

	VkRenderingAttachmentInfo rendering_attachment_info = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.pNext = NULL,
		.imageView = vd->swapchain_image_views[vd->swapchain_image_index],
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.resolveMode = VK_RESOLVE_MODE_NONE,
		.resolveImageView = NULL,
		.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = {
			0.0F,
			0.0F,
			0.0F,
			0.0F
		}
	};

	VkRenderingInfo rendering_info = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.pNext = NULL,
		.flags = 0,
		.renderArea = render_area,
		.layerCount = 1,
		.viewMask = 0,
		.colorAttachmentCount = 1,
		.pColorAttachments = &rendering_attachment_info,
		.pDepthAttachment = NULL,
		.pStencilAttachment = NULL
	};

	vkCmdBeginRendering(vd->command_buffer, &rendering_info);

	vkCmdBindPipeline(vd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vd->fill_pipeline);
	vkCmdSetScissor(vd->command_buffer, 0, 1, &render_area);
	vkCmdPushConstants(vd->command_buffer, vd->fill_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
		8, (uint32_t[]){vd->width, vd->height});
	vkCmdPushConstants(vd->command_buffer, vd->fill_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
		32, 16, (float[]){(float)color.red, (float)color.green, (float)color.blue,
		(float)color.alpha});
	for (uint32_t i = 0; i < (uint32_t)n_rects; i++) {
		vkCmdPushConstants(vd->command_buffer, vd->fill_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
			8, 16, (int32_t[]){rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2});
		vkCmdDraw(vd->command_buffer, 4, 1, 0, 0);
	}

	vkCmdEndRendering(vd->command_buffer);
}

static void vk_present(backend_t *base, const region_t *region) {
	auto vd = (struct vulkan_data *)base;

	VkImageMemoryBarrier image_memory_barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.pNext = NULL,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_NONE,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = vd->swapchain_images[vd->swapchain_image_index],
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1
		}
	};

	vkCmdPipelineBarrier(vd->command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &image_memory_barrier);

	vd->swapchain_image_layouts[vd->swapchain_image_index] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	if (vkEndCommandBuffer(vd->command_buffer) != VK_SUCCESS) {
		log_error("Failed to end command buffer.");
	}

	VkPipelineStageFlags wait_dst_stage_mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = NULL,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &vd->semaphore,
		.pWaitDstStageMask = &wait_dst_stage_mask,
		.commandBufferCount = 1,
		.pCommandBuffers = &vd->command_buffer,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &vd->semaphore
	};

	if (vkQueueSubmit(vd->queue, 1, &submit_info, vd->queue_submit_fence) != VK_SUCCESS) {
		log_error("Failed to queue submit.");
	}

	VkPresentInfoKHR present_info = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.pNext = NULL,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &vd->semaphore,
		.swapchainCount = 1,
		.pSwapchains = &vd->swapchain,
		.pImageIndices = &vd->swapchain_image_index,
		.pResults = NULL
	};

	if (vkQueuePresentKHR(vd->queue, &present_info) != VK_SUCCESS) {
		log_error("Failed to queue present.");
	}

	vd->buffer_ages[vd->swapchain_image_index] = 1;
	for (uint32_t i = 0; i < vd->swapchain_image_count; i++) {
		if (i != vd->swapchain_image_index && vd->buffer_ages[i] != -1) {
			vd->buffer_ages[i]++;
		}
	}

	if (vkAcquireNextImageKHR(vd->device, vd->swapchain, UINT64_MAX, vd->semaphore,
		vd->acquire_next_image_fence, &vd->swapchain_image_index) != VK_SUCCESS) {
		log_error("Failed to acquire next image.");
	}

	if (vkWaitForFences(vd->device, 1, &vd->acquire_next_image_fence, VK_TRUE, UINT64_MAX)
		!= VK_SUCCESS) {
		log_error("Failed to wait for fences.");
	}

	if (vkResetFences(vd->device, 1, &vd->acquire_next_image_fence) != VK_SUCCESS) {
		log_error("Failed to reset fences.");
	}

	(void)region;
}

static bool vk_bind_pixmap_dri3(struct vulkan_data *vd, struct vulkan_image *vi) {
	xcb_dri3_buffers_from_pixmap_reply_t *r = xcb_dri3_buffers_from_pixmap_reply(vd->base.c->c,
		xcb_dri3_buffers_from_pixmap(vd->base.c->c, vi->pixmap), NULL);
	if (!r) {
		log_error("Failed to get buffers from pixmap.");

		return false;
	}

	vi->width = r->width;
	vi->height = r->height;

	assert(r->nfd == 1);

	uint32_t *offsets = xcb_dri3_buffers_from_pixmap_offsets(r);
	uint32_t *strides = xcb_dri3_buffers_from_pixmap_strides(r);

	VkSubresourceLayout subresource_layout = {
		.offset = offsets[0],
		.size = 0,
		.rowPitch = strides[0],
		.arrayPitch = 0,
		.depthPitch = 0
	};

	VkImageDrmFormatModifierExplicitCreateInfoEXT image_drm_format_modifier_explicit_create_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.pNext = NULL,
		.drmFormatModifier = r->modifier,
		.drmFormatModifierPlaneCount = r->nfd,
		.pPlaneLayouts = &subresource_layout
	};

	VkExternalMemoryImageCreateInfo external_memory_image_create_info = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.pNext = &image_drm_format_modifier_explicit_create_info,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
	};

	VkImageCreateInfo image_create_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &external_memory_image_create_info,
		.flags = 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = {
			.width = vi->width,
			.height = vi->height,
			.depth = 1
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = NULL,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};

	if (vkCreateImage(vd->device, &image_create_info, NULL, &vi->image) != VK_SUCCESS) {
		log_error("Failed to create image.");

		free(r);

		return false;
	}

	int32_t *buffers = xcb_dri3_buffers_from_pixmap_buffers(r);

	VkImportMemoryFdInfoKHR import_memory_fd_info = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.pNext = NULL,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.fd = buffers[0]
	};

	VkMemoryRequirements memory_requirements;
	vkGetImageMemoryRequirements(vd->device, vi->image, &memory_requirements);

	VkPhysicalDeviceMemoryProperties physical_device_memory_properties;
	vkGetPhysicalDeviceMemoryProperties(vd->physical_device, &physical_device_memory_properties);

	VkMemoryFdPropertiesKHR memory_fd_properties = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
		.pNext = NULL
	};

	if (vkGetMemoryFdPropertiesKHRProc(vd->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		buffers[0], &memory_fd_properties) != VK_SUCCESS) {
		log_error("Failed to get memory FD properties.");

		free(r);

		return false;
	}

	uint32_t memory_type_index = UINT32_MAX;
	for (uint32_t i = 0; i < physical_device_memory_properties.memoryTypeCount; i++) {
		bool is_supported = memory_requirements.memoryTypeBits & memory_fd_properties.memoryTypeBits
			& (1 << i);
		bool has_device_local_bit_set
			= physical_device_memory_properties.memoryTypes[i].propertyFlags
			& VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		if (is_supported && has_device_local_bit_set) {
			memory_type_index = i;

			break;
		}
	}

	if (memory_type_index == UINT32_MAX) {
		log_error("Failed to find suitable memory type.");

		free(r);

		return false;
	}

	VkMemoryAllocateInfo memory_allocate_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &import_memory_fd_info,
		.allocationSize = memory_requirements.size,
		.memoryTypeIndex = memory_type_index
	};

	if (vkAllocateMemory(vd->device, &memory_allocate_info, NULL, &vi->memory) != VK_SUCCESS) {
		log_error("Failed to allocate memory.");

		free(r);

		return false;
	}

	free(r);

	if (vkBindImageMemory(vd->device, vi->image, vi->memory, 0) != VK_SUCCESS) {
		log_error("Failed to bind image memory.");

		return false;
	}

	return true;
}

static void vk_release_image_shm(struct vulkan_data *vd, struct vulkan_image *vi) {
	if (vi->staging_buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(vd->device, vi->staging_buffer, NULL);
	}

	if (vi->staging_memory != VK_NULL_HANDLE) {
		vkFreeMemory(vd->device, vi->staging_memory, NULL);
	}

	if (vi->shm_segment != XCB_NONE) {
		xcb_shm_detach(vd->base.c->c, vi->shm_segment);
	}

	if (vi->shm_address != (void *)-1) {
		shmdt(vi->shm_address);
	}

	if (vi->shm_id != -1) {
		shmctl(vi->shm_id, IPC_RMID, NULL);
	}
}

static bool vk_bind_pixmap_shm(struct vulkan_data *vd, struct vulkan_image *vi) {
	xcb_get_geometry_reply_t *r = xcb_get_geometry_reply(vd->base.c->c, xcb_get_geometry(
		vd->base.c->c, vi->pixmap), NULL);
	if (!r) {
		log_error("Failed to get geometry.");

		return false;
	}

	vi->width = r->width;
	vi->height = r->height;
	free(r);

	VkImageCreateInfo image_create_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = {
			.width = vi->width,
			.height = vi->height,
			.depth = 1
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = NULL,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};

	if (vkCreateImage(vd->device, &image_create_info, NULL, &vi->image) != VK_SUCCESS) {
		log_error("Failed to create image.");

		return false;
	}

	VkMemoryRequirements image_memory_requirements;
	vkGetImageMemoryRequirements(vd->device, vi->image, &image_memory_requirements);

	VkPhysicalDeviceMemoryProperties physical_device_memory_properties;
	vkGetPhysicalDeviceMemoryProperties(vd->physical_device, &physical_device_memory_properties);

	uint32_t image_memory_type_index = UINT32_MAX;
	for (uint32_t i = 0; i < physical_device_memory_properties.memoryTypeCount; i++) {
		bool is_supported = image_memory_requirements.memoryTypeBits & (1 << i);
		bool has_device_local_bit_set
			= physical_device_memory_properties.memoryTypes[i].propertyFlags
			& VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		if (is_supported && has_device_local_bit_set) {
			image_memory_type_index = i;

			break;
		}
	}

	if (image_memory_type_index == UINT32_MAX) {
		log_error("Failed to find suitable memory type.");

		return false;
	}

	VkMemoryAllocateInfo image_memory_allocate_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = NULL,
		.allocationSize = image_memory_requirements.size,
		.memoryTypeIndex = image_memory_type_index
	};

	if (vkAllocateMemory(vd->device, &image_memory_allocate_info, NULL, &vi->memory)
		!= VK_SUCCESS) {
		log_error("Failed to allocate memory.");

		return false;
	}

	if (vkBindImageMemory(vd->device, vi->image, vi->memory, 0) != VK_SUCCESS) {
		log_error("Failed to bind image memory.");

		return false;
	}

	size_t size = (size_t)vi->width * vi->height * 4;
	size = (size - 1) + vd->min_imported_host_pointer_alignment - (size - 1)
		% vd->min_imported_host_pointer_alignment;

	vi->shm_id = shmget(IPC_PRIVATE, size, IPC_CREAT | IPC_EXCL | 0600);
	if (vi->shm_id == -1) {
		log_error("Failed to allocate shared memory segment.");

		return false;
	}

	vi->shm_address = shmat(vi->shm_id, NULL, 0);
	if (vi->shm_address == (void *)-1) {
		log_error("Failed to attach shared memory segment.");

		return false;
	}

	vi->shm_segment = x_new_id(vd->base.c);
	xcb_generic_error_t *e = xcb_request_check(vd->base.c->c, xcb_shm_attach_checked(vd->base.c->c,
		vi->shm_segment, (uint32_t)vi->shm_id, false));
	if (e) {
		log_error("Failed to attach to shared memory segment.");

		vi->shm_segment = XCB_NONE;
		free(e);

		return false;
	}

	VkExternalMemoryBufferCreateInfo external_memory_buffer_create_info = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
		.pNext = NULL,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT
	};

	VkBufferCreateInfo buffer_create_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = &external_memory_buffer_create_info,
		.flags = 0,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = NULL
	};

	if (vkCreateBuffer(vd->device, &buffer_create_info, NULL, &vi->staging_buffer) != VK_SUCCESS) {
		log_error("Failed to create buffer.");

		return false;
	}

	VkImportMemoryHostPointerInfoEXT import_memory_host_pointer_info = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
		.pNext = NULL,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
		.pHostPointer = vi->shm_address
	};

	VkMemoryRequirements buffer_memory_requirements;
	vkGetBufferMemoryRequirements(vd->device, vi->staging_buffer, &buffer_memory_requirements);

	VkMemoryHostPointerPropertiesEXT memory_host_pointer_properties = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
		.pNext = NULL
	};

	if (vkGetMemoryHostPointerPropertiesEXTProc(vd->device,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, vi->shm_address,
		&memory_host_pointer_properties) != VK_SUCCESS) {
		log_error("Failed to get memory host pointer properties.");

		return false;
	}

	uint32_t buffer_memory_type_index = UINT32_MAX;
	for (uint32_t i = 0; i < physical_device_memory_properties.memoryTypeCount; i++) {
		bool is_supported = buffer_memory_requirements.memoryTypeBits
			& memory_host_pointer_properties.memoryTypeBits & (1 << i);
		bool has_host_visible_bit_set
			= physical_device_memory_properties.memoryTypes[i].propertyFlags
			& VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		if (is_supported && has_host_visible_bit_set) {
			buffer_memory_type_index = i;

			break;
		}
	}

	if (buffer_memory_type_index == UINT32_MAX) {
		log_error("Failed to find suitable memory type.");

		return false;
	}

	VkMemoryAllocateInfo buffer_memory_allocate_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &import_memory_host_pointer_info,
		.allocationSize = buffer_memory_requirements.size,
		.memoryTypeIndex = buffer_memory_type_index
	};

	if (vkAllocateMemory(vd->device, &buffer_memory_allocate_info, NULL, &vi->staging_memory)
		!= VK_SUCCESS) {
		log_error("Failed to allocate memory.");

		return false;
	}

	if (vkBindBufferMemory(vd->device, vi->staging_buffer, vi->staging_memory, 0) != VK_SUCCESS) {
		log_error("Failed to bind buffer memory.");

		return false;
	}

	return true;
}

static void vk_release_image(backend_t *base, image_handle image) {
	auto vd = (struct vulkan_data *)base;

	auto bi = (struct backend_image *)image;
	auto vi = (struct vulkan_image *)bi->inner;
	free(bi);

	if (--vi->refcount > 0) {
		return;
	}

	if (vkDeviceWaitIdle(vd->device) != VK_SUCCESS) {
		log_error("Failed to wait for device idle.");
	}

	if (vi->descriptor_set != VK_NULL_HANDLE) {
		vkFreeDescriptorSets(vd->device, vd->descriptor_pool, 1, &vi->descriptor_set);
	}

	if (vi->image_view != VK_NULL_HANDLE) {
		vkDestroyImageView(vd->device, vi->image_view, NULL);
	}

	if (vd->bind_pixmap_strategy == BIND_PIXMAP_STRATEGY_SHM) {
		vk_release_image_shm(vd, vi);
	}

	if (vi->image != VK_NULL_HANDLE) {
		vkDestroyImage(vd->device, vi->image, NULL);
	}

	if (vi->memory != VK_NULL_HANDLE) {
		vkFreeMemory(vd->device, vi->memory, NULL);
	}

	if (vi->owned && vi->pixmap != XCB_NONE) {
		xcb_free_pixmap(base->c->c, vi->pixmap);
	}

	free(vi);
}

static image_handle vk_bind_pixmap(backend_t *base, xcb_pixmap_t pixmap,
	struct xvisual_info visual_info, bool owned) {
	auto vd = (struct vulkan_data *)base;

	log_debug("Binding pixmap %#08x...", pixmap);

	struct vulkan_image *vi = ccalloc(1, struct vulkan_image);
	vi->refcount = 1;
	vi->has_alpha = visual_info.alpha_size > 0;
	vi->pixmap = pixmap;
	vi->owned = owned;

	struct backend_image *bi = cmalloc(struct backend_image);
	bi->inner = (struct backend_image_inner_base *)vi;
	bi->opacity = 1.0;
	bi->dim = 0.0;
	bi->max_brightness = 1.0;
	bi->corner_radius = 0.0;
	bi->color_inverted = false;
	bi->border_width = 0;

	if (vd->bind_pixmap_strategy == BIND_PIXMAP_STRATEGY_DRI3) {
		if (!vk_bind_pixmap_dri3(vd, vi)) {
			goto err;
		}
	} else if (vd->bind_pixmap_strategy == BIND_PIXMAP_STRATEGY_SHM) {
		if (!vk_bind_pixmap_shm(vd, vi)) {
			goto err;
		}
	} else {
		assert(false);
	}

	bi->ewidth = vi->width;
	bi->eheight = vi->height;

	VkImageViewCreateInfo image_view_create_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.image = vi->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.components = {
			.r = VK_COMPONENT_SWIZZLE_B,
			.g = VK_COMPONENT_SWIZZLE_G,
			.b = VK_COMPONENT_SWIZZLE_R,
			.a = vi->has_alpha ? VK_COMPONENT_SWIZZLE_A : VK_COMPONENT_SWIZZLE_ONE
		},
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1
		}
	};

	if (vkCreateImageView(vd->device, &image_view_create_info, NULL, &vi->image_view)
		!= VK_SUCCESS) {
		log_error("Failed to create image view.");

		goto err;
	}

	VkDescriptorSetAllocateInfo descriptor_set_allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.pNext = NULL,
		.descriptorPool = vd->descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &vd->descriptor_set_layout
	};

	if (vkAllocateDescriptorSets(vd->device, &descriptor_set_allocate_info, &vi->descriptor_set)
		!= VK_SUCCESS) {
		log_error("Failed to allocate descriptor sets.");

		goto err;
	}

	VkDescriptorImageInfo descriptor_image_info = {
		.sampler = vd->sampler,
		.imageView = vi->image_view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	};

	VkWriteDescriptorSet write_descriptor_set = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = NULL,
		.dstSet = vi->descriptor_set,
		.dstBinding = 0,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &descriptor_image_info,
		.pBufferInfo = NULL,
		.pTexelBufferView = NULL
	};

	vkUpdateDescriptorSets(vd->device, 1, &write_descriptor_set, 0, NULL);

	if (vkWaitForFences(vd->device, 1, &vd->queue_submit_fence, VK_TRUE, UINT64_MAX)
		!= VK_SUCCESS) {
		log_error("Failed to wait for fences.");

		goto err;
	}

	if (vkResetFences(vd->device, 1, &vd->queue_submit_fence) != VK_SUCCESS) {
		log_error("Failed to reset fences.");

		goto err;
	}

	if (vkResetCommandBuffer(vd->command_buffer, 0) != VK_SUCCESS) {
		log_error("Failed to reset command buffer.");

		goto err;
	}

	VkCommandBufferBeginInfo command_buffer_begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.pNext = NULL,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		.pInheritanceInfo = NULL
	};

	if (vkBeginCommandBuffer(vd->command_buffer, &command_buffer_begin_info) != VK_SUCCESS) {
		log_error("Failed to begin command buffer.");

		goto err;
	}

	VkImageMemoryBarrier image_memory_barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.pNext = NULL,
		.srcAccessMask = VK_ACCESS_NONE,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = vi->image,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1
		}
	};

	vkCmdPipelineBarrier(vd->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &image_memory_barrier);

	if (vkEndCommandBuffer(vd->command_buffer) != VK_SUCCESS) {
		log_error("Failed to end command buffer.");

		goto err;
	}

	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = NULL,
		.waitSemaphoreCount = 0,
		.pWaitSemaphores = NULL,
		.pWaitDstStageMask = NULL,
		.commandBufferCount = 1,
		.pCommandBuffers = &vd->command_buffer,
		.signalSemaphoreCount = 0,
		.pSignalSemaphores = NULL
	};

	if (vkQueueSubmit(vd->queue, 1, &submit_info, vd->queue_submit_fence) != VK_SUCCESS) {
		log_error("Failed to queue submit.");

		goto err;
	}

	return (image_handle)bi;

err:
	vk_release_image(base, (image_handle)bi);

	return NULL;
}

static void vk_destroy_shadow_context(backend_t *base, struct backend_shadow_context *bsc) {
	default_destroy_shadow_context(base, bsc);
}

static struct backend_shadow_context *vk_create_shadow_context(backend_t *base, double radius) {
	return default_create_shadow_context(base, radius);
}

static image_handle vk_make_mask(backend_t *base, geometry_t size, const region_t *region) {
	(void)base;
	(void)size;
	(void)region;

	return NULL;
}

static int32_t vk_buffer_age(backend_t *base) {
	auto vd = (struct vulkan_data *)base;

	return vd->buffer_ages[vd->swapchain_image_index];
}

static bool vk_set_image_property(backend_t *base, enum image_properties property,
	image_handle image, const void *value) {
	return default_set_image_property(base, property, image, value);
}

struct backend_operations vulkan_ops = {
	.init = vk_init,
	.deinit = vk_deinit,
	.prepare = vk_prepare,
	.compose = vk_compose,
	.fill = vk_fill,
	.present = vk_present,
	.bind_pixmap = vk_bind_pixmap,
	.create_shadow_context = vk_create_shadow_context,
	.destroy_shadow_context = vk_destroy_shadow_context,
	.make_mask = vk_make_mask,
	.release_image = vk_release_image,
	.buffer_age = vk_buffer_age,
	.max_buffer_age = 5,
	.set_image_property = vk_set_image_property
};
