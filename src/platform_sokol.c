/*
 * platform_sokol.c -- the only file that knows about windows, input and
 * presenting pixels.
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
#include <stdio.h>
#include <string.h>

#define SOKOL_GLCORE33
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_glue.h"

#include "app.h"
#include "font_data.h"

/* gl_VertexID 0..3 -> a full-viewport triangle strip, no vertex buffer. */
static const char *quad_vertex_shader =
    "#version 330\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    uv = p * 0.5;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

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
    sg_image canvas_image;
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
    if (!g.app) {
        fprintf(stderr, "sokol-os: out of memory\n");
        sapp_request_quit();
        return;
    }

    g.canvas_image = sg_make_image(&(sg_image_desc){
        .width = APP_WIDTH,
        .height = APP_HEIGHT,
        .pixel_format = SG_PIXELFORMAT_BGRA8,
        .usage = SG_USAGE_STREAM,
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
        .label = "sokol-os-canvas",
    });

    g.pipeline = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(&(sg_shader_desc){
            .vs.source = quad_vertex_shader,
            .fs.source = quad_fragment_shader,
            .fs.images[0] = {.name = "tex", .image_type = SG_IMAGETYPE_2D},
            .label = "canvas-blit-shader",
        }),
        .primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP,
        .label = "canvas-blit-pipeline",
    });

    g.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    g.pass_action.colors[0].clear_value[0] = 0.02f;
    g.pass_action.colors[0].clear_value[1] = 0.02f;
    g.pass_action.colors[0].clear_value[2] = 0.03f;
    g.pass_action.colors[0].clear_value[3] = 1.0f;
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
    case SAPP_KEYCODE_KP_ENTER:
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
        update_letterbox(ev->window_width, ev->window_height);
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

    sg_begin_pass(&(sg_pass){.action = g.pass_action, .swapchain = sglue_swapchain()});
    sg_apply_viewportf(g.offset_x, g.offset_y, (float)APP_WIDTH * g.scale, (float)APP_HEIGHT * g.scale, true);
    sg_apply_pipeline(g.pipeline);
    sg_apply_bindings(&g.bindings);
    sg_draw(0, 4, 1);
    sg_end_pass();
    sg_commit();
}

sapp_desc sokol_main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = event,
        .width = 1280,
        .height = 720,
        .window_title = "Sokol OS -- Snake",
        .high_dpi = true,
        .swap_interval = 1,
        .sample_count = 1,
        .depth_format = SAPP_PIXELFORMAT_NONE,
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}
