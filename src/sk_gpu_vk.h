#pragma once

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

///////////////////////////////////////////

typedef struct skg_buffer_t {
        skg_use_               use;
        skg_buffer_type_       type;
        uint32_t               stride;
        uint32_t               count;
        VkBuffer               buffer;
        VkDeviceMemory         memory;
        VkDescriptorBufferInfo descriptor_uniform;
        VkDescriptorBufferInfo descriptor_storage;
};

typedef struct skg_mesh_t {
        VkBuffer vert_buffer;
        VkBuffer ind_buffer;
} skg_mesh_t;

typedef struct skg_shader_stage_t {
	skg_stage_     type;
	VkShaderModule module;
} skg_shader_stage_t;

typedef struct skg_shader_t {
        skg_shader_meta_t *meta;
        VkShaderModule     _vertex;
        VkShaderModule     _pixel;
        VkShaderModule     _compute;
        VkPipeline         compute_pipeline;
        VkPipelineLayout   compute_layout;
} skg_shader_t;

typedef struct skg_pipeline_t {
        skg_transparency_      transparency;
        skg_cull_              cull;
        bool                   wireframe;
        bool                   depth_write;
        bool                   depth_clip;
        skg_color_write_       color_write;
        bool                   scissor;
        skg_depth_test_        depth_test;
        bool                   dirty;

        skg_shader_meta_t     *meta;
        VkShaderModule         vertex_shader;
        VkShaderModule         pixel_shader;

        int64_t                pipeline;
        VkPipelineLayout       pipeline_layout;
} skg_pipeline_t;

typedef struct skg_tex_t {
        int32_t         width;
        int32_t         height;
        int32_t         array_count;
        int32_t         array_start;
        int32_t         multisample;
        skg_use_        use;
        skg_tex_type_   type;
        skg_tex_fmt_    format;
        skg_mip_        mips;
        uint32_t        mip_count;

        VkImage         texture;
        VkDeviceMemory  texture_mem;
        VkImageView     view;
        VkSampler       sampler;
        VkImageView         rt_depth_view;
        struct skg_tex_t   *rt_depth_tex;
        VkImageLayout   layout;

        VkFramebuffer   rt_framebuffer;
        int64_t         rt_renderpass;
        VkCommandBuffer rt_commandbuffer;
} skg_tex_t;

typedef struct skg_swapchain_t {
	int32_t width;
	int32_t height;
	//skg_tex_t target;
	//skg_tex_t depth;

        VkSurfaceFormatKHR format;
        VkSwapchainKHR     swapchain;
        //VkFence           *fence;
        uint32_t           img_active;
        uint32_t           img_count;
        VkImage           *imgs;
        skg_tex_t         *textures;
        skg_tex_t         *depths;
        uint32_t           img_curr;
        VkExtent2D         extents;
        VkFence           *img_fence;

        skg_tex_fmt_       color_format;
        skg_tex_fmt_       depth_format;

        VkSemaphore sem_available[2];
        VkSemaphore sem_finished[2];
        VkFence     fence_flight[2];
        int32_t     sync_index;
} skg_swapchain_t;

typedef struct skg_platform_data_t {
        VkInstance       instance;
        VkPhysicalDevice phys_device;
        VkDevice         device;
        VkQueue          queue_gfx;
        VkQueue          queue_present;
        uint32_t         queue_gfx_index;
        uint32_t         queue_present_index;
        VkSurfaceKHR     surface;
} skg_platform_data_t;
