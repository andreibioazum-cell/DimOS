/*
 * lua_engine.c -- Lua 5.4 inside DimOS, without a GUI.
 *
 * The rule the user asked for is simple: an application written in Lua must
 * run on DimOS, and it must not draw anything. So there is no Lua access to
 * the framebuffer, no widget toolkit and no event loop for scripts. A Lua
 * application is a program that talks to the system:
 *
 *     dim.file.list()      files on the same FAT12 volume the GUI uses
 *     dim.file.read/write  ordinary 8.3 files, readable on any other system
 *     dim.clock()/time()   the real time clock
 *     dim.sleep(ms)        wait, without spinning a core
 *     dim.info()           disk, screen size, theme, apps, Lua version
 *     dim.theme/brightness/volume   read or change the shell settings
 *     dim.print/write      the console (also returned to the browser)
 *
 * The interpreter is stock Lua 5.4.7 from third_party/lua: the core, the
 * auxiliary library and the base, coroutine, table, string, math and utf8
 * libraries. `io`, `os`, `debug` and the dynamic `require` are deliberately
 * not loaded -- files go through `dim.file`, which writes the DimOS volume
 * instead of the host file system, so a Lua application cannot wander off
 * into the machine it happens to run on. `require` and `dofile` are
 * replaced with versions that read .LUA files from that same volume.
 */

#include "dimos_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define DIM_OUTPUT_BYTES (64u * 1024u)
#define DIM_CHUNK_BYTES (64u * 1024u)
#define DIM_EXIT_SENTINEL "\1dim-exit\1"

static lua_State *state;
static char console_output[DIM_OUTPUT_BYTES];
static u32 console_length;
static int exit_code;
static u8 exit_requested;
static u8 stream_output;

/* ------------------------------------------------------------------ */
/* Console capture                                                     */
/* ------------------------------------------------------------------ */

static void output_append(const char *text, u32 length) {
    if (length > DIM_OUTPUT_BYTES - 1u - console_length) {
        length = DIM_OUTPUT_BYTES - 1u - console_length;
    }
    if (length == 0u) {
        return;
    }
    memory_copy(console_output + console_length, text, length);
    console_length += length;
    console_output[console_length] = '\0';
    if (stream_output != 0u) {
        fwrite(text, 1u, length, stdout);
        fflush(stdout);
    }
}

void host_lua_stream_output(u8 on) {
    stream_output = on;
}

const char *host_lua_output(void) {
    console_output[console_length] = '\0';
    if (console_length == 0u) {
        return "";
    }
    return console_output;
}

void host_lua_output_clear(void) {
    /* The runtime host: closing the console window must not lose the last
     * few bytes of an application's output. */
    if (stream_output != 0u) {
        fflush(stdout);
    }
    console_length = 0u;
    console_output[0] = '\0';
}

const char *host_lua_version(void) {
    return LUA_VERSION_MAJOR "." LUA_VERSION_MINOR "." LUA_VERSION_RELEASE;
}

/* ------------------------------------------------------------------ */
/* Volume helpers                                                      */
/* ------------------------------------------------------------------ */

static u8 volume_find(const char *name, u32 *size) {
    char candidate[VOLUME_NAME_LENGTH];
    u16 slot;

    for (slot = 0u; slot < volume_file_count(); ++slot) {
        if (volume_file(slot, candidate, size) != 0u &&
            text_equal_ignore_case(candidate, name) != 0u) {
            return 1u;
        }
    }
    return 0u;
}

/* Reads a whole file from the volume into a freshly allocated buffer. */
static u8 *volume_slurp(const char *name, u32 *length) {
    u32 size = 0u;
    u8 *buffer;

    if (volume_present() == 0u || volume_find(name, &size) == 0u) {
        return (u8 *)0;
    }
    if (size > DIM_CHUNK_BYTES * 16u) {
        return (u8 *)0;
    }
    buffer = (u8 *)malloc(size + 1u);
    if (buffer == (u8 *)0) {
        return (u8 *)0;
    }
    if (size != 0u && volume_read_file(name, buffer, size) != size) {
        free(buffer);
        return (u8 *)0;
    }
    buffer[size] = '\0';
    *length = size;
    return buffer;
}

u32 host_lua_list_apps(char *out, u32 capacity) {
    char name[VOLUME_NAME_LENGTH];
    u32 position = 0u;
    u16 slot;

    if (capacity == 0u) {
        return 0u;
    }
    out[0] = '\0';
    for (slot = 0u; slot < volume_file_count(); ++slot) {
        u16 length;

        if (volume_file(slot, name, (u32 *)0) == 0u) {
            continue;
        }
        length = text_length(name);
        if (length < 5u || text_equal_ignore_case(name + length - 4u,
                                                  ".LUA") == 0u) {
            continue;
        }
        if (position + (u32)length + 2u >= capacity) {
            break;
        }
        memory_copy(out + position, name, length);
        position += length;
        out[position++] = '\n';
        out[position] = '\0';
    }
    return position;
}

/* ------------------------------------------------------------------ */
/* The `dim` module                                                    */
/* ------------------------------------------------------------------ */

static int dim_print(lua_State *L) {
    const int count = lua_gettop(L);
    int index;

    for (index = 1; index <= count; ++index) {
        size_t length = 0u;
        const char *text;

        if (index > 1) {
            output_append("\t", 1u);
        }
        text = luaL_tolstring(L, index, &length);
        output_append(text, (u32)length);
        lua_pop(L, 1);
    }
    output_append("\n", 1u);
    return 0;
}

static int dim_write(lua_State *L) {
    const int count = lua_gettop(L);
    int index;

    for (index = 1; index <= count; ++index) {
        size_t length = 0u;
        const char *text = luaL_tolstring(L, index, &length);

        output_append(text, (u32)length);
        lua_pop(L, 1);
    }
    return 0;
}

static int dim_log(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);

    host_console("%s", text);
    return 0;
}

static int dim_sleep(lua_State *L) {
    const lua_Integer milliseconds = luaL_checkinteger(L, 1);

    if (milliseconds > 0) {
        host_sleep((u32)((milliseconds > 3600000) ? 3600000 : milliseconds));
    }
    return 0;
}

static int dim_millis(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)time_milliseconds());
    return 1;
}

static int dim_time(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)time((time_t *)0));
    return 1;
}

static int dim_clock(lua_State *L) {
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, clock_year());
    lua_setfield(L, -2, "year");
    lua_pushinteger(L, clock_month());
    lua_setfield(L, -2, "month");
    lua_pushinteger(L, clock_day());
    lua_setfield(L, -2, "day");
    lua_pushinteger(L, clock_hours());
    lua_setfield(L, -2, "hour");
    lua_pushinteger(L, clock_minutes());
    lua_setfield(L, -2, "minute");
    lua_pushinteger(L, clock_seconds());
    lua_setfield(L, -2, "second");
    return 1;
}

static int dim_file_list(lua_State *L) {
    char name[VOLUME_NAME_LENGTH];
    u32 size = 0u;
    u16 slot;
    lua_Integer position = 1;

    lua_newtable(L);
    for (slot = 0u; slot < volume_file_count(); ++slot) {
        if (volume_file(slot, name, &size) == 0u) {
            continue;
        }
        lua_createtable(L, 0, 2);
        lua_pushstring(L, name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)size);
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, position);
        ++position;
    }
    return 1;
}

static int dim_file_read(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    u32 length = 0u;
    u8 *buffer = volume_slurp(name, &length);

    if (buffer == (u8 *)0) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not on the DimOS volume", name);
        return 2;
    }
    lua_pushlstring(L, (const char *)buffer, (size_t)length);
    free(buffer);
    return 1;
}

static int dim_file_write(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    size_t length = 0u;
    const char *data = luaL_checklstring(L, 2, &length);
    u8 ok;

    if (volume_present() == 0u) {
        lua_pushboolean(L, 0);
        return 1;
    }
    ok = volume_write_file(name, data, (u32)length);
    lua_pushboolean(L, ok != 0u);
    return 1;
}

static int dim_file_remove(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);

    lua_pushboolean(L, volume_delete_file(name) != 0u);
    return 1;
}

static int dim_file_rename(lua_State *L) {
    const char *from = luaL_checkstring(L, 1);
    const char *to = luaL_checkstring(L, 2);
    u32 length = 0u;
    u8 *buffer = volume_slurp(from, &length);
    u8 ok;

    if (buffer == (u8 *)0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    ok = volume_write_file(to, buffer, length);
    free(buffer);
    if (ok != 0u) {
        (void)volume_delete_file(from);
    }
    lua_pushboolean(L, ok != 0u);
    return 1;
}

static int dim_info(lua_State *L) {
    u16 index;
    char volume_name[VOLUME_NAME_LENGTH];

    lua_createtable(L, 0, 10);
    lua_pushstring(L, "DimOS 14");
    lua_setfield(L, -2, "kernel");
    lua_pushstring(L, host_lua_version());
    lua_setfield(L, -2, "lua");
    lua_pushstring(L, host_platform_name());
    lua_setfield(L, -2, "platform");

    lua_createtable(L, 0, 2);
    lua_pushinteger(L, (lua_Integer)host_display_width());
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, (lua_Integer)host_display_height());
    lua_setfield(L, -2, "height");
    lua_setfield(L, -2, "screen");

    lua_createtable(L, 0, 6);
    lua_pushstring(L, host_disk_path());
    lua_setfield(L, -2, "path");
    lua_pushinteger(L, (lua_Integer)host_disk_bytes());
    lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, (lua_Integer)volume_free_bytes());
    lua_setfield(L, -2, "free_bytes");
    lua_pushstring(L, ata_model());
    lua_setfield(L, -2, "model");
    lua_pushboolean(L, volume_present() != 0u);
    lua_setfield(L, -2, "mounted");
    lua_pushstring(L, volume_present() != 0u ? "FAT12" : "none");
    lua_setfield(L, -2, "filesystem");
    lua_setfield(L, -2, "disk");

    lua_pushinteger(L, (lua_Integer)gfx_current_theme());
    lua_setfield(L, -2, "theme");
    lua_pushinteger(L, (lua_Integer)gui_brightness());
    lua_setfield(L, -2, "brightness");
    lua_pushinteger(L, (lua_Integer)gui_volume());
    lua_setfield(L, -2, "volume");

    /* Files and applications, so a script can decide what to do without
     * walking the directory itself. */
    lua_newtable(L);
    {
        lua_Integer position = 1;

        for (index = 0u; index < volume_file_count(); ++index) {
            u32 size = 0u;

            if (volume_file(index, volume_name, &size) == 0u) {
                continue;
            }
            lua_createtable(L, 0, 2);
            lua_pushstring(L, volume_name);
            lua_setfield(L, -2, "name");
            lua_pushinteger(L, (lua_Integer)size);
            lua_setfield(L, -2, "size");
            lua_rawseti(L, -2, position);
            ++position;
        }
    }
    lua_setfield(L, -2, "files");

    lua_createtable(L, (int)application_count, 0);
    for (index = 0u; index < application_count; ++index) {
        lua_pushstring(L, application_list[index]->title);
        lua_rawseti(L, -2, (lua_Integer)index + 1);
    }
    lua_setfield(L, -2, "apps");
    return 1;
}

/* Package cache for require(), keyed by the 8.3 name on the volume. */
static void module_cache_push(lua_State *L, const char *name) {
    luaL_getsubtable(L, LUA_REGISTRYINDEX, "dim.modules");
    lua_getfield(L, -1, name);
    lua_remove(L, -2); /* leave the value */
}

static int dim_theme(lua_State *L) {
    if (lua_gettop(L) == 0) {
        lua_pushinteger(L, (lua_Integer)gfx_current_theme());
        return 1;
    }
    {
        const lua_Integer theme = luaL_checkinteger(L, 1);

        if (theme < 0 || theme >= (lua_Integer)THEME_COUNT) {
            return luaL_error(L, "theme must be 0..%d", (int)THEME_COUNT - 1);
        }
        gfx_select_theme((u8)theme);
        (void)settings_save();
    }
    lua_pushinteger(L, (lua_Integer)gfx_current_theme());
    return 1;
}

static int dim_brightness(lua_State *L) {
    if (lua_gettop(L) == 0) {
        lua_pushinteger(L, (lua_Integer)gui_brightness());
        return 1;
    }
    {
        const lua_Integer level = luaL_checkinteger(L, 1);

        if (level < 0 || level > 20) {
            return luaL_error(L, "brightness must be 0..20");
        }
        gui_set_brightness((u8)level);
        (void)settings_save();
    }
    lua_pushinteger(L, (lua_Integer)gui_brightness());
    return 1;
}

static int dim_volume(lua_State *L) {
    if (lua_gettop(L) == 0) {
        lua_pushinteger(L, (lua_Integer)gui_volume());
        return 1;
    }
    {
        const lua_Integer level = luaL_checkinteger(L, 1);

        if (level < 0 || level > 20) {
            return luaL_error(L, "volume must be 0..20");
        }
        gui_set_volume((u8)level);
        (void)settings_save();
    }
    lua_pushinteger(L, (lua_Integer)gui_volume());
    return 1;
}

static int dim_exit(lua_State *L) {
    exit_code = (int)luaL_optinteger(L, 1, 0);
    exit_requested = 1u;
    return luaL_error(L, DIM_EXIT_SENTINEL);
}

/* ------------------------------------------------------------------ */
/* require / dofile, reading .LUA files from the DimOS volume           */
/* ------------------------------------------------------------------ */

static int dim_require(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    u32 length = 0u;
    u8 *buffer;
    char chunk[96];

    module_cache_push(L, name);
    if (!lua_isnil(L, -1)) {
        return 1;
    }
    lua_pop(L, 1);

    buffer = volume_slurp(name, &length);
    if (buffer == (u8 *)0) {
        return luaL_error(L, "module '%s' is not on the DimOS volume", name);
    }
    snprintf(chunk, sizeof(chunk), "@%s", name);
    if (luaL_loadbuffer(L, (const char *)buffer, (size_t)length, chunk) != LUA_OK) {
        free(buffer);
        return lua_error(L);
    }
    free(buffer);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        return lua_error(L);
    }
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    /* Remember it, so a second require() is free and returns the same table. */
    luaL_getsubtable(L, LUA_REGISTRYINDEX, "dim.modules");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    return 1;
}

static int dim_dofile(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    u32 length = 0u;
    u8 *buffer = volume_slurp(name, &length);
    int results;

    if (buffer == (u8 *)0) {
        return luaL_error(L, "%s is not on the DimOS volume", name);
    }
    if (luaL_loadbuffer(L, (const char *)buffer, (size_t)length, name) != LUA_OK) {
        free(buffer);
        return lua_error(L);
    }
    free(buffer);
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        return lua_error(L);
    }
    results = lua_gettop(L);
    return results;
}

/* ------------------------------------------------------------------ */
/* Start-up                                                            */
/* ------------------------------------------------------------------ */

static void open_libraries(lua_State *L) {
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 6);
}

static const luaL_Reg dim_file_functions[] = {
    { "list", dim_file_list },
    { "read", dim_file_read },
    { "write", dim_file_write },
    { "remove", dim_file_remove },
    { "rename", dim_file_rename },
    { (const char *)0, (lua_CFunction)0 }
};

static const luaL_Reg dim_functions[] = {
    { "print", dim_print },
    { "write", dim_write },
    { "log", dim_log },
    { "sleep", dim_sleep },
    { "millis", dim_millis },
    { "time", dim_time },
    { "clock", dim_clock },
    { "info", dim_info },
    { "theme", dim_theme },
    { "brightness", dim_brightness },
    { "volume", dim_volume },
    { "exit", dim_exit },
    { "require", dim_require },
    { (const char *)0, (lua_CFunction)0 }
};

/* Turns an error into "message + Lua traceback" and hands it back; the
 * runner prints it, so a failing application leaves one readable report. */
static int error_handler_text(lua_State *L) {
    const char *message = lua_tostring(L, 1);

    if (message == (const char *)0) {
        lua_pushliteral(L, "(error object is not a string)");
        message = lua_tostring(L, -1);
    }
    luaL_traceback(L, L, message, 1);
    return 1;
}

int host_lua_start(void) {
    if (state != (lua_State *)0) {
        return 0;
    }
    state = luaL_newstate();
    if (state == (lua_State *)0) {
        return -1;
    }
    open_libraries(state);

    /* Console: both `print` and `dim.print` write to the capture buffer. */
    lua_pushcfunction(state, dim_print);
    lua_setglobal(state, "print");

    lua_newtable(state);
        lua_newtable(state);
            luaL_setfuncs(state, dim_file_functions, 0);
        lua_setfield(state, -2, "file");
        lua_pushstring(state, "14 hosted");
        lua_setfield(state, -2, "version");
        lua_pushstring(state, "DimOS 14");
        lua_setfield(state, -2, "kernel");
        lua_pushstring(state, host_platform_name());
        lua_setfield(state, -2, "platform");
        lua_pushinteger(state, (lua_Integer)SCREEN_WIDTH);
        lua_setfield(state, -2, "screen_width");
        lua_pushinteger(state, (lua_Integer)SCREEN_HEIGHT);
        lua_setfield(state, -2, "screen_height");
        luaL_setfuncs(state, dim_functions, 0);
    lua_setglobal(state, "dim");

    /* A miniature `package.loaded` keeps require() idempotent. */
    lua_newtable(state);
        lua_newtable(state);
        lua_setfield(state, -2, "loaded");
        lua_pushstring(state, ";");
        lua_setfield(state, -2, "config");
    lua_setglobal(state, "package");

    lua_pushcfunction(state, dim_require);
    lua_setglobal(state, "require");
    lua_pushcfunction(state, dim_dofile);
    lua_setglobal(state, "dofile");

    /* The host file system stays out of reach, by design. */
    lua_pushnil(state);
    lua_setglobal(state, "loadfile");
    lua_pushnil(state);
    lua_setglobal(state, "io");
    lua_pushnil(state);
    lua_setglobal(state, "os");
    lua_pushnil(state);
    lua_setglobal(state, "debug");

    lua_pushstring(state, "DimOS 14 hosted");
    lua_setglobal(state, "dim_version");
    return 0;
}

void host_lua_stop(void) {
    if (state != (lua_State *)0) {
        lua_close(state);
        state = (lua_State *)0;
    }
}

static int run_loaded_chunk(void) {
    const int results = lua_gettop(state);

    lua_pushcfunction(state, error_handler_text);
    lua_insert(state, 1);
    if (lua_pcall(state, 0, LUA_MULTRET, 1) != LUA_OK) {
        const char *message = lua_tostring(state, -1);

        if (message != (const char *)0 &&
            strstr(message, DIM_EXIT_SENTINEL) != (const char *)0) {
            const int code = exit_requested != 0u ? exit_code : 0;

            lua_settop(state, 0);
            return code;
        }
        if (message != (const char *)0) {
            output_append(message, (u32)strlen(message));
            output_append("\n", 1u);
        }
        lua_settop(state, 0);
        return 1;
    }
    /* Drop the error handler, keep whatever the chunk returned. */
    lua_remove(state, 1);
    (void)results;
    lua_settop(state, 0);
    return 0;
}

int host_lua_run_text(const char *chunk, const char *chunk_name) {
    if (state == (lua_State *)0 && host_lua_start() != 0) {
        return 1;
    }
    exit_requested = 0u;
    exit_code = 0;
    lua_settop(state, 0);
    if (luaL_loadbuffer(state, chunk, strlen(chunk), chunk_name) != LUA_OK) {
        const char *message = lua_tostring(state, -1);

        output_append(message != (const char *)0 ? message : "syntax error",
                      (u32)strlen(message != (const char *)0 ? message
                                                             : "syntax error"));
        output_append("\n", 1u);
        lua_settop(state, 0);
        return 1;
    }
    return run_loaded_chunk();
}

int host_lua_run_file(const char *path, int argument_count, char **arguments) {
    static char source[DIM_CHUNK_BYTES * 16u];
    const char *chunk = (const char *)0;
    u32 length = 0u;
    u8 *buffer = (u8 *)0;
    FILE *handle;
    int status;
    int index;

    if (state == (lua_State *)0 && host_lua_start() != 0) {
        return 1;
    }
    handle = fopen(path, "rb");
    if (handle != (FILE *)0) {
        length = (u32)fread(source, 1u, sizeof(source) - 1u, handle);
        fclose(handle);
        source[length] = '\0';
        chunk = source;
    } else {
        buffer = volume_slurp(path, &length);
        if (buffer == (u8 *)0) {
            const char *prefix = "no such application on the DimOS volume: ";

            output_append(prefix, (u32)strlen(prefix));
            output_append(path, (u32)strlen(path));
            output_append("\n", 1u);
            return 2;
        }
        chunk = (const char *)buffer;
    }

    exit_requested = 0u;
    exit_code = 0;
    lua_settop(state, 0);
    if (luaL_loadbuffer(state, chunk, (size_t)length, path) != LUA_OK) {
        const char *message = lua_tostring(state, -1);

        output_append(message != (const char *)0 ? message : "syntax error",
                      (u32)strlen(message != (const char *)0 ? message
                                                             : "syntax error"));
        output_append("\n", 1u);
        lua_settop(state, 0);
        free(buffer);
        return 1;
    }
    free(buffer);

    /* `arg[0]` is the application, `arg[1..]` its arguments. */
    lua_createtable(state, argument_count, 1);
    lua_pushstring(state, path);
    lua_rawseti(state, -2, 0);
    for (index = 0; index < argument_count; ++index) {
        lua_pushstring(state, arguments[index]);
        lua_rawseti(state, -2, (lua_Integer)index + 1);
    }
    lua_setglobal(state, "arg");

    status = run_loaded_chunk();
    return status;
}
