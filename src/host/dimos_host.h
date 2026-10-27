/*
 * dimos_host.h -- the interfaces of the DimOS hosted runtime.
 *
 * DimOS is one kernel with two ways to run it. The canonical build boots
 * the flat binary on an IBM PC compatible machine. This directory builds the
 * very same kernel C files -- the shell, the nine applications, the
 * FAT12 driver, the TrueType rasterizer -- as an ordinary program for
 * Windows, Linux and Android (Termux or an NDK build). Nothing is
 * reimplemented: the only new code is the machine underneath, and the
 * hosted runtime even keeps the hardware's shape, so the kernel still talks
 * to ports 0x60/0x64, an IDE controller, a CMOS clock and a PIT.
 *
 * On top of that machine the runtime adds three things a desktop or phone
 * needs and a bare metal PC does not:
 *
 *   - a browser front end: the composited 844x390 frame is published as
 *     changed 16x16 tiles and touches come back as PS/2 packets, so one
 *     binary gives Android tablets, phones and Windows/Linux PCs the same
 *     phone-shaped GUI;
 *   - PNG stills for tests and documentation;
 *   - Lua 5.4 with the `dim` module, so applications written in Lua run
 *     headless on the same volume the GUI uses.
 *
 * Windows and macOS: DimOS targets Android and PC only. The build script
 * refuses Darwin on purpose -- see docs/ANYWHERE.md.
 */

#ifndef DIMOS_HOST_RUNTIME_H
#define DIMOS_HOST_RUNTIME_H

#include "dimos.h"

/* The kernel's entry point: kernel.asm jumps here on the metal, the
 * hosted runtime calls it after bringing its pretend machine up. */
void kernel_main(void);

/* ------------------------------------------------------------------ */
/* Options and start-up                                                */
/* ------------------------------------------------------------------ */

#define HOST_DISK_DEFAULT_BYTES (8u * 1024u * 1024u)
#define HOST_DISK_MIN_BYTES (256u * 1024u)
#define HOST_DISK_MAX_BYTES (32u * 1024u * 1024u)
#define HOST_DEFAULT_PORT 8080u

typedef struct {
    const char *disk_path;      /* FAT12 volume image ("dimos.hdd")     */
    u32 disk_bytes;             /* size when the image has to be made   */
    const char *font_path;      /* TrueType fallback ("fonts/font.ttf") */
    const char *shot_directory; /* where --png writes its frames        */
    const char *bind_address;   /* "0.0.0.0" serves phones on the LAN   */
    u16 port;
    u16 display_width;          /* 844x390 is the phone canvas itself   */
    u16 display_height;
    u8 allow_format;            /* create/format the image when needed  */
    u8 http;                    /* 0 = no web server                    */
    u8 verbose;
} HostOptions;

/* Brings the pretend machine up: memory, IDE disk, framebuffer, clock and
 * the windows fs.c reads. Returns 0 on success. */
int host_platform_start(const HostOptions *options);
void host_platform_stop(void);

/* True when the volume image had no FAT12 boot sector and has to be
 * formatted by the kernel (volume_format). */
u8 host_volume_is_blank(void);
/* Called from kernel_main right after the disk is mounted. Seeds a fresh
 * volume with a README and the bundled Lua applications, then refreshes
 * the copied boot windows so Files sees what was just written. */
void host_after_mount(void);
const char *host_disk_path(void);
u32 host_disk_bytes(void);
u32 host_debug_lines(char *out, u32 capacity);

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

u8 *host_framebuffer_pointer(void);  /* XRGB8888, width * height pixels  */
u16 host_display_width(void);
u16 host_display_height(void);

/* ------------------------------------------------------------------ */
/* Input: the same packets a PS/2 mouse and keyboard would send        */
/* ------------------------------------------------------------------ */

void host_input_move(s16 x, s16 y);
void host_input_set_buttons(s16 x, s16 y, u8 buttons); /* 1 left, 2 right */
void host_input_click(s16 x, s16 y, u8 buttons);
void host_input_wheel(s16 x, s16 y, s8 notches);
void host_input_key(u16 key);       /* ASCII or a KEY_ constant        */
void host_input_text(const char *text);
/* 1 while host_input_text() still has characters to deliver. */
u8 host_input_text_pending(void);

/* ------------------------------------------------------------------ */
/* Frame publication (ui.c)                                            */
/* ------------------------------------------------------------------ */

#define HOST_TILE 16u
#define HOST_FRAME_HEADER_BYTES 16u /* magic, seq, count, width, height */

/* Builds the tile payload the browser asks for. Returns 1 when there is
 * something to send, 0 when the screen has not changed. `buffer` is owned
 * by ui.c and stays valid until the next call. */
u8 host_frame_payload(u8 full, const u8 **buffer, u32 *length);
u32 host_frame_sequence(void);
void host_frame_forget(void);        /* the next payload is full anyway  */

/* Writes the current frame as a PNG. Returns 0 on success. */
u8 host_write_png(const char *path);
/* Composites the frame into an RGB buffer (no alpha). */
void host_copy_rgb(u8 *target);

/* ------------------------------------------------------------------ */
/* Web front end (http.c)                                              */
/* ------------------------------------------------------------------ */

int host_http_start(const char *bind_address, u16 port);
void host_http_poll(void);   /* non-blocking; called from the 60 Hz wait */
void host_http_stop(void);
u16 host_http_port(void);
const char *host_http_last_error(void);

/* ------------------------------------------------------------------ */
/* Lua engine (lua_engine.c)                                           */
/* ------------------------------------------------------------------ */

int host_lua_start(void);                     /* 0 = ok                  */
void host_lua_stop(void);
/* Runs a chunk. `path` is either a file on the host disk or the name of a
 * .LUA file on the DimOS volume. Returns 0 on success. */
int host_lua_run_file(const char *path, int argument_count, char **arguments);
int host_lua_run_text(const char *chunk, const char *chunk_name);
const char *host_lua_output(void);             /* accumulated console text */
/* Also echo output to stdout as it is produced (run/lua commands). The web
 * front end leaves this off and returns the buffer instead. */
void host_lua_stream_output(u8 on);
void host_lua_output_clear(void);
const char *host_lua_version(void);
/* Lists the .LUA files on the volume as a newline separated string. */
u32 host_lua_list_apps(char *out, u32 capacity);

/* ------------------------------------------------------------------ */
/* Small helpers shared by the host files                              */
/* ------------------------------------------------------------------ */

const char *host_platform_name(void);
void host_scene_tick(void);   /* tests and screenshots, normally a no-op */
void host_console(const char *format, ...);
u64 host_microseconds(void);
void host_sleep(u32 milliseconds);
void host_request_exit(int code);

#endif /* DIMOS_HOST_RUNTIME_H */
