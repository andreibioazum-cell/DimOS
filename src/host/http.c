/*
 * http.c -- the browser window of the hosted runtime.
 *
 * One binary has to look like a phone on an Android tablet, on a Windows
 * laptop and in a Linux browser. Instead of dragging a GUI toolkit (and a
 * different one per platform) into a freestanding operating system, the
 * runtime serves a small page and pushes it the composited frames:
 *
 *   GET  /            the shell page (canvas, Lua console)
 *   GET  /frame?full= changed 16x16 tiles, binary, see ui.c
 *   GET  /input?...   pointer and keyboard events, turned into PS/2 packets
 *   GET  /refresh     force a complete frame
 *   GET  /info        machine facts as JSON
 *   GET  /lua?list=1  the .LUA applications on the volume
 *   GET  /lua?read=X  the source of one application
 *   POST /lua         run a chunk headlessly, answer with its console output
 *
 * The server is single threaded and non-blocking on purpose: it is pumped
 * from the shell's own 60 Hz wait loop, so nothing here shares kernel state
 * with the GUI, and there are no locks to get wrong. Everything a phone
 * needs -- touch, a scaled canvas, a phone-shaped frame -- is in the page.
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
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET host_socket;
#define HOST_INVALID INVALID_SOCKET
#define host_close_socket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int host_socket;
#define HOST_INVALID (-1)
#define host_close_socket close
typedef ssize_t host_ssize;
#endif

#define MAX_CLIENTS 6
/* Polls a connection may stay silent before it is dropped (about ten
 * seconds at 60 Hz). Chrome likes to keep sockets open for its next
 * request; the runtime would rather reuse the slot. */
#define CLIENT_PATIENCE 600u
#define INPUT_LIMIT (96u * 1024u)
#define REQUEST_LINE 1024u

typedef struct {
    host_socket fd;
    u8 used;
    u8 handled;
    u8 closing;
    u8 *incoming;
    u32 incoming_length;
    u32 incoming_capacity;
    u8 *outgoing;
    u32 outgoing_length;
    u32 outgoing_sent;
    u32 outgoing_capacity;
    u16 age; /* polls this client has been waiting for a request */
} Client;

static host_socket listener = HOST_INVALID;
static u16 bound_port;
static Client clients[MAX_CLIENTS];
static char last_error[160];
static u8 running;

static const char page_html[] =
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
"<meta name=\"theme-color\" content=\"#0b0b0f\">"
"<title>DimOS Anywhere</title><style>"
":root{color-scheme:dark}"
"*{box-sizing:border-box}"
"html,body{margin:0;min-height:100%;background:#0b0b0f;color:#e8e8ee;"
"font:15px/1.4 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif}"
"body{display:flex;flex-direction:column;align-items:center;justify-content:center;"
"gap:14px;padding:16px;min-height:100vh}"
"#frame{width:min(100%,430px);aspect-ratio:390/844;border-radius:26px;overflow:hidden;"
"background:#000;box-shadow:0 24px 60px #000a,0 0 0 1px #ffffff14}"
"canvas{display:block;width:100%;height:100%;touch-action:none}"
"#bar{display:flex;gap:12px;align-items:center;font-size:13px;color:#9a9aa8;"
"flex-wrap:wrap;justify-content:center}"
"button{font:inherit;cursor:pointer;background:#22304d;border:1px solid #33507e;"
"color:#cfe0ff;border-radius:8px;padding:6px 12px}"
"#lua{width:min(100%,430px)}"
"#lua summary{cursor:pointer;color:#9a9aa8;font-size:13px}"
"textarea{width:100%;height:130px;background:#15151c;color:#e8e8ee;"
"border:1px solid #2a2a35;border-radius:10px;padding:10px;"
"font:13px/1.45 ui-monospace,SFMono-Regular,Menlo,monospace;resize:vertical}"
"pre{background:#15151c;border:1px solid #2a2a35;border-radius:10px;padding:10px;"
"max-height:220px;overflow:auto;white-space:pre-wrap;"
"font:13px/1.45 ui-monospace,SFMono-Regular,Menlo,monospace;margin:8px 0 0}"
".row{display:flex;gap:8px;align-items:center;margin:10px 0;flex-wrap:wrap}"
"select{font:inherit;background:#15151c;color:#e8e8ee;border:1px solid #2a2a35;"
"border-radius:8px;padding:6px 8px}"
"</style></head><body>"
"<div id=\"frame\"><canvas id=\"screen\" width=\"390\" height=\"844\"></canvas></div>"
"<div id=\"bar\"><span id=\"state\">connecting to DimOS...</span>"
"<button id=\"refresh\">Redraw</button>"
"<span id=\"about\">DimOS Anywhere</span></div>"
"<details id=\"lua\"><summary>Lua console - run an application without a GUI</summary>"
"<div class=\"row\"><select id=\"apps\"><option value=\"\">(choose an app)</option></select>"
"<button id=\"load\">Load</button><button id=\"run\">Run</button></div>"
"<textarea id=\"code\">local info = dim.info()\n"
"dim.print(\"DimOS \" .. dim.version .. \" on \" .. dim.platform)\n"
"dim.print(\"volume: \" .. info.disk.bytes // 1024 .. \" KiB, free \" .. info.disk.free_bytes // 1024 .. \" KiB\")\n"
"for _, file in ipairs(dim.file.list()) do\n"
"    dim.print(string.format(\"%-12s %6d\", file.name, file.size))\n"
"end</textarea>"
"<pre id=\"out\">Ready. Lua applications have no GUI: they use dim.file, dim.print, "
"dim.sleep, dim.clock and dim.info.</pre></details>"
"<script>"
"const W0=390,H0=844;"
"const cv=document.getElementById('screen');"
"const ctx=cv.getContext('2d',{alpha:false});"
"let img=ctx.createImageData(W0,H0),full=1,busy=0,seen=0,lastSeq=0;"
"document.getElementById('refresh').onclick=()=>{full=1;};"
"function pos(e){const r=cv.getBoundingClientRect();"
"return[Math.round((e.clientX-r.left)*cv.width/r.width),"
"Math.round((e.clientY-r.top)*cv.height/r.height)];}"
"function send(q){fetch('/input?'+q,{cache:'no-store'}).catch(()=>{});}"
"let lastX=0,lastY=0,sent=0;"
"cv.addEventListener('pointerdown',e=>{e.preventDefault();"
"if(cv.setPointerCapture)cv.setPointerCapture(e.pointerId);"
"const[x,y]=pos(e);lastX=x;lastY=y;sent=1;"
"send('action='+(e.button===2?'right':'down')+'&button='+(e.button===2?2:1)+'&x='+x+'&y='+y);});"
"cv.addEventListener('pointermove',e=>{const[x,y]=pos(e);"
"if(x===lastX&&y===lastY)return;lastX=x;lastY=y;send('action=move&x='+x+'&y='+y);});"
"cv.addEventListener('pointerup',e=>{const[x,y]=pos(e);sent=0;"
"send('action=up&button=1&x='+x+'&y='+y);});"
"cv.addEventListener('pointercancel',e=>{send('action=up&button=1&x='+lastX+'&y='+lastY);});"
"cv.addEventListener('contextmenu',e=>e.preventDefault());"
"cv.addEventListener('wheel',e=>{e.preventDefault();const[x,y]=pos(e);"
"send('action=wheel&x='+x+'&y='+y+'&w='+(e.deltaY>0?-1:1));},{passive:false});"
"window.addEventListener('keydown',e=>{const k=e.key;"
"if(k.length===1){send('action=key&k='+encodeURIComponent(k));e.preventDefault();}"
"else if(k==='Enter'||k==='Backspace'||k==='Escape'||k.startsWith('Arrow')){"
"send('action=key&k='+encodeURIComponent(k));e.preventDefault();}});"
"function apply(buf){const dv=new DataView(buf);"
"const seq=dv.getUint32(4,true);"
"if(lastSeq&&seq>lastSeq+1)full=1;lastSeq=seq;"
"const w=dv.getUint16(10,true),h=dv.getUint16(12,true);"
"if(cv.width!==w||cv.height!==h){cv.width=w;cv.height=h;img=ctx.createImageData(w,h);"
"document.getElementById('frame').style.aspectRatio=w+'/'+h;}"
"const n=dv.getUint16(8,true);let o=16;"
"for(let i=0;i<n;i++){const x=dv.getUint16(o,true),y=dv.getUint16(o+2,true),"
"tw=dv.getUint16(o+4,true),th=dv.getUint16(o+6,true);o+=8;"
"for(let r=0;r<th;r++){for(let c=0;c<tw;c++){const p=dv.getUint32(o,true);o+=4;"
"const q=((y+r)*w+(x+c))*4;img.data[q]=(p>>16)&255;img.data[q+1]=(p>>8)&255;"
"img.data[q+2]=p&255;img.data[q+3]=255;}}"
"ctx.putImageData(img,0,0,x,y,tw,th);}"
"if(!seen){seen=1;document.getElementById('state').textContent='connected';}}"
"async function pump(){if(busy)return;busy=1;"
"try{const r=await fetch('/frame?full='+full,{cache:'no-store'});"
"if(r.status===200){const b=await r.arrayBuffer();apply(b);}"
"if(full&&r.status!==204)full=0;"
"document.getElementById('state').textContent='connected';}"
"catch(err){document.getElementById('state').textContent='offline - restart the runtime';}"
"busy=0;}"
"setInterval(pump,33);"
"async function refreshApps(){try{const r=await fetch('/lua?list=1');"
"const text=await r.text();const sel=document.getElementById('apps');"
"for(const name of text.split('\\n')){if(!name.trim())continue;"
"const o=document.createElement('option');o.value=name.trim();o.textContent=name.trim();"
"sel.appendChild(o);}}catch(err){}}"
"document.getElementById('load').onclick=async()=>{const name=document.getElementById('apps').value;"
"if(!name)return;const r=await fetch('/lua?read='+encodeURIComponent(name));"
"document.getElementById('code').value=await r.text();};"
"document.getElementById('run').onclick=async()=>{const code=document.getElementById('code').value;"
"const out=document.getElementById('out');out.textContent='running...';"
"try{const r=await fetch('/lua',{method:'POST',body:code});out.textContent=await r.text();}"
"catch(err){out.textContent='the runtime is not answering';}};"
"refreshApps();pump();"
"</script></body></html>";

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void set_error(const char *text) {
    snprintf(last_error, sizeof(last_error), "%s", text);
}

const char *host_http_last_error(void) {
    return last_error;
}

u16 host_http_port(void) {
    return bound_port;
}

static u8 socket_set_nonblocking(host_socket fd) {
#ifdef _WIN32
    u_long mode = 1u;

    return (u8)(ioctlsocket(fd, FIONBIO, &mode) == 0);
#else
    const int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return 0u;
    }
    return (u8)(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
#endif
}

static int socket_would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static void client_release(Client *client) {
    if (client->used == 0u) {
        return;
    }
    if (client->fd != HOST_INVALID) {
        host_close_socket(client->fd);
    }
    free(client->incoming);
    free(client->outgoing);
    memory_zero(client, sizeof(*client));
    client->fd = HOST_INVALID;
}

static u8 client_reserve_incoming(Client *client, u32 extra) {
    if (client->incoming_length + extra <= client->incoming_capacity) {
        return 1u;
    }
    {
        u32 wanted = client->incoming_capacity == 0u ? 8192u
                                                     : client->incoming_capacity;
        u8 *bigger;

        while (wanted < client->incoming_length + extra) {
            wanted *= 2u;
        }
        if (wanted > INPUT_LIMIT) {
            wanted = INPUT_LIMIT;
        }
        if (client->incoming_length + extra > wanted) {
            return 0u;
        }
        bigger = (u8 *)realloc(client->incoming, wanted);
        if (bigger == (u8 *)0) {
            return 0u;
        }
        client->incoming = bigger;
        client->incoming_capacity = wanted;
    }
    return 1u;
}

static u8 client_respond(Client *client, const char *status,
                         const char *content_type, const u8 *body,
                         u32 length) {
    char header[320];
    int header_length;
    u32 total;

    header_length = snprintf(header, sizeof(header),
                             "HTTP/1.1 %s\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %u\r\n"
                             "Cache-Control: no-store\r\n"
                             "Access-Control-Allow-Origin: *\r\n"
                             "Connection: close\r\n\r\n",
                             status, content_type, (unsigned)length);
    if (header_length <= 0) {
        return 0u;
    }
    total = (u32)header_length + length;
    client->outgoing = (u8 *)malloc(total);
    if (client->outgoing == (u8 *)0) {
        return 0u;
    }
    memcpy(client->outgoing, header, (size_t)header_length);
    if (length != 0u && body != (const u8 *)0) {
        memcpy(client->outgoing + header_length, body, length);
    }
    client->outgoing_length = total;
    client->outgoing_sent = 0u;
    client->closing = 1u;
    client->handled = 1u;
    return 1u;
}

static u8 client_respond_text(Client *client, const char *status,
                              const char *content_type, const char *text) {
    return client_respond(client, status, content_type, (const u8 *)text,
                          (u32)strlen(text));
}

/* Copies the value of `name` from a query string. Returns 1 when found. */
static u8 query_value(const char *path, const char *name, char *out,
                      u32 capacity) {
    const char *query = strchr(path, '?');
    const u32 name_length = (u32)strlen(name);

    if (query == (const char *)0 || capacity == 0u) {
        return 0u;
    }
    ++query;
    while (*query != '\0') {
        const char *end = query;

        while (*end != '\0' && *end != '&') {
            ++end;
        }
        if ((u32)(end - query) > name_length && query[name_length] == '=' &&
            memcmp(query, name, name_length) == 0) {
            const char *value = query + name_length + 1u;
            u32 length = (u32)(end - value);
            u32 index;
            u32 written = 0u;

            for (index = 0u; index < length && written + 1u < capacity;
                 ++index) {
                char character = value[index];

                if (character == '%' && index + 2u < length) {
                    u8 high;
                    u8 low;
                    const char *digits = "0123456789ABCDEF";
                    const char *first = strchr(digits, (int)(u8)
                                                character_to_upper(
                                                    (u8)value[index + 1u]));
                    const char *second = strchr(digits, (int)(u8)
                                                 character_to_upper(
                                                     (u8)value[index + 2u]));

                    if (first == (const char *)0 ||
                        second == (const char *)0) {
                        continue;
                    }
                    high = (u8)(first - digits);
                    low = (u8)(second - digits);
                    out[written++] = (char)((high << 4u) | low);
                    index += 2u;
                } else if (character == '+') {
                    out[written++] = ' ';
                } else {
                    out[written++] = character;
                }
            }
            out[written] = '\0';
            return 1u;
        }
        if (*end == '\0') {
            break;
        }
        query = end + 1;
    }
    return 0u;
}

static long query_number(const char *path, const char *name, long fallback) {
    char text[24];

    if (query_value(path, name, text, sizeof(text)) == 0u) {
        return fallback;
    }
    return strtol(text, (char **)0, 10);
}

/* ------------------------------------------------------------------ */
/* Routes                                                              */
/* ------------------------------------------------------------------ */

static void serve_info(Client *client) {
    char body[900];
    char apps[600];
    u16 slot;
    u16 written = 0u;

    host_lua_list_apps(apps, sizeof(apps));
    body[0] = '\0';
    snprintf(body, sizeof(body),
             "{\"runtime\":\"DimOS Anywhere\","
             "\"kernel\":\"DimOS 14\","
             "\"lua\":\"%s\","
             "\"platform\":\"%s\","
             "\"screen\":{\"width\":%u,\"height\":%u},"
             "\"disk\":{\"path\":\"%s\",\"bytes\":%u,\"free_bytes\":%u,"
             "\"model\":\"%s\",\"mounted\":%s},"
             "\"files\":%u,\"lua_apps\":[",
             host_lua_version(), host_platform_name(),
             (unsigned)host_display_width(), (unsigned)host_display_height(),
             host_disk_path(), (unsigned)host_disk_bytes(),
             (unsigned)volume_free_bytes(), ata_model(),
             volume_present() != 0u ? "true" : "false",
             (unsigned)volume_file_count());
    written = (u16)strlen(body);
    /* The application list is already newline separated; turn it into a
     * JSON array without dragging a parser in. */
    {
        const char *cursor = apps;
        u8 first = 1u;

        while (*cursor != '\0' && written + 40u < sizeof(body)) {
            const char *end = strchr(cursor, '\n');
            const u32 length = (end == (const char *)0)
                                   ? (u32)strlen(cursor)
                                   : (u32)(end - cursor);

            if (length != 0u) {
                written = (u16)(written + (u16)snprintf(body + written,
                                                        sizeof(body) - written,
                                                        "%s\"%.*s\"",
                                                        first != 0u ? "" : ",",
                                                        (int)length, cursor));
                first = 0u;
            }
            if (end == (const char *)0) {
                break;
            }
            cursor = end + 1;
        }
    }
    snprintf(body + written, sizeof(body) - written, "]}\n");
    (void)slot;
    client_respond_text(client, "200 OK", "application/json", body);
}

static void serve_input(const char *path) {
    char action[16];
    const long x = query_number(path, "x", -1);
    const long y = query_number(path, "y", -1);
    const long button = query_number(path, "button", 1);

    if (query_value(path, "action", action, sizeof(action)) == 0u) {
        return;
    }
    if (text_equal(action, "key") != 0u) {
        char text[8];

        if (query_value(path, "k", text, sizeof(text)) != 0u && text[0] != '\0') {
            if (text[1] == '\0') {
                host_input_key((u16)(u8)text[0]);
            } else if (text_equal_ignore_case(text, "Enter") != 0u) {
                host_input_key(KEY_ENTER);
            } else if (text_equal_ignore_case(text, "Backspace") != 0u) {
                host_input_key(KEY_BACKSPACE);
            } else if (text_equal_ignore_case(text, "Escape") != 0u) {
                host_input_key(KEY_ESCAPE);
            } else if (text_equal_ignore_case(text, "ArrowUp") != 0u) {
                host_input_key(KEY_ARROW_UP);
            } else if (text_equal_ignore_case(text, "ArrowDown") != 0u) {
                host_input_key(KEY_ARROW_DOWN);
            } else if (text_equal_ignore_case(text, "ArrowLeft") != 0u) {
                host_input_key(KEY_ARROW_LEFT);
            } else if (text_equal_ignore_case(text, "ArrowRight") != 0u) {
                host_input_key(KEY_ARROW_RIGHT);
            }
        }
        return;
    }
    if (x < 0 || y < 0) {
        return;
    }
    if (text_equal(action, "move") != 0u) {
        host_input_move((s16)x, (s16)y);
    } else if (text_equal(action, "down") != 0u) {
        host_input_set_buttons((s16)x, (s16)y, (u8)button);
    } else if (text_equal(action, "up") != 0u) {
        host_input_set_buttons((s16)x, (s16)y, 0u);
    } else if (text_equal(action, "right") != 0u) {
        host_input_click((s16)x, (s16)y, 2u);
    } else if (text_equal(action, "wheel") != 0u) {
        host_input_wheel((s16)x, (s16)y, (s8)query_number(path, "w", 1));
    }
}

static void serve_lua_read(Client *client, const char *name) {
    static char source[16384];
    const u32 length = volume_read_file(name, source, sizeof(source) - 1u);

    if (length == 0u) {
        client_respond_text(client, "404 Not Found", "text/plain",
                            "no such application on the volume\n");
        return;
    }
    source[length] = '\0';
    client_respond(client, "200 OK", "text/plain; charset=utf-8",
                   (const u8 *)source, length);
}

static void serve_lua_run(Client *client, const char *script, u32 length) {
    char *copy = (char *)malloc(length + 1u);

    if (copy == (char *)0) {
        client_respond_text(client, "500 Internal Server Error", "text/plain",
                            "out of memory\n");
        return;
    }
    memcpy(copy, script, length);
    copy[length] = '\0';
    host_lua_output_clear();
    (void)host_lua_run_text(copy, "=console");
    free(copy);
    client_respond_text(client, "200 OK", "text/plain; charset=utf-8",
                        host_lua_output());
}

/* Returns 1 when the request was complete and answered. */
static u8 request_handle(Client *client) {
    const char *data = (const char *)client->incoming;
    const char *end = strstr(data, "\r\n\r\n");
    char method[8];
    char *path;
    char *line_end;
    u32 content_length = 0u;
    const char *length_header;

    if (end == (const char *)0) {
        return 0u;
    }
    line_end = strstr(data, "\r\n");
    if (line_end == (char *)0 || (u32)(line_end - data) > REQUEST_LINE) {
        client_respond_text(client, "400 Bad Request", "text/plain",
                            "bad request\n");
        return 1u;
    }
    method[0] = '\0';
    path = (char *)0;
    {
        const char *first_space = strchr(data, ' ');
        const char *second_space = (first_space == (const char *)0)
                                       ? (const char *)0
                                       : strchr(first_space + 1, ' ');
        u32 method_length;

        if (first_space == (const char *)0 || second_space == (const char *)0) {
            client_respond_text(client, "400 Bad Request", "text/plain",
                                "bad request line\n");
            return 1u;
        }
        method_length = (u32)(first_space - data);
        if (method_length >= sizeof(method)) {
            method_length = sizeof(method) - 1u;
        }
        memcpy(method, data, method_length);
        method[method_length] = '\0';
        path = (char *)malloc((size_t)(second_space - first_space));
        if (path == (char *)0) {
            return 0u;
        }
        memcpy(path, first_space + 1, (size_t)(second_space - first_space - 1));
        path[second_space - first_space - 1] = '\0';
    }

    length_header = strstr(data, "Content-Length:");
    if (length_header == (const char *)0) {
        length_header = strstr(data, "content-length:");
    }
    if (length_header != (const char *)0) {
        content_length = (u32)strtoul(length_header + 15, (char **)0, 10);
    }
    if ((u32)(end + 4 - data) + content_length > client->incoming_length) {
        free(path);
        return 0u; /* body still on its way */
    }

    if (text_equal(method, "GET") != 0u) {
        if (strncmp(path, "/frame", 6) == 0) {
            const u8 full = (u8)(query_number(path, "full", 0) != 0);
            const u8 *body = (const u8 *)0;
            u32 length = 0u;

            if (host_frame_payload(full, &body, &length) != 0u) {
                client_respond(client, "200 OK",
                               "application/octet-stream", body, length);
            } else {
                client_respond(client, "204 No Content", "text/plain",
                               (const u8 *)0, 0u);
            }
        } else if (strncmp(path, "/input", 6) == 0) {
            serve_input(path);
            client_respond(client, "204 No Content", "text/plain",
                           (const u8 *)0, 0u);
        } else if (strncmp(path, "/refresh", 8) == 0) {
            host_frame_forget();
            client_respond_text(client, "200 OK", "text/plain", "ok\n");
        } else if (strncmp(path, "/info", 5) == 0) {
            serve_info(client);
        } else if (strncmp(path, "/lua", 4) == 0) {
            char name[32];

            if (query_value(path, "read", name, sizeof(name)) != 0u) {
                serve_lua_read(client, name);
            } else {
                char list[600];

                host_lua_list_apps(list, sizeof(list));
                client_respond_text(client, "200 OK",
                                    "text/plain; charset=utf-8", list);
            }
        } else {
            client_respond(client, "200 OK", "text/html; charset=utf-8",
                           (const u8 *)page_html,
                           (u32)(sizeof(page_html) - 1u));
        }
    } else if (text_equal(method, "POST") != 0u &&
               strncmp(path, "/lua", 4) == 0) {
        const char *body = end + 4;

        serve_lua_run(client, body, content_length);
    } else {
        client_respond_text(client, "405 Method Not Allowed", "text/plain",
                            "only GET and POST /lua are supported\n");
    }

    free(path);
    return 1u;
}

/* ------------------------------------------------------------------ */
/* Sockets                                                             */
/* ------------------------------------------------------------------ */

int host_http_start(const char *bind_address, u16 port) {
    struct sockaddr_in address;
    u32 index;

    if (running != 0u) {
        return 1;
    }
    for (index = 0u; index < MAX_CLIENTS; ++index) {
        clients[index].fd = HOST_INVALID;
    }
#ifdef _WIN32
    {
        WSADATA data;

        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            set_error("WSAStartup failed");
            return 0;
        }
    }
#endif
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == HOST_INVALID) {
        set_error("cannot create a listening socket");
        return 0;
    }
    {
        int reuse = 1;

        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse,
                   sizeof(reuse));
    }
    memory_zero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = INADDR_ANY;
    if (bind_address != (const char *)0) {
        struct in_addr parsed;

#ifdef _WIN32
        parsed.s_addr = inet_addr(bind_address);
#else
        if (inet_pton(AF_INET, bind_address, &parsed) != 1) {
            parsed.s_addr = INADDR_ANY;
        }
#endif
        address.sin_addr = parsed;
    }
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0) {
        set_error("cannot bind the web port (is another runtime running?)");
        host_close_socket(listener);
        listener = HOST_INVALID;
        return 0;
    }
    if (listen(listener, 8) != 0) {
        set_error("listen() failed");
        host_close_socket(listener);
        listener = HOST_INVALID;
        return 0;
    }
    {
        struct sockaddr_in actual;
#ifdef _WIN32
        int length = sizeof(actual);
#else
        socklen_t length = (socklen_t)sizeof(actual);
#endif

        memory_zero(&actual, sizeof(actual));
        if (getsockname(listener, (struct sockaddr *)&actual, &length) == 0) {
            bound_port = ntohs(actual.sin_port);
        } else {
            bound_port = port;
        }
    }
    socket_set_nonblocking(listener);
    running = 1u;
    last_error[0] = '\0';
    return 1;
}

static void client_accept(void) {
    for (;;) {
#ifdef _WIN32
        host_socket fd = accept(listener, (struct sockaddr *)0, (int *)0);
#else
        host_socket fd = accept(listener, (struct sockaddr *)0,
                                (socklen_t *)0);
#endif
        u32 index;

        if (fd == HOST_INVALID) {
            return;
        }
        socket_set_nonblocking(fd);
        for (index = 0u; index < MAX_CLIENTS; ++index) {
            if (clients[index].used == 0u) {
                memory_zero(&clients[index], sizeof(clients[index]));
                clients[index].used = 1u;
                clients[index].fd = fd;
                break;
            }
        }
        if (index == MAX_CLIENTS) {
            host_close_socket(fd);
        }
    }
}

static void client_read(Client *client) {
    for (;;) {
        host_ssize got;

        if (client_reserve_incoming(client, 4096u) == 0u) {
            client->closing = 1u;
            client->handled = 1u;
            return;
        }
#ifdef _WIN32
        got = recv(client->fd, (char *)client->incoming +
                               client->incoming_length, 4096, 0);
#else
        got = recv(client->fd, client->incoming + client->incoming_length,
                   4096u, 0);
#endif
        if (got > 0) {
            client->incoming_length += (u32)got;
            continue;
        }
        if (got == 0) {
            client->closing = 1u;
            return;
        }
        if (socket_would_block() != 0) {
            return;
        }
        client->closing = 1u;
        client->handled = 1u;
        return;
    }
}

static void client_write(Client *client) {
    while (client->outgoing_sent < client->outgoing_length) {
        host_ssize sent;

#ifdef _WIN32
        sent = send(client->fd, (const char *)client->outgoing +
                                 client->outgoing_sent,
                    (int)(client->outgoing_length - client->outgoing_sent), 0);
#else
        sent = send(client->fd, client->outgoing + client->outgoing_sent,
                    client->outgoing_length - client->outgoing_sent, 0);
#endif
        if (sent > 0) {
            client->outgoing_sent += (u32)sent;
            continue;
        }
        if (sent < 0 && socket_would_block() != 0) {
            return;
        }
        if (sent < 0) {
            client->outgoing_sent = client->outgoing_length;
        }
        break;
    }
}

void host_http_poll(void) {
    u32 index;

    if (running == 0u || listener == HOST_INVALID) {
        return;
    }
    client_accept();
    for (index = 0u; index < MAX_CLIENTS; ++index) {
        Client *client = &clients[index];

        if (client->used == 0u) {
            continue;
        }
        if (client->handled == 0u) {
            client_read(client);
            if (client->incoming_length != 0u &&
                request_handle(client) == 0u) {
                /* Waiting for the rest of the request; a client that never
                 * finishes it (a browser keeping a socket for later) must
                 * not hold one of the six slots forever. */
                if (++client->age > CLIENT_PATIENCE) {
                    client_release(client);
                }
                continue;
            }
            if (client->handled == 0u) {
                client_release(client); /* half-open: the peer went away */
                continue;
            }
        }
        client_write(client);
        if (client->outgoing_sent >= client->outgoing_length) {
            client_release(client);
        }
    }
}

void host_http_stop(void) {
    u32 index;

    for (index = 0u; index < MAX_CLIENTS; ++index) {
        client_release(&clients[index]);
    }
    if (listener != HOST_INVALID) {
        host_close_socket(listener);
        listener = HOST_INVALID;
    }
#ifdef _WIN32
    if (running != 0u) {
        WSACleanup();
    }
#endif
    running = 0u;
}
