#include "our_gl.h"
#include "model.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <string>
#include <string_view>

extern mat<4,4> ModelView, Perspective; // "OpenGL" state matrices and
extern std::vector<double> zbuffer;     // the depth buffer

struct PhongShader : IShader {
    const Model &model;
    const TGAImage &diffusemap;
    const TGAImage &normalmap;
    const TGAImage &specularmap;
    vec4 l;              // light direction in eye coordinates
    mat<4,4> normal_matrix;
    int diffuse_width;
    int diffuse_height;
    int normal_width;
    int normal_height;
    int specular_width;
    int specular_height;
    vec2  varying_uv[3]; // triangle uv coordinates, written by the vertex shader, read by the fragment shader
    vec4 varying_nrm[3]; // normal per vertex to be interpolated by the fragment shader
    vec4 tri[3];         // triangle in view coordinates
    vec4 tangent;
    vec4 bitangent;

    PhongShader(const vec3 light, const Model &m) : model(m), diffusemap(m.diffuse()), normalmap(m.normal_map()), specularmap(m.specular()) {
        l = normalized((ModelView*vec4{light.x, light.y, light.z, 0.})); // transform the light vector to view coordinates
        normal_matrix = ModelView.invert_transpose();
        diffuse_width = diffusemap.width();
        diffuse_height = diffusemap.height();
        normal_width = normalmap.width();
        normal_height = normalmap.height();
        specular_width = specularmap.width();
        specular_height = specularmap.height();
    }

    virtual vec4 vertex(const int face, const int vert) {
        varying_uv[vert]  = model.uv(face, vert);
        varying_nrm[vert] = normal_matrix * model.normal(face, vert);
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
