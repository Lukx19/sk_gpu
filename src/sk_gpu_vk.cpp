#include "sk_gpu_dev.h"
#ifdef SKG_VULKAN
///////////////////////////////////////////
// Vulkan Implementation                 //
///////////////////////////////////////////

#pragma comment(lib,"vulkan-1.lib")

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__linux__)
#include <X11/Xlib.h>
#define VK_USE_PLATFORM_XLIB_KHR
#endif

#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
static Display *vk_cached_display     = nullptr;
static void    *vk_cached_visual_info = nullptr;
static void    *vk_cached_fbconfig    = nullptr;
static Window   vk_cached_window      = 0;
#endif

static bool vk_debug_utils_instance_enabled = false;
static bool vk_debug_utils_naming_enabled   = false;
static bool vk_debug_utils_labels_enabled   = false;
static PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT_fn = nullptr;
static PFN_vkCmdBeginDebugUtilsLabelEXT vkCmdBeginDebugUtilsLabelEXT_fn = nullptr;
static PFN_vkCmdEndDebugUtilsLabelEXT   vkCmdEndDebugUtilsLabelEXT_fn   = nullptr;

///////////////////////////////////////////

#if defined(__linux__)
struct skg_linux_native_window_t {
        Display *display;
        Window   window;
};
#endif

template <typename T> struct array_t {
	T     *data;
	size_t count;
	size_t capacity;

	size_t      add        (const T &item)           { if (count+1 > capacity) { resize(capacity * 2 < 4 ? 4 : capacity * 2); } data[count] = item; count += 1; return count - 1; }
	void        insert     (size_t at, const T &item){ if (count+1 > capacity) resize(capacity<1?1:capacity*2); memmove(&data[at+1], &data[at], (count-at)*sizeof(T)); memcpy(&data[at], &item, sizeof(T)); count += 1;}
	void        trim       ()                        { resize(count); }
	void        remove     (size_t at)               { memmove(&data[at], &data[at+1], (count - (at + 1))*sizeof(T)); count -= 1; }
	void        pop        ()                        { remove(count - 1); }
	void        clear      ()                        { count = 0; }
	T          &last       () const                  { return data[count - 1]; }
	inline void set        (size_t id, const T &val) { data[id] = val; }
	inline T   &get        (size_t id) const         { return data[id]; }
	inline T   &operator[] (size_t id) const         { return data[id]; }
	void        reverse    ()                        { for(size_t i=0; i<count/2; i+=1) {T tmp = get(i);set(i, get(count-i-1));set(count-i-1, tmp);}};
	array_t<T>  copy       () const                  { array_t<T> result = {malloc(sizeof(T) * capacity),count,capacity}; memcpy(result.data, data, sizeof(T) * count); return result; }
	void        each       (void (*e)(T &))          { for (size_t i=0; i<count; i++) e(data[i]); }
	void        free       ()                        { ::free(data); *this = {}; }
	void        resize     (size_t to_capacity)      { if (count > to_capacity) count = to_capacity; void *old = data; void *new_mem = malloc(sizeof(T) * to_capacity); memcpy(new_mem, old, sizeof(T) * count); data = (T*)new_mem; ::free(old); capacity = to_capacity; }
	int64_t     index_of   (const T &item) const     { for (size_t i = 0; i < count; i++) if (memcmp(data[i], item, sizeof(T)) == 0) return i; return -1; }
	template <typename T, typename D>
	int64_t     index_of   (const D T::*key, const D &item) const { const size_t offset = (size_t)&((T*)0->*key); for (size_t i = 0; i < count; i++) if (memcmp(((uint8_t *)&data[i]) + offset, &item, sizeof(D)) == 0) return i; return -1; }
};

uint64_t hash_fnv64_data(const void *data, size_t data_size, uint64_t start_hash = 14695981039346656037) {
        uint64_t hash = start_hash;
        uint8_t *bytes = (uint8_t *)data;
        for (size_t i = 0; i < data_size; i++)
                hash = (hash ^ bytes[i]) * 1099511628211;
        return hash;
}

static bool vk_extension_supported(const VkExtensionProperties *props, uint32_t count, const char *name) {
        if (props == nullptr || name == nullptr)
                return false;
        for (uint32_t i = 0; i < count; i++)
                if (strcmp(props[i].extensionName, name) == 0)
                        return true;
        return false;
}

static void vk_assign_debug_name(char **dst, const char *name) {
        if (dst == nullptr)
                return;
        if (*dst != nullptr) {
                free(*dst);
                *dst = nullptr;
        }
        if (name == nullptr || name[0] == '\0')
                return;

        size_t len = strlen(name);
        char  *copy = (char *)malloc(len + 1);
        if (copy == nullptr)
                return;

        memcpy(copy, name, len + 1);
        *dst = copy;
}

static void vk_set_debug_name(VkObjectType type, uint64_t handle, const char *name) {
        if (!vk_debug_utils_naming_enabled || vkSetDebugUtilsObjectNameEXT_fn == nullptr)
                return;
        if (handle == 0 || name == nullptr || name[0] == '\0')
                return;

        VkDebugUtilsObjectNameInfoEXT info = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
        info.objectType   = type;
        info.objectHandle = handle;
        info.pObjectName  = name;
        vkSetDebugUtilsObjectNameEXT_fn(skg_device.device, &info);
}

static void vk_shader_apply_debug_name(skg_shader_t *shader);
static void vk_pipeline_apply_debug_name(skg_pipeline_t *pipeline);

//////////////////////////////////////

struct skg_device_t {
	VkSurfaceKHR     surface;
	VkPhysicalDevice phys_device;
	VkDevice         device;
	VkQueue          queue_gfx;
	VkQueue          queue_present;
	uint32_t         queue_gfx_index;
	uint32_t         queue_present_index;
};

struct vk_swapchain_t {
	VkSurfaceFormatKHR format;
	VkSwapchainKHR     swapchain;
	uint32_t           img_count;
	VkImage            imgs[4];
	VkExtent2D         extents;
};

skg_device_t skg_device = {};
VkPhysicalDeviceFeatures    vk_device_features   = {};
VkPhysicalDeviceProperties  vk_device_properties = {};
static char *vk_adapter_name = nullptr;

struct {
        bool tiled_multisample;
        bool discard_framebuffer;
        bool fmt_pvrtc1;
        bool fmt_pvrtc2;
        bool fmt_astc;
        bool fmt_atc;
        bool multiview;
        bool multiview_tiled_msaa;
} vk_device_caps = {};

//////////////////////////////////////
// Pipeline & Renderpass Info       //
//////////////////////////////////////

struct vk_renderpass_t {
	uint64_t     hash;
	int32_t      ref_count;
	VkRenderPass renderpass;
};
struct vk_pipeline_info_t {
	VkGraphicsPipelineCreateInfo           create;
	VkPipelineColorBlendStateCreateInfo    blend_info;
	VkPipelineColorBlendAttachmentState    blend_attch;
	VkPipelineShaderStageCreateInfo        shader_stages[2];
	VkPipelineVertexInputStateCreateInfo   vertex_info;
	VkPipelineInputAssemblyStateCreateInfo input_asm;
	VkPipelineRasterizationStateCreateInfo rasterizer;
	VkPipelineMultisampleStateCreateInfo   multisample;
	VkPipelineDepthStencilStateCreateInfo  depth_info;
	VkDynamicState                         dynamic_states[2];
	VkPipelineDynamicStateCreateInfo       dynamic_state;
};
struct vk_pipeline_t {
	uint64_t            hash;
	int32_t             ref_count;
	array_t<VkPipeline> pipelines;
	vk_pipeline_info_t  info;
};
array_t<vk_renderpass_t> vk_renderpass_cache = {};
array_t<vk_pipeline_t>   vk_pipeline_cache   = {};

struct vk_named_texture_t {
        skg_tex_t *tex;
        char      *name;
};
array_t<vk_named_texture_t> vk_named_textures = {};

static int32_t vk_named_texture_find_tex(const skg_tex_t *tex) {
        for (size_t i = 0; i < vk_named_textures.count; i++)
                if (vk_named_textures[i].tex == tex)
                        return (int32_t)i;
        return -1;
}

static int32_t vk_named_texture_find_name(const char *name) {
        if (name == nullptr)
                return -1;

        for (size_t i = 0; i < vk_named_textures.count; i++)
                if (vk_named_textures[i].name != nullptr && strcmp(vk_named_textures[i].name, name) == 0)
                        return (int32_t)i;
        return -1;
}

static void vk_named_texture_remove_index(size_t index) {
        if (index >= vk_named_textures.count)
                return;

        if (vk_named_textures[index].name != nullptr) {
                free(vk_named_textures[index].name);
                vk_named_textures[index].name = nullptr;
        }
        vk_named_textures.remove(index);
}

static void vk_named_texture_unregister(const skg_tex_t *tex) {
        int32_t index = vk_named_texture_find_tex(tex);
        if (index >= 0)
                vk_named_texture_remove_index((size_t)index);
}

static void vk_named_texture_unregister_name(const char *name) {
        int32_t index = vk_named_texture_find_name(name);
        if (index >= 0)
                vk_named_texture_remove_index((size_t)index);
}

static void vk_named_texture_clear() {
        for (size_t i = 0; i < vk_named_textures.count; i++) {
                if (vk_named_textures[i].name != nullptr) {
                        free(vk_named_textures[i].name);
                        vk_named_textures[i].name = nullptr;
                }
        }
        vk_named_textures.free();
}

//////////////////////////////////////
// Pipelines                        //
//////////////////////////////////////

uint64_t _vk_pipeline_hash(VkGraphicsPipelineCreateInfo &info) {
	uint64_t hash = hash_fnv64_data(&info.flags, sizeof(VkPipelineCreateFlags));
	hash = hash_fnv64_data(&info.basePipelineIndex, sizeof(int32_t),  hash);
	hash = hash_fnv64_data(&info.stageCount,        sizeof(uint32_t), hash);
	hash = hash_fnv64_data(&info.subpass,           sizeof(uint32_t), hash);
	if (info.pDepthStencilState ) hash = hash_fnv64_data(info.pDepthStencilState,  sizeof(VkPipelineDepthStencilStateCreateInfo),  hash);
	if (info.pInputAssemblyState) hash = hash_fnv64_data(info.pInputAssemblyState, sizeof(VkPipelineInputAssemblyStateCreateInfo), hash);
	if (info.pMultisampleState  ) hash = hash_fnv64_data(info.pMultisampleState,   sizeof(VkPipelineMultisampleStateCreateInfo),   hash);
	if (info.pRasterizationState) hash = hash_fnv64_data(info.pRasterizationState, sizeof(VkPipelineRasterizationStateCreateInfo), hash);
	for (size_t i = 0; i < info.stageCount; i++) {
		hash = hash_fnv64_data(&info.pStages[i], sizeof(VkPipelineShaderStageCreateInfo), hash);
	}
	return hash;
}
void _vk_pipeline_copy(vk_pipeline_info_t &dest, vk_pipeline_info_t &from) {
	memcpy(&dest, &from, sizeof(vk_pipeline_info_t));
	dest.dynamic_state.pDynamicStates = dest.dynamic_states;
	dest.blend_info.pAttachments = &dest.blend_attch;
	dest.create.pColorBlendState   = &dest.blend_info;
	dest.create.pDepthStencilState = &dest.depth_info;
	dest.create.pDynamicState      = &dest.dynamic_state;
	dest.create.pInputAssemblyState= &dest.input_asm;
	dest.create.pMultisampleState  = &dest.multisample;
	dest.create.pRasterizationState= &dest.rasterizer;
	dest.create.pStages            =  dest.shader_stages;
	//dest.create.pTessellationState
	dest.create.pVertexInputState  = &dest.vertex_info;
}
void _vk_pipelines_addpass(int64_t pass) {
	for (size_t i = 0; i < vk_pipeline_cache.count; i++) {
		while (vk_pipeline_cache[i].pipelines.count < pass) vk_pipeline_cache[i].pipelines.add({});
		vk_pipeline_cache[i].info.create.renderPass = vk_renderpass_cache[pass].renderpass;
		vkCreateGraphicsPipelines(skg_device.device, VK_NULL_HANDLE, 1, &vk_pipeline_cache[i].info.create, nullptr, &vk_pipeline_cache[i].pipelines[pass]);
	}
}
void _vk_pipelines_rempass(int64_t pass) {
	for (size_t i = 0; i < vk_pipeline_cache.count; i++) {
		if (vk_pipeline_cache[i].pipelines.count < pass) continue;
		vk_pipeline_cache[i].info.create.renderPass = vk_renderpass_cache[pass].renderpass;
		vkDestroyPipeline(skg_device.device, vk_pipeline_cache[i].pipelines[pass], nullptr);
	}
}

int64_t vk_pipeline_ref(vk_pipeline_info_t &info) {
	uint64_t hash  = _vk_pipeline_hash(info.create);
	int64_t  index = vk_pipeline_cache.index_of(&vk_pipeline_t::hash, hash);
	if (index < 0) {
		index = vk_pipeline_cache.count;
		vk_pipeline_cache.add({});
		vk_pipeline_cache[index].hash = hash;
		_vk_pipeline_copy(vk_pipeline_cache[index].info, info);
	}
	if (vk_pipeline_cache[index].ref_count == 0) {
		for (size_t i = 0; i < vk_renderpass_cache.count; i++) {
			VkPipeline pipeline = {};
			vk_pipeline_cache[index].info.create.renderPass = vk_renderpass_cache[i].renderpass;
			vkCreateGraphicsPipelines(skg_device.device, VK_NULL_HANDLE, 1, &vk_pipeline_cache[index].info.create, nullptr, &pipeline);
			vk_pipeline_cache[index].pipelines.add(pipeline);
		}
	}
	vk_pipeline_cache[index].ref_count += 1;
	return index;
}
void vk_pipeline_release(int64_t id) {
	vk_pipeline_cache[id].ref_count -= 1;
	if (vk_pipeline_cache[id].ref_count == 0) {
		for (size_t i = 0; i < vk_pipeline_cache[id].pipelines.count; i++) {
			vkDestroyPipeline(skg_device.device, vk_pipeline_cache[id].pipelines[i], nullptr);
		}
		vk_pipeline_cache[id].pipelines.free();
	}
}

//////////////////////////////////////
// Renderpasses                     //
//////////////////////////////////////

uint64_t _vk_renderpass_hash(VkRenderPassCreateInfo &info) {
	uint64_t hash = hash_fnv64_data(&info.flags, sizeof(VkRenderPassCreateFlags));
	hash = hash_fnv64_data(&info.attachmentCount, sizeof(uint32_t), hash);
	hash = hash_fnv64_data(&info.dependencyCount, sizeof(uint32_t), hash);
	hash = hash_fnv64_data(&info.subpassCount,    sizeof(uint32_t), hash);

	for (uint32_t i = 0; i < info.attachmentCount; i++) hash = hash_fnv64_data(&info.pAttachments [i], sizeof(VkAttachmentDescription), hash);
	for (uint32_t i = 0; i < info.dependencyCount; i++) hash = hash_fnv64_data(&info.pDependencies[i], sizeof(VkSubpassDependency), hash);
	for (uint32_t i = 0; i < info.subpassCount; i++) {
		const VkSubpassDescription *pass = &info.pSubpasses[i];
		hash = hash_fnv64_data(&pass->flags, sizeof(VkSubpassDescriptionFlags), hash);
		if (pass->pDepthStencilAttachment) hash = hash_fnv64_data(&pass->pDepthStencilAttachment, sizeof(VkAttachmentReference), hash);
		for (uint32_t t = 0; t < pass->colorAttachmentCount;    t++) hash = hash_fnv64_data(&pass->pColorAttachments[t], sizeof(VkAttachmentReference), hash);
		for (uint32_t t = 0; t < pass->inputAttachmentCount;    t++) hash = hash_fnv64_data(&pass->pInputAttachments[t], sizeof(VkAttachmentReference), hash);
		for (uint32_t t = 0; t < pass->preserveAttachmentCount; t++) hash = hash_fnv64_data(&pass->pPreserveAttachments[t], sizeof(uint32_t), hash);
	}
	return hash;
}
int64_t vk_renderpass_ref(VkRenderPassCreateInfo &info) {
	uint64_t hash  = _vk_renderpass_hash(info);
	int64_t  index = vk_renderpass_cache.index_of(&vk_renderpass_t::hash, hash);
	if (index < 0) {
		index = vk_renderpass_cache.count;
		vk_renderpass_cache.add({});
		vk_renderpass_cache[index].hash = hash;
	}
	if (vk_renderpass_cache[index].ref_count == 0) {
		vkCreateRenderPass(skg_device.device, &info, nullptr, &vk_renderpass_cache[index].renderpass);
		_vk_pipelines_addpass(index);
	}
	vk_renderpass_cache[index].ref_count += 1;
	return index;
}
void vk_renderpass_release(int64_t id) {
	vk_renderpass_cache[id].ref_count -= 1;
	if (vk_renderpass_cache[id].ref_count == 0) {
		_vk_pipelines_rempass(id);
		vkDestroyRenderPass(skg_device.device, vk_renderpass_cache[id].renderpass, nullptr);
	}
}

//////////////////////////////////////

#define D3D_FRAME_COUNT 2

VkInstance    vk_inst               = VK_NULL_HANDLE;
VkCommandPool vk_cmd_pool           = VK_NULL_HANDLE;
VkCommandPool vk_cmd_pool_transient = VK_NULL_HANDLE;

// Vertex layout info
VkVertexInputBindingDescription      skg_vert_bind    = { 0, sizeof(skg_vert_t), VK_VERTEX_INPUT_RATE_VERTEX };
VkVertexInputAttributeDescription    skg_vert_attrs[] = {
	{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(skg_vert_t, pos)  },
	{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(skg_vert_t, norm) },
	{ 2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(skg_vert_t, uv)   },
	{ 3, 0, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(skg_vert_t, col)  }, };
VkPipelineVertexInputStateCreateInfo skg_vertex_layout = { 
	VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
	1,                       &skg_vert_bind,
	_countof(skg_vert_attrs), skg_vert_attrs };

const skg_tex_t *skg_active_rendertarget = nullptr;
VkPipeline *vk_active_pipeline = nullptr;
VkPipelineLayout vk_active_graphics_pipeline_layout = VK_NULL_HANDLE;
static VkPipeline       vk_active_compute_pipeline        = VK_NULL_HANDLE;
static VkPipelineLayout vk_active_compute_pipeline_layout = VK_NULL_HANDLE;
static VkViewport vk_active_viewport = {0, 0, 0, 0, 0.0f, 1.0f};
static VkRect2D   vk_active_scissor  = {{0, 0}, {0, 0}};
static bool       vk_descriptor_dirty_graphics = true;
static bool       vk_descriptor_dirty_compute  = true;

static const uint32_t VK_MAX_UNIFORM_SLOTS  = 16;
static const uint32_t VK_MAX_TEXTURE_SLOTS  = 16;
static const uint32_t VK_MAX_STORAGE_SLOTS  = 8;
static const uint32_t VK_MAX_BUFFER_SLOTS   = 8;
static const uint32_t VK_BINDING_UNIFORMS   = 0;
static const uint32_t VK_BINDING_TEXTURES   = VK_BINDING_UNIFORMS + VK_MAX_UNIFORM_SLOTS;
static const uint32_t VK_BINDING_STORAGE    = VK_BINDING_TEXTURES + VK_MAX_TEXTURE_SLOTS;
static const uint32_t VK_BINDING_BUFFERS    = VK_BINDING_STORAGE + VK_MAX_STORAGE_SLOTS;
static const uint32_t VK_BINDING_RW_BUFFERS = VK_BINDING_BUFFERS + VK_MAX_BUFFER_SLOTS;
static const uint32_t VK_DESCRIPTOR_BINDING_COUNT = VK_BINDING_RW_BUFFERS + VK_MAX_BUFFER_SLOTS;

VkDescriptorSetLayout vk_descriptor_layout = VK_NULL_HANDLE;
VkDescriptorPool      vk_descriptor_pool   = VK_NULL_HANDLE;
VkDescriptorSet       vk_descriptor_set    = VK_NULL_HANDLE;
static VkBuffer       vk_dummy_storage_buffer = VK_NULL_HANDLE;
static VkDeviceMemory vk_dummy_storage_memory = VK_NULL_HANDLE;
static VkDescriptorBufferInfo vk_dummy_storage_info = {};
static VkImage        vk_dummy_image          = VK_NULL_HANDLE;
static VkDeviceMemory vk_dummy_image_memory   = VK_NULL_HANDLE;
static VkImageView    vk_dummy_image_view     = VK_NULL_HANDLE;
static VkSampler      vk_dummy_sampler        = VK_NULL_HANDLE;
static VkDescriptorImageInfo vk_dummy_texture_info = {};
static VkDescriptorImageInfo vk_dummy_storage_image_info = {};

static void vk_mark_descriptor_dirty(uint32_t stage_bits) {
        if (stage_bits & (skg_stage_vertex | skg_stage_pixel))
                vk_descriptor_dirty_graphics = true;
        if (stage_bits & skg_stage_compute)
                vk_descriptor_dirty_compute  = true;
}

static void vk_bind_descriptor_set(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipelineLayout layout) {
        if (cmd == VK_NULL_HANDLE || layout == VK_NULL_HANDLE)
                return;
        vkCmdBindDescriptorSets(cmd, bind_point, layout, 0, 1, &vk_descriptor_set, 0, nullptr);
}

static void vk_flush_descriptor_binding_graphics() {
        if (!vk_descriptor_dirty_graphics)
                return;
        if (skg_active_rendertarget == nullptr)
                return;

        VkCommandBuffer cmd = skg_active_rendertarget->rt_commandbuffer;
        if (cmd == VK_NULL_HANDLE)
                return;
        if (vk_active_graphics_pipeline_layout == VK_NULL_HANDLE)
                return;

        vk_bind_descriptor_set(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_active_graphics_pipeline_layout);
        vk_descriptor_dirty_graphics = false;
}

static void vk_flush_descriptor_binding_compute(VkCommandBuffer cmd) {
        if (!vk_descriptor_dirty_compute)
                return;
        if (cmd == VK_NULL_HANDLE)
                return;
        if (vk_active_compute_pipeline_layout == VK_NULL_HANDLE)
                return;

        vk_bind_descriptor_set(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk_active_compute_pipeline_layout);
        vk_descriptor_dirty_compute = false;
}

skg_tex_fmt_ skg_native_to_tex_fmt(VkFormat format);

///////////////////////////////////////////

bool vk_create_instance(const char *app_name, VkInstance *out_inst) {
        VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app_info.pApplicationName   = app_name;
        app_info.pEngineName        = "No Engine";
        app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
        app_info.apiVersion         = VK_API_VERSION_1_0;

        uint32_t ext_count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
        VkExtensionProperties *ext_props = nullptr;
        if (ext_count > 0) {
                ext_props = (VkExtensionProperties *)malloc(sizeof(VkExtensionProperties) * ext_count);
                vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, ext_props);
        }

#if defined(_WIN32)
        const char *platform_ext = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#elif defined(__linux__)
        const char *platform_ext = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
#else
        const char *platform_ext = nullptr;
#endif

        bool has_surface       = vk_extension_supported(ext_props, ext_count, VK_KHR_SURFACE_EXTENSION_NAME);
        bool has_platform      = platform_ext ? vk_extension_supported(ext_props, ext_count, platform_ext) : false;
        bool has_debug_report  = vk_extension_supported(ext_props, ext_count, VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
        bool has_debug_utils   = vk_extension_supported(ext_props, ext_count, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        array_t<const char*> instance_exts = {};
        if (has_surface) instance_exts.add(VK_KHR_SURFACE_EXTENSION_NAME);
        if (platform_ext && has_platform) instance_exts.add(platform_ext);

        if (!has_surface || (platform_ext && !has_platform)) {
                if (ext_props) free(ext_props);
                instance_exts.free();
                return false;
        }

        if (has_debug_report) instance_exts.add(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
        vk_debug_utils_instance_enabled = has_debug_utils;
        if (has_debug_utils) instance_exts.add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        const char *layers[] = {
                "VK_LAYER_KHRONOS_validation"
        };

        VkInstanceCreateInfo create_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        create_info.pApplicationInfo        = &app_info;
        create_info.enabledExtensionCount   = (uint32_t)instance_exts.count;
        create_info.ppEnabledExtensionNames = instance_exts.data;
        create_info.enabledLayerCount       = _countof(layers);
        create_info.ppEnabledLayerNames     = layers;

        VkResult result = vkCreateInstance(&create_info, 0, out_inst);

        if (ext_props) free(ext_props);
        instance_exts.free();

        if (result != VK_SUCCESS)
                return false;

        if (has_debug_report) {
                VkDebugReportCallbackCreateInfoEXT callback_info = { VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT };
                callback_info.flags =
                        VK_DEBUG_REPORT_WARNING_BIT_EXT |
                        VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT |
                        VK_DEBUG_REPORT_ERROR_BIT_EXT |
                        VK_DEBUG_REPORT_DEBUG_BIT_EXT;
                callback_info.pfnCallback = [](
                        VkDebugReportFlagsEXT,
                        VkDebugReportObjectTypeEXT,
                        uint64_t,
                        size_t,
                        int32_t,
                        const char*,
                        const char*                                 pMessage,
                        void*) {
                                skg_log(skg_log_info, pMessage);
                                return (VkBool32)VK_FALSE;
                };

                PFN_vkCreateDebugReportCallbackEXT debugCallback = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(*out_inst, "vkCreateDebugReportCallbackEXT");
                if (debugCallback != nullptr) {
                        VkDebugReportCallbackEXT callback = VK_NULL_HANDLE;
                        debugCallback(*out_inst, &callback_info, nullptr, &callback);
                }
        }

        return true;
}

///////////////////////////////////////////

bool vk_create_device(VkInstance inst, void *app_hwnd, skg_device_t *out_device) {
	*out_device = {};

#if defined(_WIN32)
	VkWin32SurfaceCreateInfoKHR surface_info = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
	surface_info.hinstance = GetModuleHandle(0);
	surface_info.hwnd      = (HWND)app_hwnd;
	if (vkCreateWin32SurfaceKHR(inst, &surface_info, nullptr, &out_device->surface) != VK_SUCCESS)
		return false;
#elif defined(__linux__)
        Display *display = nullptr;
        Window   window  = 0;
        if (app_hwnd != nullptr) {
                const skg_linux_native_window_t *native = (const skg_linux_native_window_t *)app_hwnd;
                display = native->display;
                window  = native->window;
        } else if (vk_cached_display != nullptr && vk_cached_window != 0) {
                display = vk_cached_display;
                window  = vk_cached_window;
        }
        if (display == nullptr || window == 0)
                return false;

	VkXlibSurfaceCreateInfoKHR surface_info = { VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
	surface_info.dpy    = display;
	surface_info.window = window;
	if (vkCreateXlibSurfaceKHR(inst, &surface_info, nullptr, &out_device->surface) != VK_SUCCESS)
		return false;
#else
	(void)app_hwnd;
#endif

	// Get physical device list
	uint32_t          device_count;
	VkPhysicalDevice *device_handles;
	vkEnumeratePhysicalDevices(inst, &device_count, 0);
	if (device_count == 0)
		return false;
	device_handles = (VkPhysicalDevice*)malloc(sizeof(VkPhysicalDevice) * device_count);
	vkEnumeratePhysicalDevices(inst, &device_count, device_handles);

	// Pick a physical device that meets our requirements
        VkQueueFamilyProperties         *queue_props;
        VkPhysicalDeviceProperties       device_props;
        VkPhysicalDeviceFeatures         device_features;
        VkPhysicalDeviceFeatures         best_features = {};
        VkPhysicalDeviceProperties       best_props    = {};
        int32_t          max_score         = 0;
        uint32_t         max_gfx_index     = 0;
        uint32_t         max_present_index = 0;
        VkPhysicalDevice max_device        = {};
        for (uint32_t i = 0; i < device_count; i++) {
		int32_t score = 10;
		int32_t gfx_index = -1;
		int32_t present_index = -1;

		// Check if it has a queue for presenting, and graphics
		uint32_t queue_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device_handles[i], &queue_count, NULL);
		queue_props = (VkQueueFamilyProperties*)malloc(sizeof(VkQueueFamilyProperties) * queue_count);
		vkGetPhysicalDeviceQueueFamilyProperties(device_handles[i], &queue_count, queue_props);
		for (uint32_t j = 0; j < queue_count; ++j) {
			VkBool32 supports_present = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(device_handles[i], j, out_device->surface, &supports_present);
			
			if (supports_present)
				present_index = j;

			if (queue_props[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)
				gfx_index = j;
		}

		// Get information about the device
		vkGetPhysicalDeviceProperties      (device_handles[i], &device_props);
                vkGetPhysicalDeviceFeatures        (device_handles[i], &device_features);

                // Score the device
                if (device_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                        score += 1000;
                score += device_props.limits.maxImageDimension2D;
		if (gfx_index == -1 || present_index == -1)
			score = 0;

		// And record it if it was the best scoring device
		free(queue_props);
                if (score > max_score) {
                        max_score         = score;
                        max_gfx_index     = gfx_index;
                        max_present_index = present_index;
                        max_device        = device_handles[i];
                        best_props        = device_props;
                        best_features     = device_features;
                }
        }
        out_device->phys_device         = max_device;
        out_device->queue_gfx_index     = max_gfx_index;
        out_device->queue_present_index = max_present_index;
        free(device_handles);
        if (max_score == 0)
                return false;

        if (vk_adapter_name) {
                free(vk_adapter_name);
                vk_adapter_name = nullptr;
        }
        size_t adapter_len = strlen(best_props.deviceName);
        vk_adapter_name = (char*)malloc(adapter_len + 1);
        memcpy(vk_adapter_name, best_props.deviceName, adapter_len + 1);
        vk_device_features   = best_features;
        vk_device_properties = best_props;

        // Create a logical device from the physical device
        const float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo device_queue_info[2] = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	device_queue_info[0] = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	device_queue_info[0].queueFamilyIndex = out_device->queue_gfx_index;
	device_queue_info[0].queueCount       = 1;
	device_queue_info[0].pQueuePriorities = &queue_priority;
	device_queue_info[1] = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	device_queue_info[1].queueFamilyIndex = out_device->queue_present_index;
	device_queue_info[1].queueCount       = 1;
	device_queue_info[1].pQueuePriorities = &queue_priority;

        uint32_t device_ext_count = 0;
        vkEnumerateDeviceExtensionProperties(out_device->phys_device, nullptr, &device_ext_count, nullptr);
        VkExtensionProperties *device_ext_props = nullptr;
        if (device_ext_count > 0) {
                device_ext_props = (VkExtensionProperties *)malloc(sizeof(VkExtensionProperties) * device_ext_count);
                vkEnumerateDeviceExtensionProperties(out_device->phys_device, nullptr, &device_ext_count, device_ext_props);
        }

        bool has_swapchain_ext = vk_extension_supported(device_ext_props, device_ext_count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        bool has_debug_utils   = vk_extension_supported(device_ext_props, device_ext_count, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#if defined(VK_EXT_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_EXTENSION_NAME)
        bool has_render_to_single = vk_extension_supported(device_ext_props, device_ext_count, VK_EXT_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_EXTENSION_NAME);
#else
        bool has_render_to_single = vk_extension_supported(device_ext_props, device_ext_count, "VK_EXT_multisampled_render_to_single_sampled");
#endif
#if defined(VK_KHR_MULTIVIEW_EXTENSION_NAME)
        bool has_multiview_ext = vk_extension_supported(device_ext_props, device_ext_count, VK_KHR_MULTIVIEW_EXTENSION_NAME);
#else
        bool has_multiview_ext = vk_extension_supported(device_ext_props, device_ext_count, "VK_KHR_multiview");
#endif
#if defined(VK_IMG_FORMAT_PVRTC_EXTENSION_NAME)
        bool has_pvrtc_ext = vk_extension_supported(device_ext_props, device_ext_count, VK_IMG_FORMAT_PVRTC_EXTENSION_NAME);
#else
        bool has_pvrtc_ext = vk_extension_supported(device_ext_props, device_ext_count, "VK_IMG_format_pvrtc");
#endif
#if defined(VK_AMD_TEXTURE_COMPRESSION_ATC_EXTENSION_NAME)
        bool has_atc_ext = vk_extension_supported(device_ext_props, device_ext_count, VK_AMD_TEXTURE_COMPRESSION_ATC_EXTENSION_NAME);
#else
        bool has_atc_ext = vk_extension_supported(device_ext_props, device_ext_count, "VK_AMD_texture_compression_ATC");
#endif
#if defined(VK_KHR_TEXTURE_COMPRESSION_ASTC_LDR_EXTENSION_NAME)
        bool has_astc_ext = vk_extension_supported(device_ext_props, device_ext_count, VK_KHR_TEXTURE_COMPRESSION_ASTC_LDR_EXTENSION_NAME);
#else
        bool has_astc_ext = vk_extension_supported(device_ext_props, device_ext_count, "VK_KHR_texture_compression_astc_ldr");
#endif
        if (device_ext_props) free(device_ext_props);
        if (!has_swapchain_ext)
                return false;

        vk_device_caps.tiled_multisample      = has_render_to_single;
        vk_device_caps.discard_framebuffer    = true;
        vk_device_caps.fmt_pvrtc1             = has_pvrtc_ext;
        vk_device_caps.fmt_pvrtc2             = has_pvrtc_ext;
        vk_device_caps.fmt_atc                = has_atc_ext;
        vk_device_caps.multiview              = has_multiview_ext;
        vk_device_caps.multiview_tiled_msaa   = has_multiview_ext && has_render_to_single;

        VkFormatProperties astc_unorm = {};
        VkFormatProperties astc_srgb  = {};
        vkGetPhysicalDeviceFormatProperties(out_device->phys_device, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &astc_unorm);
        vkGetPhysicalDeviceFormatProperties(out_device->phys_device, VK_FORMAT_ASTC_4x4_SRGB_BLOCK,  &astc_srgb);
        VkFormatFeatureFlags astc_features = astc_unorm.optimalTilingFeatures | astc_srgb.optimalTilingFeatures;
        vk_device_caps.fmt_astc = has_astc_ext || (astc_features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;

        array_t<const char*> enabled_exts = {};
        enabled_exts.add(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        bool enable_debug_utils = vk_debug_utils_instance_enabled && has_debug_utils;
        if (enable_debug_utils)
                enabled_exts.add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        vkSetDebugUtilsObjectNameEXT_fn = nullptr;
        vkCmdBeginDebugUtilsLabelEXT_fn = nullptr;
        vkCmdEndDebugUtilsLabelEXT_fn   = nullptr;
        vk_debug_utils_naming_enabled   = false;
        vk_debug_utils_labels_enabled   = false;

        VkDeviceCreateInfo device_create_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        device_create_info.queueCreateInfoCount    = _countof(device_queue_info);
        device_create_info.pQueueCreateInfos       = device_queue_info;
        device_create_info.enabledExtensionCount   = (uint32_t)enabled_exts.count;
        device_create_info.ppEnabledExtensionNames = enabled_exts.data;
        device_create_info.pEnabledFeatures        = &vk_device_features;

        if (vkCreateDevice(out_device->phys_device, &device_create_info, NULL, &out_device->device) != VK_SUCCESS) {
                enabled_exts.free();
                return false;
        }

        if (enable_debug_utils) {
                vkSetDebugUtilsObjectNameEXT_fn = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetDeviceProcAddr(out_device->device, "vkSetDebugUtilsObjectNameEXT");
                vkCmdBeginDebugUtilsLabelEXT_fn = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetDeviceProcAddr(out_device->device, "vkCmdBeginDebugUtilsLabelEXT");
                vkCmdEndDebugUtilsLabelEXT_fn   = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetDeviceProcAddr(out_device->device, "vkCmdEndDebugUtilsLabelEXT");
                vk_debug_utils_naming_enabled = vkSetDebugUtilsObjectNameEXT_fn != nullptr;
                vk_debug_utils_labels_enabled = vkCmdBeginDebugUtilsLabelEXT_fn != nullptr && vkCmdEndDebugUtilsLabelEXT_fn != nullptr;
        }

        enabled_exts.free();

        vkGetDeviceQueue(out_device->device, out_device->queue_gfx_index,     0, &out_device->queue_gfx);
        vkGetDeviceQueue(out_device->device, out_device->queue_present_index, 0, &out_device->queue_present);
        return true;
}

///////////////////////////////////////////

VkSurfaceFormatKHR vk_get_preferred_fmt(skg_device_t &device) {
	VkSurfaceFormatKHR result;
	uint32_t format_count = 1;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device.phys_device, device.surface, &format_count, nullptr);
	VkSurfaceFormatKHR *formats = (VkSurfaceFormatKHR *)malloc(format_count * sizeof(VkSurfaceFormatKHR));
	vkGetPhysicalDeviceSurfaceFormatsKHR(device.phys_device, device.surface, &format_count, formats);
	result = formats[0];
	free(formats);
	result.format = result.format == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM : result.format;
	return result;
}

///////////////////////////////////////////

VkPresentModeKHR vk_get_presentation_mode(skg_device_t &device) {
        VkPresentModeKHR  result     = VK_PRESENT_MODE_FIFO_KHR; // always supported.
        uint32_t          mode_count = 0;
        VkPresentModeKHR *modes      = nullptr;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device.phys_device, device.surface, &mode_count, NULL);
	modes = (VkPresentModeKHR*)malloc(sizeof(VkPresentModeKHR) * mode_count);
	vkGetPhysicalDeviceSurfacePresentModesKHR(device.phys_device, device.surface, &mode_count, modes);

	for (uint32_t i = 0; i < mode_count; i++) {
		if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
			result = VK_PRESENT_MODE_MAILBOX_KHR;
			break;
		}
	}

        free(modes);
        return result;
}

///////////////////////////////////////////

static VkSamplerAddressMode vk_address_mode_from_skg(skg_tex_address_ address) {
        switch (address) {
        case skg_tex_address_clamp:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case skg_tex_address_mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case skg_tex_address_repeat:
        default:                     return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
}

static VkFilter vk_filter_from_sample(skg_tex_sample_ sample) {
        switch (sample) {
        case skg_tex_sample_point:       return VK_FILTER_NEAREST;
        case skg_tex_sample_linear:
        case skg_tex_sample_anisotropic:
        default:                         return VK_FILTER_LINEAR;
        }
}

static VkSamplerMipmapMode vk_mipmap_mode_from_sample(skg_tex_sample_ sample) {
        switch (sample) {
        case skg_tex_sample_point:       return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case skg_tex_sample_linear:
        case skg_tex_sample_anisotropic:
        default:                         return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }
}

static VkCompareOp vk_compare_op_from_skg(skg_sample_compare_ compare) {
        switch (compare) {
        case skg_sample_compare_less:           return VK_COMPARE_OP_LESS;
        case skg_sample_compare_less_or_eq:     return VK_COMPARE_OP_LESS_OR_EQUAL;
        case skg_sample_compare_greater:        return VK_COMPARE_OP_GREATER;
        case skg_sample_compare_greater_or_eq:  return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case skg_sample_compare_equal:          return VK_COMPARE_OP_EQUAL;
        case skg_sample_compare_not_equal:      return VK_COMPARE_OP_NOT_EQUAL;
        case skg_sample_compare_always:         return VK_COMPARE_OP_ALWAYS;
        case skg_sample_compare_never:          return VK_COMPARE_OP_NEVER;
        case skg_sample_compare_none:
        default:                                return VK_COMPARE_OP_ALWAYS;
        }
}

void skg_setup_xlib(void *dpy, void *vi, void *fbconfig, void *drawable) {
#if defined(__linux__)
        vk_cached_display     = (Display *)dpy;
        vk_cached_visual_info = vi;
        vk_cached_fbconfig    = fbconfig;
        if (drawable != nullptr)
                vk_cached_window = *(Window *)drawable;
        else
                vk_cached_window = 0;
#else
        (void)dpy;
        (void)vi;
        (void)fbconfig;
        (void)drawable;
#endif
}

int32_t skg_init(const char *app_name, void *app_hwnd, void *adapter_id) {
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;

        if (!vk_create_instance (app_name, &vk_inst)) return -1;
	if (!vk_create_device   (vk_inst, app_hwnd, &skg_device)) return -2;

	VkCommandPoolCreateInfo cmd_pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	cmd_pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cmd_pool_info.queueFamilyIndex = skg_device.queue_gfx_index;
	vkCreateCommandPool(skg_device.device, &cmd_pool_info, 0, &vk_cmd_pool);

	// A command pool for short-lived command buffers
	cmd_pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	cmd_pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	cmd_pool_info.queueFamilyIndex = skg_device.queue_gfx_index;
        vkCreateCommandPool(skg_device.device, &cmd_pool_info, 0, &vk_cmd_pool_transient);

        VkDescriptorSetLayoutBinding bindings[VK_DESCRIPTOR_BINDING_COUNT] = {};
        for (uint32_t i = 0; i < VK_MAX_UNIFORM_SLOTS; i++) {
                VkDescriptorSetLayoutBinding *binding = &bindings[VK_BINDING_UNIFORMS + i];
                binding->binding         = VK_BINDING_UNIFORMS + i;
                binding->descriptorCount = 1;
                binding->descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                binding->stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        }

        for (uint32_t i = 0; i < VK_MAX_TEXTURE_SLOTS; i++) {
                VkDescriptorSetLayoutBinding *binding = &bindings[VK_BINDING_TEXTURES + i];
                binding->binding         = VK_BINDING_TEXTURES + i;
                binding->descriptorCount = 1;
                binding->descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                binding->stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        }
        for (uint32_t i = 0; i < VK_MAX_STORAGE_SLOTS; i++) {
                VkDescriptorSetLayoutBinding *binding = &bindings[VK_BINDING_STORAGE + i];
                binding->binding         = VK_BINDING_STORAGE + i;
                binding->descriptorCount = 1;
                binding->descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                binding->stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        }
        for (uint32_t i = 0; i < VK_MAX_BUFFER_SLOTS; i++) {
                VkDescriptorSetLayoutBinding *binding = &bindings[VK_BINDING_BUFFERS + i];
                binding->binding         = VK_BINDING_BUFFERS + i;
                binding->descriptorCount = 1;
                binding->descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                binding->stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        }
        for (uint32_t i = 0; i < VK_MAX_BUFFER_SLOTS; i++) {
                VkDescriptorSetLayoutBinding *binding = &bindings[VK_BINDING_RW_BUFFERS + i];
                binding->binding         = VK_BINDING_RW_BUFFERS + i;
                binding->descriptorCount = 1;
                binding->descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                binding->stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layout_info.bindingCount = _countof(bindings);
        layout_info.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(skg_device.device, &layout_info, nullptr, &vk_descriptor_layout) != VK_SUCCESS) {
                skg_log(skg_log_critical, "failed to create descriptor layout!");
                return -3;
        }

        VkDescriptorPoolSize pool_sizes[] = {
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_MAX_UNIFORM_SLOTS },
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_MAX_TEXTURE_SLOTS },
                { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          VK_MAX_STORAGE_SLOTS },
                { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_MAX_BUFFER_SLOTS * 2 }
        };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.poolSizeCount = _countof(pool_sizes);
        pool_info.pPoolSizes    = pool_sizes;
        pool_info.maxSets       = 1;
        if (vkCreateDescriptorPool(skg_device.device, &pool_info, nullptr, &vk_descriptor_pool) != VK_SUCCESS) {
                skg_log(skg_log_critical, "failed to create descriptor pool!");
                return -4;
        }

        VkDescriptorSetAllocateInfo alloc_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc_info.descriptorPool     = vk_descriptor_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts        = &vk_descriptor_layout;
        if (vkAllocateDescriptorSets(skg_device.device, &alloc_info, &vk_descriptor_set) != VK_SUCCESS) {
                skg_log(skg_log_critical, "failed to allocate descriptor set!");
                return -5;
        }

        VkDeviceSize dummy_size = sizeof(uint32_t);
        vk_create_buffer(dummy_size,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &vk_dummy_storage_buffer, &vk_dummy_storage_memory);
        if (vk_dummy_storage_buffer != VK_NULL_HANDLE) {
                vk_dummy_storage_info.buffer = vk_dummy_storage_buffer;
                vk_dummy_storage_info.offset = 0;
                vk_dummy_storage_info.range  = dummy_size;
        }

        VkImageCreateInfo image_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        image_info.imageType     = VK_IMAGE_TYPE_2D;
        image_info.extent.width  = 1;
        image_info.extent.height = 1;
        image_info.extent.depth  = 1;
        image_info.mipLevels     = 1;
        image_info.arrayLayers   = 1;
        image_info.format        = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(skg_device.device, &image_info, nullptr, &vk_dummy_image) == VK_SUCCESS) {
                VkMemoryRequirements mem_reqs;
                vkGetImageMemoryRequirements(skg_device.device, vk_dummy_image, &mem_reqs);

                VkMemoryAllocateInfo alloc_info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                alloc_info.allocationSize  = mem_reqs.size;
                alloc_info.memoryTypeIndex = vk_find_mem_type(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (vkAllocateMemory(skg_device.device, &alloc_info, nullptr, &vk_dummy_image_memory) == VK_SUCCESS) {
                        vkBindImageMemory(skg_device.device, vk_dummy_image, vk_dummy_image_memory, 0);

                        VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                        view_info.image    = vk_dummy_image;
                        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                        view_info.format   = image_info.format;
                        view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                        view_info.subresourceRange.baseMipLevel   = 0;
                        view_info.subresourceRange.levelCount     = 1;
                        view_info.subresourceRange.baseArrayLayer = 0;
                        view_info.subresourceRange.layerCount     = 1;
                        if (vkCreateImageView(skg_device.device, &view_info, nullptr, &vk_dummy_image_view) == VK_SUCCESS) {
                                VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                                sampler_info.magFilter    = VK_FILTER_LINEAR;
                                sampler_info.minFilter    = VK_FILTER_LINEAR;
                                sampler_info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                                sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                                sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                                sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                                sampler_info.minLod       = 0.0f;
                                sampler_info.maxLod       = 0.0f;
                                sampler_info.maxAnisotropy = 1.0f;
                                if (vkCreateSampler(skg_device.device, &sampler_info, nullptr, &vk_dummy_sampler) != VK_SUCCESS) {
                                        vk_dummy_sampler = VK_NULL_HANDLE;
                                }

                                VkCommandBuffer cmd = vk_begin_transient_cmd();
                                if (cmd != VK_NULL_HANDLE) {
                                        vk_transition_image(cmd, vk_dummy_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
                                        vk_end_transient_cmd(cmd);
                                }

                                vk_dummy_texture_info.sampler     = vk_dummy_sampler;
                                vk_dummy_texture_info.imageView   = vk_dummy_image_view;
                                vk_dummy_texture_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                                vk_dummy_storage_image_info.sampler     = VK_NULL_HANDLE;
                                vk_dummy_storage_image_info.imageView   = vk_dummy_image_view;
                                vk_dummy_storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                        } else {
                                vkDestroyImage(skg_device.device, vk_dummy_image, nullptr);
                                vk_dummy_image = VK_NULL_HANDLE;
                                vkFreeMemory(skg_device.device, vk_dummy_image_memory, nullptr);
                                vk_dummy_image_memory = VK_NULL_HANDLE;
                        }
                } else {
                        vkDestroyImage(skg_device.device, vk_dummy_image, nullptr);
                        vk_dummy_image = VK_NULL_HANDLE;
                }
        }

        return 1;
}

///////////////////////////////////////////

const char* skg_adapter_name() {
        return vk_adapter_name ? vk_adapter_name : "Unknown";
}

///////////////////////////////////////////

void skg_shutdown() {
        vkDeviceWaitIdle(skg_device.device);
        if (vk_dummy_storage_buffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(skg_device.device, vk_dummy_storage_buffer, nullptr);
                vk_dummy_storage_buffer = VK_NULL_HANDLE;
        }
        if (vk_dummy_storage_memory != VK_NULL_HANDLE) {
                vkFreeMemory(skg_device.device, vk_dummy_storage_memory, nullptr);
                vk_dummy_storage_memory = VK_NULL_HANDLE;
        }
        vk_dummy_storage_info = {};
        if (vk_dummy_sampler != VK_NULL_HANDLE) {
                vkDestroySampler(skg_device.device, vk_dummy_sampler, nullptr);
                vk_dummy_sampler = VK_NULL_HANDLE;
        }
        if (vk_dummy_image_view != VK_NULL_HANDLE) {
                vkDestroyImageView(skg_device.device, vk_dummy_image_view, nullptr);
                vk_dummy_image_view = VK_NULL_HANDLE;
        }
        if (vk_dummy_image != VK_NULL_HANDLE) {
                vkDestroyImage(skg_device.device, vk_dummy_image, nullptr);
                vk_dummy_image = VK_NULL_HANDLE;
        }
        if (vk_dummy_image_memory != VK_NULL_HANDLE) {
                vkFreeMemory(skg_device.device, vk_dummy_image_memory, nullptr);
                vk_dummy_image_memory = VK_NULL_HANDLE;
        }
        vk_dummy_texture_info = {};
        vk_dummy_storage_image_info = {};
        if (vk_descriptor_pool)   vkDestroyDescriptorPool  (skg_device.device, vk_descriptor_pool,   nullptr);
        if (vk_descriptor_layout) vkDestroyDescriptorSetLayout(skg_device.device, vk_descriptor_layout, nullptr);
        vkDestroyCommandPool(skg_device.device, vk_cmd_pool_transient, 0);
        vkDestroyCommandPool(skg_device.device, vk_cmd_pool, 0);
        vkDestroySurfaceKHR(vk_inst, skg_device.surface, 0);
        vkDestroyDevice(skg_device.device, 0);
        vkDestroyInstance(vk_inst, 0);
        if (vk_adapter_name) {
                free(vk_adapter_name);
                vk_adapter_name = nullptr;
        }

        vk_named_texture_clear();
}

///////////////////////////////////////////

void skg_draw_begin() {
        // Reset cached pipeline handles so subsequent draw calls rebind the
        // appropriate state after any render-pass transitions. This mirrors
        // the D3D11 backend which re-establishes its pipeline state at the
        // start of each frame.
        vk_active_pipeline                    = nullptr;
        vk_active_graphics_pipeline_layout    = VK_NULL_HANDLE;
        vk_active_compute_pipeline            = VK_NULL_HANDLE;
        vk_active_compute_pipeline_layout     = VK_NULL_HANDLE;

        // Mark descriptors dirty so any resources bound before the new frame
        // will be re-applied the next time a pipeline is activated. This
        // ensures that descriptor sets are rebound after swapchain/image
        // transitions.
        vk_descriptor_dirty_graphics = true;
        vk_descriptor_dirty_compute  = true;

        // If a render target is already active (for example when the caller
        // begins a new pass without rebinding), restore the tracked viewport
        // and scissor rectangles so dynamic state remains valid.
        if (skg_active_rendertarget != nullptr &&
                skg_active_rendertarget->rt_commandbuffer != VK_NULL_HANDLE) {
                vkCmdSetViewport(skg_active_rendertarget->rt_commandbuffer, 0, 1, &vk_active_viewport);
                vkCmdSetScissor (skg_active_rendertarget->rt_commandbuffer, 0, 1, &vk_active_scissor);
        }
}

///////////////////////////////////////////

skg_platform_data_t skg_get_platform_data() {
        skg_platform_data_t result = {};
        result.instance            = vk_inst;
        result.phys_device         = skg_device.phys_device;
        result.device              = skg_device.device;
        result.queue_gfx           = skg_device.queue_gfx;
        result.queue_present       = skg_device.queue_present;
        result.queue_gfx_index     = skg_device.queue_gfx_index;
        result.queue_present_index = skg_device.queue_present_index;
        result.surface             = skg_device.surface;
        return result;
}

///////////////////////////////////////////

bool skg_capability(skg_cap_ capability) {
        switch (capability) {
        case skg_cap_tex_layer_select:           return vk_device_features.shaderOutputLayer == VK_TRUE;
        case skg_cap_wireframe:                  return vk_device_features.fillModeNonSolid == VK_TRUE;
        case skg_cap_tiled_multisample:          return vk_device_caps.tiled_multisample;
        case skg_cap_discard_framebuffer:        return vk_device_caps.discard_framebuffer;
        case skg_cap_fmt_pvrtc1:                 return vk_device_caps.fmt_pvrtc1;
        case skg_cap_fmt_pvrtc2:                 return vk_device_caps.fmt_pvrtc2;
        case skg_cap_fmt_astc:                   return vk_device_caps.fmt_astc;
        case skg_cap_fmt_atc:                    return vk_device_caps.fmt_atc;
        case skg_cap_multiview:                  return vk_device_caps.multiview;
        case skg_cap_multiview_tiled_multisample:return vk_device_caps.multiview_tiled_msaa;
        default:                                 return false;
        }
}

///////////////////////////////////////////

void skg_event_begin(const char *name) {
        if (!vk_debug_utils_labels_enabled || name == nullptr)
                return;
        VkCommandBuffer cmd = (skg_active_rendertarget != nullptr) ? skg_active_rendertarget->rt_commandbuffer : VK_NULL_HANDLE;
        if (cmd == VK_NULL_HANDLE)
                return;
        VkDebugUtilsLabelEXT label = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
        label.pLabelName = name;
        vkCmdBeginDebugUtilsLabelEXT_fn(cmd, &label);
}

///////////////////////////////////////////

void skg_event_end() {
        if (!vk_debug_utils_labels_enabled)
                return;
        VkCommandBuffer cmd = (skg_active_rendertarget != nullptr) ? skg_active_rendertarget->rt_commandbuffer : VK_NULL_HANDLE;
        if (cmd == VK_NULL_HANDLE)
                return;
        vkCmdEndDebugUtilsLabelEXT_fn(cmd);
}

///////////////////////////////////////////

void skg_viewport(const int32_t *xywh) {
        if (xywh == nullptr)
                return;

        vk_active_viewport.x        = (float)xywh[0];
        vk_active_viewport.y        = (float)xywh[1];
        vk_active_viewport.width    = (float)xywh[2];
        vk_active_viewport.height   = (float)xywh[3];
        vk_active_viewport.minDepth = 0.0f;
        vk_active_viewport.maxDepth = 1.0f;

        if (skg_active_rendertarget && skg_active_rendertarget->rt_commandbuffer != VK_NULL_HANDLE) {
                vkCmdSetViewport(skg_active_rendertarget->rt_commandbuffer, 0, 1, &vk_active_viewport);
        }
}

///////////////////////////////////////////

void skg_viewport_get(int32_t *out_xywh) {
        if (out_xywh == nullptr)
                return;
        out_xywh[0] = (int32_t)vk_active_viewport.x;
        out_xywh[1] = (int32_t)vk_active_viewport.y;
        out_xywh[2] = (int32_t)vk_active_viewport.width;
        out_xywh[3] = (int32_t)vk_active_viewport.height;
}

///////////////////////////////////////////

void skg_scissor(const int32_t *xywh) {
        if (xywh == nullptr)
                return;

        vk_active_scissor.offset.x      = xywh[0];
        vk_active_scissor.offset.y      = xywh[1];
        vk_active_scissor.extent.width  = (uint32_t)xywh[2];
        vk_active_scissor.extent.height = (uint32_t)xywh[3];

        if (skg_active_rendertarget && skg_active_rendertarget->rt_commandbuffer != VK_NULL_HANDLE) {
                vkCmdSetScissor(skg_active_rendertarget->rt_commandbuffer, 0, 1, &vk_active_scissor);
        }
}

///////////////////////////////////////////

void skg_target_clear(bool depth, const float *clear_color_4) {
        if (skg_active_rendertarget == nullptr || skg_active_rendertarget->rt_commandbuffer == VK_NULL_HANDLE)
                return;

        VkClearAttachment attachments[2];
        uint32_t attachment_count = 0;

        if (clear_color_4 != nullptr) {
                attachments[attachment_count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                attachments[attachment_count].colorAttachment = 0;
                attachments[attachment_count].clearValue.color.float32[0] = clear_color_4[0];
                attachments[attachment_count].clearValue.color.float32[1] = clear_color_4[1];
                attachments[attachment_count].clearValue.color.float32[2] = clear_color_4[2];
                attachments[attachment_count].clearValue.color.float32[3] = clear_color_4[3];
                attachment_count++;
        }

        if (depth) {
                attachments[attachment_count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                attachments[attachment_count].colorAttachment = 0;
                attachments[attachment_count].clearValue.depthStencil.depth   = 1.0f;
                attachments[attachment_count].clearValue.depthStencil.stencil = 0;
                attachment_count++;
        }

        if (attachment_count == 0)
                return;

        VkClearRect clear_rect = {};
        clear_rect.rect = vk_active_scissor;
        if (clear_rect.rect.extent.width == 0 || clear_rect.rect.extent.height == 0) {
                clear_rect.rect.offset = {0, 0};
                clear_rect.rect.extent.width  = (uint32_t)skg_active_rendertarget->width;
                clear_rect.rect.extent.height = (uint32_t)skg_active_rendertarget->height;
        }
        clear_rect.baseArrayLayer = 0;
        clear_rect.layerCount     = 1;

        vkCmdClearAttachments(skg_active_rendertarget->rt_commandbuffer, attachment_count, attachments, 1, &clear_rect);
}

///////////////////////////////////////////

void skg_tex_target_discard(skg_tex_t *render_target) {
        if (render_target == nullptr)
                return;
        if (render_target->texture == VK_NULL_HANDLE) {
                render_target->layout = VK_IMAGE_LAYOUT_UNDEFINED;
                return;
        }

        VkImageLayout current_layout = render_target->layout != VK_IMAGE_LAYOUT_UNDEFINED
                ? render_target->layout
                : vk_target_layout_for_tex(render_target);
        VkImageAspectFlags aspect = vk_aspect_from_format(render_target->format);
        uint32_t mip_levels = render_target->mip_count   > 0 ? render_target->mip_count   : 1u;
        uint32_t layers     = render_target->array_count > 0 ? render_target->array_count : 1u;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        bool transitioned = false;
        if (skg_active_rendertarget != nullptr) {
                if (skg_active_rendertarget == render_target) {
                        cmd = render_target->rt_commandbuffer;
                        transitioned = cmd != VK_NULL_HANDLE;
                } else if (skg_active_rendertarget->rt_depth_tex == render_target) {
                        cmd = skg_active_rendertarget->rt_commandbuffer;
                        transitioned = cmd != VK_NULL_HANDLE;
                }
        }

        if (cmd != VK_NULL_HANDLE) {
                vk_transition_image(cmd, render_target->texture, current_layout, VK_IMAGE_LAYOUT_UNDEFINED, aspect, mip_levels, layers);
        } else {
                cmd = vk_begin_transient_cmd();
                if (cmd != VK_NULL_HANDLE) {
                        vk_transition_image(cmd, render_target->texture, current_layout, VK_IMAGE_LAYOUT_UNDEFINED, aspect, mip_levels, layers);
                        vk_end_transient_cmd(cmd);
                        transitioned = true;
                }
        }

        if (!transitioned)
                return;

        render_target->layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

///////////////////////////////////////////

void skg_tex_target_bind(float clear_color[4], const skg_tex_t *render_target, const skg_tex_t *depth_target) {
        if (render_target == nullptr || render_target->rt_commandbuffer == VK_NULL_HANDLE)
                return;

        skg_tex_t *mutable_target = (skg_tex_t *)render_target;
        if (mutable_target->rt_renderpass < 0) {
                if (depth_target != nullptr)
                        skg_tex_attach_depth(mutable_target, (skg_tex_t *)depth_target);
                else
                        return;
        } else if (depth_target != nullptr && mutable_target->rt_depth_view != depth_target->view) {
                skg_tex_attach_depth(mutable_target, (skg_tex_t *)depth_target);
        }

        skg_active_rendertarget = mutable_target;

        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        beginInfo.pInheritanceInfo = nullptr;

        if (vkBeginCommandBuffer(mutable_target->rt_commandbuffer, &beginInfo) != VK_SUCCESS) {
                skg_log(skg_log_critical, "failed to begin recording command buffer!");
                return;
        }

        VkClearValue clear_values[2] = {};
        if (clear_color) {
                memcpy(clear_values[0].color.float32, clear_color, sizeof(float) * 4);
        } else {
                clear_values[0].color.float32[0] = 0.0f;
                clear_values[0].color.float32[1] = 0.0f;
                clear_values[0].color.float32[2] = 0.0f;
                clear_values[0].color.float32[3] = 1.0f;
        }
        if (mutable_target->rt_depth_view != VK_NULL_HANDLE) {
                clear_values[1].depthStencil.depth   = 1.0f;
                clear_values[1].depthStencil.stencil = 0;
        }

        VkRenderPassBeginInfo renderPassInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        renderPassInfo.renderPass  = vk_renderpass_cache[mutable_target->rt_renderpass].renderpass;
        renderPassInfo.framebuffer = mutable_target->rt_framebuffer;
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent.width  = (uint32_t)mutable_target->width;
        renderPassInfo.renderArea.extent.height = (uint32_t)mutable_target->height;
        renderPassInfo.clearValueCount = mutable_target->rt_depth_view != VK_NULL_HANDLE ? 2u : 1u;
        renderPassInfo.pClearValues    = clear_values;

        vkCmdBeginRenderPass(mutable_target->rt_commandbuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport = {};
        viewport.x        = 0.0f;
        viewport.y        = 0.0f;
        viewport.width    = (float)mutable_target->width;
        viewport.height   = (float)mutable_target->height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor = {};
        scissor.offset = {0, 0};
        scissor.extent.width  = (uint32_t)mutable_target->width;
        scissor.extent.height = (uint32_t)mutable_target->height;

        vkCmdSetViewport(mutable_target->rt_commandbuffer, 0, 1, &viewport);
        vkCmdSetScissor (mutable_target->rt_commandbuffer, 0, 1, &scissor);
}

///////////////////////////////////////////

void skg_tex_target_bind(skg_tex_t *render_target, int32_t layer_idx, int32_t mip_level) {
        (void)layer_idx;
        (void)mip_level;

        if (render_target == nullptr) {
                skg_active_rendertarget = nullptr;
                return;
        }

        if (render_target->type != skg_tex_type_rendertarget) {
                skg_log(skg_log_warning, "Vulkan backend expects a color render target when binding framebuffers");
                return;
        }

        skg_tex_t *depth_tex = render_target->rt_depth_tex;
        skg_tex_target_bind(nullptr, render_target, depth_tex);
}

///////////////////////////////////////////

skg_tex_t *skg_tex_target_get() {
        return (skg_tex_t*)skg_active_rendertarget;
}

///////////////////////////////////////////

void skg_draw(int32_t index_start, int32_t index_base, int32_t index_count, int32_t instance_count) {
        VkCommandBuffer cmd = skg_active_rendertarget ? skg_active_rendertarget->rt_commandbuffer : VK_NULL_HANDLE;
        if (cmd == VK_NULL_HANDLE)
                return;
        // D3D11 issues DrawIndexedInstanced with an index base bias. Vulkan exposes the same
        // behaviour via the firstIndex and vertexOffset parameters.
        vkCmdDrawIndexed(cmd, index_count, instance_count, index_start, index_base, 0);
}

///////////////////////////////////////////

void skg_compute(uint32_t thread_count_x, uint32_t thread_count_y, uint32_t thread_count_z) {
        if (vk_active_compute_pipeline == VK_NULL_HANDLE || vk_active_compute_pipeline_layout == VK_NULL_HANDLE)
                return;

        VkCommandBufferAllocateInfo alloc_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc_info.commandPool        = vk_cmd_pool_transient;
        alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(skg_device.device, &alloc_info, &cmd) != VK_SUCCESS) {
                skg_log(skg_log_critical, "Failed to allocate compute command buffer");
                return;
        }

        VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
                skg_log(skg_log_critical, "Failed to begin compute command buffer");
                vkFreeCommandBuffers(skg_device.device, vk_cmd_pool_transient, 1, &cmd);
                return;
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk_active_compute_pipeline);
        vk_flush_descriptor_binding_compute(cmd);
        vkCmdDispatch(cmd, thread_count_x, thread_count_y, thread_count_z);

        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit_info = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers    = &cmd;

        if (vkQueueSubmit(skg_device.queue_gfx, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
                skg_log(skg_log_critical, "Failed to submit compute work");
        }
        vkQueueWaitIdle(skg_device.queue_gfx);

        vkFreeCommandBuffers(skg_device.device, vk_cmd_pool_transient, 1, &cmd);
}

///////////////////////////////////////////
// Buffer                                //
///////////////////////////////////////////

int32_t vk_find_mem_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(skg_device.phys_device, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((type_filter & (1 << i)) && (props.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}
	return -1;
}

void vk_create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer *out_buffer, VkDeviceMemory *out_memory) {
	VkBufferCreateInfo buff_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buff_info.size        = size;
	buff_info.usage       = usage;
	buff_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if (vkCreateBuffer(skg_device.device, &buff_info, nullptr, out_buffer) != VK_SUCCESS) {
		skg_log(skg_log_critical, "failed to create buffer!");
	}

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(skg_device.device, *out_buffer, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	allocInfo.allocationSize  = memRequirements.size;
	allocInfo.memoryTypeIndex = vk_find_mem_type(memRequirements.memoryTypeBits, properties);

	if (vkAllocateMemory(skg_device.device, &allocInfo, nullptr, out_memory) != VK_SUCCESS) {
		skg_log(skg_log_critical, "failed to allocate buffer memory!");
	}

	vkBindBufferMemory(skg_device.device, *out_buffer, *out_memory, 0);
}

void vk_copy_buffer(VkBuffer src, VkBuffer dest, VkDeviceSize size) {
	VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool        = vk_cmd_pool_transient;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer;
	vkAllocateCommandBuffers(skg_device.device, &allocInfo, &commandBuffer);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	VkBufferCopy copyRegion = {};
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, src, dest, 1, &copyRegion);

	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;

	vkQueueSubmit  (skg_device.queue_gfx, 1, &submitInfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(skg_device.queue_gfx);
	vkFreeCommandBuffers(skg_device.device, vk_cmd_pool_transient, 1, &commandBuffer);
}

static VkCommandBuffer vk_begin_transient_cmd() {
        VkCommandBufferAllocateInfo alloc_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandPool        = vk_cmd_pool_transient;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(skg_device.device, &alloc_info, &cmd) != VK_SUCCESS)
                return VK_NULL_HANDLE;

        VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
                vkFreeCommandBuffers(skg_device.device, vk_cmd_pool_transient, 1, &cmd);
                return VK_NULL_HANDLE;
        }
        return cmd;
}

static void vk_end_transient_cmd(VkCommandBuffer cmd) {
        if (cmd == VK_NULL_HANDLE)
                return;
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit_info = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers    = &cmd;
        vkQueueSubmit  (skg_device.queue_gfx, 1, &submit_info, VK_NULL_HANDLE);
        vkQueueWaitIdle(skg_device.queue_gfx);

        vkFreeCommandBuffers(skg_device.device, vk_cmd_pool_transient, 1, &cmd);
}

static void vk_transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkImageAspectFlags aspect, uint32_t mip_levels, uint32_t layer_count) {
        if (cmd == VK_NULL_HANDLE)
                return;

        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout           = old_layout;
        barrier.newLayout           = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = image;
        barrier.subresourceRange.aspectMask     = aspect;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = mip_levels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = layer_count;

        VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

        switch (old_layout) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                src_stage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                break;
        default:
                barrier.srcAccessMask = 0;
                break;
        }

        switch (new_layout) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                dst_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
                break;
        case VK_IMAGE_LAYOUT_GENERAL:
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                break;
        default:
                barrier.dstAccessMask = 0;
                break;
        }

        vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

skg_buffer_t skg_buffer_create(const void *data, uint32_t size_count, uint32_t size_stride, skg_buffer_type_ type, skg_use_ use) {
        skg_buffer_t result = {};
        result.type   = type;
        result.use    = use;
        result.stride = size_stride;
        result.count  = size_count;

        uint32_t size_bytes = size_count * size_stride;

        VkBufferUsageFlags usage = 0;
        switch (type) {
        case skg_buffer_type_vertex:   usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;  break;
        case skg_buffer_type_index:    usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;   break;
        case skg_buffer_type_constant: usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; break;
        case skg_buffer_type_compute:  usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; break;
        }

        if ((use & skg_use_compute_read) != 0 || (use & skg_use_compute_write) != 0)
                usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        if (usage == 0)
                usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

        if (use == skg_use_static) {
                VkBuffer       stage_buffer;
                VkDeviceMemory stage_memory;
                vk_create_buffer(size_bytes,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &stage_buffer, &stage_memory);

                void *gpu_data;
                vkMapMemory(skg_device.device, stage_memory, 0, size_bytes, 0, &gpu_data);
                if (data != nullptr) {
                        memcpy(gpu_data, data, (size_t)size_bytes);
                } else {
                        memset(gpu_data, 0, (size_t)size_bytes);
                }
                vkUnmapMemory(skg_device.device, stage_memory);

                vk_create_buffer(size_bytes,
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        usage,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &result.buffer, &result.memory);

                vk_copy_buffer(stage_buffer, result.buffer, size_bytes);

                vkDestroyBuffer(skg_device.device, stage_buffer, nullptr);
                vkFreeMemory(skg_device.device, stage_memory, nullptr);
        } else {
                vk_create_buffer(size_bytes,
                        usage,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &result.buffer, &result.memory);

                if (data != nullptr)
                        skg_buffer_set_contents(&result, data, size_bytes);
        }

        if (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) {
                result.descriptor_uniform.buffer = result.buffer;
                result.descriptor_uniform.offset = 0;
                result.descriptor_uniform.range  = size_bytes;
        }
        if (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
                result.descriptor_storage.buffer = result.buffer;
                result.descriptor_storage.offset = 0;
                result.descriptor_storage.range  = size_bytes;
        }

        return result;
}

///////////////////////////////////////////

void skg_buffer_name(skg_buffer_t *buffer, const char *name) {
        if (buffer == nullptr)
                return;
        vk_set_debug_name(VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer->buffer, name);
}

///////////////////////////////////////////

bool skg_buffer_is_valid(const skg_buffer_t *buffer) {
        return buffer != nullptr && buffer->buffer != VK_NULL_HANDLE;
}

///////////////////////////////////////////

void skg_buffer_set_contents(skg_buffer_t *buffer, const void *data, uint32_t size_bytes) {
        if (buffer->use != skg_use_dynamic)
                return;

        void *gpu_data;
        vkMapMemory(skg_device.device, buffer->memory, 0, size_bytes, 0, &gpu_data);
        memcpy(gpu_data, data, (size_t)size_bytes);
        vkUnmapMemory(skg_device.device, buffer->memory);
}

///////////////////////////////////////////

void skg_buffer_get_contents(const skg_buffer_t *buffer, void *ref_buffer, uint32_t buffer_size) {
        if (!buffer || buffer->memory == VK_NULL_HANDLE || ref_buffer == nullptr)
                return;

        uint32_t available = buffer->stride * buffer->count;
        if (buffer_size > available)
                buffer_size = available;

        void *mapped = nullptr;
        if (vkMapMemory(skg_device.device, buffer->memory, 0, buffer_size, 0, &mapped) != VK_SUCCESS) {
                memset(ref_buffer, 0, buffer_size);
                return;
        }
        memcpy(ref_buffer, mapped, buffer_size);
        vkUnmapMemory(skg_device.device, buffer->memory);
}

///////////////////////////////////////////

void skg_buffer_bind(const skg_buffer_t *buffer, skg_bind_t bind) {
        if (buffer == nullptr || buffer->buffer == VK_NULL_HANDLE)
                return;

        VkCommandBuffer cmd = skg_active_rendertarget ? skg_active_rendertarget->rt_commandbuffer : VK_NULL_HANDLE;

        switch (bind.register_type) {
        case skg_register_vertex: {
                if (cmd == VK_NULL_HANDLE)
                        return;
                VkDeviceSize offsets[] = {0};
                VkBuffer     buffers[] = {buffer->buffer};
                vkCmdBindVertexBuffers(cmd, bind.slot, 1, buffers, offsets);
        } break;
        case skg_register_index:
                if (cmd != VK_NULL_HANDLE)
                        vkCmdBindIndexBuffer(cmd, buffer->buffer, 0, VK_INDEX_TYPE_UINT32);
                break;
        case skg_register_constant: {
                if (bind.slot >= VK_MAX_UNIFORM_SLOTS) {
                        skg_log(skg_log_warning, "Constant buffer slot exceeds Vulkan descriptor layout");
                        return;
                }
                if (buffer->descriptor_uniform.buffer == VK_NULL_HANDLE) {
                        skg_log(skg_log_warning, "Buffer lacks uniform descriptor info");
                        return;
                }
                VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                write.dstSet          = vk_descriptor_set;
                write.dstBinding      = VK_BINDING_UNIFORMS + bind.slot;
                write.descriptorCount = 1;
                write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                write.pBufferInfo     = &buffer->descriptor_uniform;
                vkUpdateDescriptorSets(skg_device.device, 1, &write, 0, nullptr);
                vk_mark_descriptor_dirty(bind.stage_bits);
                if (bind.stage_bits & (skg_stage_vertex | skg_stage_pixel))
                        vk_flush_descriptor_binding_graphics();
                if (bind.stage_bits & skg_stage_compute)
                        vk_flush_descriptor_binding_compute(cmd);
        } break;
        case skg_register_resource: {
                if (bind.slot >= VK_MAX_BUFFER_SLOTS) {
                        skg_log(skg_log_warning, "Buffer slot exceeds Vulkan descriptor layout");
                        return;
                }
                if (buffer->descriptor_storage.buffer == VK_NULL_HANDLE) {
                        skg_log(skg_log_warning, "Buffer lacks storage descriptor info");
                        return;
                }
                VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                write.dstSet          = vk_descriptor_set;
                write.dstBinding      = VK_BINDING_BUFFERS + bind.slot;
                write.descriptorCount = 1;
                write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo     = &buffer->descriptor_storage;
                vkUpdateDescriptorSets(skg_device.device, 1, &write, 0, nullptr);
                vk_mark_descriptor_dirty(bind.stage_bits);
                if (bind.stage_bits & (skg_stage_vertex | skg_stage_pixel))
                        vk_flush_descriptor_binding_graphics();
                if (bind.stage_bits & skg_stage_compute)
                        vk_flush_descriptor_binding_compute(cmd);
        } break;
        case skg_register_readwrite: {
                if (bind.slot >= VK_MAX_BUFFER_SLOTS) {
                        skg_log(skg_log_warning, "Buffer slot exceeds Vulkan descriptor layout");
                        return;
                }
                if (buffer->descriptor_storage.buffer == VK_NULL_HANDLE) {
                        skg_log(skg_log_warning, "Buffer lacks storage descriptor info");
                        return;
                }
                VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                write.dstSet          = vk_descriptor_set;
                write.dstBinding      = VK_BINDING_RW_BUFFERS + bind.slot;
                write.descriptorCount = 1;
                write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo     = &buffer->descriptor_storage;
                vkUpdateDescriptorSets(skg_device.device, 1, &write, 0, nullptr);
                vk_mark_descriptor_dirty(bind.stage_bits);
                if (bind.stage_bits & (skg_stage_vertex | skg_stage_pixel))
                        vk_flush_descriptor_binding_graphics();
                if (bind.stage_bits & skg_stage_compute)
                        vk_flush_descriptor_binding_compute(cmd);
        } break;
        default: break;
        }
}

///////////////////////////////////////////

void skg_buffer_clear(skg_bind_t bind) {
        if (vk_dummy_storage_buffer == VK_NULL_HANDLE)
                return;

        uint32_t binding_index = 0;
        switch (bind.register_type) {
        case skg_register_resource:
                if (bind.slot >= VK_MAX_BUFFER_SLOTS) {
                        skg_log(skg_log_warning, "Buffer slot exceeds Vulkan descriptor layout");
                        return;
                }
                binding_index = VK_BINDING_BUFFERS + bind.slot;
                break;
        case skg_register_readwrite:
                if (bind.slot >= VK_MAX_BUFFER_SLOTS) {
                        skg_log(skg_log_warning, "Buffer slot exceeds Vulkan descriptor layout");
                        return;
                }
                binding_index = VK_BINDING_RW_BUFFERS + bind.slot;
                break;
        default:
                return;
        }

        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = vk_descriptor_set;
        write.dstBinding      = binding_index;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo     = &vk_dummy_storage_info;
        vkUpdateDescriptorSets(skg_device.device, 1, &write, 0, nullptr);
        vk_mark_descriptor_dirty(bind.stage_bits);
        if (bind.stage_bits & (skg_stage_vertex | skg_stage_pixel))
                vk_flush_descriptor_binding_graphics();
        if (bind.stage_bits & skg_stage_compute)
                vk_flush_descriptor_binding_compute(skg_active_rendertarget ? skg_active_rendertarget->rt_commandbuffer : VK_NULL_HANDLE);
}

///////////////////////////////////////////

void skg_buffer_destroy(skg_buffer_t *buffer) {
        if (buffer->buffer != VK_NULL_HANDLE)
                vkDestroyBuffer(skg_device.device, buffer->buffer, nullptr);
        if (buffer->memory != VK_NULL_HANDLE)
                vkFreeMemory(skg_device.device, buffer->memory, nullptr);
        *buffer = {};
}

///////////////////////////////////////////
// Mesh                                  //
///////////////////////////////////////////

skg_mesh_t skg_mesh_create(const skg_buffer_t *vert_buffer, const skg_buffer_t *ind_buffer) {
        skg_mesh_t result = {};
        skg_mesh_set_verts(&result, vert_buffer);
        skg_mesh_set_inds (&result, ind_buffer);
        return result;
}

void skg_mesh_name(skg_mesh_t *mesh, const char *name) {
        if (mesh == nullptr || name == nullptr || name[0] == '\0')
                return;

        if (mesh->vert_buffer != VK_NULL_HANDLE) {
                char buffer_name[256];
                snprintf(buffer_name, sizeof(buffer_name), "%s_verts", name);
                vk_set_debug_name(VK_OBJECT_TYPE_BUFFER, (uint64_t)mesh->vert_buffer, buffer_name);
        }
        if (mesh->ind_buffer != VK_NULL_HANDLE) {
                char buffer_name[256];
                snprintf(buffer_name, sizeof(buffer_name), "%s_inds", name);
                vk_set_debug_name(VK_OBJECT_TYPE_BUFFER, (uint64_t)mesh->ind_buffer, buffer_name);
        }
}

void skg_mesh_set_verts(skg_mesh_t *mesh, const skg_buffer_t *vert_buffer) {
        mesh->vert_buffer = vert_buffer ? vert_buffer->buffer : VK_NULL_HANDLE;
}

void skg_mesh_set_inds(skg_mesh_t *mesh, const skg_buffer_t *ind_buffer) {
        mesh->ind_buffer = ind_buffer ? ind_buffer->buffer : VK_NULL_HANDLE;
}

void skg_mesh_bind(const skg_mesh_t *mesh) {
        VkCommandBuffer cmd = skg_active_rendertarget->rt_commandbuffer;

        if (mesh->vert_buffer != VK_NULL_HANDLE) {
                VkBuffer     buffers[] = { mesh->vert_buffer };
                VkDeviceSize offsets[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
        }
        if (mesh->ind_buffer != VK_NULL_HANDLE) {
                vkCmdBindIndexBuffer(cmd, mesh->ind_buffer, 0, VK_INDEX_TYPE_UINT32);
        }
}

void skg_mesh_destroy(skg_mesh_t *mesh) {
        *mesh = {};
}

///////////////////////////////////////////
// Shader Stage                          //
///////////////////////////////////////////

skg_shader_stage_t skg_shader_stage_create(const uint8_t *shader_data, size_t shader_size, skg_stage_ type) {
	skg_shader_stage_t result = {};
	result.type = type;

	VkShaderModuleCreateInfo shader_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	shader_info.codeSize = shader_size;
	shader_info.pCode    = (uint32_t*)shader_data;

	if (vkCreateShaderModule(skg_device.device, &shader_info, nullptr, &result.module) != VK_SUCCESS) {
		skg_log(skg_log_critical, "Failed to create shader module!");
	}

	return result;
}
void skg_shader_stage_destroy(skg_shader_stage_t *stage) {
        if (stage->module != VK_NULL_HANDLE)
                vkDestroyShaderModule(skg_device.device, stage->module, nullptr);
        *stage = {};
}

///////////////////////////////////////////
// Shader                                //
///////////////////////////////////////////

static void vk_shader_apply_debug_name(skg_shader_t *shader) {
        if (!vk_debug_utils_naming_enabled || shader == nullptr || shader->debug_name == nullptr)
                return;

        char name_buffer[256];
        if (shader->_vertex != VK_NULL_HANDLE) {
                snprintf(name_buffer, sizeof(name_buffer), "%s_vs", shader->debug_name);
                vk_set_debug_name(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)shader->_vertex, name_buffer);
        }
        if (shader->_pixel != VK_NULL_HANDLE) {
                snprintf(name_buffer, sizeof(name_buffer), "%s_ps", shader->debug_name);
                vk_set_debug_name(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)shader->_pixel, name_buffer);
        }
        if (shader->_compute != VK_NULL_HANDLE) {
                snprintf(name_buffer, sizeof(name_buffer), "%s_cs", shader->debug_name);
                vk_set_debug_name(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)shader->_compute, name_buffer);
        }
        if (shader->compute_layout != VK_NULL_HANDLE) {
                snprintf(name_buffer, sizeof(name_buffer), "%s_cs_layout", shader->debug_name);
                vk_set_debug_name(VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)shader->compute_layout, name_buffer);
        }
        if (shader->compute_pipeline != VK_NULL_HANDLE) {
                snprintf(name_buffer, sizeof(name_buffer), "%s_cs_pipeline", shader->debug_name);
                vk_set_debug_name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)shader->compute_pipeline, name_buffer);
        }
}

skg_shader_t skg_shader_create_manual(skg_shader_meta_t *meta, skg_shader_stage_t v_shader, skg_shader_stage_t p_shader, skg_shader_stage_t c_shader) {
        skg_shader_t result = {};
        result.meta     = meta;
        result._vertex  = v_shader.module;
        result._pixel   = p_shader.module;
        result._compute = c_shader.module;
        result.compute_pipeline = VK_NULL_HANDLE;
        result.compute_layout   = VK_NULL_HANDLE;
        if (result.meta) skg_shader_meta_reference(result.meta);

        if (result._vertex == VK_NULL_HANDLE && result._compute == VK_NULL_HANDLE) {
                skg_logf(skg_log_warning, "Shader '%s' has no valid stages for Vulkan!", meta ? meta->name : "<unnamed>");
        }

        return result;
}

void skg_shader_name(skg_shader_t *shader, const char *name) {
        if (shader == nullptr)
                return;
        vk_assign_debug_name(&shader->debug_name, name);
        vk_shader_apply_debug_name(shader);
}

bool skg_shader_is_valid(const skg_shader_t *shader) {
        return shader != nullptr && shader->meta != nullptr
                && ((shader->_vertex != VK_NULL_HANDLE && shader->_pixel != VK_NULL_HANDLE)
                        || shader->_compute != VK_NULL_HANDLE);
}

static bool vk_shader_ensure_compute_pipeline(skg_shader_t *shader) {
        if (shader == nullptr || shader->_compute == VK_NULL_HANDLE)
                return false;

        if (shader->compute_pipeline != VK_NULL_HANDLE && shader->compute_layout != VK_NULL_HANDLE)
                return true;

        if (shader->compute_layout == VK_NULL_HANDLE) {
                VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
                layout_info.setLayoutCount = 1;
                layout_info.pSetLayouts    = &vk_descriptor_layout;
                if (vkCreatePipelineLayout(skg_device.device, &layout_info, nullptr, &shader->compute_layout) != VK_SUCCESS) {
                        shader->compute_layout = VK_NULL_HANDLE;
                        skg_log(skg_log_critical, "Failed to create compute pipeline layout");
                        return false;
                }
        }

        VkComputePipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipeline_info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = shader->_compute;
        pipeline_info.stage.pName  = "cs";
        pipeline_info.layout       = shader->compute_layout;

        if (vkCreateComputePipelines(skg_device.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &shader->compute_pipeline) != VK_SUCCESS) {
                shader->compute_pipeline = VK_NULL_HANDLE;
                skg_log(skg_log_critical, "Failed to create Vulkan compute pipeline");
                return false;
        }

        vk_shader_apply_debug_name(shader);
        return true;
}

void skg_shader_compute_bind(const skg_shader_t *shader) {
        if (shader == nullptr) {
                vk_active_compute_pipeline        = VK_NULL_HANDLE;
                vk_active_compute_pipeline_layout = VK_NULL_HANDLE;
                return;
        }

        skg_shader_t *mut = (skg_shader_t*)shader;
        if (!vk_shader_ensure_compute_pipeline(mut))
                return;

        vk_active_compute_pipeline        = mut->compute_pipeline;
        vk_active_compute_pipeline_layout = mut->compute_layout;
        vk_descriptor_dirty_compute       = true;
}

void skg_shader_set(const skg_shader_t *shader) {
        (void)shader;
        // D3D11 sets shaders directly on the context, but Vulkan requires a fully baked
        // pipeline. Applications should bind an skg_pipeline_t instead.
}
void skg_shader_destroy(skg_shader_t *shader) {
        if (shader == nullptr)
                return;

        if (shader->compute_pipeline != VK_NULL_HANDLE) {
                if (vk_active_compute_pipeline == shader->compute_pipeline)
                        vk_active_compute_pipeline = VK_NULL_HANDLE;
                vkDestroyPipeline(skg_device.device, shader->compute_pipeline, nullptr);
                shader->compute_pipeline = VK_NULL_HANDLE;
        }
        if (shader->compute_layout != VK_NULL_HANDLE) {
                if (vk_active_compute_pipeline_layout == shader->compute_layout)
                        vk_active_compute_pipeline_layout = VK_NULL_HANDLE;
                vkDestroyPipelineLayout(skg_device.device, shader->compute_layout, nullptr);
                shader->compute_layout = VK_NULL_HANDLE;
        }

        skg_shader_meta_release(shader->meta);
        if (shader->debug_name) {
                free(shader->debug_name);
                shader->debug_name = nullptr;
        }
        *shader = {};
}

///////////////////////////////////////////
// Swapchain                             //
///////////////////////////////////////////

///////////////////////////////////////////
// skg_pipeline                          //
///////////////////////////////////////////
// The Direct3D 11 backend rebuilds blend, rasterizer, and depth-stencil state
// objects whenever pipeline setters change. Vulkan bakes those same decisions
// into a graphics pipeline object, so we regenerate the pipeline with the same
// high-level settings when the state is marked dirty.

static VkColorComponentFlags vk_color_write_mask(skg_color_write_ mask) {
        switch (mask) {
        case skg_color_write_rgb:  return VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        case skg_color_write_a:    return VK_COLOR_COMPONENT_A_BIT;
        case skg_color_write_none: return 0;
        default:                   return VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }
}

static VkCompareOp vk_depth_compare_from_skg(skg_depth_test_ test) {
        switch (test) {
        case skg_depth_test_always:        return VK_COMPARE_OP_ALWAYS;
        case skg_depth_test_equal:         return VK_COMPARE_OP_EQUAL;
        case skg_depth_test_greater:       return VK_COMPARE_OP_GREATER;
        case skg_depth_test_greater_or_eq: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case skg_depth_test_less:          return VK_COMPARE_OP_LESS;
        case skg_depth_test_less_or_eq:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case skg_depth_test_never:         return VK_COMPARE_OP_NEVER;
        case skg_depth_test_not_equal:     return VK_COMPARE_OP_NOT_EQUAL;
        default:                           return VK_COMPARE_OP_LESS;
        }
}

static void vk_pipeline_release_handles(skg_pipeline_t *pipeline) {
        if (pipeline->pipeline >= 0) {
                vk_pipeline_release(pipeline->pipeline);
                pipeline->pipeline = -1;
        }
        if (pipeline->pipeline_layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(skg_device.device, pipeline->pipeline_layout, nullptr);
                pipeline->pipeline_layout = VK_NULL_HANDLE;
        }
}

static void vk_pipeline_apply_debug_name(skg_pipeline_t *pipeline) {
        if (!vk_debug_utils_naming_enabled || pipeline == nullptr || pipeline->debug_name == nullptr)
                return;

        if (pipeline->pipeline_layout != VK_NULL_HANDLE)
                vk_set_debug_name(VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)pipeline->pipeline_layout, pipeline->debug_name);

        if (pipeline->pipeline >= 0) {
                vk_pipeline_t &cached = vk_pipeline_cache[pipeline->pipeline];
                for (size_t i = 0; i < cached.pipelines.count; i++) {
                        VkPipeline handle = cached.pipelines[i];
                        if (handle == VK_NULL_HANDLE)
                                continue;
                        char pipeline_name[256];
                        snprintf(pipeline_name, sizeof(pipeline_name), "%s[%zu]", pipeline->debug_name, i);
                        vk_set_debug_name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)handle, pipeline_name);
                }
        }
}

static bool vk_pipeline_ensure_ready(skg_pipeline_t *pipeline) {
        if (!pipeline) return false;
        if (!pipeline->dirty && pipeline->pipeline >= 0 && pipeline->pipeline_layout != VK_NULL_HANDLE)
                return true;

        vk_pipeline_release_handles(pipeline);

        if (pipeline->vertex_shader == VK_NULL_HANDLE) {
                skg_log(skg_log_warning, "Vulkan pipeline missing vertex shader stage");
                return false;
        }

        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount         = 1;
        layout_info.pSetLayouts            = &vk_descriptor_layout;
        layout_info.pushConstantRangeCount = 0;
        layout_info.pPushConstantRanges    = nullptr;
        if (vkCreatePipelineLayout(skg_device.device, &layout_info, nullptr, &pipeline->pipeline_layout) != VK_SUCCESS) {
                skg_log(skg_log_critical, "Failed to create Vulkan pipeline layout");
                return false;
        }

        vk_pipeline_info_t info = {};
        info.vertex_info = skg_vertex_layout;
        info.input_asm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        info.input_asm.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        info.input_asm.primitiveRestartEnable = VK_FALSE;

        info.rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        info.rasterizer.rasterizerDiscardEnable = VK_FALSE;
        info.rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
        info.rasterizer.lineWidth               = 1.0f;
        info.rasterizer.frontFace               = VK_FRONT_FACE_CLOCKWISE;
        info.rasterizer.depthBiasEnable         = VK_FALSE;
        info.rasterizer.depthBiasConstantFactor = 0.0f;
        info.rasterizer.depthBiasClamp          = 0.0f;
        info.rasterizer.depthBiasSlopeFactor    = 0.0f;

        switch (pipeline->cull) {
        case skg_cull_front: info.rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT; break;
        case skg_cull_none:  info.rasterizer.cullMode = VK_CULL_MODE_NONE;      break;
        default:             info.rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;  break;
        }

        if (pipeline->wireframe) {
                if (vk_device_features.fillModeNonSolid) {
                        info.rasterizer.polygonMode = VK_POLYGON_MODE_LINE;
                } else {
                        skg_log(skg_log_warning, "Wireframe rendering requested but VK_FEATURE_FILL_MODE_NON_SOLID is unavailable");
                }
        }

        bool depth_clamp_requested = !pipeline->depth_clip;
        info.rasterizer.depthClampEnable = depth_clamp_requested && vk_device_features.depthClamp;
        if (depth_clamp_requested && !vk_device_features.depthClamp)
                skg_log(skg_log_warning, "Depth clamping requested but device does not support VK_FEATURE_DEPTH_CLAMP");

        info.multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        info.multisample.sampleShadingEnable   = VK_FALSE;
        info.multisample.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT;
        info.multisample.minSampleShading      = 1.0f;
        info.multisample.pSampleMask           = nullptr;
        info.multisample.alphaToOneEnable      = VK_FALSE;
        info.multisample.alphaToCoverageEnable = pipeline->transparency == skg_transparency_alpha_to_coverage ? VK_TRUE : VK_FALSE;

        info.depth_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        info.depth_info.depthTestEnable  = (pipeline->depth_test != skg_depth_test_always) || pipeline->depth_write;
        info.depth_info.depthWriteEnable = pipeline->depth_write ? VK_TRUE : VK_FALSE;
        info.depth_info.depthCompareOp   = vk_depth_compare_from_skg(pipeline->depth_test);
        info.depth_info.depthBoundsTestEnable = VK_FALSE;
        info.depth_info.stencilTestEnable     = VK_FALSE;
        info.depth_info.minDepthBounds        = 0.0f;
        info.depth_info.maxDepthBounds        = 1.0f;

        info.blend_attch.colorWriteMask = vk_color_write_mask(pipeline->color_write);
        info.blend_attch.blendEnable    = VK_FALSE;
        info.blend_attch.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        info.blend_attch.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        info.blend_attch.colorBlendOp        = VK_BLEND_OP_ADD;
        info.blend_attch.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        info.blend_attch.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        info.blend_attch.alphaBlendOp        = VK_BLEND_OP_ADD;

        switch (pipeline->transparency) {
        case skg_transparency_blend:
                info.blend_attch.blendEnable           = VK_TRUE;
                info.blend_attch.srcColorBlendFactor   = VK_BLEND_FACTOR_SRC_ALPHA;
                info.blend_attch.dstColorBlendFactor   = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                info.blend_attch.colorBlendOp          = VK_BLEND_OP_ADD;
                info.blend_attch.srcAlphaBlendFactor   = VK_BLEND_FACTOR_ONE;
                info.blend_attch.dstAlphaBlendFactor   = VK_BLEND_FACTOR_ONE;
                info.blend_attch.alphaBlendOp          = VK_BLEND_OP_MAX;
                break;
        case skg_transparency_add:
                info.blend_attch.blendEnable           = VK_TRUE;
                info.blend_attch.srcColorBlendFactor   = VK_BLEND_FACTOR_ONE;
                info.blend_attch.dstColorBlendFactor   = VK_BLEND_FACTOR_ONE;
                info.blend_attch.colorBlendOp          = VK_BLEND_OP_ADD;
                info.blend_attch.srcAlphaBlendFactor   = VK_BLEND_FACTOR_ONE;
                info.blend_attch.dstAlphaBlendFactor   = VK_BLEND_FACTOR_ONE;
                info.blend_attch.alphaBlendOp          = VK_BLEND_OP_ADD;
                break;
        default:
                break;
        }

        info.blend_info.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        info.blend_info.logicOpEnable   = VK_FALSE;
        info.blend_info.logicOp         = VK_LOGIC_OP_COPY;
        info.blend_info.attachmentCount = 1;
        info.blend_info.pAttachments    = &info.blend_attch;
        info.blend_info.blendConstants[0] = 0.0f;
        info.blend_info.blendConstants[1] = 0.0f;
        info.blend_info.blendConstants[2] = 0.0f;
        info.blend_info.blendConstants[3] = 0.0f;

        info.dynamic_states[0] = VK_DYNAMIC_STATE_VIEWPORT;
        info.dynamic_states[1] = VK_DYNAMIC_STATE_SCISSOR;
        info.dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        info.dynamic_state.dynamicStateCount = 2;
        info.dynamic_state.pDynamicStates    = info.dynamic_states;

        VkViewport viewport = {};
        viewport.width    = 1.0f;
        viewport.height   = 1.0f;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor = {};
        scissor.extent.width  = 1;
        scissor.extent.height = 1;

        VkPipelineViewportStateCreateInfo viewport_state = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport_state.viewportCount = 1;
        viewport_state.pViewports    = &viewport;
        viewport_state.scissorCount  = 1;
        viewport_state.pScissors     = &scissor;

        uint32_t stage_count = 0;
        VkPipelineShaderStageCreateInfo &vs_stage = info.shader_stages[stage_count++];
        vs_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vs_stage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        vs_stage.module = pipeline->vertex_shader;
        vs_stage.pName  = "vs";

        if (pipeline->pixel_shader != VK_NULL_HANDLE) {
                VkPipelineShaderStageCreateInfo &ps_stage = info.shader_stages[stage_count++];
                ps_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                ps_stage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
                ps_stage.module = pipeline->pixel_shader;
                ps_stage.pName  = "ps";
        }

        info.create.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.create.stageCount          = stage_count;
        info.create.pStages             = info.shader_stages;
        info.create.pVertexInputState   = &info.vertex_info;
        info.create.pInputAssemblyState = &info.input_asm;
        info.create.pViewportState      = &viewport_state;
        info.create.pRasterizationState = &info.rasterizer;
        info.create.pMultisampleState   = &info.multisample;
        info.create.pDepthStencilState  = &info.depth_info;
        info.create.pColorBlendState    = &info.blend_info;
        info.create.pDynamicState       = &info.dynamic_state;
        info.create.layout              = pipeline->pipeline_layout;
        info.create.subpass             = 0;
        info.create.basePipelineHandle  = VK_NULL_HANDLE;
        info.create.basePipelineIndex   = -1;

        pipeline->pipeline = vk_pipeline_ref(info);
        if (pipeline->pipeline < 0) {
                skg_log(skg_log_critical, "Failed to create Vulkan graphics pipeline");
                vk_pipeline_release_handles(pipeline);
                return false;
        }

        pipeline->dirty = false;
        vk_pipeline_apply_debug_name(pipeline);
        return true;
}

skg_pipeline_t skg_pipeline_create(skg_shader_t *shader) {
        skg_pipeline_t result = {};
        result.transparency = skg_transparency_none;
        result.cull         = skg_cull_back;
        result.wireframe    = false;
        result.depth_write  = true;
        result.depth_clip   = true;
        result.color_write  = skg_color_write_rgba;
        result.scissor      = false;
        result.depth_test   = skg_depth_test_less;
        result.dirty        = true;
        result.pipeline     = -1;
        result.pipeline_layout = VK_NULL_HANDLE;
        result.meta = shader ? shader->meta : nullptr;
        if (result.meta) skg_shader_meta_reference(result.meta);
        result.vertex_shader = shader ? shader->_vertex : VK_NULL_HANDLE;
        result.pixel_shader  = shader ? shader->_pixel  : VK_NULL_HANDLE;
        if (result.vertex_shader == VK_NULL_HANDLE)
                skg_log(skg_log_warning, "Creating Vulkan pipeline without a vertex shader");
        return result;
}

void skg_pipeline_name(skg_pipeline_t *pipeline, const char* name) {
        if (pipeline == nullptr)
                return;
        vk_assign_debug_name(&pipeline->debug_name, name);
        vk_pipeline_apply_debug_name(pipeline);
}

void skg_pipeline_bind(const skg_pipeline_t *pipeline) {
        if (pipeline == nullptr || skg_active_rendertarget == nullptr)
                return;

        skg_pipeline_t *mut = (skg_pipeline_t*)pipeline;
        if (!vk_pipeline_ensure_ready(mut))
                return;

        vk_active_pipeline = &vk_pipeline_cache[mut->pipeline].pipelines[skg_active_rendertarget->rt_renderpass];
        vk_active_graphics_pipeline_layout = mut->pipeline_layout;
        vk_descriptor_dirty_graphics = true;
        vkCmdBindPipeline(skg_active_rendertarget->rt_commandbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, *vk_active_pipeline);
        vk_flush_descriptor_binding_graphics();
}

void skg_pipeline_set_transparency(skg_pipeline_t *pipeline, skg_transparency_ transparency) {
        if (pipeline->transparency != transparency) {
                pipeline->transparency = transparency;
                pipeline->dirty = true;
        }
}

skg_transparency_ skg_pipeline_get_transparency(const skg_pipeline_t *pipeline) {
        return pipeline->transparency;
}

void skg_pipeline_set_cull(skg_pipeline_t *pipeline, skg_cull_ cull) {
        if (pipeline->cull != cull) {
                pipeline->cull = cull;
                pipeline->dirty = true;
        }
}

skg_cull_ skg_pipeline_get_cull(const skg_pipeline_t *pipeline) {
        return pipeline->cull;
}

void skg_pipeline_set_wireframe(skg_pipeline_t *pipeline, bool wireframe) {
        if (pipeline->wireframe != wireframe) {
                pipeline->wireframe = wireframe;
                pipeline->dirty = true;
        }
}

bool skg_pipeline_get_wireframe(const skg_pipeline_t *pipeline) {
        return pipeline->wireframe;
}

void skg_pipeline_set_depth_write(skg_pipeline_t *pipeline, bool write) {
        if (pipeline->depth_write != write) {
                pipeline->depth_write = write;
                pipeline->dirty = true;
        }
}

bool skg_pipeline_get_depth_write(const skg_pipeline_t *pipeline) {
        return pipeline->depth_write;
}

void skg_pipeline_set_depth_clip(skg_pipeline_t *pipeline, bool clip) {
        if (pipeline->depth_clip != clip) {
                pipeline->depth_clip = clip;
                pipeline->dirty = true;
        }
}

bool skg_pipeline_get_depth_clip(const skg_pipeline_t *pipeline) {
        return pipeline->depth_clip;
}

void skg_pipeline_set_color_write(skg_pipeline_t *pipeline, skg_color_write_ write) {
        if (pipeline->color_write != write) {
                pipeline->color_write = write;
                pipeline->dirty = true;
        }
}

skg_color_write_ skg_pipeline_get_color_write(const skg_pipeline_t *pipeline) {
        return pipeline->color_write;
}

void skg_pipeline_set_depth_test(skg_pipeline_t *pipeline, skg_depth_test_ test) {
        if (pipeline->depth_test != test) {
                pipeline->depth_test = test;
                pipeline->dirty = true;
        }
}

skg_depth_test_ skg_pipeline_get_depth_test(const skg_pipeline_t *pipeline) {
        return pipeline->depth_test;
}

void skg_pipeline_set_scissor(skg_pipeline_t *pipeline, bool enable) {
        if (pipeline->scissor != enable) {
                pipeline->scissor = enable;
                // Vulkan's scissor enablement is handled through dynamic state. We track the flag for parity
                // with the Direct3D implementation but it does not force a pipeline rebuild.
        }
}

bool skg_pipeline_get_scissor(const skg_pipeline_t *pipeline) {
        return pipeline->scissor;
}

void skg_pipeline_destroy(skg_pipeline_t *pipeline) {
        vk_pipeline_release_handles(pipeline);
        if (pipeline->meta)
                skg_shader_meta_release(pipeline->meta);
        if (pipeline->debug_name) {
                free(pipeline->debug_name);
                pipeline->debug_name = nullptr;
        }
        *pipeline = {};
}

static void vk_swapchain_cleanup_textures(skg_swapchain_t *swapchain) {
        if (swapchain->textures) {
                for (uint32_t i = 0; i < swapchain->img_count; i++) {
                        if (swapchain->textures[i].texture != VK_NULL_HANDLE)
                                swapchain->textures[i].texture = VK_NULL_HANDLE;
                        skg_tex_destroy(&swapchain->textures[i]);
                }
                free(swapchain->textures);
                swapchain->textures = nullptr;
        }
        if (swapchain->depths) {
                for (uint32_t i = 0; i < swapchain->img_count; i++)
                        skg_tex_destroy(&swapchain->depths[i]);
                free(swapchain->depths);
                swapchain->depths = nullptr;
        }
        if (swapchain->imgs) {
                free(swapchain->imgs);
                swapchain->imgs = nullptr;
        }
        if (swapchain->img_fence) {
                for (uint32_t i = 0; i < swapchain->img_count; i++)
                        if (swapchain->img_fence[i])
                                vkDestroyFence(skg_device.device, swapchain->img_fence[i], nullptr);
                free(swapchain->img_fence);
                swapchain->img_fence = nullptr;
        }
        swapchain->img_count = 0;
        swapchain->img_curr  = 0;
        swapchain->img_active = 0;
}

static bool vk_swapchain_init_textures(skg_swapchain_t *swapchain) {
        vkGetSwapchainImagesKHR(skg_device.device, swapchain->swapchain, &swapchain->img_count, nullptr);
        swapchain->imgs      = (VkImage   *)malloc(sizeof(VkImage  ) * swapchain->img_count);
        swapchain->textures  = (skg_tex_t *)malloc(sizeof(skg_tex_t) * swapchain->img_count);
        swapchain->img_fence = (VkFence   *)malloc(sizeof(VkFence  ) * swapchain->img_count);
        if (swapchain->imgs == nullptr || swapchain->textures == nullptr || swapchain->img_fence == nullptr) {
                skg_log(skg_log_critical, "Out of memory creating swapchain textures");
                return false;
        }
        memset(swapchain->textures, 0, sizeof(skg_tex_t) * swapchain->img_count);
        memset(swapchain->img_fence, 0, sizeof(VkFence)   * swapchain->img_count);
        vkGetSwapchainImagesKHR(skg_device.device, swapchain->swapchain, &swapchain->img_count, swapchain->imgs);

        if (swapchain->depth_format != skg_tex_fmt_none) {
                swapchain->depths = (skg_tex_t *)malloc(sizeof(skg_tex_t) * swapchain->img_count);
                if (swapchain->depths == nullptr) {
                        skg_log(skg_log_critical, "Out of memory creating swapchain depth textures");
                        return false;
                }
                memset(swapchain->depths, 0, sizeof(skg_tex_t) * swapchain->img_count);
        }

        VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (uint32_t i = 0; i < swapchain->img_count; i++) {
                swapchain->textures[i] = skg_tex_create_from_existing(&swapchain->imgs[i], skg_tex_type_rendertarget,
                        swapchain->color_format, swapchain->width, swapchain->height, 1, 1, 1);
                if (vkCreateFence(skg_device.device, &fence_info, nullptr, &swapchain->img_fence[i]) != VK_SUCCESS) {
                        skg_log(skg_log_critical, "failed to create swapchain fence");
                        return false;
                }
                if (swapchain->depths) {
                        swapchain->depths[i] = vk_tex_create_depth(swapchain->depth_format, swapchain->width, swapchain->height);
                        skg_tex_attach_depth(&swapchain->textures[i], &swapchain->depths[i]);
                }
        }

        return true;
}

skg_swapchain_t skg_swapchain_create(void *hwnd, skg_tex_fmt_ format, skg_tex_fmt_ depth_format, int32_t width, int32_t height) {
        skg_swapchain_t  result = {};
        VkPresentModeKHR mode   = vk_get_presentation_mode(skg_device);
        result.format           = vk_get_preferred_fmt    (skg_device);

        (void)hwnd;

        result.color_format = skg_native_to_tex_fmt(result.format.format);
        switch (result.color_format) {
        case skg_tex_fmt_rgba32_linear: result.color_format = skg_tex_fmt_rgba32; break;
        case skg_tex_fmt_bgra32_linear: result.color_format = skg_tex_fmt_bgra32; break;
        default: break;
        }
        result.depth_format = depth_format;

        VkSurfaceCapabilitiesKHR surface_caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(skg_device.phys_device, skg_device.surface, &surface_caps);

        result.extents = surface_caps.currentExtent;
	if (result.extents.width == UINT32_MAX) {
		if (width < surface_caps.minImageExtent.width)
			width = surface_caps.minImageExtent.width;
		if (width > surface_caps.maxImageExtent.width)
			width = surface_caps.maxImageExtent.width;
		result.extents.width = width;
		
		if (height < surface_caps.minImageExtent.height)
			height = surface_caps.minImageExtent.height;
                if (height > surface_caps.maxImageExtent.height)
                        height = surface_caps.maxImageExtent.height;
                result.extents.height = height;
        }
        result.width  = (int32_t)result.extents.width;
        result.height = (int32_t)result.extents.height;

        VkSwapchainCreateInfoKHR swapchain_info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        swapchain_info.surface          = skg_device.surface;
        swapchain_info.minImageCount    = surface_caps.maxImageCount > 0 && surface_caps.minImageCount+1 > surface_caps.maxImageCount
                ? surface_caps.minImageCount
		: surface_caps.minImageCount + 1;
	swapchain_info.imageFormat      = result.format.format;
	swapchain_info.imageColorSpace  = result.format.colorSpace;
	swapchain_info.imageExtent      = result.extents;
        swapchain_info.imageArrayLayers = 1; // 2 for stereo;
        swapchain_info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapchain_info.preTransform     = surface_caps.currentTransform;
	swapchain_info.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapchain_info.presentMode      = mode;
	swapchain_info.clipped          = VK_TRUE;

	// Exclusive mode is faster, but can't be used if the presentation queue
	// and graphics queue are separate.
	if (skg_device.queue_gfx_index != skg_device.queue_present_index) {
		uint32_t queue_indices[] = { skg_device.queue_gfx_index, skg_device.queue_present_index };
		swapchain_info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
		swapchain_info.queueFamilyIndexCount = _countof(queue_indices);
		swapchain_info.pQueueFamilyIndices   = queue_indices;
	}

        VkResult call_result = vkCreateSwapchainKHR(skg_device.device, &swapchain_info, 0, &result.swapchain);
        if (call_result != VK_SUCCESS) {
                skg_log(skg_log_critical, "Failed to create swapchain!");
                return result;
        }

        if (!vk_swapchain_init_textures(&result)) {
                vk_swapchain_cleanup_textures(&result);
                vkDestroySwapchainKHR(skg_device.device, result.swapchain, 0);
                result.swapchain = VK_NULL_HANDLE;
                return result;
        }

        // Create synchronization objects
        VkSemaphoreCreateInfo sem_info  = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo     fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (size_t i = 0; i < 2; i++) {
                if (vkCreateSemaphore(skg_device.device, &sem_info,   nullptr, &result.sem_available[i]) != VK_SUCCESS ||
                        vkCreateSemaphore(skg_device.device, &sem_info,   nullptr, &result.sem_finished [i]) != VK_SUCCESS ||
                        vkCreateFence    (skg_device.device, &fence_info, nullptr, &result.fence_flight [i]) != VK_SUCCESS) {

			skg_log(skg_log_critical, "failed to create synchronization objects for a frame!");
                }
        }

        result.img_curr = 0;

        return result;
}
void skg_swapchain_resize(skg_swapchain_t *swapchain, int32_t width, int32_t height) {
        if (swapchain == nullptr || swapchain->swapchain == VK_NULL_HANDLE)
                return;

        vkDeviceWaitIdle(skg_device.device);

        vk_swapchain_cleanup_textures(swapchain);

        VkSurfaceCapabilitiesKHR surface_caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(skg_device.phys_device, skg_device.surface, &surface_caps);

        VkExtent2D new_extent = surface_caps.currentExtent;
        if (new_extent.width == UINT32_MAX) {
                if ((uint32_t)width  < surface_caps.minImageExtent.width)  width  = surface_caps.minImageExtent.width;
                if ((uint32_t)width  > surface_caps.maxImageExtent.width)  width  = surface_caps.maxImageExtent.width;
                if ((uint32_t)height < surface_caps.minImageExtent.height) height = surface_caps.minImageExtent.height;
                if ((uint32_t)height > surface_caps.maxImageExtent.height) height = surface_caps.maxImageExtent.height;
                new_extent = { (uint32_t)width, (uint32_t)height };
        }

        VkSwapchainCreateInfoKHR swapchain_info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        swapchain_info.surface          = skg_device.surface;
        swapchain_info.minImageCount    = surface_caps.maxImageCount > 0 && surface_caps.minImageCount+1 > surface_caps.maxImageCount
                ? surface_caps.minImageCount
                : surface_caps.minImageCount + 1;
        swapchain_info.imageFormat      = swapchain->format.format;
        swapchain_info.imageColorSpace  = swapchain->format.colorSpace;
        swapchain_info.imageExtent      = new_extent;
        swapchain_info.imageArrayLayers = 1;
        swapchain_info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchain_info.preTransform     = surface_caps.currentTransform;
        swapchain_info.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapchain_info.presentMode      = vk_get_presentation_mode(skg_device);
        swapchain_info.clipped          = VK_TRUE;
        swapchain_info.oldSwapchain     = swapchain->swapchain;

        if (skg_device.queue_gfx_index != skg_device.queue_present_index) {
                uint32_t queue_indices[] = { skg_device.queue_gfx_index, skg_device.queue_present_index };
                swapchain_info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
                swapchain_info.queueFamilyIndexCount = _countof(queue_indices);
                swapchain_info.pQueueFamilyIndices   = queue_indices;
        }

        VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
        if (vkCreateSwapchainKHR(skg_device.device, &swapchain_info, nullptr, &new_swapchain) != VK_SUCCESS) {
                skg_log(skg_log_critical, "Failed to resize swapchain");
                return;
        }

        vkDestroySwapchainKHR(skg_device.device, swapchain->swapchain, nullptr);
        swapchain->swapchain = new_swapchain;
        swapchain->extents   = new_extent;
        swapchain->width     = (int32_t)new_extent.width;
        swapchain->height    = (int32_t)new_extent.height;

        if (!vk_swapchain_init_textures(swapchain)) {
                vk_swapchain_cleanup_textures(swapchain);
        }

        swapchain->img_curr = 0;
}
void skg_swapchain_present(skg_swapchain_t *swapchain) {
	vkCmdEndRenderPass(skg_active_rendertarget->rt_commandbuffer);
	vkEndCommandBuffer(skg_active_rendertarget->rt_commandbuffer);

	VkSubmitInfo         submitInfo       = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	VkSemaphore          waitSemaphores[] = {swapchain->sem_available[swapchain->sync_index]};
	VkPipelineStageFlags waitStages[]     = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores    = waitSemaphores;
	submitInfo.pWaitDstStageMask  = waitStages;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers    = &skg_active_rendertarget->rt_commandbuffer;

	VkSemaphore signalSemaphores[] = {swapchain->sem_finished[swapchain->sync_index]};
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores    = signalSemaphores;

	vkResetFences(skg_device.device, 1, &swapchain->fence_flight[swapchain->sync_index]);
	if (vkQueueSubmit(skg_device.queue_gfx, 1, &submitInfo, swapchain->fence_flight[swapchain->sync_index]) != VK_SUCCESS) {
		skg_log(skg_log_critical, "failed to submit draw command buffer!");
	}

	VkSwapchainKHR   swapChains[] = {swapchain->swapchain};
	VkPresentInfoKHR presentInfo  = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores    = signalSemaphores;
	presentInfo.swapchainCount     = 1;
	presentInfo.pSwapchains        = swapChains;
	presentInfo.pImageIndices      = &swapchain->img_active;
	presentInfo.pResults           = nullptr; // Optional

	vkQueuePresentKHR(skg_device.queue_present, &presentInfo);

        swapchain->sync_index = (swapchain->sync_index + 1) % 2;
}

void skg_swapchain_bind(skg_swapchain_t *swapchain) {
        if (swapchain == nullptr)
                return;
        const skg_tex_t *target = skg_swapchain_get_target(swapchain);
        const skg_tex_t *depth  = skg_swapchain_get_depth (swapchain);
        skg_tex_target_bind(nullptr, target, depth);
}

const skg_tex_t *skg_swapchain_get_target(const skg_swapchain_t *swapchain) {
        if (swapchain == nullptr || swapchain->textures == nullptr)
                return nullptr;
        uint32_t index = swapchain->img_active < swapchain->img_count ? swapchain->img_active : 0;
        return &swapchain->textures[index];
}
const skg_tex_t *skg_swapchain_get_depth(const skg_swapchain_t *swapchain) {
        if (swapchain == nullptr || swapchain->depths == nullptr)
                return nullptr;
        uint32_t index = swapchain->img_active < swapchain->img_count ? swapchain->img_active : 0;
        return &swapchain->depths[index];
}

void skg_swapchain_get_next(skg_swapchain_t *swapchain, const skg_tex_t **out_target, const skg_tex_t **out_depth) {
        vkWaitForFences(skg_device.device, 1, &swapchain->fence_flight[swapchain->sync_index], VK_TRUE, UINT64_MAX);
        vkAcquireNextImageKHR(skg_device.device, swapchain->swapchain, UINT64_MAX, swapchain->sem_available[swapchain->sync_index], VK_NULL_HANDLE, &swapchain->img_active);
        swapchain->img_curr = swapchain->img_active;
        *out_target = swapchain->textures ? &swapchain->textures[swapchain->img_active] : nullptr;
        *out_depth  = swapchain->depths   ? &swapchain->depths  [swapchain->img_active] : nullptr;

        if (swapchain->img_fence && swapchain->img_fence[swapchain->img_active] != VK_NULL_HANDLE) {
                vkWaitForFences(skg_device.device, 1, &swapchain->img_fence[swapchain->img_active], VK_TRUE, UINT64_MAX);
        }
        if (swapchain->img_fence)
                swapchain->img_fence[swapchain->img_active] = swapchain->fence_flight[swapchain->sync_index];
}

void skg_swapchain_destroy(skg_swapchain_t *swapchain) {
        vk_swapchain_cleanup_textures(swapchain);
        for (size_t i = 0; i < 2; i++) {
                vkDestroySemaphore(skg_device.device, swapchain->sem_finished [i], nullptr);
                vkDestroySemaphore(skg_device.device, swapchain->sem_available[i], nullptr);
                vkDestroyFence    (skg_device.device, swapchain->fence_flight [i], nullptr);
        }

        if (swapchain->swapchain)
                vkDestroySwapchainKHR(skg_device.device, swapchain->swapchain, 0);
        *swapchain = {};
}

///////////////////////////////////////////
// Texture                               //
///////////////////////////////////////////

static VkSampleCountFlagBits vk_sample_count_from_int(int32_t samples) {
        if (samples >= 64) return VK_SAMPLE_COUNT_64_BIT;
        if (samples >= 32) return VK_SAMPLE_COUNT_32_BIT;
        if (samples >= 16) return VK_SAMPLE_COUNT_16_BIT;
        if (samples >= 8 ) return VK_SAMPLE_COUNT_8_BIT;
        if (samples >= 4 ) return VK_SAMPLE_COUNT_4_BIT;
        if (samples >= 2 ) return VK_SAMPLE_COUNT_2_BIT;
        return VK_SAMPLE_COUNT_1_BIT;
}

static VkImageAspectFlags vk_aspect_from_format(skg_tex_fmt_ format) {
        if (skg_tex_fmt_is_depth(format)) {
                VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
                if (format == skg_tex_fmt_depthstencil)
                        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
                return aspect;
        }
        return VK_IMAGE_ASPECT_COLOR_BIT;
}

static VkImageLayout vk_target_layout_for_tex(const skg_tex_t *tex) {
        if (tex->type == skg_tex_type_zbuffer || tex->type == skg_tex_type_depthtarget)
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if ((tex->use & skg_use_compute_write) != 0)
                return VK_IMAGE_LAYOUT_GENERAL;
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static bool vk_tex_decode_surface(const skg_tex_t *tex, int32_t surface, uint32_t *out_layer, uint32_t *out_mip) {
        if (tex == nullptr || surface < 0)
                return false;

        uint32_t mip_levels  = tex->mip_count   > 0 ? tex->mip_count   : 1;
        uint32_t layer_count = tex->array_count > 0 ? tex->array_count : 1;
        uint32_t surf_index  = (uint32_t)surface;

        if (surf_index >= mip_levels * layer_count) {
                skg_log(skg_log_warning, "Texture surface index out of range");
                return false;
        }

        if (out_mip)   *out_mip   = surf_index % mip_levels;
        if (out_layer) *out_layer = surf_index / mip_levels;
        return true;
}

static void vk_destroy_tex_gpu_objects(skg_tex_t *tex) {
        if (tex->rt_framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(skg_device.device, tex->rt_framebuffer, nullptr);
                tex->rt_framebuffer = VK_NULL_HANDLE;
        }
        if (tex->rt_renderpass >= 0) {
                vk_renderpass_release(tex->rt_renderpass);
                tex->rt_renderpass = -1;
        }
        if (tex->rt_commandbuffer != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(skg_device.device, vk_cmd_pool, 1, &tex->rt_commandbuffer);
                tex->rt_commandbuffer = VK_NULL_HANDLE;
        }
        if (tex->view != VK_NULL_HANDLE) {
                vkDestroyImageView(skg_device.device, tex->view, nullptr);
                tex->view = VK_NULL_HANDLE;
        }
        if (tex->sampler != VK_NULL_HANDLE) {
                vkDestroySampler(skg_device.device, tex->sampler, nullptr);
                tex->sampler = VK_NULL_HANDLE;
        }
        if (tex->texture != VK_NULL_HANDLE) {
                vkDestroyImage(skg_device.device, tex->texture, nullptr);
                tex->texture = VK_NULL_HANDLE;
        }
        if (tex->texture_mem != VK_NULL_HANDLE) {
                vkFreeMemory(skg_device.device, tex->texture_mem, nullptr);
                tex->texture_mem = VK_NULL_HANDLE;
        }
        tex->rt_depth_view = VK_NULL_HANDLE;
        tex->rt_depth_tex  = nullptr;
        tex->layout        = VK_IMAGE_LAYOUT_UNDEFINED;
}

static void vk_generate_mips(VkCommandBuffer cmd, VkImage image, VkFormat format, int32_t width, int32_t height, uint32_t mip_levels, uint32_t array_layers, VkImageAspectFlags aspect) {
        if (cmd == VK_NULL_HANDLE || mip_levels <= 1)
                return;

        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(skg_device.phys_device, format, &props);
        VkFilter blit_filter = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

        for (uint32_t layer = 0; layer < array_layers; layer++) {
                int32_t mip_width  = width;
                int32_t mip_height = height;

                for (uint32_t level = 1; level < mip_levels; level++) {
                        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                        barrier.subresourceRange.aspectMask     = aspect;
                        barrier.subresourceRange.baseArrayLayer = layer;
                        barrier.subresourceRange.layerCount     = 1;
                        barrier.subresourceRange.levelCount     = 1;

                        barrier.subresourceRange.baseMipLevel = level - 1;
                        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        vkCmdPipelineBarrier(cmd,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                0, nullptr, 0, nullptr, 1, &barrier);

                        VkImageBlit blit = {};
                        blit.srcOffsets[0] = {0, 0, 0};
                        blit.srcOffsets[1] = { mip_width, mip_height, 1 };
                        blit.srcSubresource.aspectMask     = aspect;
                        blit.srcSubresource.mipLevel       = level - 1;
                        blit.srcSubresource.baseArrayLayer = layer;
                        blit.srcSubresource.layerCount     = 1;

                        int32_t dst_width  = mip_width  > 1 ? mip_width  / 2 : 1;
                        int32_t dst_height = mip_height > 1 ? mip_height / 2 : 1;

                        blit.dstOffsets[0] = {0, 0, 0};
                        blit.dstOffsets[1] = { dst_width, dst_height, 1 };
                        blit.dstSubresource.aspectMask     = aspect;
                        blit.dstSubresource.mipLevel       = level;
                        blit.dstSubresource.baseArrayLayer = layer;
                        blit.dstSubresource.layerCount     = 1;

                        vkCmdBlitImage(cmd,
                                image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                1, &blit, blit_filter);

                        mip_width  = dst_width;
                        mip_height = dst_height;
                }
        }

        VkImageMemoryBarrier final_barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        final_barrier.subresourceRange.aspectMask     = aspect;
        final_barrier.subresourceRange.baseMipLevel   = mip_levels - 1;
        final_barrier.subresourceRange.levelCount     = 1;
        final_barrier.subresourceRange.baseArrayLayer = 0;
        final_barrier.subresourceRange.layerCount     = array_layers;
        final_barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        final_barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        final_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        final_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                0, nullptr, 0, nullptr, 1, &final_barrier);
}

void skg_tex_create_views(skg_tex_t *tex) {
        VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view_info.image  = tex->texture;
        view_info.format = (VkFormat)skg_tex_fmt_to_native(tex->format);

        bool single_layer_view = tex->array_start >= 0;
        uint32_t base_layer    = single_layer_view ? (uint32_t)tex->array_start : 0;
        uint32_t layer_count   = single_layer_view ? 1u : (tex->array_count > 0 ? (uint32_t)tex->array_count : 1u);
        if (layer_count == 0)
                layer_count = 1;

        if ((tex->use & skg_use_cubemap) != 0) {
                if (single_layer_view) {
                        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                } else if (layer_count == 6) {
                        view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
                } else {
                        view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
                }
        } else if (!single_layer_view && layer_count > 1) {
                view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        } else {
                view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        }

        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

        VkImageAspectFlags aspect = vk_aspect_from_format(tex->format);
        view_info.subresourceRange.aspectMask     = aspect;
        view_info.subresourceRange.baseMipLevel   = 0;
        view_info.subresourceRange.levelCount     = tex->mip_count > 0 ? tex->mip_count : 1;
        view_info.subresourceRange.baseArrayLayer = base_layer;
        view_info.subresourceRange.layerCount     = layer_count;

        if (vkCreateImageView(skg_device.device, &view_info, nullptr, &tex->view) != VK_SUCCESS)
                skg_log(skg_log_critical, "vkCreateImageView failed");

        if (tex->type == skg_tex_type_rendertarget) {
                tex->rt_depth_view = VK_NULL_HANDLE;

                VkAttachmentDescription color_attch = {};
                color_attch.format         = view_info.format;
                color_attch.samples        = VK_SAMPLE_COUNT_1_BIT;
                color_attch.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
                color_attch.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		color_attch.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		color_attch.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		color_attch.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
		color_attch.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference color_attch_ref = {};
		color_attch_ref.attachment = 0;
		color_attch_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments    = &color_attch_ref;

		VkSubpassDependency dependency = {};
		dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass    = 0;
		dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		pass_info.attachmentCount = 1;
		pass_info.pAttachments    = &color_attch;
		pass_info.subpassCount    = 1;
		pass_info.pSubpasses      = &subpass;
		pass_info.dependencyCount = 1;
		pass_info.pDependencies   = &dependency;

		tex->rt_renderpass = vk_renderpass_ref(pass_info);

		VkImageView             attachments[]   = { tex->view };
		VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebuffer_info.renderPass      = vk_renderpass_cache[tex->rt_renderpass].renderpass;
		framebuffer_info.attachmentCount = _countof(attachments);
		framebuffer_info.pAttachments    = attachments;
		framebuffer_info.width           = tex->width;
		framebuffer_info.height          = tex->height;
		framebuffer_info.layers          = 1;

		if (vkCreateFramebuffer(skg_device.device, &framebuffer_info, nullptr, &tex->rt_framebuffer) != VK_SUCCESS) {
			skg_log(skg_log_critical, "failed to create framebuffer!");
		}

		VkCommandBufferAllocateInfo alloc_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		alloc_info.commandPool        = vk_cmd_pool;
		alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc_info.commandBufferCount = 1;

                if (vkAllocateCommandBuffers(skg_device.device, &alloc_info, &tex->rt_commandbuffer) != VK_SUCCESS) {
                        skg_log(skg_log_critical, "failed to allocate command buffers!");
                }
        }
}

skg_tex_t skg_tex_create_from_existing(void *native_tex, skg_tex_type_ type, skg_tex_fmt_ format,
        int32_t width, int32_t height, int32_t array_count, int32_t multisample, int32_t framebuffer_multisample) {

        VkImage image = native_tex != nullptr ? *(VkImage *)native_tex : VK_NULL_HANDLE;
        return vk_tex_wrap_existing(image, type, format, width, height, array_count, -1, multisample, framebuffer_multisample);
}

skg_tex_t skg_tex_create_from_layer(void *native_tex, skg_tex_type_ type, skg_tex_fmt_ format,
        int32_t width, int32_t height, int32_t array_layer) {

        VkImage image = native_tex != nullptr ? *(VkImage *)native_tex : VK_NULL_HANDLE;
        return vk_tex_wrap_existing(image, type, format, width, height, 1, array_layer, 1, 1);
}

static skg_tex_t vk_tex_create_depth(skg_tex_fmt_ format, int32_t width, int32_t height) {
        skg_tex_t result = {};
        result.type   = skg_tex_type_zbuffer;
        result.use    = skg_use_static;
        result.format = format;
        result.width  = width;
        result.height = height;
        result.array_count = 1;
        result.array_start = -1;
        result.multisample = 1;
        result.mip_count = 1;
        result.layout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImageCreateInfo image_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        image_info.imageType     = VK_IMAGE_TYPE_2D;
        image_info.extent.width  = width;
        image_info.extent.height = height;
        image_info.extent.depth  = 1;
        image_info.mipLevels     = 1;
        image_info.arrayLayers   = 1;
        image_info.format        = (VkFormat)skg_tex_fmt_to_native(format);
        image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image_info.samples       = VK_SAMPLE_COUNT_1_BIT;

        if (vkCreateImage(skg_device.device, &image_info, nullptr, &result.texture) != VK_SUCCESS) {
                skg_log(skg_log_critical, "Failed to create depth image");
                return result;
        }

        VkMemoryRequirements mem_reqs;
        vkGetImageMemoryRequirements(skg_device.device, result.texture, &mem_reqs);

        VkMemoryAllocateInfo alloc_info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc_info.allocationSize  = mem_reqs.size;
        alloc_info.memoryTypeIndex = vk_find_mem_type(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(skg_device.device, &alloc_info, nullptr, &result.texture_mem) != VK_SUCCESS) {
                skg_log(skg_log_critical, "Failed to allocate depth memory");
                vkDestroyImage(skg_device.device, result.texture, nullptr);
                result.texture = VK_NULL_HANDLE;
                return result;
        }
        vkBindImageMemory(skg_device.device, result.texture, result.texture_mem, 0);

        skg_tex_create_views(&result);
        skg_tex_settings(&result, skg_tex_address_clamp, skg_tex_sample_linear, skg_sample_compare_none, 0);

        return result;
}

static skg_tex_t vk_tex_wrap_existing(VkImage image, skg_tex_type_ type, skg_tex_fmt_ format,
        int32_t width, int32_t height, int32_t array_count, int32_t array_start,
        int32_t multisample, int32_t framebuffer_multisample) {

        skg_tex_t result = {};
        result.type         = type;
        result.use          = skg_use_static;
        result.format       = format;
        result.width        = width;
        result.height       = height;
        result.array_count  = array_count > 0 ? array_count : 1;
        result.array_start  = array_start >= 0 ? array_start : -1;
        int32_t resolved_ms = framebuffer_multisample > 0 ? framebuffer_multisample : multisample;
        result.multisample  = resolved_ms > 0 ? resolved_ms : 1;
        result.mips         = skg_mip_none;
        result.mip_count    = 1;
        result.texture      = image;
        result.texture_mem  = VK_NULL_HANDLE;
        result.rt_renderpass = -1;

        if (type == skg_tex_type_zbuffer || type == skg_tex_type_depthtarget)
                result.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        else if (type == skg_tex_type_rendertarget)
                result.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        else
                result.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        if (result.texture != VK_NULL_HANDLE)
                skg_tex_create_views(&result);
        skg_tex_settings(&result, skg_tex_address_repeat, skg_tex_sample_linear, skg_sample_compare_none, 0);
        return result;
}

void skg_tex_attach_depth(skg_tex_t *tex, skg_tex_t *depth) {
        if (tex == nullptr || depth == nullptr)
                return;
        if (tex->type != skg_tex_type_rendertarget) {
                skg_log(skg_log_warning, "Only render targets can have a depth attachment");
                return;
        }
        if (depth->type != skg_tex_type_zbuffer && depth->type != skg_tex_type_depthtarget) {
                skg_log(skg_log_warning, "Depth attachment must be a depth texture");
                return;
        }

        if (tex->rt_framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(skg_device.device, tex->rt_framebuffer, nullptr);
                tex->rt_framebuffer = VK_NULL_HANDLE;
        }
        if (tex->rt_renderpass >= 0) {
                vk_renderpass_release(tex->rt_renderpass);
                tex->rt_renderpass = -1;
        }

        VkAttachmentDescription attachments[2] = {};
        VkImageLayout color_final_layout = tex->texture_mem == VK_NULL_HANDLE
                ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        attachments[0].format         = (VkFormat)skg_tex_fmt_to_native(tex->format);
        attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout    = color_final_layout;

        attachments[1].format         = (VkFormat)skg_tex_fmt_to_native(depth->format);
        attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_ref = {};
        color_ref.attachment = 0;
        color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depth_ref = {};
        depth_ref.attachment = 1;
        depth_ref.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &color_ref;
        subpass.pDepthStencilAttachment = &depth_ref;

        VkSubpassDependency dependency = {};
        dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass    = 0;
        dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        pass_info.attachmentCount = _countof(attachments);
        pass_info.pAttachments    = attachments;
        pass_info.subpassCount    = 1;
        pass_info.pSubpasses      = &subpass;
        pass_info.dependencyCount = 1;
        pass_info.pDependencies   = &dependency;

        tex->rt_renderpass = vk_renderpass_ref(pass_info);

        VkImageView attachment_views[] = { tex->view, depth->view };
        VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebuffer_info.renderPass      = vk_renderpass_cache[tex->rt_renderpass].renderpass;
        framebuffer_info.attachmentCount = _countof(attachment_views);
        framebuffer_info.pAttachments    = attachment_views;
        framebuffer_info.width           = tex->width;
        framebuffer_info.height          = tex->height;
        framebuffer_info.layers          = 1;

        if (vkCreateFramebuffer(skg_device.device, &framebuffer_info, nullptr, &tex->rt_framebuffer) != VK_SUCCESS) {
                tex->rt_framebuffer = VK_NULL_HANDLE;
                skg_log(skg_log_critical, "failed to create framebuffer with depth attachment");
        }

        tex->rt_depth_view = depth->view;
        tex->rt_depth_tex  = depth;

        if (tex->rt_commandbuffer == VK_NULL_HANDLE) {
                VkCommandBufferAllocateInfo alloc_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                alloc_info.commandPool        = vk_cmd_pool;
                alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                alloc_info.commandBufferCount = 1;
                vkAllocateCommandBuffers(skg_device.device, &alloc_info, &tex->rt_commandbuffer);
        }
}
skg_tex_t            skg_tex_create(skg_tex_type_ type, skg_use_ use, skg_tex_fmt_ format, skg_mip_ mip_maps) {
        skg_tex_t result = {};
        result.type = type;
        result.use = use;
        result.format = format;
        result.mips = mip_maps;
        result.array_count = 1;
        result.array_start = -1;
        result.multisample = 1;
        result.mip_count = 1;
        result.rt_renderpass = -1;
        result.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return result;
}
void skg_tex_name(skg_tex_t *tex, const char* name) {
        if (tex == nullptr)
                return;

        vk_named_texture_unregister(tex);

        if (name == nullptr || name[0] == '\0')
                return;

        vk_named_texture_unregister_name(name);

        size_t name_len = strlen(name);
        char  *name_copy = (char *)malloc(name_len + 1);
        if (name_copy == nullptr) {
                skg_log(skg_log_warning, "Failed to allocate storage for texture name");
                return;
        }

        memcpy(name_copy, name, name_len + 1);

        vk_named_texture_t entry = {};
        entry.tex  = tex;
        entry.name = name_copy;
        vk_named_textures.add(entry);
}
skg_tex_t *skg_tex_find(const char *name) {
        int32_t index = vk_named_texture_find_name(name);
        return index >= 0 ? vk_named_textures[index].tex : nullptr;
}
void skg_tex_settings(skg_tex_t *tex, skg_tex_address_ address, skg_tex_sample_ sample, skg_sample_compare_ compare, int32_t anisotropy) {
        if (tex == nullptr)
                return;

        if (tex->sampler)
                vkDestroySampler(skg_device.device, tex->sampler, nullptr);

        VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler_info.magFilter    = vk_filter_from_sample(sample);
        sampler_info.minFilter    = vk_filter_from_sample(sample);
        sampler_info.mipmapMode   = vk_mipmap_mode_from_sample(sample);
        sampler_info.addressModeU = vk_address_mode_from_skg(address);
        sampler_info.addressModeV = vk_address_mode_from_skg(address);
        sampler_info.addressModeW = vk_address_mode_from_skg(address);
        sampler_info.mipLodBias   = 0.0f;

        float requested_aniso = (float)(anisotropy > 0 ? anisotropy : 1);
        float max_supported   = vk_device_features.samplerAnisotropy && vk_device_properties.limits.maxSamplerAnisotropy > 0.0f
                ? vk_device_properties.limits.maxSamplerAnisotropy
                : 1.0f;
        if (sample == skg_tex_sample_anisotropic || anisotropy > 1) {
                if (vk_device_features.samplerAnisotropy && max_supported > 1.0f) {
                        if (requested_aniso < 1.0f)
                                requested_aniso = max_supported;
                        if (requested_aniso > max_supported)
                                requested_aniso = max_supported;
                        sampler_info.anisotropyEnable = VK_TRUE;
                        sampler_info.maxAnisotropy    = requested_aniso;
                } else {
                        sampler_info.anisotropyEnable = VK_FALSE;
                        sampler_info.maxAnisotropy    = 1.0f;
                }
        } else {
                sampler_info.anisotropyEnable = VK_FALSE;
                sampler_info.maxAnisotropy    = 1.0f;
        }

        sampler_info.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampler_info.unnormalizedCoordinates = VK_FALSE;
        sampler_info.compareEnable           = compare != skg_sample_compare_none ? VK_TRUE : VK_FALSE;
        sampler_info.compareOp               = vk_compare_op_from_skg(compare);
        sampler_info.minLod                  = 0.0f;
        sampler_info.maxLod                  = VK_LOD_CLAMP_NONE;
        sampler_info.maxAnisotropy           = sampler_info.anisotropyEnable ? sampler_info.maxAnisotropy : 1.0f;

        if (vkCreateSampler(skg_device.device, &sampler_info, nullptr, &tex->sampler) != VK_SUCCESS) {
                tex->sampler = VK_NULL_HANDLE;
                skg_log(skg_log_critical, "Failed to create sampler state");
        }
}
void skg_tex_set_contents(skg_tex_t *tex, const void *data, int32_t width, int32_t height) {
        const void *data_arr[1] = { data };
        skg_tex_set_contents_arr(tex, data ? data_arr : nullptr, 1, 1, width, height, 1);
}
void skg_tex_set_contents_arr(skg_tex_t *tex, const void **array_data, int32_t array_count, int32_t array_mip_count, int32_t width, int32_t height, int32_t multisample) {
        if (tex == nullptr)
                return;

        if (tex->use != skg_use_dynamic && tex->texture != VK_NULL_HANDLE) {
                skg_log(skg_log_warning, "Only dynamic textures can be updated after creation");
                return;
        }
        if (tex->use == skg_use_dynamic && (tex->mips == skg_mip_generate || array_count > 1)) {
                skg_log(skg_log_warning, "Dynamic textures do not support autogenerated mips or texture arrays");
                return;
        }

        if (array_count <= 0)
                array_count = 1;
        if (array_mip_count <= 0)
                array_mip_count = 1;
        if (multisample <= 0)
                multisample = 1;

        bool     can_generate_mips = (width > 1 || height > 1)
                && tex->mips == skg_mip_generate
                && array_data != nullptr && array_data[0] != nullptr
                && skg_can_make_mips(tex->format)
                && array_mip_count <= 1;
        uint32_t mip_levels = can_generate_mips ? (uint32_t)skg_mip_count(width, height) : (uint32_t)array_mip_count;
        if (mip_levels == 0)
                mip_levels = 1;

        VkFormat vk_format = (VkFormat)skg_tex_fmt_to_native(tex->format);
        if (vk_format == VK_FORMAT_UNDEFINED) {
                skg_log(skg_log_critical, "Unsupported Vulkan texture format");
                return;
        }

        bool recreate_image = tex->texture == VK_NULL_HANDLE
                || tex->width       != width
                || tex->height      != height
                || tex->array_count != array_count
                || tex->multisample != multisample
                || tex->mip_count   != mip_levels;

        tex->width       = width;
        tex->height      = height;
        tex->array_count = array_count;
        tex->array_start = -1;
        tex->multisample = multisample;
        tex->mip_count   = mip_levels;

        if (recreate_image)
                vk_destroy_tex_gpu_objects(tex);

        if (tex->texture == VK_NULL_HANDLE) {
                VkImageCreateInfo image_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
                image_info.imageType     = VK_IMAGE_TYPE_2D;
                image_info.extent.width  = (uint32_t)width;
                image_info.extent.height = (uint32_t)height;
                image_info.extent.depth  = 1;
                image_info.mipLevels     = mip_levels;
                image_info.arrayLayers   = (uint32_t)array_count;
                image_info.format        = vk_format;
                image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
                image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                image_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                if (tex->type == skg_tex_type_rendertarget)
                        image_info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                else if (tex->type == skg_tex_type_zbuffer || tex->type == skg_tex_type_depthtarget)
                        image_info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                else
                        image_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
                if ((tex->use & skg_use_compute_write) != 0)
                        image_info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
                image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
                image_info.samples       = vk_sample_count_from_int(multisample);
                image_info.flags         = 0;
                if ((tex->use & skg_use_cubemap) != 0)
                        image_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

                if (vkCreateImage(skg_device.device, &image_info, nullptr, &tex->texture) != VK_SUCCESS) {
                        tex->texture = VK_NULL_HANDLE;
                        skg_log(skg_log_critical, "Failed to create Vulkan texture");
                        return;
                }

                VkMemoryRequirements mem_requirements;
                vkGetImageMemoryRequirements(skg_device.device, tex->texture, &mem_requirements);

                VkMemoryAllocateInfo alloc_info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                alloc_info.allocationSize  = mem_requirements.size;
                alloc_info.memoryTypeIndex = vk_find_mem_type(mem_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (vkAllocateMemory(skg_device.device, &alloc_info, nullptr, &tex->texture_mem) != VK_SUCCESS) {
                        vkDestroyImage(skg_device.device, tex->texture, nullptr);
                        tex->texture = VK_NULL_HANDLE;
                        skg_log(skg_log_critical, "Failed to allocate texture memory");
                        return;
                }
                vkBindImageMemory(skg_device.device, tex->texture, tex->texture_mem, 0);
        }

        if (tex->view == VK_NULL_HANDLE)
                skg_tex_create_views(tex);

        bool has_data = false;
        if (array_data != nullptr) {
                for (int32_t arr = 0; arr < array_count; arr++) {
                        if (array_data[arr] != nullptr) {
                                has_data = true;
                                break;
                        }
                }
        }
        VkImageAspectFlags aspect = vk_aspect_from_format(tex->format);
        VkImageLayout final_layout = vk_target_layout_for_tex(tex);

        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        VkBufferImageCopy *copy_regions = nullptr;
        uint32_t copy_levels = can_generate_mips ? 1 : mip_levels;
        size_t   total_bytes = 0;

        if (has_data) {
                copy_regions = (VkBufferImageCopy *)malloc(sizeof(VkBufferImageCopy) * (size_t)array_count * copy_levels);
                if (copy_regions == nullptr) {
                        skg_log(skg_log_critical, "Out of memory while uploading texture data");
                        return;
                }

                size_t region_index = 0;
                for (int32_t arr = 0; arr < array_count; arr++) {
                        const uint8_t *base_data = array_data[arr] ? (const uint8_t *)array_data[arr] : nullptr;
                        size_t mip_offset = 0;
                        int32_t mip_width  = width;
                        int32_t mip_height = height;

                        for (uint32_t level = 0; level < copy_levels; level++) {
                                size_t mip_size = skg_tex_fmt_memory(tex->format, mip_width, mip_height);
                                VkBufferImageCopy *region = &copy_regions[region_index++];
                                region->bufferOffset = total_bytes;
                                region->bufferRowLength   = 0;
                                region->bufferImageHeight = 0;
                                region->imageSubresource.aspectMask     = aspect;
                                region->imageSubresource.mipLevel       = level;
                                region->imageSubresource.baseArrayLayer = (uint32_t)arr;
                                region->imageSubresource.layerCount     = 1;
                                region->imageOffset = {0, 0, 0};
                                region->imageExtent.width  = (uint32_t)mip_width;
                                region->imageExtent.height = (uint32_t)mip_height;
                                region->imageExtent.depth  = 1;

                                total_bytes += mip_size;
                                if (base_data)
                                        mip_offset += mip_size;

                                mip_width  = mip_width  > 1 ? mip_width  / 2 : 1;
                                mip_height = mip_height > 1 ? mip_height / 2 : 1;
                        }
                }

                if (total_bytes > 0) {
                        vk_create_buffer(total_bytes,
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                &staging_buffer, &staging_memory);
                        if (staging_buffer == VK_NULL_HANDLE) {
                                free(copy_regions);
                                skg_log(skg_log_critical, "Failed to allocate staging buffer for texture upload");
                                return;
                        }

                        void *mapped = nullptr;
                        if (vkMapMemory(skg_device.device, staging_memory, 0, total_bytes, 0, &mapped) != VK_SUCCESS) {
                                vkDestroyBuffer(skg_device.device, staging_buffer, nullptr);
                                vkFreeMemory  (skg_device.device, staging_memory, nullptr);
                                free(copy_regions);
                                skg_log(skg_log_critical, "Failed to map staging buffer for texture upload");
                                return;
                        }

                        size_t write_offset = 0;
                        for (int32_t arr = 0; arr < array_count; arr++) {
                                const uint8_t *base_data = array_data[arr] ? (const uint8_t *)array_data[arr] : nullptr;
                                size_t mip_offset = 0;
                                int32_t mip_width  = width;
                                int32_t mip_height = height;
                                for (uint32_t level = 0; level < copy_levels; level++) {
                                        size_t mip_size = skg_tex_fmt_memory(tex->format, mip_width, mip_height);
                                        if (base_data)
                                                memcpy((uint8_t *)mapped + write_offset, base_data + mip_offset, mip_size);
                                        else
                                                memset((uint8_t *)mapped + write_offset, 0, mip_size);

                                        write_offset += mip_size;
                                        if (base_data)
                                                mip_offset += mip_size;
                                        mip_width  = mip_width  > 1 ? mip_width  / 2 : 1;
                                        mip_height = mip_height > 1 ? mip_height / 2 : 1;
                                }
                        }

                        vkUnmapMemory(skg_device.device, staging_memory);
                }
        }

        VkImageLayout previous_layout = tex->layout;
        bool needs_transition_only = !has_data
                && !recreate_image
                && tex->texture != VK_NULL_HANDLE
                && previous_layout != final_layout;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if ((has_data || recreate_image || needs_transition_only) && tex->texture != VK_NULL_HANDLE) {
                cmd = vk_begin_transient_cmd();
                if (cmd == VK_NULL_HANDLE) {
                        if (staging_buffer != VK_NULL_HANDLE) {
                                vkDestroyBuffer(skg_device.device, staging_buffer, nullptr);
                                vkFreeMemory  (skg_device.device, staging_memory, nullptr);
                        }
                        free(copy_regions);
                        skg_log(skg_log_critical, "Failed to acquire command buffer for texture upload");
                        return;
                }
        }

        if (has_data && cmd != VK_NULL_HANDLE) {
                vk_transition_image(cmd, tex->texture, tex->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect, mip_levels, array_count);
                tex->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

                if (staging_buffer != VK_NULL_HANDLE && copy_regions != nullptr && total_bytes > 0) {
                        vkCmdCopyBufferToImage(cmd, staging_buffer, tex->texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)(array_count * copy_levels), copy_regions);
                }

                if (can_generate_mips)
                        vk_generate_mips(cmd, tex->texture, vk_format, width, height, mip_levels, array_count, aspect);

                VkImageLayout intermediate_layout = can_generate_mips ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                vk_transition_image(cmd, tex->texture, intermediate_layout, final_layout, aspect, mip_levels, array_count);
                tex->layout = final_layout;
        } else if (recreate_image && cmd != VK_NULL_HANDLE) {
                vk_transition_image(cmd, tex->texture, VK_IMAGE_LAYOUT_UNDEFINED, final_layout, aspect, mip_levels, array_count);
                tex->layout = final_layout;
        } else if (needs_transition_only && cmd != VK_NULL_HANDLE) {
                vk_transition_image(cmd, tex->texture, previous_layout, final_layout, aspect, mip_levels, array_count);
                tex->layout = final_layout;
        }

        if (cmd != VK_NULL_HANDLE)
                vk_end_transient_cmd(cmd);

        if (staging_buffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(skg_device.device, staging_buffer, nullptr);
                vkFreeMemory  (skg_device.device, staging_memory, nullptr);
        }
        free(copy_regions);

        if (tex->sampler == VK_NULL_HANDLE)
                skg_tex_settings(tex, skg_tex_address_repeat, skg_tex_sample_linear, skg_sample_compare_none, 0);
}

///////////////////////////////////////////

void skg_tex_copy_to(const skg_tex_t *tex, int32_t tex_surface, skg_tex_t *destination, int32_t dest_surface) {
        if (tex == nullptr || destination == nullptr)
                return;
        if (tex->texture == VK_NULL_HANDLE || destination->texture == VK_NULL_HANDLE) {
                skg_log(skg_log_warning, "Attempted to copy an uninitialized Vulkan texture");
                return;
        }
        if ((tex_surface < 0) != (dest_surface < 0)) {
                skg_log(skg_log_warning, "Texture copy requires both surfaces or neither");
                return;
        }

        bool src_ms = tex->multisample > 1;
        bool dst_ms = destination->multisample > 1;
        if (!src_ms && dst_ms) {
                skg_log(skg_log_warning, "Cannot copy a single-sample texture into a multisampled destination on Vulkan");
                return;
        }
        if (src_ms && dst_ms && tex->multisample != destination->multisample) {
                skg_log(skg_log_warning, "Sample count mismatch between source and destination textures");
                return;
        }

        if (destination->texture == VK_NULL_HANDLE) {
                if (destination->use == skg_use_dynamic) {
                        skg_tex_set_contents_arr(destination, nullptr, tex->array_count, 1, tex->width, tex->height, tex->multisample);
                }
                if (destination->texture == VK_NULL_HANDLE) {
                        skg_log(skg_log_warning, "Destination texture has no Vulkan image for copy");
                        return;
                }
        }
        if (destination->width != tex->width || destination->height != tex->height || destination->multisample != tex->multisample) {
                skg_log(skg_log_warning, "Source and destination textures must share dimensions and sample count for copy");
                return;
        }

        VkImageAspectFlags src_aspect = vk_aspect_from_format(tex->format);
        VkImageAspectFlags dst_aspect = vk_aspect_from_format(destination->format);
        if (src_aspect != dst_aspect) {
                skg_log(skg_log_warning, "Texture copy requires matching aspects between source and destination");
                return;
        }

        VkImageLayout src_initial = tex->layout != VK_IMAGE_LAYOUT_UNDEFINED ? tex->layout : vk_target_layout_for_tex(tex);
        VkImageLayout dst_initial = destination->layout != VK_IMAGE_LAYOUT_UNDEFINED ? destination->layout : vk_target_layout_for_tex(destination);

        uint32_t src_layers = tex->array_count > 0 ? (uint32_t)tex->array_count : 1u;
        uint32_t dst_layers = destination->array_count > 0 ? (uint32_t)destination->array_count : 1u;
        uint32_t src_mips   = tex->mip_count   > 0 ? tex->mip_count   : 1u;
        uint32_t dst_mips   = destination->mip_count > 0 ? destination->mip_count : 1u;

        VkCommandBuffer cmd = vk_begin_transient_cmd();
        if (cmd == VK_NULL_HANDLE) {
                skg_log(skg_log_critical, "Failed to acquire command buffer for texture copy");
                return;
        }

        vk_transition_image(cmd, tex->texture, src_initial, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_aspect, src_mips, src_layers);
        vk_transition_image(cmd, destination->texture, dst_initial, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_aspect, dst_mips, dst_layers);

        if (tex_surface >= 0) {
                uint32_t src_layer = 0, src_mip = 0, dst_layer = 0, dst_mip = 0;
                if (!vk_tex_decode_surface(tex, tex_surface, &src_layer, &src_mip) ||
                        !vk_tex_decode_surface(destination, dest_surface, &dst_layer, &dst_mip)) {

                        vk_transition_image(cmd, tex->texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_initial, src_aspect, src_mips, src_layers);
                        vk_transition_image(cmd, destination->texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_initial, dst_aspect, dst_mips, dst_layers);
                        vk_end_transient_cmd(cmd);
                        return;
                }

                int32_t mip_width = tex->width;
                int32_t mip_height = tex->height;
                skg_mip_dimensions(tex->width, tex->height, (int32_t)src_mip, &mip_width, &mip_height);

                if (src_ms && !dst_ms) {
                        VkImageResolve region = {};
                        region.srcSubresource.aspectMask     = src_aspect;
                        region.srcSubresource.baseArrayLayer = src_layer;
                        region.srcSubresource.layerCount     = 1;
                        region.srcSubresource.mipLevel       = src_mip;
                        region.dstSubresource.aspectMask     = dst_aspect;
                        region.dstSubresource.baseArrayLayer = dst_layer;
                        region.dstSubresource.layerCount     = 1;
                        region.dstSubresource.mipLevel       = dst_mip;
                        region.extent.width  = (uint32_t)mip_width;
                        region.extent.height = (uint32_t)mip_height;
                        region.extent.depth  = 1;
                        vkCmdResolveImage(cmd, tex->texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                destination->texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                } else {
                        VkImageCopy region = {};
                        region.srcSubresource.aspectMask     = src_aspect;
                        region.srcSubresource.baseArrayLayer = src_layer;
                        region.srcSubresource.layerCount     = 1;
                        region.srcSubresource.mipLevel       = src_mip;
                        region.dstSubresource.aspectMask     = dst_aspect;
                        region.dstSubresource.baseArrayLayer = dst_layer;
                        region.dstSubresource.layerCount     = 1;
                        region.dstSubresource.mipLevel       = dst_mip;
                        region.extent.width  = (uint32_t)mip_width;
                        region.extent.height = (uint32_t)mip_height;
                        region.extent.depth  = 1;
                        vkCmdCopyImage(cmd, tex->texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                destination->texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                }
        } else {
                uint32_t layer_limit = src_layers < dst_layers ? src_layers : dst_layers;
                uint32_t mip_limit   = src_mips   < dst_mips   ? src_mips   : dst_mips;

                for (uint32_t layer = 0; layer < layer_limit; layer++) {
                        for (uint32_t mip = 0; mip < mip_limit; mip++) {
                                int32_t mip_width = tex->width;
                                int32_t mip_height = tex->height;
                                skg_mip_dimensions(tex->width, tex->height, (int32_t)mip, &mip_width, &mip_height);

                                if (src_ms && !dst_ms) {
                                        VkImageResolve region = {};
                                        region.srcSubresource.aspectMask     = src_aspect;
                                        region.srcSubresource.baseArrayLayer = layer;
                                        region.srcSubresource.layerCount     = 1;
                                        region.srcSubresource.mipLevel       = mip;
                                        region.dstSubresource.aspectMask     = dst_aspect;
                                        region.dstSubresource.baseArrayLayer = layer;
                                        region.dstSubresource.layerCount     = 1;
                                        region.dstSubresource.mipLevel       = mip;
                                        region.extent.width  = (uint32_t)mip_width;
                                        region.extent.height = (uint32_t)mip_height;
                                        region.extent.depth  = 1;
                                        vkCmdResolveImage(cmd, tex->texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                destination->texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                                } else {
                                        VkImageCopy region = {};
                                        region.srcSubresource.aspectMask     = src_aspect;
                                        region.srcSubresource.baseArrayLayer = layer;
                                        region.srcSubresource.layerCount     = 1;
                                        region.srcSubresource.mipLevel       = mip;
                                        region.dstSubresource.aspectMask     = dst_aspect;
                                        region.dstSubresource.baseArrayLayer = layer;
                                        region.dstSubresource.layerCount     = 1;
                                        region.dstSubresource.mipLevel       = mip;
                                        region.extent.width  = (uint32_t)mip_width;
                                        region.extent.height = (uint32_t)mip_height;
                                        region.extent.depth  = 1;
                                        vkCmdCopyImage(cmd, tex->texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                destination->texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                                }
                        }
                }
        }

        vk_transition_image(cmd, tex->texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_initial, src_aspect, src_mips, src_layers);
        vk_transition_image(cmd, destination->texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_initial, dst_aspect, dst_mips, dst_layers);
        vk_end_transient_cmd(cmd);

        ((skg_tex_t *)tex)->layout = src_initial;
        destination->layout = dst_initial;
}

///////////////////////////////////////////

void skg_tex_copy_to_swapchain(const skg_tex_t *tex, skg_swapchain_t *destination) {
        if (tex == nullptr || destination == nullptr || destination->textures == nullptr || destination->img_count == 0)
                return;

        uint32_t index = destination->img_curr < destination->img_count ? destination->img_curr : 0;
        skg_tex_t *target = &destination->textures[index];
        skg_tex_copy_to(tex, -1, target, -1);
}

///////////////////////////////////////////

bool skg_tex_is_valid(const skg_tex_t *tex) {
        return tex != nullptr && tex->texture != VK_NULL_HANDLE;
}

///////////////////////////////////////////

bool skg_tex_get_contents(skg_tex_t *tex, void *ref_data, size_t data_size) {
        return skg_tex_get_mip_contents_arr(tex, 0, 0, ref_data, data_size);
}

///////////////////////////////////////////

bool skg_tex_get_mip_contents(skg_tex_t *tex, int32_t mip_level, void *ref_data, size_t data_size) {
        return skg_tex_get_mip_contents_arr(tex, mip_level, 0, ref_data, data_size);
}

///////////////////////////////////////////

bool skg_tex_get_mip_contents_arr(skg_tex_t *tex, int32_t mip_level, int32_t arr_index, void *ref_data, size_t data_size) {
        if (tex == nullptr || ref_data == nullptr)
                return false;
        if (tex->texture == VK_NULL_HANDLE) {
                skg_log(skg_log_warning, "Texture has no GPU resource to read from");
                return false;
        }
        if (tex->multisample > 1) {
                skg_log(skg_log_warning, "Reading back multisampled textures is not supported on Vulkan yet");
                return false;
        }

        uint32_t mip_count = tex->mip_count > 0 ? tex->mip_count : 1;
        if (mip_level < 0 || (uint32_t)mip_level >= mip_count) {
                skg_log(skg_log_critical, "Requested mip level is out of range");
                return false;
        }
        if (arr_index < 0 || (uint32_t)arr_index >= (uint32_t)(tex->array_count > 0 ? tex->array_count : 1)) {
                skg_log(skg_log_critical, "Requested array slice is out of range");
                return false;
        }

        int32_t mip_width  = 0;
        int32_t mip_height = 0;
        skg_mip_dimensions(tex->width, tex->height, mip_level, &mip_width, &mip_height);
        size_t mip_size = skg_tex_fmt_memory(tex->format, mip_width, mip_height);
        if (data_size != mip_size) {
                skg_log(skg_log_critical, "Insufficient buffer size for texture readback");
                return false;
        }

        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        vk_create_buffer(mip_size,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &staging_buffer, &staging_memory);
        if (staging_buffer == VK_NULL_HANDLE || staging_memory == VK_NULL_HANDLE) {
                skg_log(skg_log_critical, "Failed to allocate staging resources for texture readback");
                if (staging_buffer != VK_NULL_HANDLE)
                        vkDestroyBuffer(skg_device.device, staging_buffer, nullptr);
                if (staging_memory != VK_NULL_HANDLE)
                        vkFreeMemory  (skg_device.device, staging_memory, nullptr);
                return false;
        }

        VkCommandBuffer cmd = vk_begin_transient_cmd();
        if (cmd == VK_NULL_HANDLE) {
                skg_log(skg_log_critical, "Failed to acquire command buffer for texture readback");
                vkDestroyBuffer(skg_device.device, staging_buffer, nullptr);
                vkFreeMemory  (skg_device.device, staging_memory, nullptr);
                return false;
        }

        VkImageAspectFlags aspect = vk_aspect_from_format(tex->format);
        uint32_t layer_count = tex->array_count > 0 ? (uint32_t)tex->array_count : 1;
        VkImageLayout original_layout = tex->layout;
        if (original_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                skg_log(skg_log_warning, "Texture layout is undefined; cannot read back data");
                vk_end_transient_cmd(cmd);
                vkDestroyBuffer(skg_device.device, staging_buffer, nullptr);
                vkFreeMemory  (skg_device.device, staging_memory, nullptr);
                return false;
        }

        bool transitioned = false;
        if (original_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
                vk_transition_image(cmd, tex->texture, original_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect, mip_count, layer_count);
                tex->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                transitioned = true;
        }

        VkBufferImageCopy region = {};
        region.bufferOffset = 0;
        region.bufferRowLength   = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask     = aspect;
        region.imageSubresource.mipLevel       = (uint32_t)mip_level;
        region.imageSubresource.baseArrayLayer = (uint32_t)arr_index;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent.width  = (uint32_t)mip_width;
        region.imageExtent.height = (uint32_t)mip_height;
        region.imageExtent.depth  = 1;

        vkCmdCopyImageToBuffer(cmd, tex->texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_buffer, 1, &region);

        if (transitioned) {
                vk_transition_image(cmd, tex->texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, original_layout, aspect, mip_count, layer_count);
        }

        vk_end_transient_cmd(cmd);
        tex->layout = original_layout;

        void *mapped = nullptr;
        VkResult map_result = vkMapMemory(skg_device.device, staging_memory, 0, mip_size, 0, &mapped);
        if (map_result != VK_SUCCESS || mapped == nullptr) {
                skg_log(skg_log_critical, "Failed to map staging buffer for texture readback");
                if (mapped)
                        vkUnmapMemory(skg_device.device, staging_memory);
                vkDestroyBuffer(skg_device.device, staging_buffer, nullptr);
                vkFreeMemory  (skg_device.device, staging_memory, nullptr);
                return false;
        }

        memcpy(ref_data, mapped, mip_size);
        vkUnmapMemory(skg_device.device, staging_memory);

        vkDestroyBuffer(skg_device.device, staging_buffer, nullptr);
        vkFreeMemory  (skg_device.device, staging_memory, nullptr);

        return true;
}

///////////////////////////////////////////

bool skg_tex_gen_mips(skg_tex_t *tex) {
        if (tex == nullptr || tex->texture == VK_NULL_HANDLE)
                return false;
        if (tex->mip_count <= 1)
                return true;
        if (tex->multisample > 1) {
                skg_log(skg_log_warning, "Cannot generate mips for multisampled textures on Vulkan");
                return false;
        }
        if (skg_tex_fmt_is_depth(tex->format)) {
                skg_log(skg_log_warning, "Cannot generate mips for depth textures on Vulkan");
                return false;
        }

        VkFormat vk_format = (VkFormat)skg_tex_fmt_to_native(tex->format);
        if (vk_format == VK_FORMAT_UNDEFINED) {
                skg_log(skg_log_warning, "Unsupported texture format for Vulkan mip generation");
                return false;
        }

        uint32_t mip_levels  = tex->mip_count > 0 ? tex->mip_count : 1u;
        uint32_t layer_count = tex->array_count > 0 ? (uint32_t)tex->array_count : 1u;
        VkImageAspectFlags aspect = vk_aspect_from_format(tex->format);

        VkCommandBuffer cmd = vk_begin_transient_cmd();
        if (cmd == VK_NULL_HANDLE) {
                skg_log(skg_log_critical, "Failed to acquire command buffer for mip generation");
                return false;
        }

        VkImageLayout original_layout = tex->layout;
        VkImageLayout final_layout    = vk_target_layout_for_tex(tex);
        if (tex->texture_mem == VK_NULL_HANDLE && original_layout != VK_IMAGE_LAYOUT_UNDEFINED)
                final_layout = original_layout;
        if (final_layout == VK_IMAGE_LAYOUT_UNDEFINED)
                final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        vk_transition_image(cmd, tex->texture, original_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect, mip_levels, layer_count);
        tex->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        vk_generate_mips(cmd, tex->texture, vk_format, tex->width, tex->height, mip_levels, layer_count, aspect);

        vk_transition_image(cmd, tex->texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, final_layout, aspect, mip_levels, layer_count);
        vk_end_transient_cmd(cmd);

        tex->layout = final_layout;
        vk_mark_descriptor_dirty(skg_stage_vertex | skg_stage_pixel | skg_stage_compute);
        return true;
}

///////////////////////////////////////////

void* skg_tex_get_native(const skg_tex_t *tex) {
        return tex != nullptr ? (void*)tex->texture : nullptr;
}

///////////////////////////////////////////

void skg_tex_clear(skg_bind_t bind) {
        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = vk_descriptor_set;
        write.descriptorCount = 1;

        switch (bind.register_type) {
        case skg_register_resource: {
                if (bind.slot >= VK_MAX_TEXTURE_SLOTS) {
                        skg_log(skg_log_warning, "Texture slot exceeds Vulkan descriptor layout");
                        return;
                }
                write.dstBinding     = VK_BINDING_TEXTURES + bind.slot;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                if (vk_dummy_image_view == VK_NULL_HANDLE) {
                        skg_log(skg_log_warning, "No dummy texture available for clear");
                        return;
                }
                write.pImageInfo     = &vk_dummy_texture_info;
        } break;
        case skg_register_readwrite: {
                if (bind.slot >= VK_MAX_STORAGE_SLOTS) {
                        skg_log(skg_log_warning, "Storage slot exceeds Vulkan descriptor layout");
                        return;
                }
                write.dstBinding     = VK_BINDING_STORAGE + bind.slot;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                if (vk_dummy_image_view == VK_NULL_HANDLE) {
                        skg_log(skg_log_warning, "No dummy storage texture available for clear");
                        return;
                }
                write.pImageInfo     = &vk_dummy_storage_image_info;
        } break;
        default:
                skg_log(skg_log_warning, "You can only clear textures bound as resources or read/write");
                return;
        }

        vkUpdateDescriptorSets(skg_device.device, 1, &write, 0, nullptr);
        vk_mark_descriptor_dirty(bind.stage_bits);
        if (bind.stage_bits & (skg_stage_vertex | skg_stage_pixel))
                vk_flush_descriptor_binding_graphics();
        if (bind.stage_bits & skg_stage_compute)
                vk_flush_descriptor_binding_compute(skg_active_rendertarget ? skg_active_rendertarget->rt_commandbuffer : VK_NULL_HANDLE);
}

void skg_tex_bind(const skg_tex_t *tex, skg_bind_t bind) {
        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = vk_descriptor_set;
        write.descriptorCount = 1;

        VkDescriptorImageInfo image_info = {};
        const VkDescriptorImageInfo *image_ptr = nullptr;

        switch (bind.register_type) {
        case skg_register_resource: {
                uint32_t binding = VK_BINDING_TEXTURES + bind.slot;
                if (binding >= VK_BINDING_TEXTURES + VK_MAX_TEXTURE_SLOTS) {
                        skg_log(skg_log_warning, "Texture slot exceeds Vulkan descriptor layout");
                        return;
                }
                write.dstBinding     = binding;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                if (tex != nullptr && tex->view != VK_NULL_HANDLE && tex->sampler != VK_NULL_HANDLE) {
                        image_info.sampler     = tex->sampler;
                        image_info.imageView   = tex->view;
                        VkImageLayout layout = tex->layout != VK_IMAGE_LAYOUT_UNDEFINED
                                ? tex->layout
                                : (skg_tex_fmt_is_depth(tex->format)
                                        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        image_info.imageLayout = layout;
                        image_ptr = &image_info;
                } else {
                        if (vk_dummy_image_view == VK_NULL_HANDLE) {
                                skg_log(skg_log_warning, "No dummy texture available for bind");
                                return;
                        }
                        image_ptr = &vk_dummy_texture_info;
                }
        } break;
        case skg_register_readwrite: {
                uint32_t binding = VK_BINDING_STORAGE + bind.slot;
                if (binding >= VK_BINDING_STORAGE + VK_MAX_STORAGE_SLOTS) {
                        skg_log(skg_log_warning, "Storage slot exceeds Vulkan descriptor layout");
                        return;
                }
                write.dstBinding     = binding;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                if (tex != nullptr && tex->view != VK_NULL_HANDLE) {
                        image_info.imageView   = tex->view;
                        image_info.imageLayout = tex->layout != VK_IMAGE_LAYOUT_UNDEFINED
                                ? tex->layout
                                : VK_IMAGE_LAYOUT_GENERAL;
                        image_ptr = &image_info;
                } else {
                        if (vk_dummy_image_view == VK_NULL_HANDLE) {
                                skg_log(skg_log_warning, "No dummy storage texture available for bind");
                                return;
                        }
                        image_ptr = &vk_dummy_storage_image_info;
                }
        } break;
        default:
                skg_log(skg_log_critical, "Unsupported texture bind register type");
                return;
        }

        if (image_ptr == nullptr)
                return;

        write.pImageInfo = image_ptr;
        vkUpdateDescriptorSets(skg_device.device, 1, &write, 0, nullptr);
        vk_mark_descriptor_dirty(bind.stage_bits);
        if (bind.stage_bits & (skg_stage_vertex | skg_stage_pixel))
                vk_flush_descriptor_binding_graphics();
        if (bind.stage_bits & skg_stage_compute)
                vk_flush_descriptor_binding_compute(skg_active_rendertarget ? skg_active_rendertarget->rt_commandbuffer : VK_NULL_HANDLE);
}

///////////////////////////////////////////

void skg_tex_destroy(skg_tex_t *tex) {
        vk_named_texture_unregister(tex);

        if (tex->rt_framebuffer) vkDestroyFramebuffer(skg_device.device, tex->rt_framebuffer, nullptr);
        if (tex->rt_renderpass ) vk_renderpass_release(tex->rt_renderpass);
        if (tex->rt_commandbuffer != VK_NULL_HANDLE)
                vkFreeCommandBuffers(skg_device.device, vk_cmd_pool, 1, &tex->rt_commandbuffer);

        if (tex->view)    vkDestroyImageView  (skg_device.device, tex->view,    nullptr);
        if (tex->sampler) vkDestroySampler    (skg_device.device, tex->sampler, nullptr);
        if (tex->texture_mem != VK_NULL_HANDLE) {
                if (tex->texture) vkDestroyImage(skg_device.device, tex->texture, nullptr);
                vkFreeMemory(skg_device.device, tex->texture_mem, nullptr);
        }
        *tex = {};
}

///////////////////////////////////////////

int64_t skg_tex_fmt_to_native(skg_tex_fmt_ format) {
        switch (format) {
        case skg_tex_fmt_rgba32:        return VK_FORMAT_R8G8B8A8_SRGB;
        case skg_tex_fmt_rgba32_linear: return VK_FORMAT_R8G8B8A8_UNORM;
        case skg_tex_fmt_bgra32:        return VK_FORMAT_B8G8R8A8_SRGB;
	case skg_tex_fmt_bgra32_linear: return VK_FORMAT_B8G8R8A8_UNORM;
	case skg_tex_fmt_rgba64:        return VK_FORMAT_R16G16B16A16_UNORM;
	case skg_tex_fmt_rgba128:       return VK_FORMAT_R32G32B32A32_SFLOAT;
	case skg_tex_fmt_depth16:       return VK_FORMAT_D16_UNORM;
	case skg_tex_fmt_depth32:       return VK_FORMAT_D32_SFLOAT;
	case skg_tex_fmt_depthstencil:  return VK_FORMAT_D24_UNORM_S8_UINT;
	case skg_tex_fmt_r8:            return VK_FORMAT_R8_UNORM;
	case skg_tex_fmt_r16:           return VK_FORMAT_R16_UNORM;
	case skg_tex_fmt_r32:           return VK_FORMAT_R32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
        }
}

///////////////////////////////////////////

skg_tex_fmt_ skg_tex_fmt_from_native(int64_t format) {
        return skg_native_to_tex_fmt((VkFormat)format);
}

///////////////////////////////////////////

bool skg_tex_fmt_supported(skg_tex_fmt_ format) {
        VkFormat vk_format = (VkFormat)skg_tex_fmt_to_native(format);
        if (vk_format == VK_FORMAT_UNDEFINED)
                return false;

        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(skg_device.phys_device, vk_format, &props);

        VkFormatFeatureFlags features = props.optimalTilingFeatures;
        if (skg_tex_fmt_is_depth(format))
                return (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        return (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

///////////////////////////////////////////

skg_tex_fmt_ skg_native_to_tex_fmt(VkFormat format) {
        switch (format) {
        case VK_FORMAT_R8G8B8A8_SRGB:       return skg_tex_fmt_rgba32;
        case VK_FORMAT_R8G8B8A8_UNORM:      return skg_tex_fmt_rgba32_linear;
        case VK_FORMAT_B8G8R8A8_SRGB:       return skg_tex_fmt_bgra32;
	case VK_FORMAT_B8G8R8A8_UNORM:      return skg_tex_fmt_bgra32_linear;
	case VK_FORMAT_R16G16B16A16_UNORM:  return skg_tex_fmt_rgba64;
	case VK_FORMAT_R32G32B32A32_SFLOAT: return skg_tex_fmt_rgba128;
	case VK_FORMAT_D16_UNORM:           return skg_tex_fmt_depth16;
	case VK_FORMAT_D32_SFLOAT:          return skg_tex_fmt_depth32;
	case VK_FORMAT_D24_UNORM_S8_UINT:   return skg_tex_fmt_depthstencil;
	case VK_FORMAT_R8_UNORM:            return skg_tex_fmt_r8;
	case VK_FORMAT_R16_UNORM:           return skg_tex_fmt_r16;
	case VK_FORMAT_R32_SFLOAT:          return skg_tex_fmt_r32;
	default: return skg_tex_fmt_none;
	}
}

#endif
