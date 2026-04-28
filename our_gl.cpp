#include <algorithm>
#include <cmath>
#include <cstdint>
#include <arm_neon.h>
#include "our_gl.h"

mat<4,4> ModelView, Viewport, Perspective; // "OpenGL" state matrices
std::vector<float> zbuffer;                // depth buffer

namespace {

double orient2d(const vec2 &a, const vec2 &b, const vec2 &p) {
    return (b.x-a.x)*(p.y-a.y) - (b.y-a.y)*(p.x-a.x);
}

double orient2d(const vec2 &a, const vec2 &b, const double px, const double py) {
    return (b.x-a.x)*(py-a.y) - (b.y-a.y)*(px-a.x);
}

} // namespace

void lookat(const vec3 eye, const vec3 center, const vec3 up) {
    vec3 n = normalized(eye-center);
    vec3 l = normalized(cross(up,n));
    vec3 m = normalized(cross(n, l));
    ModelView = mat<4,4>{{{l.x,l.y,l.z,0}, {m.x,m.y,m.z,0}, {n.x,n.y,n.z,0}, {0,0,0,1}}} *
                mat<4,4>{{{1,0,0,-center.x}, {0,1,0,-center.y}, {0,0,1,-center.z}, {0,0,0,1}}};
}

void init_perspective(const double f) {
    Perspective = {{{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0, -1/f,1}}};
}

void init_viewport(const int x, const int y, const int w, const int h) {
    Viewport = {{{w/2., 0, 0, x+w/2.}, {0, h/2., 0, y+h/2.}, {0,0,1,0}, {0,0,0,1}}};
}

void init_zbuffer(const int width, const int height) {
    zbuffer = std::vector<float>(width*height, -1000.f);
}

std::uint32_t IShader::fragment4(const float *bar0, const float *bar1, const float *bar2, std::uint32_t active_mask, TGAColor *colors) const {
    std::uint32_t keep_mask = 0;
    for (int lane = 0; lane < 4; lane++) {
        if (((active_mask >> lane) & 1u) == 0) continue;
        const auto [discard, color] = fragment(vec3{bar0[lane], bar1[lane], bar2[lane]});
        if (!discard) {
            colors[lane] = color;
            keep_mask |= 1u << lane;
        }
    }
    return keep_mask;
}

void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer) {
    const vec4 ndc[3] = { clip[0]/clip[0].w, clip[1]/clip[1].w, clip[2]/clip[2].w };                // normalized device coordinates
    const vec2 screen[3] = { (Viewport*ndc[0]).xy(), (Viewport*ndc[1]).xy(), (Viewport*ndc[2]).xy() }; // screen coordinates

    const double area = orient2d(screen[0], screen[1], screen[2]);
    if (area < 1.) return; // backface culling + discarding triangles that cover less than a pixel

    const int width = framebuffer.width();
    const int height = framebuffer.height();
    const auto [bbminx, bbmaxx] = std::minmax({screen[0].x, screen[1].x, screen[2].x});
    const auto [bbminy, bbmaxy] = std::minmax({screen[0].y, screen[1].y, screen[2].y});
    const int minx = std::max(0, static_cast<int>(std::floor(bbminx)));
    const int maxx = std::min(width-1, static_cast<int>(std::ceil(bbmaxx)));
    const int miny = std::max(0, static_cast<int>(std::floor(bbminy)));
    const int maxy = std::min(height-1, static_cast<int>(std::ceil(bbmaxy)));
    if (minx > maxx || miny > maxy) return;

    const float inv_area = static_cast<float>(1. / area);
    const float inv_w0 = static_cast<float>(1. / clip[0].w);
    const float inv_w1 = static_cast<float>(1. / clip[1].w);
    const float inv_w2 = static_cast<float>(1. / clip[2].w);
    const float z0 = static_cast<float>(ndc[0].z);
    const float z1 = static_cast<float>(ndc[1].z);
    const float z2 = static_cast<float>(ndc[2].z);

    const float w0_step_x = static_cast<float>(screen[1].y - screen[2].y);
    const float w1_step_x = static_cast<float>(screen[2].y - screen[0].y);
    const float w2_step_x = static_cast<float>(screen[0].y - screen[1].y);
    const float w0_step_y = static_cast<float>(screen[2].x - screen[1].x);
    const float w1_step_y = static_cast<float>(screen[0].x - screen[2].x);
    const float w2_step_y = static_cast<float>(screen[1].x - screen[0].x);
    const float z_step_x = (w0_step_x*z0 + w1_step_x*z1 + w2_step_x*z2) * inv_area;
    const float z_step_y = (w0_step_y*z0 + w1_step_y*z1 + w2_step_y*z2) * inv_area;

    const double start_x = static_cast<double>(minx);
    const double start_y = static_cast<double>(miny);
    const float row_w0 = static_cast<float>(orient2d(screen[1], screen[2], start_x, start_y));
    const float row_w1 = static_cast<float>(orient2d(screen[2], screen[0], start_x, start_y));
    const float row_w2 = static_cast<float>(orient2d(screen[0], screen[1], start_x, start_y));
    const float row_z = (row_w0*z0 + row_w1*z1 + row_w2*z2) * inv_area;
    [[maybe_unused]] const int pixels = (maxx-minx+1) * (maxy-miny+1);
    const int framebuffer_bpp = framebuffer.bytespp();
    std::uint8_t *framebuffer_data = framebuffer.raw_data();
    float *zbuffer_data = zbuffer.data();
    const float32x4_t zero = vdupq_n_f32(0.f);
    const float32x4_t inv_area_vec = vdupq_n_f32(inv_area);
    const float32x4_t inv_w0_vec = vdupq_n_f32(inv_w0);
    const float32x4_t inv_w1_vec = vdupq_n_f32(inv_w1);
    const float32x4_t inv_w2_vec = vdupq_n_f32(inv_w2);
    const float32x4_t lane_offsets_ps = {0.f, 1.f, 2.f, 3.f};

    #pragma omp parallel for if ((maxy - miny) >= 32) schedule(static)
    for (int y=miny; y<=maxy; y++) {
        float w0 = row_w0 + (y-miny) * w0_step_y;
        float w1 = row_w1 + (y-miny) * w1_step_y;
        float w2 = row_w2 + (y-miny) * w2_step_y;
        float z = row_z + (y-miny) * z_step_y;
        const int row_offset = y * width;
        int x = minx;

        for (; x <= maxx - 3; x += 4) {
            const float32x4_t w0_vec = vmlaq_f32(vdupq_n_f32(w0), lane_offsets_ps, vdupq_n_f32(w0_step_x));
            const float32x4_t w1_vec = vmlaq_f32(vdupq_n_f32(w1), lane_offsets_ps, vdupq_n_f32(w1_step_x));
            const float32x4_t w2_vec = vmlaq_f32(vdupq_n_f32(w2), lane_offsets_ps, vdupq_n_f32(w2_step_x));
            const float32x4_t z_vec = vmlaq_f32(vdupq_n_f32(z), lane_offsets_ps, vdupq_n_f32(z_step_x));

            const uint32x4_t inside = vandq_u32(vandq_u32(vcgeq_f32(w0_vec, zero), vcgeq_f32(w1_vec, zero)), vcgeq_f32(w2_vec, zero));
            std::uint32_t active_mask = 0;
            active_mask |= (vgetq_lane_u32(inside, 0) >> 31) << 0;
            active_mask |= (vgetq_lane_u32(inside, 1) >> 31) << 1;
            active_mask |= (vgetq_lane_u32(inside, 2) >> 31) << 2;
            active_mask |= (vgetq_lane_u32(inside, 3) >> 31) << 3;
            if (active_mask == 0) {
                w0 += 4.f * w0_step_x;
                w1 += 4.f * w1_step_x;
                w2 += 4.f * w2_step_x;
                z += 4.f * z_step_x;
                continue;
            }

            alignas(16) float zbuf_vals[4] = {
                zbuffer_data[row_offset + x + 0],
                zbuffer_data[row_offset + x + 1],
                zbuffer_data[row_offset + x + 2],
                zbuffer_data[row_offset + x + 3]
            };
            const float32x4_t zbuf_vec = vld1q_f32(zbuf_vals);
            const uint32x4_t depth_pass = vcgtq_f32(z_vec, zbuf_vec);
            active_mask &= ((vgetq_lane_u32(depth_pass, 0) >> 31) << 0) |
                           ((vgetq_lane_u32(depth_pass, 1) >> 31) << 1) |
                           ((vgetq_lane_u32(depth_pass, 2) >> 31) << 2) |
                           ((vgetq_lane_u32(depth_pass, 3) >> 31) << 3);
            if (active_mask == 0) {
                w0 += 4.f * w0_step_x;
                w1 += 4.f * w1_step_x;
                w2 += 4.f * w2_step_x;
                z += 4.f * z_step_x;
                continue;
            }

            const float32x4_t bc0_screen = vmulq_f32(w0_vec, inv_area_vec);
            const float32x4_t bc1_screen = vmulq_f32(w1_vec, inv_area_vec);
            const float32x4_t bc2_screen = vmulq_f32(w2_vec, inv_area_vec);
            const float32x4_t bc0_clip_num = vmulq_f32(bc0_screen, inv_w0_vec);
            const float32x4_t bc1_clip_num = vmulq_f32(bc1_screen, inv_w1_vec);
            const float32x4_t bc2_clip_num = vmulq_f32(bc2_screen, inv_w2_vec);
            const float32x4_t bc_sum = vaddq_f32(vaddq_f32(bc0_clip_num, bc1_clip_num), bc2_clip_num);
            const float32x4_t bc_inv = vdivq_f32(vdupq_n_f32(1.f), bc_sum);
            const float32x4_t bc0_clip = vmulq_f32(bc0_clip_num, bc_inv);
            const float32x4_t bc1_clip = vmulq_f32(bc1_clip_num, bc_inv);
            const float32x4_t bc2_clip = vmulq_f32(bc2_clip_num, bc_inv);

            alignas(16) float bc0_vals[4];
            alignas(16) float bc1_vals[4];
            alignas(16) float bc2_vals[4];
            alignas(16) float z_vals[4];
            vst1q_f32(bc0_vals, bc0_clip);
            vst1q_f32(bc1_vals, bc1_clip);
            vst1q_f32(bc2_vals, bc2_clip);
            vst1q_f32(z_vals, z_vec);

            TGAColor colors[4];
            const std::uint32_t keep_mask = shader.fragment4(bc0_vals, bc1_vals, bc2_vals, active_mask, colors);
            if (keep_mask != 0) {
                for (int lane = 0; lane < 4; lane++) {
                    if (((keep_mask >> lane) & 1u) == 0) continue;
                    const int idx = row_offset + x + lane;
                    zbuffer_data[idx] = z_vals[lane];
                    std::uint8_t *pixel = framebuffer_data + idx * framebuffer_bpp;
                    pixel[0] = colors[lane][0];
                    pixel[1] = colors[lane][1];
                    pixel[2] = colors[lane][2];
                    if (framebuffer_bpp == TGAImage::RGBA) pixel[3] = colors[lane][3];
                }
            }

            w0 += 4.f * w0_step_x;
            w1 += 4.f * w1_step_x;
            w2 += 4.f * w2_step_x;
            z += 4.f * z_step_x;
        }

        for (; x<=maxx; x++) {
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                const int idx = row_offset + x;
                if (z > zbuffer_data[idx]) {
                    const vec3 bc_screen = { w0*inv_area, w1*inv_area, w2*inv_area };
                    vec3 bc_clip = { bc_screen.x*inv_w0, bc_screen.y*inv_w1, bc_screen.z*inv_w2 }; // check https://github.com/ssloy/tinyrenderer/wiki/Technical-difficulties-linear-interpolation-with-perspective-deformations
                    bc_clip = bc_clip / (bc_clip.x + bc_clip.y + bc_clip.z);
                    auto [discard, color] = shader.fragment(bc_clip);
                    if (!discard) {
                        zbuffer_data[idx] = z;
                        std::uint8_t *pixel = framebuffer_data + idx * framebuffer_bpp;
                        pixel[0] = color[0];
                        pixel[1] = color[1];
                        pixel[2] = color[2];
                        if (framebuffer_bpp == TGAImage::RGBA) pixel[3] = color[3];
                    }
                }
            }
            w0 += w0_step_x;
            w1 += w1_step_x;
            w2 += w2_step_x;
            z += z_step_x;
        }
    }
}
