#include <algorithm>
#include <cmath>
#include "our_gl.h"

mat<4,4> ModelView, Viewport, Perspective; // "OpenGL" state matrices
std::vector<double> zbuffer;               // depth buffer

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
    zbuffer = std::vector(width*height, -1000.);
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

    const double inv_area = 1. / area;
    const double inv_w0 = 1. / clip[0].w;
    const double inv_w1 = 1. / clip[1].w;
    const double inv_w2 = 1. / clip[2].w;
    const double z0 = ndc[0].z;
    const double z1 = ndc[1].z;
    const double z2 = ndc[2].z;

    const double w0_step_x = screen[1].y - screen[2].y;
    const double w1_step_x = screen[2].y - screen[0].y;
    const double w2_step_x = screen[0].y - screen[1].y;
    const double w0_step_y = screen[2].x - screen[1].x;
    const double w1_step_y = screen[0].x - screen[2].x;
    const double w2_step_y = screen[1].x - screen[0].x;
    const double z_step_x = (w0_step_x*z0 + w1_step_x*z1 + w2_step_x*z2) * inv_area;
    const double z_step_y = (w0_step_y*z0 + w1_step_y*z1 + w2_step_y*z2) * inv_area;

    const double start_x = static_cast<double>(minx);
    const double start_y = static_cast<double>(miny);
    const double row_w0 = orient2d(screen[1], screen[2], start_x, start_y);
    const double row_w1 = orient2d(screen[2], screen[0], start_x, start_y);
    const double row_w2 = orient2d(screen[0], screen[1], start_x, start_y);
    const double row_z = (row_w0*z0 + row_w1*z1 + row_w2*z2) * inv_area;
    const int pixels = (maxx-minx+1) * (maxy-miny+1);

    #pragma omp parallel for if(pixels > 4096) schedule(static)
    for (int y=miny; y<=maxy; y++) {
        double w0 = row_w0 + (y-miny) * w0_step_y;
        double w1 = row_w1 + (y-miny) * w1_step_y;
        double w2 = row_w2 + (y-miny) * w2_step_y;
        double z = row_z + (y-miny) * z_step_y;
        const int row_offset = y * width;

        for (int x=minx; x<=maxx; x++) {
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                const int idx = row_offset + x;
                if (z > zbuffer[idx]) {
                    const vec3 bc_screen = { w0*inv_area, w1*inv_area, w2*inv_area };
                    vec3 bc_clip = { bc_screen.x*inv_w0, bc_screen.y*inv_w1, bc_screen.z*inv_w2 }; // check https://github.com/ssloy/tinyrenderer/wiki/Technical-difficulties-linear-interpolation-with-perspective-deformations
                    bc_clip = bc_clip / (bc_clip.x + bc_clip.y + bc_clip.z);
                    auto [discard, color] = shader.fragment(bc_clip);
                    if (!discard) {
                        zbuffer[idx] = z;
                        framebuffer.set(x, y, color);
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
