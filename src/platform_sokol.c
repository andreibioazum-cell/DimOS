/*
 * platform_sokol.c -- the only file that knows about windows, input and
 * presenting pixels. Everything visible is rendered by sokol.
 *
 * Sokol is used for three things here: create a window (sokol_app), receive
 * input events, and blit one canvas-sized texture onto the screen. There is
 * deliberately no 3D in this file:
 *
 *   - no depth buffer, no stencil, no MSAA, no matrices, no projection;
 *   - no geometry, no vertex buffers: the full-window quad is generated from
 *     gl_VertexID inside the vertex shader;
 *   - one texture, one pipeline, two triangles per frame;
 *   - when the OS has nothing to animate (idle desktop), the canvas is not
 *     even redrawn and the texture is not uploaded at all.
 *
 * That is the whole GPU workload of the system: 1280x720 pixels stretched
 * into the window with a single bilinear sample per pixel.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOKOL_GLCORE33
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_glue.h"

#include "app.h"
#include "font_data.h"

static const char *quad_vertex_shader =
    "#version 330\n"
    "in vec2 position;\n"
    "in vec2 texcoord0;\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    uv = texcoord0;\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "}\n";

/* Four vertices: the last geometry in the system, 64 bytes in total. */
static const float quad_vertices[] = {
    /* x     y    u    v */
    -1.0f, 1.0f, 0.0f, 0.0f,
    1.0f, 1.0f, 1.0f, 0.0f,
    -1.0f, -1.0f, 0.0f, 1.0f,
    1.0f, -1.0f, 1.0f, 1.0f,
};

static const char *quad_fragment_shader =
    "#version 330\n"
    "in vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    frag_color = texture(tex, uv);\n"
    "}\n";

static struct {
    App *app;
    bool skip_boot;
    sg_image canvas_image;
    sg_buffer quad_buffer;
    sg_pipeline pipeline;
    sg_pass_action pass_action;
    sg_bindings bindings;
    float scale;
    float offset_x;
    float offset_y;
    int framebuffer_width;
    int framebuffer_height;
    unsigned long long frames;
    unsigned long long uploads;
} g;

static void update_letterbox(int fb_width, int fb_height) {
    g.framebuffer_width = fb_width;
    g.framebuffer_height = fb_height;
    const float scale_x = (float)fb_width / (float)APP_WIDTH;
    const float scale_y = (float)fb_height / (float)APP_HEIGHT;
    g.scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (g.scale <= 0.0f) {
        g.scale = 1.0f;
    }
    g.offset_x = ((float)fb_width - (float)APP_WIDTH * g.scale) * 0.5f;
    g.offset_y = ((float)fb_height - (float)APP_HEIGHT * g.scale) * 0.5f;
}

static void window_to_canvas(float wx, float wy, float *cx, float *cy) {
    *cx = (wx - g.offset_x) / g.scale;
    *cy = (wy - g.offset_y) / g.scale;
}

static void init(void) {
    sg_setup(&(sg_desc){
        .context = sapp_sgcontext(),
        .logger.func = slog_func,
    });

    g.app = app_create(font_ttf, font_ttf_size);
    if (g.app && g.skip_boot) {
        app_skip_boot(g.app);
    }
    if (!g.app) {
        fprintf(stderr, "sokol-os: out of memory\n");
        sapp_request_quit();
        return;
    }

    g.quad_buffer = sg_make_buffer(&(sg_buffer_desc){
        .data = SG_RANGE(quad_vertices),
        .label = "canvas-quad-vertices",
    });

    g.canvas_image = sg_make_image(&(sg_image_desc){
        .width = APP_WIDTH,
        .height = APP_HEIGHT,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage = SG_USAGE_STREAM,
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
        .label = "sokol-os-canvas",
    });

    g.pipeline = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(&(sg_shader_desc){
            .attrs = {
                [0] = {.name = "position"},
                [1] = {.name = "texcoord0"},
            },
            .vs.source = quad_vertex_shader,
            .fs.source = quad_fragment_shader,
            .fs.images[0] = {.name = "tex", .image_type = SG_IMAGETYPE_2D},
            .label = "canvas-blit-shader",
        }),
        .layout = {
            .attrs = {
                [0] = {.format = SG_VERTEXFORMAT_FLOAT2},
                [1] = {.format = SG_VERTEXFORMAT_FLOAT2},
            },
        },
        .primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP,
        /* No depth test, no depth writes: this pipeline is 2D by contract. */
        .depth = {
            .write_enabled = false,
            .compare = SG_COMPAREFUNC_ALWAYS,
        },
        .label = "canvas-blit-pipeline",
    });

    g.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    g.pass_action.colors[0].clear_value = (sg_color){0.02f, 0.02f, 0.03f, 1.0f};
    g.bindings.vertex_buffers[0] = g.quad_buffer;
    g.bindings.fs_images[0] = g.canvas_image;

    update_letterbox(sapp_width(), sapp_height());
}

static void cleanup(void) {
    app_destroy(g.app);
    g.app = NULL;
    sg_shutdown();
}

static AppKey map_key(sapp_keycode code) {
    switch (code) {
    case SAPP_KEYCODE_ESCAPE:
        return KEY_ESCAPE;
    case SAPP_KEYCODE_ENTER:
        return KEY_ENTER;
    case SAPP_KEYCODE_SPACE:
        return KEY_SPACE;
    case SAPP_KEYCODE_TAB:
        return KEY_TAB;
    case SAPP_KEYCODE_LEFT:
        return KEY_LEFT;
    case SAPP_KEYCODE_RIGHT:
        return KEY_RIGHT;
    case SAPP_KEYCODE_UP:
        return KEY_UP;
    case SAPP_KEYCODE_DOWN:
        return KEY_DOWN;
    case SAPP_KEYCODE_A:
        return KEY_A;
    case SAPP_KEYCODE_D:
        return KEY_D;
    case SAPP_KEYCODE_W:
        return KEY_W;
    case SAPP_KEYCODE_S:
        return KEY_S;
    case SAPP_KEYCODE_P:
        return KEY_P;
    case SAPP_KEYCODE_R:
        return KEY_R;
    case SAPP_KEYCODE_F1:
        return KEY_F1;
    default:
        return KEY_UNKNOWN;
    }
}

static void event(const sapp_event *ev) {
    if (!g.app) {
        return;
    }
    InputEvent in;
    memset(&in, 0, sizeof(in));
    switch (ev->type) {
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        in.type = INPUT_MOUSE_MOVE;
        window_to_canvas(ev->mouse_x, ev->mouse_y, &in.x, &in.y);
        app_input(g.app, &in);
        break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
        in.type = INPUT_MOUSE_DOWN;
        window_to_canvas(ev->mouse_x, ev->mouse_y, &in.x, &in.y);
        app_input(g.app, &in);
        break;
    case SAPP_EVENTTYPE_MOUSE_UP:
        in.type = INPUT_MOUSE_UP;
        window_to_canvas(ev->mouse_x, ev->mouse_y, &in.x, &in.y);
        app_input(g.app, &in);
        break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        in.type = INPUT_MOUSE_WHEEL;
        window_to_canvas(ev->mouse_x, ev->mouse_y, &in.x, &in.y);
        in.wheel = ev->scroll_y;
        app_input(g.app, &in);
        break;
    case SAPP_EVENTTYPE_KEY_DOWN:
        if (ev->key_repeat) {
            break;
        }
        in.type = INPUT_KEY_DOWN;
        in.key = map_key(ev->key_code);
        window_to_canvas(ev->mouse_x, ev->mouse_y, &in.x, &in.y);
        app_input(g.app, &in);
        break;
    case SAPP_EVENTTYPE_RESIZED:
        update_letterbox(ev->framebuffer_width, ev->framebuffer_height);
        break;
    default:
        break;
    }
}

static void frame(void) {
    const int width = sapp_width();
    const int height = sapp_height();
    if (width != g.framebuffer_width || height != g.framebuffer_height) {
        update_letterbox(width, height);
    }
    if (!g.app) {
        return;
    }

    if (app_needs_frame(g.app)) {
        app_update(g.app, (float)sapp_frame_duration());
        app_draw_frame(g.app);
        const Canvas *canvas = app_canvas(g.app);
        sg_update_image(g.canvas_image, &(sg_image_data){
            .subimage[0][0] = {
                .ptr = canvas->pixels,
                .size = (size_t)canvas->width * (size_t)canvas->height * sizeof(uint32_t),
            },
        });
        g.uploads += 1;
    }
    g.frames += 1;

    if (app_quit_requested(g.app)) {
        sapp_request_quit();
    }

    sg_begin_default_pass(&g.pass_action, width, height);
    sg_apply_viewportf(g.offset_x, g.offset_y, (float)APP_WIDTH * g.scale, (float)APP_HEIGHT * g.scale, true);
    sg_apply_pipeline(g.pipeline);
    sg_apply_bindings(&g.bindings);
    sg_draw(0, 4, 1);
    sg_end_pass();
    sg_commit();
}

static bool has_flag(int argc, char *argv[], const char *name) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

sapp_desc sokol_main(int argc, char *argv[]) {
    /* --fullscreen and --skip-boot are used by the live ISO, where the real
     * kernel has already printed its boot log on the first console. */
    const bool fullscreen = has_flag(argc, argv, "--fullscreen") || getenv("DIMOS_FULLSCREEN") != NULL;
    g.skip_boot = has_flag(argc, argv, "--skip-boot") || getenv("DIMOS_SKIP_BOOT") != NULL;
    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = event,
        .width = 1280,
        .height = 720,
        .window_title = "DimOS",
        .fullscreen = fullscreen,
        .high_dpi = true,
        .swap_interval = 1,
        .sample_count = 1,
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}
