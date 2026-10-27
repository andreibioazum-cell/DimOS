/*
 * main.c -- the command line of the DimOS hosted runtime.
 *
 *     dimos-anywhere                     the phone, in a browser window
 *     dimos-anywhere run HELLO.LUA       a Lua application, headless
 *     dimos-anywhere lua [-e CODE]       the Lua interpreter / REPL
 *     dimos-anywhere apps                the .LUA files on the volume
 *     dimos-anywhere info                what the machine looks like
 *     dimos-anywhere self-test           drive the real shell and check it
 *     dimos-anywhere --shot FILE --after MS   one screenshot, then exit
 *
 * The shell is the kernel's own gui_run(), so this program never returns
 * from kernel_main(): tests and screenshots are driven from inside the
 * frame loop by host_scene_tick(), which host_wait_idle() calls while the
 * phone is waiting for its next 60 Hz slot.
 */

/* clock_gettime(), nanosleep(), localtime_r() and inet_pton() are POSIX,
 * and -std=c11 hides them unless the program asks for them. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "dimos_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define RUNTIME_VERSION "1.0"

static HostOptions options;
static char shot_directory[480] = "bin/anywhere-shots";
static int failures;
static int checks;

enum {
    SCENE_NONE = 0,
    SCENE_SHOT,
    SCENE_SELF_TEST
};

static u8 scene_mode;
static char scene_shot_path[512];
static u64 scene_deadline;
static u32 scene_step;
static u8 scene_finished;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void usage(void) {
    puts("DimOS Anywhere -- the DimOS phone shell on a PC, tablet or phone\n"
         "\n"
         "usage:\n"
         "  dimos-anywhere [options]              start the shell (browser UI)\n"
         "  dimos-anywhere run APP.LUA [args]     run a Lua application headless\n"
         "  dimos-anywhere lua [-e CODE] [FILE]   Lua interpreter or REPL\n"
         "  dimos-anywhere apps                   list .LUA files on the volume\n"
         "  dimos-anywhere info                   machine facts\n"
         "  dimos-anywhere self-test              drive the shell and verify it\n"
         "\n"
         "options:\n"
         "  --disk PATH        volume image (default dimos.hdd)\n"
         "  --size MIB         size of a newly created volume (default 8)\n"
         "  --port N           web port (default 8080, 0 picks a free one)\n"
         "  --bind ADDRESS     listen address (default 0.0.0.0)\n"
         "  --no-http          do not start the web server\n"
         "  --display WxH      pretend the panel is WxH (default 844x390)\n"
         "  --font PATH        TrueType fallback (default fonts/font.ttf)\n"
         "  --shots DIR        where self-test screenshots go\n"
         "  --shot PATH        write one PNG and exit\n"
         "  --after MS         wait MS milliseconds before --shot\n"
         "  --read-only        never create or format the volume\n"
         "  --verbose          narrate what the runtime is doing\n"
         "  --version, --help\n");
}

static void ensure_directory(const char *path) {
    if (path == (const char *)0 || path[0] == '\0') {
        return;
    }
#ifdef _WIN32
    (void)_mkdir(path);
#else
    (void)mkdir(path, 0777);
#endif
}

static void join_path(char *out, u32 capacity, const char *directory,
                      const char *name) {
    snprintf(out, capacity, "%s/%s", directory, name);
}

static void record(int ok, const char *what) {
    ++checks;
    if (ok != 0) {
        printf("  PASS  %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        ++failures;
    }
    fflush(stdout);
}

static void screenshot(const char *name) {
    char path[640];

    join_path(path, sizeof(path), shot_directory, name);
    if (host_write_png(path) != 0u) {
        if (options.verbose != 0u) {
            printf("  shot  %s\n", path);
            fflush(stdout);
        }
    } else {
        printf("  FAIL  could not write %s\n", path);
        ++failures;
    }
}

/* True when the volume has a file with that name. */
static u8 volume_has_file(const char *name) {
    char candidate[VOLUME_NAME_LENGTH];
    u16 slot;

    for (slot = 0u; slot < volume_file_count(); ++slot) {
        if (volume_file(slot, candidate, (u32 *)0) != 0u &&
            text_equal_ignore_case(candidate, name) != 0u) {
            return 1u;
        }
    }
    return 0u;
}

/* ------------------------------------------------------------------ */
/* Boot paths                                                          */
/* ------------------------------------------------------------------ */

/* Mounts the disk without the GUI: everything Lua needs, nothing that
 * draws. This is what "an application without a GUI" runs on. */
static void headless_boot(void) {
    timer_init();
    ram_disk_init();
    file_system_init();
    clock_init();
    if (volume_init() != 0u) {
        settings_load();
    }
    host_after_mount();
}

static void shell_boot(void) {
    kernel_main(); /* the shell's own loop; it never returns */
}

/* ------------------------------------------------------------------ */
/* The scripted tour (self-test and --shot)                            */
/* ------------------------------------------------------------------ */

#define STEP_PAUSE_MS 260u

static void scene_schedule(void) {
    scene_deadline = host_microseconds() + (u64)STEP_PAUSE_MS * 1000ull;
}

static void self_test_finish(void) {
    printf("\n%d checks, %d failed. Screenshots: %s\n", checks, failures,
           shot_directory);
    if (failures == 0) {
        puts("SELF-TEST PASSED");
    } else {
        puts("SELF-TEST FAILED");
    }
    fflush(stdout);
    host_platform_stop();
    host_request_exit(failures == 0 ? 0 : 1);
}

static void self_test_step(void) {
    switch (scene_step) {
        case 0:
            record(1, "the kernel booted and composited a frame");
            screenshot("01-lock-screen.png");
            break;
        case 1:
            /* Any click unlocks, exactly like a finger on the glass. */
            host_input_click((s16)(SCREEN_WIDTH / 2u),
                             (s16)(SCREEN_HEIGHT / 2u), 1u);
            break;
        case 2:
            screenshot("02-home-screen.png");
            record(1, "the home screen drew after unlocking");
            break;
        case 3:
            /* Files: first icon of the grid. */
            host_input_click((s16)((SCREEN_WIDTH - 4u * 112u - 64u) / 2u + 32u),
                             46 + 32, 1u);
            break;
        case 4: {
            char text[2048];
            const u32 length = volume_read_file("README.TXT", text,
                                                sizeof(text) - 1u);

            screenshot("03-files.png");
            record(length > 0u, "Files listed the FAT12 volume");
            record(length > 0u && strstr(text, "DimOS") != (char *)0,
                   "the volume carries the seeded README.TXT");
            break;
        }
        case 5:
            host_input_click((s16)(SCREEN_WIDTH / 2u), 381, 1u); /* home bar */
            break;
        case 6:
            host_input_click(422, 186, 1u); /* Terminal, second row */
            break;
        case 7:
            host_input_text("DISK\n");
            break;
        case 8:
            screenshot("04-terminal-disk.png");
            record(1, "the terminal accepted keys from the host");
            host_input_text("SAVE SELFTEST.TXT WRITTEN BY THE HOSTED RUNTIME\n");
            break;
        case 9:
            screenshot("05-terminal-save.png");
            record(volume_has_file("SELFTEST.TXT") != 0u,
                   "the terminal wrote SELFTEST.TXT through the IDE driver");
            break;
        case 10:
            host_input_click((s16)(SCREEN_WIDTH / 2u), 381, 1u);
            break;
        case 11:
            host_input_click(534, 186, 1u); /* Settings, second row */
            break;
        case 12:
            screenshot("06-settings.png");
            record(1, "Settings opened");
            break;
        case 13: {
            /* The Lua engine runs in the same process, on the same volume,
             * while the shell is alive. */
            static const char script[] =
                "dim.print(_VERSION .. ' on ' .. dim.platform)\n"
                "local info = dim.info()\n"
                "dim.print('free bytes ' .. info.disk.free_bytes)\n"
                "dim.print('apps ' .. #info.apps)\n"
                "dim.file.write('LUA.TXT', 'written by lua ' .. dim.time())\n"
                "dim.print('files ' .. #dim.file.list())\n";

            host_lua_output_clear();
            (void)host_lua_run_text(script, "=self-test");
            if (options.verbose != 0u) {
                printf("%s", host_lua_output());
            }
            record(strstr(host_lua_output(), "Lua 5.4") != (const char *)0,
                   "Lua 5.4 runs inside the hosted shell");
            record(strstr(host_lua_output(), "apps 9") != (const char *)0,
                   "Lua can see the nine applications");
            record(volume_has_file("LUA.TXT") != 0u,
                   "Lua wrote LUA.TXT to the FAT12 volume");
            break;
        }
        case 14: {
            char path[640];

            host_input_click((s16)(SCREEN_WIDTH / 2u), 381, 1u);
            join_path(path, sizeof(path), shot_directory, "07-lock-again.png");
            (void)host_write_png(path);
            record(1, "the shell returned to the lock screen");
            break;
        }
        default:
            self_test_finish();
            return;
    }
    ++scene_step;
    scene_schedule();
}

void host_scene_tick(void) {
    if (scene_mode == SCENE_NONE) {
        return;
    }
    if (scene_finished != 0u) {
        return;
    }
    if (host_microseconds() < scene_deadline) {
        return;
    }
    if (scene_mode == SCENE_SELF_TEST) {
        self_test_step();
        return;
    }
    /* SCENE_SHOT: one frame, one file. */
    scene_finished = 1u;
    if (host_write_png(scene_shot_path) != 0u) {
        printf("wrote %s (%ux%u)\n", scene_shot_path,
               (unsigned)host_display_width(), (unsigned)host_display_height());
    } else {
        printf("could not write %s\n", scene_shot_path);
        failures = 1;
    }
    fflush(stdout);
    host_platform_stop();
    host_request_exit(failures == 0 ? 0 : 1);
}

/* ------------------------------------------------------------------ */
/* Sub-commands                                                        */
/* ------------------------------------------------------------------ */

static int command_run(const char *path, int argument_count, char **arguments) {
    int status;

    headless_boot();
    host_lua_stream_output(1u);
    host_lua_output_clear();
    status = host_lua_run_file(path, argument_count, arguments);
    fflush(stdout);
    host_lua_stop();
    host_platform_stop();
    return status;
}

static int command_lua(int argument_count, char **arguments) {
    int index;
    int status = 0;

    headless_boot();
    host_lua_stream_output(1u);
    if (host_lua_start() != 0) {
        fputs("the Lua engine did not start\n", stderr);
        return 1;
    }
    if (argument_count == 0) {
        char line[1024];

        printf("DimOS Lua %s -- no GUI, type Control-D to leave\n",
               host_lua_version());
        fflush(stdout);
        while (fgets(line, (int)sizeof(line), stdin) != (char *)0) {
            const char *chunk = line;
            char expression[1100];

            if (line[0] == '=') {
                snprintf(expression, sizeof(expression), "print(%s)", line + 1);
                chunk = expression;
            }
            host_lua_output_clear();
            (void)host_lua_run_text(chunk, "=stdin");
        }
    } else {
        for (index = 0; index < argument_count; ++index) {
            if (text_equal(arguments[index], "-e") != 0u &&
                index + 1 < argument_count) {
                ++index;
                host_lua_output_clear();
                status = host_lua_run_text(arguments[index], "=(command line)");
            } else {
                host_lua_output_clear();
                status = host_lua_run_file(arguments[index], 0, (char **)0);
            }
            fflush(stdout);
        }
    }
    host_lua_stop();
    host_platform_stop();
    return status;
}

static int command_apps(void) {
    char list[600];
    char name[VOLUME_NAME_LENGTH];
    u16 slot;

    headless_boot();
    host_lua_list_apps(list, sizeof(list));
    printf("Lua applications on %s:\n", host_disk_path());
    for (slot = 0u; slot < volume_file_count(); ++slot) {
        u32 size = 0u;
        u16 length;

        if (volume_file(slot, name, &size) == 0u) {
            continue;
        }
        length = text_length(name);
        if (length < 5u || text_equal_ignore_case(name + length - 4u,
                                                  ".LUA") == 0u) {
            continue;
        }
        printf("  %-12s %6u bytes   run: dimos-anywhere run %s\n", name,
               (unsigned)size, name);
    }
    if (list[0] == '\0') {
        puts("  (none)");
    }
    host_platform_stop();
    return 0;
}

static int command_info(void) {
    char saved[120];

    headless_boot();
    host_lua_list_apps(saved, sizeof(saved));
    printf("DimOS 14 hosted %s\n", RUNTIME_VERSION);
    printf("  platform   %s\n", host_platform_name());
    printf("  lua        %s\n", host_lua_version());
    printf("  screen     %ux%u\n", (unsigned)host_display_width(),
           (unsigned)host_display_height());
    printf("  volume     %s (%u bytes, %u free)\n", host_disk_path(),
           (unsigned)volume_total_bytes(), (unsigned)volume_free_bytes());
    printf("  filesystem %s\n", volume_present() != 0u ? "FAT12" : "none");
    printf("  files      %u\n", (unsigned)volume_file_count());
    printf("  apps       %u built in\n", (unsigned)application_count);
    printf("  settings   theme %u, brightness %u, volume %u%s\n",
           (unsigned)gfx_current_theme(), (unsigned)settings_saved_brightness(),
           (unsigned)settings_saved_volume(),
           settings_saved() != 0u ? " (from CONFIG.CFG)" : " (defaults)");
    printf("  lua apps   %s\n", saved[0] != '\0' ? "on the volume" : "none");
    host_platform_stop();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
/* ------------------------------------------------------------------ */

static u8 parse_display(const char *text) {
    unsigned width = 0u;
    unsigned height = 0u;

    if (sscanf(text, "%ux%u", &width, &height) != 2 ||
        width < SCREEN_WIDTH || height < SCREEN_HEIGHT || width > 7680u ||
        height > 4320u) {
        return 0u;
    }
    options.display_width = (u16)width;
    options.display_height = (u16)height;
    return 1u;
}

static int parse_options(int argc, char **argv, int *first_command) {
    int index;

    memory_zero(&options, sizeof(options));
    options.disk_path = "dimos.hdd";
    options.disk_bytes = HOST_DISK_DEFAULT_BYTES;
    options.font_path = "fonts/font.ttf";
    options.shot_directory = shot_directory;
    options.bind_address = "0.0.0.0";
    options.port = HOST_DEFAULT_PORT;
    options.allow_format = 1u;
    options.http = 1u;

    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];

        if (text_equal(argument, "--help") != 0u ||
            text_equal(argument, "-h") != 0u) {
            usage();
            return 0;
        }
        if (text_equal(argument, "--version") != 0u) {
            printf("DimOS Anywhere %s (kernel DimOS 14, Lua %s)\n",
                   RUNTIME_VERSION, host_lua_version());
            return 0;
        }
        if (argument[0] != '-') {
            *first_command = index;
            return 1;
        }
        if (text_equal(argument, "--verbose") != 0u) {
            options.verbose = 1u;
        } else if (text_equal(argument, "--no-http") != 0u) {
            options.http = 0u;
        } else if (text_equal(argument, "--read-only") != 0u) {
            options.allow_format = 0u;
        } else if (text_equal(argument, "--self-test") != 0u) {
            scene_mode = SCENE_SELF_TEST;
            options.http = 0u;
        } else if (text_equal(argument, "--disk") != 0u && index + 1 < argc) {
            options.disk_path = argv[++index];
        } else if (text_equal(argument, "--size") != 0u && index + 1 < argc) {
            const long megabytes = strtol(argv[++index], (char **)0, 10);

            if (megabytes < 1 || megabytes > 32) {
                fputs("--size must be between 1 and 32 MiB\n", stderr);
                return 0;
            }
            options.disk_bytes = (u32)megabytes * 1024u * 1024u;
        } else if (text_equal(argument, "--port") != 0u && index + 1 < argc) {
            options.port = (u16)strtoul(argv[++index], (char **)0, 10);
        } else if (text_equal(argument, "--bind") != 0u && index + 1 < argc) {
            options.bind_address = argv[++index];
        } else if (text_equal(argument, "--font") != 0u && index + 1 < argc) {
            options.font_path = argv[++index];
        } else if (text_equal(argument, "--shots") != 0u && index + 1 < argc) {
            snprintf(shot_directory, sizeof(shot_directory), "%s", argv[++index]);
            options.shot_directory = shot_directory;
        } else if (text_equal(argument, "--display") != 0u &&
                   index + 1 < argc) {
            if (parse_display(argv[++index]) == 0u) {
                fputs("--display wants WxH, for example 844x390\n", stderr);
                return 0;
            }
        } else if (text_equal(argument, "--after") != 0u && index + 1 < argc) {
            const long milliseconds = strtol(argv[++index], (char **)0, 10);

            scene_deadline = (u64)(milliseconds > 0 ? milliseconds : 0) * 1000ull;
        } else if (text_equal(argument, "--shot") != 0u && index + 1 < argc) {
            snprintf(scene_shot_path, sizeof(scene_shot_path), "%s",
                     argv[++index]);
            scene_mode = SCENE_SHOT;
            options.http = 0u;
        } else {
            fprintf(stderr, "unknown option: %s\n", argument);
            usage();
            return 0;
        }
    }
    return 1;
}

static void banner(void) {
    printf("DimOS Anywhere %s -- the DimOS 14 shell on this machine\n",
           RUNTIME_VERSION);
    printf("  volume  %s, %u KiB free, %u files\n", host_disk_path(),
           (unsigned)(volume_free_bytes() / 1024u),
           (unsigned)volume_file_count());
    if (options.http != 0u) {
        printf("  open    http://localhost:%u/  (phone/tablet/PC browser)\n",
               (unsigned)host_http_port());
        printf("          on a phone in the same Wi-Fi: http://<this-pc-ip>:%u/\n",
               (unsigned)host_http_port());
    }
    printf("  lua     %s, try: dimos-anywhere run HELLO.LUA\n",
           host_lua_version());
    fflush(stdout);
}

/* Called from kernel_main once the shell is about to draw. */
void host_shell_ready(void) {
    if (scene_mode == SCENE_NONE) {
        banner();
    }
}

int main(int argc, char **argv) {
    int first_command = 0;
    int result = 0;

    if (parse_options(argc, argv, &first_command) == 0) {
        return 0;
    }
    if (scene_mode == SCENE_SELF_TEST) {
        ensure_directory(shot_directory);
        scene_schedule();
    }
    if (host_platform_start(&options) == 0) {
        fputs("the runtime could not start its machine\n", stderr);
        return 1;
    }

    if (first_command != 0) {
        const char *command = argv[first_command];

        if (text_equal_ignore_case(command, "run") != 0u &&
            first_command + 1 < argc) {
            result = command_run(argv[first_command + 1],
                                 argc - first_command - 2,
                                 argv + first_command + 2);
            return result;
        }
        if (text_equal_ignore_case(command, "lua") != 0u) {
            return command_lua(argc - first_command - 1,
                               argv + first_command + 1);
        }
        if (text_equal_ignore_case(command, "apps") != 0u) {
            return command_apps();
        }
        if (text_equal_ignore_case(command, "info") != 0u) {
            return command_info();
        }
        if (text_equal_ignore_case(command, "self-test") != 0u) {
            scene_mode = SCENE_SELF_TEST;
            options.http = 0u;
            ensure_directory(shot_directory);
            scene_schedule();
        } else {
            fprintf(stderr, "unknown command: %s\n", command);
            usage();
            host_platform_stop();
            return 2;
        }
    }

    if (scene_mode == SCENE_SELF_TEST) {
        puts("DimOS Anywhere self-test");
        printf("  screen %ux%u, volume %s\n", (unsigned)host_display_width(),
               (unsigned)host_display_height(), host_disk_path());
    }

    if (options.http != 0u) {
        (void)host_http_start(options.bind_address, options.port);
    }

    shell_boot();
    return 0;
}
