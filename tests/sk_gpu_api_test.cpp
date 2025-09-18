#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cmath>

#define SKG_IMPL
#include "sk_gpu.h"

namespace {

bool check_bytes_equal(const uint32_t *a, const uint32_t *b, size_t count) {
    return std::memcmp(a, b, count * sizeof(uint32_t)) == 0;
}

bool check_color_close(uint32_t pixel, const float expected[4], uint8_t tolerance) {
    const uint8_t *rgba = reinterpret_cast<const uint8_t*>(&pixel);
    for (int i = 0; i < 4; ++i) {
        uint8_t expected_byte = static_cast<uint8_t>(std::roundf(std::fmax(0.0f, std::fmin(1.0f, expected[i])) * 255.0f));
        if (std::abs(static_cast<int>(rgba[i]) - static_cast<int>(expected_byte)) > tolerance) {
            return false;
        }
    }
    return true;
}

bool test_adapter_name() {
    const char *name = skg_adapter_name();
    if (name == nullptr || std::strlen(name) == 0) {
        std::fprintf(stderr, "Adapter name was empty.\n");
        return false;
    }
    std::printf("Adapter: %s\n", name);
    return true;
}

bool test_buffer_roundtrip() {
    const float vertices[] = {
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.0f,  0.5f, 0.0f,
    };

    skg_buffer_t vbuf = skg_buffer_create(vertices, 3, sizeof(float) * 3, skg_buffer_type_vertex, skg_use_static);
    if (!skg_buffer_is_valid(&vbuf)) {
        std::fprintf(stderr, "Static vertex buffer creation failed.\n");
        return false;
    }

    skg_bind_t bind_info = {};
    bind_info.slot          = 0;
    bind_info.stage_bits    = skg_stage_vertex;
    bind_info.register_type = skg_register_vertex;
    skg_buffer_bind(&vbuf, bind_info);

    bool success = skg_buffer_is_valid(&vbuf);
    skg_buffer_destroy(&vbuf);

    if (!success) {
        std::fprintf(stderr, "Vertex buffer bind failed.\n");
    }
    return success;
}

bool test_dynamic_constant_buffer() {
    const float values[] = { 1.0f, 2.0f, 3.0f, 4.0f };
    skg_buffer_t cbuf = skg_buffer_create(nullptr, 4, sizeof(float), skg_buffer_type_constant, skg_use_dynamic);
    if (!skg_buffer_is_valid(&cbuf)) {
        std::fprintf(stderr, "Dynamic constant buffer creation failed.\n");
        return false;
    }

    skg_buffer_set_contents(&cbuf, values, sizeof(values));

    skg_bind_t bind_info = {};
    bind_info.slot          = 0;
    bind_info.stage_bits    = skg_stage_vertex;
    bind_info.register_type = skg_register_constant;
    skg_buffer_bind(&cbuf, bind_info);
    skg_buffer_destroy(&cbuf);

    return true;
}

bool test_texture_upload_and_readback() {
    constexpr int32_t width = 4;
    constexpr int32_t height = 4;
    std::vector<uint32_t> pixels(width * height);
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            uint8_t value = static_cast<uint8_t>(x * 16 + y * 16);
            pixels[x + y * width] = (255u << 24) | (value << 16) | (value << 8) | value;
        }
    }

    skg_tex_t tex = skg_tex_create(skg_tex_type_image, skg_use_static, skg_tex_fmt_rgba32_linear, skg_mip_generate);
    skg_tex_settings(&tex, skg_tex_address_clamp, skg_tex_sample_linear, skg_sample_compare_none, 1);
    skg_tex_set_contents(&tex, pixels.data(), width, height);

    if (!skg_tex_is_valid(&tex)) {
        std::fprintf(stderr, "Image texture creation failed.\n");
        skg_tex_destroy(&tex);
        return false;
    }

    std::vector<uint32_t> readback(width * height, 0);
    if (!skg_tex_get_contents(&tex, readback.data(), readback.size() * sizeof(uint32_t))) {
        std::fprintf(stderr, "Texture readback call failed.\n");
        skg_tex_destroy(&tex);
        return false;
    }

    bool success = check_bytes_equal(pixels.data(), readback.data(), readback.size());
    if (!success) {
        std::fprintf(stderr, "Texture readback mismatch.\n");
    }

    skg_tex_destroy(&tex);
    return success;
}

bool test_render_target_clear() {
    constexpr int32_t width = 8;
    constexpr int32_t height = 8;
    const float clear_color[4] = { 0.125f, 0.25f, 0.5f, 1.0f };

    skg_tex_t target = skg_tex_create(skg_tex_type_rendertarget, skg_use_static, skg_tex_fmt_rgba32_linear, skg_mip_none);
    skg_tex_set_contents(&target, nullptr, width, height);
    if (!skg_tex_is_valid(&target)) {
        std::fprintf(stderr, "Render target creation failed.\n");
        skg_tex_destroy(&target);
        return false;
    }

    skg_tex_target_bind(&target, 0, 0);
    skg_target_clear(true, clear_color);
    skg_tex_target_bind(nullptr, 0, 0);

    std::vector<uint32_t> readback(width * height, 0);
    if (!skg_tex_get_contents(&target, readback.data(), readback.size() * sizeof(uint32_t))) {
        std::fprintf(stderr, "Render target readback failed.\n");
        skg_tex_destroy(&target);
        return false;
    }

    bool success = check_color_close(readback[0], clear_color, 8);
    if (!success) {
        std::fprintf(stderr, "Render target clear color mismatch.\n");
    }

    skg_tex_destroy(&target);
    return success;
}

bool test_texture_copy() {
    constexpr int32_t width = 4;
    constexpr int32_t height = 4;
    std::vector<uint32_t> pixels(width * height);
    for (int32_t i = 0; i < width * height; ++i) {
        pixels[i] = 0xff000000u | static_cast<uint32_t>(i * 3);
    }

    skg_tex_t src = skg_tex_create(skg_tex_type_image, skg_use_static, skg_tex_fmt_rgba32_linear, skg_mip_none);
    skg_tex_set_contents(&src, pixels.data(), width, height);
    skg_tex_t dst = skg_tex_create(skg_tex_type_image, skg_use_static, skg_tex_fmt_rgba32_linear, skg_mip_none);
    skg_tex_set_contents(&dst, nullptr, width, height);

    if (!skg_tex_is_valid(&src) || !skg_tex_is_valid(&dst)) {
        std::fprintf(stderr, "Texture copy setup failed.\n");
        skg_tex_destroy(&src);
        skg_tex_destroy(&dst);
        return false;
    }

    skg_tex_copy_to(&src, 0, &dst, 0);

    std::vector<uint32_t> readback(width * height, 0);
    if (!skg_tex_get_contents(&dst, readback.data(), readback.size() * sizeof(uint32_t))) {
        std::fprintf(stderr, "Destination texture readback failed.\n");
        skg_tex_destroy(&src);
        skg_tex_destroy(&dst);
        return false;
    }

    bool success = check_bytes_equal(pixels.data(), readback.data(), readback.size());
    if (!success) {
        std::fprintf(stderr, "Texture copy mismatch.\n");
    }

    skg_tex_destroy(&src);
    skg_tex_destroy(&dst);
    return success;
}

bool test_mesh_lifecycle() {
    skg_vert_t verts[3] = {};
    verts[0].pos[0] = -1.0f; verts[1].pos[0] = 0.0f; verts[2].pos[0] = 1.0f;
    uint32_t indices[3] = {0, 1, 2};

    skg_buffer_t vbuf = skg_buffer_create(verts, 3, sizeof(skg_vert_t), skg_buffer_type_vertex, skg_use_static);
    skg_buffer_t ibuf = skg_buffer_create(indices, 3, sizeof(uint32_t), skg_buffer_type_index, skg_use_static);

    if (!skg_buffer_is_valid(&vbuf) || !skg_buffer_is_valid(&ibuf)) {
        std::fprintf(stderr, "Mesh buffer creation failed.\n");
        skg_buffer_destroy(&vbuf);
        skg_buffer_destroy(&ibuf);
        return false;
    }

    skg_mesh_t mesh = skg_mesh_create(&vbuf, &ibuf);
    skg_mesh_bind(&mesh);
    skg_mesh_destroy(&mesh);

    skg_buffer_destroy(&vbuf);
    skg_buffer_destroy(&ibuf);
    return true;
}

bool run_all_tests() {
    struct TestEntry {
        const char *name;
        bool (*fn)();
    } tests[] = {
        {"adapter", test_adapter_name},
        {"buffer_roundtrip", test_buffer_roundtrip},
        {"dynamic_constant_buffer", test_dynamic_constant_buffer},
        {"texture_upload", test_texture_upload_and_readback},
        {"render_target_clear", test_render_target_clear},
        {"texture_copy", test_texture_copy},
        {"mesh_lifecycle", test_mesh_lifecycle},
    };

    bool success = true;
    for (const TestEntry &test : tests) {
        bool result = test.fn();
        std::printf("[ %s ] %s\n", result ? "PASS" : "FAIL", test.name);
        if (!result) {
            success = false;
        }
    }
    return success;
}

} // namespace

int main() {
    if (!skg_init("sk_gpu_api_test", nullptr)) {
        std::fprintf(stderr, "skg_init failed. Ensure an OpenGL context is available.\n");
        return EXIT_FAILURE;
    }

    bool result = run_all_tests();

    skg_shutdown();
    return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
