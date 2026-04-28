#include "our_gl.h"
#include "model.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <string>
#include <string_view>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define TINY_SIMD_NEON 1
#elif defined(__SSE2__)
#include <immintrin.h>
#define TINY_SIMD_SSE 1
#endif

extern mat<4,4> ModelView, Perspective; // "OpenGL" state matrices and
extern std::vector<float> zbuffer;      // the depth buffer

namespace {

#if defined(TINY_SIMD_NEON)
float32x4_t rsqrt_nr(float32x4_t v) {
    float32x4_t estimate = vrsqrteq_f32(v);
    estimate = vmulq_f32(estimate, vrsqrtsq_f32(vmulq_f32(v, estimate), estimate));
    estimate = vmulq_f32(estimate, vrsqrtsq_f32(vmulq_f32(v, estimate), estimate));
    return estimate;
}
#elif defined(TINY_SIMD_SSE)
__m128 rsqrt_nr(__m128 v) {
    __m128 estimate = _mm_rsqrt_ps(v);
    const __m128 half = _mm_set1_ps(0.5f);
    const __m128 three_halves = _mm_set1_ps(1.5f);
    estimate = _mm_mul_ps(estimate, _mm_sub_ps(three_halves, _mm_mul_ps(half, _mm_mul_ps(v, _mm_mul_ps(estimate, estimate)))));
    estimate = _mm_mul_ps(estimate, _mm_sub_ps(three_halves, _mm_mul_ps(half, _mm_mul_ps(v, _mm_mul_ps(estimate, estimate)))));
    return estimate;
}
#endif

} // namespace

struct PhongShader : IShader {
    const Model &model;
    const TGAImage &diffusemap;
    const TGAImage &normalmap;
    const TGAImage &specularmap;
    vec4 l;              // light direction in eye coordinates
    mat<4,4> normal_matrix;
    int diffuse_width;
    int diffuse_height;
    int diffuse_bpp;
    int normal_width;
    int normal_height;
    int normal_bpp;
    int specular_width;
    int specular_height;
    int specular_bpp;
    const std::uint8_t *diffuse_data;
    const std::uint8_t *normal_data;
    const std::uint8_t *specular_data;
    vec2  varying_uv[3]; // triangle uv coordinates, written by the vertex shader, read by the fragment shader
    vec4 varying_nrm[3]; // normal per vertex to be interpolated by the fragment shader
    vec4 tri[3];         // triangle in view coordinates
    vec4 tangent;
    vec4 bitangent;
    float varying_uv_x[3] = {};
    float varying_uv_y[3] = {};
    float varying_nrm_x[3] = {};
    float varying_nrm_y[3] = {};
    float varying_nrm_z[3] = {};
    float tangent_x = 0.f;
    float tangent_y = 0.f;
    float tangent_z = 0.f;
    float bitangent_x = 0.f;
    float bitangent_y = 0.f;
    float bitangent_z = 0.f;
    float light_x = 0.f;
    float light_y = 0.f;
    float light_z = 0.f;

    PhongShader(const vec3 light, const Model &m) : model(m), diffusemap(m.diffuse()), normalmap(m.normal_map()), specularmap(m.specular()) {
        l = normalized((ModelView*vec4{light.x, light.y, light.z, 0.})); // transform the light vector to view coordinates
        normal_matrix = ModelView.invert_transpose();
        diffuse_width = diffusemap.width();
        diffuse_height = diffusemap.height();
        diffuse_bpp = diffusemap.bytespp();
        normal_width = normalmap.width();
        normal_height = normalmap.height();
        normal_bpp = normalmap.bytespp();
        specular_width = specularmap.width();
        specular_height = specularmap.height();
        specular_bpp = specularmap.bytespp();
        diffuse_data = diffusemap.raw_data();
        normal_data = normalmap.raw_data();
        specular_data = specularmap.raw_data();
        light_x = static_cast<float>(l.x);
        light_y = static_cast<float>(l.y);
        light_z = static_cast<float>(l.z);
    }

    virtual vec4 vertex(const int face, const int vert) {
        varying_uv[vert]  = model.uv(face, vert);
        varying_nrm[vert] = normal_matrix * model.normal(face, vert);
        varying_uv_x[vert] = static_cast<float>(varying_uv[vert].x);
        varying_uv_y[vert] = static_cast<float>(varying_uv[vert].y);
        varying_nrm_x[vert] = static_cast<float>(varying_nrm[vert].x);
        varying_nrm_y[vert] = static_cast<float>(varying_nrm[vert].y);
        varying_nrm_z[vert] = static_cast<float>(varying_nrm[vert].z);
        vec4 gl_Position = ModelView * model.vert(face, vert);
        tri[vert] = gl_Position;
        if (vert == 2) {
            const vec4 edge0 = tri[1] - tri[0];
            const vec4 edge1 = tri[2] - tri[0];
            const vec2 duv0 = varying_uv[1] - varying_uv[0];
            const vec2 duv1 = varying_uv[2] - varying_uv[0];
            const double inv_det = 1. / (duv0.x*duv1.y - duv0.y*duv1.x);
            tangent = normalized((edge0 * duv1.y - edge1 * duv0.y) * inv_det);
            bitangent = normalized((edge1 * duv0.x - edge0 * duv1.x) * inv_det);
            tangent_x = static_cast<float>(tangent.x);
            tangent_y = static_cast<float>(tangent.y);
            tangent_z = static_cast<float>(tangent.z);
            bitangent_x = static_cast<float>(bitangent.x);
            bitangent_y = static_cast<float>(bitangent.y);
            bitangent_z = static_cast<float>(bitangent.z);
        }
        return Perspective * gl_Position;                         // in clip coordinates
    }

    virtual std::pair<bool,TGAColor> fragment(const vec3 bar) const {
        vec2 uv = varying_uv[0] * bar[0] + varying_uv[1] * bar[1] + varying_uv[2] * bar[2];
        const int tx = uv.x * diffuse_width;
        const int ty = uv.y * diffuse_height;
        const int nx = uv.x * normal_width;
        const int ny = uv.y * normal_height;
        const int sx = uv.x * specular_width;
        const int sy = uv.y * specular_height;
        vec4 geometric_normal = normalized(varying_nrm[0]*bar[0] + varying_nrm[1]*bar[1] + varying_nrm[2]*bar[2]);
        TGAColor tangent_space_sample = normalmap.get(nx, ny);
        vec4 mapped_normal = normalized(vec4{(double)tangent_space_sample[2], (double)tangent_space_sample[1], (double)tangent_space_sample[0], 0}*2./255. - vec4{1,1,1,0});
        vec4 n = normalized(tangent * mapped_normal.x + bitangent * mapped_normal.y + geometric_normal * mapped_normal.z);
        double ambient  = .4;                                     // ambient light intensity
        const double nl = n * l;
        double diffuse  = std::max(0., nl);                       // diffuse light intensity
        double specular = 0.;
        if (nl > 0.) {
            const double rz = 2.*nl*n.z - l.z;
            if (rz > 0.) {
                specular = (.5+2.*specularmap.get(sx, sy)[0]/255.) * std::pow(rz, 35);  // specular intensity, note that the camera lies on the z-axis (in eye coordinates), therefore simple r.z, since (0,0,1)*(r.x, r.y, r.z) = r.z
            }
        }
        TGAColor gl_FragColor = diffusemap.get(tx, ty);
        for (int channel : {0,1,2})
            gl_FragColor[channel] = std::min<int>(255, gl_FragColor[channel]*(ambient + diffuse + specular));
        return {false, gl_FragColor};                             // do not discard the pixel
    }

#if defined(TINY_SIMD_NEON)
    virtual std::uint32_t fragment4(const float *bar0, const float *bar1, const float *bar2, std::uint32_t active_mask, TGAColor *colors) const override {
        const float32x4_t b0 = vld1q_f32(bar0);
        const float32x4_t b1 = vld1q_f32(bar1);
        const float32x4_t b2 = vld1q_f32(bar2);

        const float32x4_t uvx = vmlaq_f32(vmlaq_f32(vmulq_f32(vdupq_n_f32(varying_uv_x[0]), b0), vdupq_n_f32(varying_uv_x[1]), b1), vdupq_n_f32(varying_uv_x[2]), b2);
        const float32x4_t uvy = vmlaq_f32(vmlaq_f32(vmulq_f32(vdupq_n_f32(varying_uv_y[0]), b0), vdupq_n_f32(varying_uv_y[1]), b1), vdupq_n_f32(varying_uv_y[2]), b2);

        int32x4_t tx = vcvtq_s32_f32(vmulq_f32(uvx, vdupq_n_f32(static_cast<float>(diffuse_width))));
        int32x4_t ty = vcvtq_s32_f32(vmulq_f32(uvy, vdupq_n_f32(static_cast<float>(diffuse_height))));
        int32x4_t nx = vcvtq_s32_f32(vmulq_f32(uvx, vdupq_n_f32(static_cast<float>(normal_width))));
        int32x4_t ny = vcvtq_s32_f32(vmulq_f32(uvy, vdupq_n_f32(static_cast<float>(normal_height))));
        int32x4_t sx = vcvtq_s32_f32(vmulq_f32(uvx, vdupq_n_f32(static_cast<float>(specular_width))));
        int32x4_t sy = vcvtq_s32_f32(vmulq_f32(uvy, vdupq_n_f32(static_cast<float>(specular_height))));

        const int32x4_t zero_i = vdupq_n_s32(0);
        tx = vminq_s32(vmaxq_s32(tx, zero_i), vdupq_n_s32(diffuse_width - 1));
        ty = vminq_s32(vmaxq_s32(ty, zero_i), vdupq_n_s32(diffuse_height - 1));
        nx = vminq_s32(vmaxq_s32(nx, zero_i), vdupq_n_s32(normal_width - 1));
        ny = vminq_s32(vmaxq_s32(ny, zero_i), vdupq_n_s32(normal_height - 1));
        sx = vminq_s32(vmaxq_s32(sx, zero_i), vdupq_n_s32(specular_width - 1));
        sy = vminq_s32(vmaxq_s32(sy, zero_i), vdupq_n_s32(specular_height - 1));

        const float32x4_t geom_x = vmlaq_f32(vmlaq_f32(vmulq_f32(vdupq_n_f32(varying_nrm_x[0]), b0), vdupq_n_f32(varying_nrm_x[1]), b1), vdupq_n_f32(varying_nrm_x[2]), b2);
        const float32x4_t geom_y = vmlaq_f32(vmlaq_f32(vmulq_f32(vdupq_n_f32(varying_nrm_y[0]), b0), vdupq_n_f32(varying_nrm_y[1]), b1), vdupq_n_f32(varying_nrm_y[2]), b2);
        const float32x4_t geom_z = vmlaq_f32(vmlaq_f32(vmulq_f32(vdupq_n_f32(varying_nrm_z[0]), b0), vdupq_n_f32(varying_nrm_z[1]), b1), vdupq_n_f32(varying_nrm_z[2]), b2);

        const float32x4_t geom_len2 = vmlaq_f32(vmlaq_f32(vmulq_f32(geom_x, geom_x), geom_y, geom_y), geom_z, geom_z);
        const float32x4_t geom_inv_len = rsqrt_nr(geom_len2);
        const float32x4_t geom_nx = vmulq_f32(geom_x, geom_inv_len);
        const float32x4_t geom_ny = vmulq_f32(geom_y, geom_inv_len);
        const float32x4_t geom_nz = vmulq_f32(geom_z, geom_inv_len);

        alignas(16) int tx_vals[4];
        alignas(16) int ty_vals[4];
        alignas(16) int nx_vals[4];
        alignas(16) int ny_vals[4];
        alignas(16) int sx_vals[4];
        alignas(16) int sy_vals[4];
        vst1q_s32(tx_vals, tx);
        vst1q_s32(ty_vals, ty);
        vst1q_s32(nx_vals, nx);
        vst1q_s32(ny_vals, ny);
        vst1q_s32(sx_vals, sx);
        vst1q_s32(sy_vals, sy);

        alignas(16) float mapped_x_vals[4] = {};
        alignas(16) float mapped_y_vals[4] = {};
        alignas(16) float mapped_z_vals[4] = {};
        alignas(16) float spec_strength_vals[4] = {};
        alignas(16) std::uint8_t diffuse_b_vals[4] = {};
        alignas(16) std::uint8_t diffuse_g_vals[4] = {};
        alignas(16) std::uint8_t diffuse_r_vals[4] = {};

        for (int lane = 0; lane < 4; lane++) {
            if (((active_mask >> lane) & 1u) == 0) continue;
            const int n_offset = (nx_vals[lane] + ny_vals[lane] * normal_width) * normal_bpp;
            mapped_x_vals[lane] = normal_data[n_offset + 2] * (2.f / 255.f) - 1.f;
            mapped_y_vals[lane] = normal_data[n_offset + 1] * (2.f / 255.f) - 1.f;
            mapped_z_vals[lane] = normal_data[n_offset + 0] * (2.f / 255.f) - 1.f;

            const int d_offset = (tx_vals[lane] + ty_vals[lane] * diffuse_width) * diffuse_bpp;
            diffuse_b_vals[lane] = diffuse_data[d_offset + 0];
            diffuse_g_vals[lane] = diffuse_data[d_offset + 1];
            diffuse_r_vals[lane] = diffuse_data[d_offset + 2];

            const int s_offset = (sx_vals[lane] + sy_vals[lane] * specular_width) * specular_bpp;
            spec_strength_vals[lane] = .5f + 2.f * specular_data[s_offset] * (1.f / 255.f);
        }

        float32x4_t mapped_x = vld1q_f32(mapped_x_vals);
        float32x4_t mapped_y = vld1q_f32(mapped_y_vals);
        float32x4_t mapped_z = vld1q_f32(mapped_z_vals);

        const float32x4_t mapped_len2 = vmlaq_f32(vmlaq_f32(vmulq_f32(mapped_x, mapped_x), mapped_y, mapped_y), mapped_z, mapped_z);
        const float32x4_t mapped_inv_len = rsqrt_nr(mapped_len2);
        mapped_x = vmulq_f32(mapped_x, mapped_inv_len);
        mapped_y = vmulq_f32(mapped_y, mapped_inv_len);
        mapped_z = vmulq_f32(mapped_z, mapped_inv_len);

        const float32x4_t n_x = vmlaq_f32(vmlaq_f32(vmulq_f32(vdupq_n_f32(tangent_x), mapped_x), vdupq_n_f32(bitangent_x), mapped_y), geom_nx, mapped_z);
        const float32x4_t n_y = vmlaq_f32(vmlaq_f32(vmulq_f32(vdupq_n_f32(tangent_y), mapped_x), vdupq_n_f32(bitangent_y), mapped_y), geom_ny, mapped_z);
        const float32x4_t n_z = vmlaq_f32(vmlaq_f32(vmulq_f32(vdupq_n_f32(tangent_z), mapped_x), vdupq_n_f32(bitangent_z), mapped_y), geom_nz, mapped_z);

        const float32x4_t n_len2 = vmlaq_f32(vmlaq_f32(vmulq_f32(n_x, n_x), n_y, n_y), n_z, n_z);
        const float32x4_t n_inv_len = rsqrt_nr(n_len2);
        const float32x4_t nn_x = vmulq_f32(n_x, n_inv_len);
        const float32x4_t nn_y = vmulq_f32(n_y, n_inv_len);
        const float32x4_t nn_z = vmulq_f32(n_z, n_inv_len);

        const float32x4_t nl = vmlaq_f32(vmlaq_f32(vmulq_f32(nn_x, vdupq_n_f32(light_x)), nn_y, vdupq_n_f32(light_y)), nn_z, vdupq_n_f32(light_z));
        const float32x4_t zero = vdupq_n_f32(0.f);
        const float32x4_t diffuse = vmaxq_f32(zero, nl);

        const float32x4_t rz = vsubq_f32(vmulq_f32(vmulq_f32(vdupq_n_f32(2.f), nl), nn_z), vdupq_n_f32(light_z));
        const float32x4_t positive_rz = vmaxq_f32(zero, rz);
        const float32x4_t rz2 = vmulq_f32(positive_rz, positive_rz);
        const float32x4_t rz4 = vmulq_f32(rz2, rz2);
        const float32x4_t rz8 = vmulq_f32(rz4, rz4);
        const float32x4_t rz16 = vmulq_f32(rz8, rz8);
        const float32x4_t rz32 = vmulq_f32(rz16, rz16);
        const float32x4_t rz35 = vmulq_f32(vmulq_f32(rz32, rz2), positive_rz);
        const float32x4_t specular = vmulq_f32(vld1q_f32(spec_strength_vals), rz35);

        const float32x4_t scale = vaddq_f32(vdupq_n_f32(.4f), vaddq_f32(diffuse, specular));
        alignas(16) float scale_vals[4];
        vst1q_f32(scale_vals, scale);

        std::uint32_t keep_mask = 0;
        for (int lane = 0; lane < 4; lane++) {
            if (((active_mask >> lane) & 1u) == 0) continue;
            TGAColor color = { diffuse_b_vals[lane], diffuse_g_vals[lane], diffuse_r_vals[lane], 255, static_cast<std::uint8_t>(diffuse_bpp) };
            color[0] = std::min(255, static_cast<int>(color[0] * scale_vals[lane]));
            color[1] = std::min(255, static_cast<int>(color[1] * scale_vals[lane]));
            color[2] = std::min(255, static_cast<int>(color[2] * scale_vals[lane]));
            colors[lane] = color;
            keep_mask |= 1u << lane;
        }
        return keep_mask;
    }
#elif defined(TINY_SIMD_SSE)
    virtual std::uint32_t fragment4(const float *bar0, const float *bar1, const float *bar2, std::uint32_t active_mask, TGAColor *colors) const override {
        const __m128 b0 = _mm_loadu_ps(bar0);
        const __m128 b1 = _mm_loadu_ps(bar1);
        const __m128 b2 = _mm_loadu_ps(bar2);

        const __m128 uvx = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(varying_uv_x[0]), b0), _mm_mul_ps(_mm_set1_ps(varying_uv_x[1]), b1)), _mm_mul_ps(_mm_set1_ps(varying_uv_x[2]), b2));
        const __m128 uvy = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(varying_uv_y[0]), b0), _mm_mul_ps(_mm_set1_ps(varying_uv_y[1]), b1)), _mm_mul_ps(_mm_set1_ps(varying_uv_y[2]), b2));

        const __m128 txf = _mm_mul_ps(uvx, _mm_set1_ps(static_cast<float>(diffuse_width)));
        const __m128 tyf = _mm_mul_ps(uvy, _mm_set1_ps(static_cast<float>(diffuse_height)));
        const __m128 nxf = _mm_mul_ps(uvx, _mm_set1_ps(static_cast<float>(normal_width)));
        const __m128 nyf = _mm_mul_ps(uvy, _mm_set1_ps(static_cast<float>(normal_height)));
        const __m128 sxf = _mm_mul_ps(uvx, _mm_set1_ps(static_cast<float>(specular_width)));
        const __m128 syf = _mm_mul_ps(uvy, _mm_set1_ps(static_cast<float>(specular_height)));

        const __m128 geom_x = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(varying_nrm_x[0]), b0), _mm_mul_ps(_mm_set1_ps(varying_nrm_x[1]), b1)), _mm_mul_ps(_mm_set1_ps(varying_nrm_x[2]), b2));
        const __m128 geom_y = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(varying_nrm_y[0]), b0), _mm_mul_ps(_mm_set1_ps(varying_nrm_y[1]), b1)), _mm_mul_ps(_mm_set1_ps(varying_nrm_y[2]), b2));
        const __m128 geom_z = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(varying_nrm_z[0]), b0), _mm_mul_ps(_mm_set1_ps(varying_nrm_z[1]), b1)), _mm_mul_ps(_mm_set1_ps(varying_nrm_z[2]), b2));

        const __m128 geom_len2 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(geom_x, geom_x), _mm_mul_ps(geom_y, geom_y)), _mm_mul_ps(geom_z, geom_z));
        const __m128 geom_inv_len = rsqrt_nr(geom_len2);
        const __m128 geom_nx = _mm_mul_ps(geom_x, geom_inv_len);
        const __m128 geom_ny = _mm_mul_ps(geom_y, geom_inv_len);
        const __m128 geom_nz = _mm_mul_ps(geom_z, geom_inv_len);

        alignas(16) int tx_vals[4];
        alignas(16) int ty_vals[4];
        alignas(16) int nx_vals[4];
        alignas(16) int ny_vals[4];
        alignas(16) int sx_vals[4];
        alignas(16) int sy_vals[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(tx_vals), _mm_cvttps_epi32(txf));
        _mm_store_si128(reinterpret_cast<__m128i*>(ty_vals), _mm_cvttps_epi32(tyf));
        _mm_store_si128(reinterpret_cast<__m128i*>(nx_vals), _mm_cvttps_epi32(nxf));
        _mm_store_si128(reinterpret_cast<__m128i*>(ny_vals), _mm_cvttps_epi32(nyf));
        _mm_store_si128(reinterpret_cast<__m128i*>(sx_vals), _mm_cvttps_epi32(sxf));
        _mm_store_si128(reinterpret_cast<__m128i*>(sy_vals), _mm_cvttps_epi32(syf));

        alignas(16) float mapped_x_vals[4] = {};
        alignas(16) float mapped_y_vals[4] = {};
        alignas(16) float mapped_z_vals[4] = {};
        alignas(16) float spec_strength_vals[4] = {};
        alignas(16) std::uint8_t diffuse_b_vals[4] = {};
        alignas(16) std::uint8_t diffuse_g_vals[4] = {};
        alignas(16) std::uint8_t diffuse_r_vals[4] = {};

        for (int lane = 0; lane < 4; lane++) {
            if (((active_mask >> lane) & 1u) == 0) continue;
            tx_vals[lane] = std::clamp(tx_vals[lane], 0, diffuse_width - 1);
            ty_vals[lane] = std::clamp(ty_vals[lane], 0, diffuse_height - 1);
            nx_vals[lane] = std::clamp(nx_vals[lane], 0, normal_width - 1);
            ny_vals[lane] = std::clamp(ny_vals[lane], 0, normal_height - 1);
            sx_vals[lane] = std::clamp(sx_vals[lane], 0, specular_width - 1);
            sy_vals[lane] = std::clamp(sy_vals[lane], 0, specular_height - 1);

            const int n_offset = (nx_vals[lane] + ny_vals[lane] * normal_width) * normal_bpp;
            mapped_x_vals[lane] = normal_data[n_offset + 2] * (2.f / 255.f) - 1.f;
            mapped_y_vals[lane] = normal_data[n_offset + 1] * (2.f / 255.f) - 1.f;
            mapped_z_vals[lane] = normal_data[n_offset + 0] * (2.f / 255.f) - 1.f;

            const int d_offset = (tx_vals[lane] + ty_vals[lane] * diffuse_width) * diffuse_bpp;
            diffuse_b_vals[lane] = diffuse_data[d_offset + 0];
            diffuse_g_vals[lane] = diffuse_data[d_offset + 1];
            diffuse_r_vals[lane] = diffuse_data[d_offset + 2];

            const int s_offset = (sx_vals[lane] + sy_vals[lane] * specular_width) * specular_bpp;
            spec_strength_vals[lane] = .5f + 2.f * specular_data[s_offset] * (1.f / 255.f);
        }

        __m128 mapped_x = _mm_load_ps(mapped_x_vals);
        __m128 mapped_y = _mm_load_ps(mapped_y_vals);
        __m128 mapped_z = _mm_load_ps(mapped_z_vals);

        const __m128 mapped_len2 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(mapped_x, mapped_x), _mm_mul_ps(mapped_y, mapped_y)), _mm_mul_ps(mapped_z, mapped_z));
        const __m128 mapped_inv_len = rsqrt_nr(mapped_len2);
        mapped_x = _mm_mul_ps(mapped_x, mapped_inv_len);
        mapped_y = _mm_mul_ps(mapped_y, mapped_inv_len);
        mapped_z = _mm_mul_ps(mapped_z, mapped_inv_len);

        const __m128 n_x = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(tangent_x), mapped_x), _mm_mul_ps(_mm_set1_ps(bitangent_x), mapped_y)), _mm_mul_ps(geom_nx, mapped_z));
        const __m128 n_y = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(tangent_y), mapped_x), _mm_mul_ps(_mm_set1_ps(bitangent_y), mapped_y)), _mm_mul_ps(geom_ny, mapped_z));
        const __m128 n_z = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(tangent_z), mapped_x), _mm_mul_ps(_mm_set1_ps(bitangent_z), mapped_y)), _mm_mul_ps(geom_nz, mapped_z));

        const __m128 n_len2 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(n_x, n_x), _mm_mul_ps(n_y, n_y)), _mm_mul_ps(n_z, n_z));
        const __m128 n_inv_len = rsqrt_nr(n_len2);
        const __m128 nn_x = _mm_mul_ps(n_x, n_inv_len);
        const __m128 nn_y = _mm_mul_ps(n_y, n_inv_len);
        const __m128 nn_z = _mm_mul_ps(n_z, n_inv_len);

        const __m128 nl = _mm_add_ps(_mm_add_ps(_mm_mul_ps(nn_x, _mm_set1_ps(light_x)), _mm_mul_ps(nn_y, _mm_set1_ps(light_y))), _mm_mul_ps(nn_z, _mm_set1_ps(light_z)));
        const __m128 zero = _mm_setzero_ps();
        const __m128 diffuse = _mm_max_ps(zero, nl);

        const __m128 rz = _mm_sub_ps(_mm_mul_ps(_mm_mul_ps(_mm_set1_ps(2.f), nl), nn_z), _mm_set1_ps(light_z));
        const __m128 positive_rz = _mm_max_ps(zero, rz);
        const __m128 rz2 = _mm_mul_ps(positive_rz, positive_rz);
        const __m128 rz4 = _mm_mul_ps(rz2, rz2);
        const __m128 rz8 = _mm_mul_ps(rz4, rz4);
        const __m128 rz16 = _mm_mul_ps(rz8, rz8);
        const __m128 rz32 = _mm_mul_ps(rz16, rz16);
        const __m128 rz35 = _mm_mul_ps(_mm_mul_ps(rz32, rz2), positive_rz);
        const __m128 specular = _mm_mul_ps(_mm_load_ps(spec_strength_vals), rz35);

        const __m128 scale = _mm_add_ps(_mm_set1_ps(.4f), _mm_add_ps(diffuse, specular));
        alignas(16) float scale_vals[4];
        _mm_store_ps(scale_vals, scale);

        std::uint32_t keep_mask = 0;
        for (int lane = 0; lane < 4; lane++) {
            if (((active_mask >> lane) & 1u) == 0) continue;
            TGAColor color = { diffuse_b_vals[lane], diffuse_g_vals[lane], diffuse_r_vals[lane], 255, static_cast<std::uint8_t>(diffuse_bpp) };
            color[0] = std::min(255, static_cast<int>(color[0] * scale_vals[lane]));
            color[1] = std::min(255, static_cast<int>(color[1] * scale_vals[lane]));
            color[2] = std::min(255, static_cast<int>(color[2] * scale_vals[lane]));
            colors[lane] = color;
            keep_mask |= 1u << lane;
        }
        return keep_mask;
    }
#endif
};

int main(int argc, char** argv) {
    bool perf_mode = false;
    std::vector<const char*> model_paths;
    model_paths.reserve(argc > 1 ? argc - 1 : 0);

    for (int i = 1; i < argc; i++) {
        if (std::string_view(argv[i]) == "--perf") {
            perf_mode = true;
            continue;
        }
        model_paths.push_back(argv[i]);
    }

    if (model_paths.empty()) {
        std::cerr << "Usage: " << argv[0] << " [--perf] obj/model.obj [more models...]" << std::endl;
        return 1;
    }

    constexpr int width  = 800;      // output image size
    constexpr int height = 800;
    constexpr vec3  light{ 1, 1, 1}; // light source
    constexpr vec3    eye{-1, 0, 2}; // camera position
    constexpr vec3 center{ 0, 0, 0}; // camera direction
    constexpr vec3     up{ 0, 1, 0}; // camera up vector

    lookat(eye, center, up);                                   // build the ModelView   matrix
    init_perspective(norm(eye-center));                        // build the Perspective matrix
    init_viewport(width/16, height/16, width*7/8, height*7/8); // build the Viewport    matrix
    init_zbuffer(width, height);
    TGAImage framebuffer(width, height, TGAImage::RGB, {177, 195, 209, 255});

    const auto run_start = std::chrono::system_clock::now();
    const auto load_start = std::chrono::steady_clock::now();

    std::vector<Model> models;
    models.reserve(model_paths.size());

    for (const char* model_path : model_paths) {
        models.emplace_back(model_path);
    }

    const auto load_end = std::chrono::steady_clock::now();
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(load_end - load_start);
    const auto render_start = std::chrono::steady_clock::now();

    for (const Model &model : models) {           // iterate through all input objects
        PhongShader shader(light, model);
        for (int f=0; f<model.nfaces(); f++) {      // iterate through all facets
            Triangle clip = { shader.vertex(f, 0),  // assemble the primitive
                              shader.vertex(f, 1),
                              shader.vertex(f, 2) };
            rasterize(clip, shader, framebuffer);   // rasterize the primitive
        }
    }

    const auto render_end = std::chrono::steady_clock::now();
    const auto render_ms = std::chrono::duration_cast<std::chrono::milliseconds>(render_end - render_start);

    framebuffer.write_tga_file("framebuffer.tga");

    const std::time_t run_start_time = std::chrono::system_clock::to_time_t(run_start);
    std::tm local_tm{};
    localtime_r(&run_start_time, &local_tm);

    std::ofstream times_file("times.txt", std::ios::app);
    times_file << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S")
               << " Load time: " << load_ms.count() << " ms"
               << " Render time: " << render_ms.count() << " ms" << std::endl;

    std::cerr << "Load time: " << load_ms.count() << " ms" << std::endl;
    std::cerr << "Render time: " << render_ms.count() << " ms" << std::endl;

    return 0;
}
