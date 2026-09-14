/*
 * tools/vkviewer/vkraster.c -- the DimOS rasterizer, on the GPU.
 *
 * The viewer in vkviewer.c took a finished picture from the kernel and
 * put it on screen. This program does not: the kernel it runs has no
 * rasterizer at all (src/kernel/gfx.c is replaced by
 * tools/vkviewer/gfx_record.c), so what arrives here is a list of
 * drawing commands and nothing else. Every pixel of the 320x200 frame is
 * decided by raster.comp, a compute shader, one invocation per pixel.
 *
 * Two Vulkan passes per frame:
 *
 *   1. compute -- raster.comp reads the command list, the font atlas,
 *      the icon atlas and the palette, and writes an R8_UINT image of
 *      palette indices. This is the work gfx.c used to do on the CPU.
 *   2. graphics -- the same dimos.frag as before turns those indices
 *      into colour and adds the CRT treatment.
 *
 * The command list, atlases and palette are read out of the emulated
 * machine by capture_commands.py and passed in as files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "dimos_commands.h"

#define SOURCE_WIDTH 320u
#define SOURCE_HEIGHT 200u
#define SOURCE_PIXELS (SOURCE_WIDTH * SOURCE_HEIGHT)
#define PALETTE_ENTRIES 256u
#define GLYPH_BYTES (128u * 64u)
#define ART_BYTES (ART_SLOTS * ART_SLOT_CELLS)

/* Mirrors the push constant block in raster.comp. */
typedef struct {
    unsigned int command_count;
    unsigned int font_mode;
    unsigned int blit_used;
    unsigned int spare;
} RasterSettings;

/* Mirrors the push constant block in dimos.frag. */
typedef struct {
    float source_size[2];
    float target_size[2];
    float scanline;
    float mask;
    float curvature;
    float glow;
} DisplaySettings;

/* The packed form raster.comp reads: six uints per command. */
typedef struct {
    unsigned int kind_and_x;
    unsigned int y_and_width;
    unsigned int height_colors;
    unsigned int a0_a1;
    unsigned int reference;
    unsigned int spare;
} PackedCommand;

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    VkCommandPool command_pool;
    VkPhysicalDeviceMemoryProperties memory_properties;
} Context;

static void die(const char *what) {
    fprintf(stderr, "vkraster: %s\n", what);
    exit(1);
}

static void check(VkResult result, const char *what) {
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkraster: %s failed (VkResult %d)\n", what, (int)result);
        exit(1);
    }
}

static void *read_file(const char *path, size_t *size_out) {
    FILE *handle = fopen(path, "rb");
    void *data;
    long size;

    if (handle == NULL) {
        fprintf(stderr, "vkraster: cannot open %s\n", path);
        exit(1);
    }
    fseek(handle, 0, SEEK_END);
    size = ftell(handle);
    fseek(handle, 0, SEEK_SET);
    if (size < 0) {
        die("bad input file");
    }
    data = malloc((size_t)size + 1u);
    if (data == NULL) {
        die("out of memory");
    }
    if (size > 0 && fread(data, 1, (size_t)size, handle) != (size_t)size) {
        die("short read");
    }
    fclose(handle);
    if (size_out != NULL) {
        *size_out = (size_t)size;
    }
    return data;
}

/* ------------------------------------------------------------------ */
/* PNG output                                                          */
/* ------------------------------------------------------------------ */

static unsigned long crc_table[256];
static int crc_table_ready;

static unsigned long crc32_update(unsigned long crc, const unsigned char *data,
                                  size_t length) {
    size_t index;

    if (!crc_table_ready) {
        unsigned long n;
        for (n = 0; n < 256; ++n) {
            unsigned long c = n;
            int k;
            for (k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
            }
            crc_table[n] = c;
        }
        crc_table_ready = 1;
    }
    for (index = 0; index < length; ++index) {
        crc = crc_table[(crc ^ data[index]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static void put_be32(unsigned char *out, unsigned long value) {
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static void png_chunk(FILE *out, const char *tag, const unsigned char *data,
                      size_t length) {
    unsigned char header[8];
    unsigned char trailer[4];
    unsigned long crc;

    put_be32(header, (unsigned long)length);
    memcpy(header + 4, tag, 4);
    fwrite(header, 1, 8, out);
    fwrite(data, 1, length, out);
    crc = crc32_update(0xFFFFFFFFUL, header + 4, 4);
    crc = crc32_update(crc, data, length) ^ 0xFFFFFFFFUL;
    put_be32(trailer, crc);
    fwrite(trailer, 1, 4, out);
}

static void write_png(const char *path, const unsigned char *rgba, uint32_t width,
                      uint32_t height, uint32_t row_pitch) {
    FILE *out = fopen(path, "wb");
    static const unsigned char signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    unsigned char ihdr[13];
    unsigned char *raw;
    unsigned char *zlib;
    size_t raw_size = (size_t)height * (1u + (size_t)width * 3u);
    size_t zlib_size;
    size_t offset = 0;
    size_t position = 0;
    unsigned long adler_a = 1, adler_b = 0;
    uint32_t y, x;

    if (out == NULL) {
        die("cannot open the output file");
    }
    raw = malloc(raw_size);
    if (raw == NULL) {
        die("out of memory");
    }
    for (y = 0; y < height; ++y) {
        raw[offset++] = 0;
        for (x = 0; x < width; ++x) {
            const unsigned char *pixel = rgba + (size_t)y * row_pitch + (size_t)x * 4u;
            raw[offset++] = pixel[0];
            raw[offset++] = pixel[1];
            raw[offset++] = pixel[2];
        }
    }
    for (offset = 0; offset < raw_size; ++offset) {
        adler_a = (adler_a + raw[offset]) % 65521UL;
        adler_b = (adler_b + adler_a) % 65521UL;
    }
    zlib_size = 2 + raw_size + 5 * ((raw_size + 65534) / 65535) + 4;
    zlib = malloc(zlib_size);
    if (zlib == NULL) {
        die("out of memory");
    }
    zlib[position++] = 0x78;
    zlib[position++] = 0x01;
    offset = 0;
    while (offset < raw_size) {
        size_t block = raw_size - offset;
        int last;

        if (block > 65535) {
            block = 65535;
        }
        last = (offset + block == raw_size) ? 1 : 0;
        zlib[position++] = (unsigned char)last;
        zlib[position++] = (unsigned char)(block & 0xFF);
        zlib[position++] = (unsigned char)(block >> 8);
        zlib[position++] = (unsigned char)(~block & 0xFF);
        zlib[position++] = (unsigned char)((~block >> 8) & 0xFF);
        memcpy(zlib + position, raw + offset, block);
        position += block;
        offset += block;
    }
    put_be32(zlib + position, (adler_b << 16) | adler_a);
    position += 4;

    put_be32(ihdr, width);
    put_be32(ihdr + 4, height);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    fwrite(signature, 1, 8, out);
    png_chunk(out, "IHDR", ihdr, sizeof(ihdr));
    png_chunk(out, "IDAT", zlib, position);
    png_chunk(out, "IEND", (const unsigned char *)"", 0);
    fclose(out);
    free(raw);
    free(zlib);
}

/* ------------------------------------------------------------------ */
/* Vulkan helpers                                                      */
/* ------------------------------------------------------------------ */

static uint32_t find_memory(const Context *context, uint32_t type_bits,
                            VkMemoryPropertyFlags wanted) {
    uint32_t index;

    for (index = 0; index < context->memory_properties.memoryTypeCount; ++index) {
        const VkMemoryType *type = &context->memory_properties.memoryTypes[index];

        if ((type_bits & (1u << index)) != 0 &&
            (type->propertyFlags & wanted) == wanted) {
            return index;
        }
    }
    die("no suitable memory type");
    return 0;
}

static void create_buffer(const Context *context, VkDeviceSize size,
                          VkBufferUsageFlags usage, VkMemoryPropertyFlags wanted,
                          VkBuffer *buffer, VkDeviceMemory *memory) {
    VkBufferCreateInfo info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation;

    if (size == 0) {
        size = 4;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(context->device, &info, NULL, buffer), "vkCreateBuffer");

    vkGetBufferMemoryRequirements(context->device, *buffer, &requirements);
    memset(&allocation, 0, sizeof(allocation));
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = find_memory(context, requirements.memoryTypeBits, wanted);
    check(vkAllocateMemory(context->device, &allocation, NULL, memory), "vkAllocateMemory");
    check(vkBindBufferMemory(context->device, *buffer, *memory, 0), "vkBindBufferMemory");
}

/* A host visible buffer filled from `data`, zero padded to `size`. */
static void create_filled_buffer(const Context *context, const void *data,
                                 size_t data_size, VkDeviceSize size,
                                 VkBufferUsageFlags usage, VkBuffer *buffer,
                                 VkDeviceMemory *memory) {
    void *mapped;

    create_buffer(context, size, usage,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  buffer, memory);
    check(vkMapMemory(context->device, *memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    memset(mapped, 0, (size_t)size);
    if (data != NULL && data_size > 0) {
        memcpy(mapped, data, data_size < (size_t)size ? data_size : (size_t)size);
    }
    vkUnmapMemory(context->device, *memory);
}

static VkShaderModule load_shader(VkDevice device, const char *path) {
    size_t size = 0;
    uint32_t *code = read_file(path, &size);
    VkShaderModuleCreateInfo info;
    VkShaderModule module;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = code;
    check(vkCreateShaderModule(device, &info, NULL, &module), "vkCreateShaderModule");
    free(code);
    return module;
}

static VkCommandBuffer begin_commands(const Context *context) {
    VkCommandBufferAllocateInfo allocation;
    VkCommandBufferBeginInfo begin;
    VkCommandBuffer commands;

    memset(&allocation, 0, sizeof(allocation));
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(context->device, &allocation, &commands),
          "vkAllocateCommandBuffers");

    memset(&begin, 0, sizeof(begin));
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
    return commands;
}

static void end_commands(const Context *context, VkCommandBuffer commands) {
    VkSubmitInfo submit;

    check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    check(vkQueueSubmit(context->queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    check(vkQueueWaitIdle(context->queue), "vkQueueWaitIdle");
    vkFreeCommandBuffers(context->device, context->command_pool, 1, &commands);
}

static void transition(VkCommandBuffer commands, VkImage image, VkImageLayout from,
                       VkImageLayout to, VkAccessFlags source_access,
                       VkAccessFlags target_access, VkPipelineStageFlags source_stage,
                       VkPipelineStageFlags target_stage) {
    VkImageMemoryBarrier barrier;

    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = target_access;
    vkCmdPipelineBarrier(commands, source_stage, target_stage, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
}

int main(int argc, char **argv) {
    const char *command_path = NULL;
    const char *palette_path = NULL;
    const char *glyph_path = NULL;
    const char *art_path = NULL;
    const char *blit_path = NULL;
    const char *base_path = NULL;
    const char *output_path = "dimos-raster.png";
    const char *shader_directory = "tools/vkviewer/shaders";
    const char *indices_path = NULL;
    uint32_t scale = 4;
    uint32_t target_width = 0;
    uint32_t target_height = 0;
    DisplaySettings display;
    RasterSettings raster;
    Context context;
    int argument;
    int list_devices = 0;

    char raster_path[1024];
    char vertex_path[1024];
    char fragment_path[1024];

    unsigned char *command_bytes = NULL;
    unsigned char *palette_bytes = NULL;
    unsigned char *glyph_bytes = NULL;
    unsigned char *art_bytes = NULL;
    unsigned char *blit_bytes = NULL;
    unsigned char *base_bytes = NULL;
    size_t command_size = 0, palette_size = 0, glyph_size = 0, art_size = 0,
           blit_size = 0, base_size = 0;

    display.scanline = 0.45f;
    display.mask = 0.22f;
    display.curvature = 0.10f;
    display.glow = 0.14f;

    for (argument = 1; argument < argc; ++argument) {
        const char *option = argv[argument];
        const char *value = (argument + 1 < argc) ? argv[argument + 1] : NULL;

        if (strcmp(option, "--list-devices") == 0) {
            list_devices = 1;
            continue;
        }
        if (value == NULL) {
            fprintf(stderr, "vkraster: %s needs a value\n", option);
            return 1;
        }
        ++argument;
        if (strcmp(option, "--commands") == 0) { command_path = value; }
        else if (strcmp(option, "--palette") == 0) { palette_path = value; }
        else if (strcmp(option, "--glyphs") == 0) { glyph_path = value; }
        else if (strcmp(option, "--art") == 0) { art_path = value; }
        else if (strcmp(option, "--blit") == 0) { blit_path = value; }
        else if (strcmp(option, "--base-palette") == 0) { base_path = value; }
        else if (strcmp(option, "--out") == 0) { output_path = value; }
        else if (strcmp(option, "--dump-indices") == 0) { indices_path = value; }
        else if (strcmp(option, "--shaders") == 0) { shader_directory = value; }
        else if (strcmp(option, "--scale") == 0) { scale = (uint32_t)atoi(value); }
        else if (strcmp(option, "--width") == 0) { target_width = (uint32_t)atoi(value); }
        else if (strcmp(option, "--height") == 0) { target_height = (uint32_t)atoi(value); }
        else if (strcmp(option, "--scanline") == 0) { display.scanline = (float)atof(value); }
        else if (strcmp(option, "--mask") == 0) { display.mask = (float)atof(value); }
        else if (strcmp(option, "--curvature") == 0) { display.curvature = (float)atof(value); }
        else if (strcmp(option, "--glow") == 0) { display.glow = (float)atof(value); }
        else {
            fprintf(stderr, "vkraster: unknown option %s\n", option);
            return 1;
        }
    }

    if (scale == 0) { scale = 1; }
    if (target_width == 0) { target_width = SOURCE_WIDTH * scale; }
    if (target_height == 0) { target_height = SOURCE_HEIGHT * scale; }
    snprintf(raster_path, sizeof(raster_path), "%s/raster.comp.spv", shader_directory);
    snprintf(vertex_path, sizeof(vertex_path), "%s/dimos.vert.spv", shader_directory);
    snprintf(fragment_path, sizeof(fragment_path), "%s/dimos.frag.spv", shader_directory);

    memset(&context, 0, sizeof(context));

    {
        VkApplicationInfo application;
        VkInstanceCreateInfo info;

        memset(&application, 0, sizeof(application));
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "DimOS rasterizer";
        application.apiVersion = VK_API_VERSION_1_0;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &application;
        check(vkCreateInstance(&info, NULL, &context.instance), "vkCreateInstance");
    }

    {
        VkPhysicalDevice devices[16];
        uint32_t count = 16;
        uint32_t index;
        uint32_t chosen = 0;
        VkPhysicalDeviceProperties properties;

        check(vkEnumeratePhysicalDevices(context.instance, &count, devices),
              "vkEnumeratePhysicalDevices");
        if (count == 0) {
            die("no Vulkan device found");
        }
        for (index = 0; index < count; ++index) {
            vkGetPhysicalDeviceProperties(devices[index], &properties);
            if (list_devices) {
                printf("device %u: %s\n", index, properties.deviceName);
            }
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                chosen = index;
            }
        }
        if (list_devices) {
            vkDestroyInstance(context.instance, NULL);
            return 0;
        }
        context.physical_device = devices[chosen];
        vkGetPhysicalDeviceProperties(context.physical_device, &properties);
        printf("vkraster: rasterizing on %s\n", properties.deviceName);
        vkGetPhysicalDeviceMemoryProperties(context.physical_device,
                                            &context.memory_properties);
    }

    {
        VkQueueFamilyProperties families[16];
        uint32_t count = 16;
        uint32_t index;
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info;
        VkDeviceCreateInfo device_info;

        vkGetPhysicalDeviceQueueFamilyProperties(context.physical_device, &count, families);
        context.queue_family = 0xFFFFFFFFu;
        for (index = 0; index < count; ++index) {
            const VkQueueFlags wanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;

            if ((families[index].queueFlags & wanted) == wanted) {
                context.queue_family = index;
                break;
            }
        }
        if (context.queue_family == 0xFFFFFFFFu) {
            die("no queue supports both graphics and compute");
        }
        memset(&queue_info, 0, sizeof(queue_info));
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = context.queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;

        memset(&device_info, 0, sizeof(device_info));
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        check(vkCreateDevice(context.physical_device, &device_info, NULL, &context.device),
              "vkCreateDevice");
        vkGetDeviceQueue(context.device, context.queue_family, 0, &context.queue);
    }

    {
        VkCommandPoolCreateInfo info;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.queueFamilyIndex = context.queue_family;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(context.device, &info, NULL, &context.command_pool),
              "vkCreateCommandPool");
    }

    if (command_path == NULL || palette_path == NULL || glyph_path == NULL ||
        art_path == NULL) {
        die("--commands, --palette, --glyphs and --art are required");
    }
    command_bytes = read_file(command_path, &command_size);
    palette_bytes = read_file(palette_path, &palette_size);
    glyph_bytes = read_file(glyph_path, &glyph_size);
    art_bytes = read_file(art_path, &art_size);
    if (blit_path != NULL) {
        blit_bytes = read_file(blit_path, &blit_size);
    }
    if (base_path != NULL) {
        base_bytes = read_file(base_path, &base_size);
    }

    /* The list the kernel recorded, repacked into the six uint form the
     * shader reads. */
    uint32_t command_count = 0;
    PackedCommand *packed = NULL;
    {
        const DimosCommandHeader *header = (const DimosCommandHeader *)command_bytes;
        const DimosCommand *source;
        uint32_t index;

        if (command_size < sizeof(DimosCommandHeader)) {
            die("the command file is truncated");
        }
        if (header->magic != COMMAND_MAGIC) {
            die("the command file does not start with the DimOS marker");
        }
        command_count = header->count;
        if (command_count > COMMAND_LIMIT) {
            die("the command list is longer than the kernel's own limit");
        }
        if (command_size < sizeof(DimosCommandHeader) +
                               (size_t)command_count * sizeof(DimosCommand)) {
            die("the command file is shorter than its own header claims");
        }
        raster.command_count = command_count;
        raster.font_mode = header->font_mode;
        raster.blit_used = (blit_bytes != NULL) ? header->blit_used : 0u;
        raster.spare = 0u;

        source = (const DimosCommand *)(command_bytes + sizeof(DimosCommandHeader));
        packed = calloc(command_count ? command_count : 1u, sizeof(PackedCommand));
        if (packed == NULL) {
            die("out of memory");
        }
        for (index = 0; index < command_count; ++index) {
            const DimosCommand *c = &source[index];

            packed[index].kind_and_x =
                (uint32_t)c->kind | ((uint32_t)(uint16_t)c->x << 16);
            packed[index].y_and_width =
                (uint32_t)(uint16_t)c->y | ((uint32_t)(uint16_t)c->width << 16);
            packed[index].height_colors = (uint32_t)(uint16_t)c->height |
                                          ((uint32_t)c->color << 16) |
                                          ((uint32_t)c->background << 24);
            packed[index].a0_a1 =
                (uint32_t)(uint16_t)c->a0 | ((uint32_t)(uint16_t)c->a1 << 16);
            packed[index].reference = c->reference;
            packed[index].spare = c->spare;
        }
        printf("vkraster: %u commands, font_mode=%u, blit=%u\n", command_count,
               raster.font_mode, raster.blit_used);
    }

    /* Buffers the compute shader reads. */
    VkBuffer command_buffer, palette_buffer, base_buffer, glyph_buffer, art_buffer,
        blit_buffer;
    VkDeviceMemory command_memory, palette_memory, base_memory, glyph_memory, art_memory,
        blit_memory;

    create_filled_buffer(&context, packed, (size_t)command_count * sizeof(PackedCommand),
                         (VkDeviceSize)(command_count ? command_count : 1u) *
                             sizeof(PackedCommand),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &command_buffer,
                         &command_memory);

    {
        /* Two palettes, and the difference matters.
         *
         * The DAC palette is what the screen shows: themed, six bits per
         * channel, the table the display pass resolves indices through.
         *
         * The base palette is what gfx.c *blends* in: untinted, eight
         * bits, exported by the kernel. Mixing happens there and the
         * result is looked up in the colour cube; the theme is applied
         * afterwards by the DAC. Blending in the themed values instead
         * would land on different cube slots, so the rasterizer is given
         * the base table and the display pass the DAC one. */
        float dac[PALETTE_ENTRIES * 4];
        uint32_t base[PALETTE_ENTRIES * 4];
        uint32_t index;

        if (palette_size < PALETTE_ENTRIES * 3u) {
            die("the palette file is smaller than 256 RGB triples");
        }
        for (index = 0; index < PALETTE_ENTRIES; ++index) {
            dac[index * 4 + 0] = (float)palette_bytes[index * 3 + 0] / 255.0f;
            dac[index * 4 + 1] = (float)palette_bytes[index * 3 + 1] / 255.0f;
            dac[index * 4 + 2] = (float)palette_bytes[index * 3 + 2] / 255.0f;
            dac[index * 4 + 3] = 1.0f;
        }
        create_filled_buffer(&context, dac, sizeof(dac), sizeof(dac),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &palette_buffer,
                             &palette_memory);

        if (base_bytes != NULL && base_size >= PALETTE_ENTRIES * 3u) {
            for (index = 0; index < PALETTE_ENTRIES; ++index) {
                base[index * 4 + 0] = base_bytes[index * 3 + 0];
                base[index * 4 + 1] = base_bytes[index * 3 + 1];
                base[index * 4 + 2] = base_bytes[index * 3 + 2];
                base[index * 4 + 3] = 255u;
            }
        } else {
            /* No base table given: blend in the DAC values. The picture
             * is still correct, the cube slots may differ by one. */
            for (index = 0; index < PALETTE_ENTRIES; ++index) {
                base[index * 4 + 0] = palette_bytes[index * 3 + 0];
                base[index * 4 + 1] = palette_bytes[index * 3 + 1];
                base[index * 4 + 2] = palette_bytes[index * 3 + 2];
                base[index * 4 + 3] = 255u;
            }
        }
        create_filled_buffer(&context, base, sizeof(base), sizeof(base),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &base_buffer,
                             &base_memory);
    }

    create_filled_buffer(&context, glyph_bytes, glyph_size, GLYPH_BYTES,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &glyph_buffer, &glyph_memory);
    create_filled_buffer(&context, art_bytes, art_size, ART_BYTES,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &art_buffer, &art_memory);
    create_filled_buffer(&context, blit_bytes, blit_size, SOURCE_PIXELS,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &blit_buffer, &blit_memory);

    /* The indexed image the compute shader writes and the display pass
     * reads: exactly the 320x200 bytes the kernel used to produce. */
    VkImage indexed_image;
    VkDeviceMemory indexed_memory;
    VkImageView indexed_view;
    {
        VkImageCreateInfo info;
        VkMemoryRequirements requirements;
        VkMemoryAllocateInfo allocation;
        VkImageViewCreateInfo view_info;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8_UINT;
        info.extent.width = SOURCE_WIDTH;
        info.extent.height = SOURCE_HEIGHT;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(context.device, &info, NULL, &indexed_image), "vkCreateImage");

        vkGetImageMemoryRequirements(context.device, indexed_image, &requirements);
        memset(&allocation, 0, sizeof(allocation));
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = find_memory(&context, requirements.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(context.device, &allocation, NULL, &indexed_memory),
              "vkAllocateMemory");
        check(vkBindImageMemory(context.device, indexed_image, indexed_memory, 0),
              "vkBindImageMemory");

        memset(&view_info, 0, sizeof(view_info));
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = indexed_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8_UINT;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        check(vkCreateImageView(context.device, &view_info, NULL, &indexed_view),
              "vkCreateImageView");
    }

    /* ---------------- pass 1: the rasterizer ---------------- */
    VkDescriptorSetLayout raster_layout;
    VkPipelineLayout raster_pipeline_layout;
    VkPipeline raster_pipeline;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet raster_set;
    {
        VkDescriptorSetLayoutBinding bindings[6];
        VkDescriptorSetLayoutCreateInfo layout_info;
        VkDescriptorPoolSize sizes[3];
        VkDescriptorPoolCreateInfo pool_info;
        VkDescriptorSetAllocateInfo allocation;
        VkPushConstantRange push_range;
        VkPipelineLayoutCreateInfo pipeline_layout_info;
        VkComputePipelineCreateInfo pipeline_info;
        VkShaderModule module;
        uint32_t index;

        memset(bindings, 0, sizeof(bindings));
        for (index = 0; index < 5; ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        bindings[5].binding = 5;
        bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[5].descriptorCount = 1;
        bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        memset(&layout_info, 0, sizeof(layout_info));
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 6;
        layout_info.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(context.device, &layout_info, NULL,
                                          &raster_layout),
              "vkCreateDescriptorSetLayout");

        memset(sizes, 0, sizeof(sizes));
        sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[0].descriptorCount = 6; /* five for the raster set, one for display */
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sizes[1].descriptorCount = 1;
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[2].descriptorCount = 1;

        memset(&pool_info, 0, sizeof(pool_info));
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 2;
        pool_info.poolSizeCount = 3;
        pool_info.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(context.device, &pool_info, NULL, &descriptor_pool),
              "vkCreateDescriptorPool");

        memset(&allocation, 0, sizeof(allocation));
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = descriptor_pool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &raster_layout;
        check(vkAllocateDescriptorSets(context.device, &allocation, &raster_set),
              "vkAllocateDescriptorSets");

        {
            VkDescriptorBufferInfo buffers[5];
            VkDescriptorImageInfo image_info;
            VkWriteDescriptorSet writes[6];
            VkBuffer handles[5];

            handles[0] = command_buffer;
            handles[1] = base_buffer;
            handles[2] = glyph_buffer;
            handles[3] = art_buffer;
            handles[4] = blit_buffer;

            memset(buffers, 0, sizeof(buffers));
            memset(writes, 0, sizeof(writes));
            for (index = 0; index < 5; ++index) {
                buffers[index].buffer = handles[index];
                buffers[index].range = VK_WHOLE_SIZE;
                writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[index].dstSet = raster_set;
                writes[index].dstBinding = index;
                writes[index].descriptorCount = 1;
                writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[index].pBufferInfo = &buffers[index];
            }
            memset(&image_info, 0, sizeof(image_info));
            image_info.imageView = indexed_view;
            image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[5].dstSet = raster_set;
            writes[5].dstBinding = 5;
            writes[5].descriptorCount = 1;
            writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[5].pImageInfo = &image_info;
            vkUpdateDescriptorSets(context.device, 6, writes, 0, NULL);
        }

        memset(&push_range, 0, sizeof(push_range));
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.size = sizeof(RasterSettings);

        memset(&pipeline_layout_info, 0, sizeof(pipeline_layout_info));
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &raster_layout;
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_range;
        check(vkCreatePipelineLayout(context.device, &pipeline_layout_info, NULL,
                                     &raster_pipeline_layout),
              "vkCreatePipelineLayout");

        module = load_shader(context.device, raster_path);
        memset(&pipeline_info, 0, sizeof(pipeline_info));
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = raster_pipeline_layout;
        check(vkCreateComputePipelines(context.device, VK_NULL_HANDLE, 1, &pipeline_info,
                                       NULL, &raster_pipeline),
              "vkCreateComputePipelines");
        vkDestroyShaderModule(context.device, module, NULL);
    }

    {
        VkCommandBuffer commands = begin_commands(&context);

        transition(commands, indexed_image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, raster_pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE,
                                raster_pipeline_layout, 0, 1, &raster_set, 0, NULL);
        vkCmdPushConstants(commands, raster_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(RasterSettings), &raster);
        vkCmdDispatch(commands, (SOURCE_WIDTH + 7u) / 8u, (SOURCE_HEIGHT + 7u) / 8u, 1);
        transition(commands, indexed_image, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        end_commands(&context, commands);
    }

    /* Optionally hand the raw indices back, so a test can compare them
     * against what the software kernel produces. */
    if (indices_path != NULL) {
        VkBuffer readback;
        VkDeviceMemory readback_memory;
        VkCommandBuffer commands;
        VkBufferImageCopy region;
        void *mapped;
        FILE *out;

        create_buffer(&context, SOURCE_PIXELS, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &readback, &readback_memory);
        commands = begin_commands(&context);
        transition(commands, indexed_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        memset(&region, 0, sizeof(region));
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = SOURCE_WIDTH;
        region.imageExtent.height = SOURCE_HEIGHT;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(commands, indexed_image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
        transition(commands, indexed_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        end_commands(&context, commands);

        check(vkMapMemory(context.device, readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped),
              "vkMapMemory");
        out = fopen(indices_path, "wb");
        if (out == NULL) {
            die("cannot write the index dump");
        }
        fwrite(mapped, 1, SOURCE_PIXELS, out);
        fclose(out);
        vkUnmapMemory(context.device, readback_memory);
        vkDestroyBuffer(context.device, readback, NULL);
        vkFreeMemory(context.device, readback_memory, NULL);
        printf("vkraster: wrote %s (%u bytes of palette indices)\n", indices_path,
               SOURCE_PIXELS);
    }

    /* ---------------- pass 2: the display ---------------- */
    VkImage target_image;
    VkDeviceMemory target_memory;
    VkImageView target_view;
    {
        VkImageCreateInfo info;
        VkMemoryRequirements requirements;
        VkMemoryAllocateInfo allocation;
        VkImageViewCreateInfo view_info;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent.width = target_width;
        info.extent.height = target_height;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(context.device, &info, NULL, &target_image), "vkCreateImage");

        vkGetImageMemoryRequirements(context.device, target_image, &requirements);
        memset(&allocation, 0, sizeof(allocation));
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = find_memory(&context, requirements.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(context.device, &allocation, NULL, &target_memory),
              "vkAllocateMemory");
        check(vkBindImageMemory(context.device, target_image, target_memory, 0),
              "vkBindImageMemory");

        memset(&view_info, 0, sizeof(view_info));
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = target_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        check(vkCreateImageView(context.device, &view_info, NULL, &target_view),
              "vkCreateImageView");
    }

    VkRenderPass render_pass;
    {
        VkAttachmentDescription attachment;
        VkAttachmentReference reference;
        VkSubpassDescription subpass;
        VkRenderPassCreateInfo info;

        memset(&attachment, 0, sizeof(attachment));
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        memset(&reference, 0, sizeof(reference));
        reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        memset(&subpass, 0, sizeof(subpass));
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        check(vkCreateRenderPass(context.device, &info, NULL, &render_pass),
              "vkCreateRenderPass");
    }

    VkFramebuffer framebuffer;
    {
        VkFramebufferCreateInfo info;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = render_pass;
        info.attachmentCount = 1;
        info.pAttachments = &target_view;
        info.width = target_width;
        info.height = target_height;
        info.layers = 1;
        check(vkCreateFramebuffer(context.device, &info, NULL, &framebuffer),
              "vkCreateFramebuffer");
    }

    VkSampler sampler;
    {
        VkSamplerCreateInfo info;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_NEAREST;
        info.minFilter = VK_FILTER_NEAREST;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        check(vkCreateSampler(context.device, &info, NULL, &sampler), "vkCreateSampler");
    }

    VkDescriptorSetLayout display_layout;
    VkDescriptorSet display_set;
    VkPipelineLayout display_pipeline_layout;
    VkPipeline display_pipeline;
    {
        VkDescriptorSetLayoutBinding bindings[2];
        VkDescriptorSetLayoutCreateInfo layout_info;
        VkDescriptorSetAllocateInfo allocation;
        VkDescriptorImageInfo image_info;
        VkDescriptorBufferInfo buffer_info;
        VkWriteDescriptorSet writes[2];
        VkShaderModule vertex_module;
        VkShaderModule fragment_module;
        VkPipelineShaderStageCreateInfo stages[2];
        VkPipelineVertexInputStateCreateInfo vertex_input;
        VkPipelineInputAssemblyStateCreateInfo assembly;
        VkViewport viewport;
        VkRect2D scissor;
        VkPipelineViewportStateCreateInfo viewport_state;
        VkPipelineRasterizationStateCreateInfo rasterization;
        VkPipelineMultisampleStateCreateInfo multisample;
        VkPipelineColorBlendAttachmentState blend_attachment;
        VkPipelineColorBlendStateCreateInfo blend;
        VkPushConstantRange push_range;
        VkPipelineLayoutCreateInfo pipeline_layout_info;
        VkGraphicsPipelineCreateInfo info;

        memset(bindings, 0, sizeof(bindings));
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        memset(&layout_info, 0, sizeof(layout_info));
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 2;
        layout_info.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(context.device, &layout_info, NULL,
                                          &display_layout),
              "vkCreateDescriptorSetLayout");

        memset(&allocation, 0, sizeof(allocation));
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = descriptor_pool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &display_layout;
        check(vkAllocateDescriptorSets(context.device, &allocation, &display_set),
              "vkAllocateDescriptorSets");

        memset(&image_info, 0, sizeof(image_info));
        image_info.sampler = sampler;
        image_info.imageView = indexed_view;
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&buffer_info, 0, sizeof(buffer_info));
        buffer_info.buffer = palette_buffer;
        buffer_info.range = VK_WHOLE_SIZE;

        memset(writes, 0, sizeof(writes));
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = display_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &image_info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = display_set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(context.device, 2, writes, 0, NULL);

        vertex_module = load_shader(context.device, vertex_path);
        fragment_module = load_shader(context.device, fragment_path);

        memset(stages, 0, sizeof(stages));
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_module;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_module;
        stages[1].pName = "main";

        memset(&vertex_input, 0, sizeof(vertex_input));
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        memset(&assembly, 0, sizeof(assembly));
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        memset(&viewport, 0, sizeof(viewport));
        viewport.width = (float)target_width;
        viewport.height = (float)target_height;
        viewport.maxDepth = 1.0f;
        memset(&scissor, 0, sizeof(scissor));
        scissor.extent.width = target_width;
        scissor.extent.height = target_height;

        memset(&viewport_state, 0, sizeof(viewport_state));
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.pViewports = &viewport;
        viewport_state.scissorCount = 1;
        viewport_state.pScissors = &scissor;

        memset(&rasterization, 0, sizeof(rasterization));
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

        memset(&multisample, 0, sizeof(multisample));
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        memset(&blend_attachment, 0, sizeof(blend_attachment));
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        memset(&blend, 0, sizeof(blend));
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;

        memset(&push_range, 0, sizeof(push_range));
        push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        push_range.size = sizeof(DisplaySettings);

        memset(&pipeline_layout_info, 0, sizeof(pipeline_layout_info));
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &display_layout;
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_range;
        check(vkCreatePipelineLayout(context.device, &pipeline_layout_info, NULL,
                                     &display_pipeline_layout),
              "vkCreatePipelineLayout");

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport_state;
        info.pRasterizationState = &rasterization;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.layout = display_pipeline_layout;
        info.renderPass = render_pass;
        check(vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &info, NULL,
                                        &display_pipeline),
              "vkCreateGraphicsPipelines");
        vkDestroyShaderModule(context.device, vertex_module, NULL);
        vkDestroyShaderModule(context.device, fragment_module, NULL);
    }

    display.source_size[0] = (float)SOURCE_WIDTH;
    display.source_size[1] = (float)SOURCE_HEIGHT;
    display.target_size[0] = (float)target_width;
    display.target_size[1] = (float)target_height;

    VkBuffer readback;
    VkDeviceMemory readback_memory;
    create_buffer(&context, (VkDeviceSize)target_width * target_height * 4u,
                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &readback, &readback_memory);
    {
        VkCommandBuffer commands = begin_commands(&context);
        VkClearValue clear;
        VkRenderPassBeginInfo begin;
        VkBufferImageCopy region;

        memset(&clear, 0, sizeof(clear));
        memset(&begin, 0, sizeof(begin));
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = render_pass;
        begin.framebuffer = framebuffer;
        begin.renderArea.extent.width = target_width;
        begin.renderArea.extent.height = target_height;
        begin.clearValueCount = 1;
        begin.pClearValues = &clear;

        vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, display_pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                display_pipeline_layout, 0, 1, &display_set, 0, NULL);
        vkCmdPushConstants(commands, display_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(DisplaySettings), &display);
        vkCmdDraw(commands, 3, 1, 0, 0);
        vkCmdEndRenderPass(commands);

        memset(&region, 0, sizeof(region));
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = target_width;
        region.imageExtent.height = target_height;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(commands, target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback, 1, &region);
        end_commands(&context, commands);
    }

    {
        void *mapped;

        check(vkMapMemory(context.device, readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped),
              "vkMapMemory");
        write_png(output_path, (const unsigned char *)mapped, target_width, target_height,
                  target_width * 4u);
        vkUnmapMemory(context.device, readback_memory);
        printf("vkraster: wrote %s (%ux%u)\n", output_path, target_width, target_height);
    }

    vkDestroyBuffer(context.device, readback, NULL);
    vkFreeMemory(context.device, readback_memory, NULL);
    vkDestroyPipeline(context.device, display_pipeline, NULL);
    vkDestroyPipelineLayout(context.device, display_pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(context.device, display_layout, NULL);
    vkDestroyPipeline(context.device, raster_pipeline, NULL);
    vkDestroyPipelineLayout(context.device, raster_pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(context.device, raster_layout, NULL);
    vkDestroyDescriptorPool(context.device, descriptor_pool, NULL);
    vkDestroySampler(context.device, sampler, NULL);
    vkDestroyFramebuffer(context.device, framebuffer, NULL);
    vkDestroyRenderPass(context.device, render_pass, NULL);
    vkDestroyImageView(context.device, target_view, NULL);
    vkDestroyImage(context.device, target_image, NULL);
    vkFreeMemory(context.device, target_memory, NULL);
    vkDestroyImageView(context.device, indexed_view, NULL);
    vkDestroyImage(context.device, indexed_image, NULL);
    vkFreeMemory(context.device, indexed_memory, NULL);
    vkDestroyBuffer(context.device, command_buffer, NULL);
    vkFreeMemory(context.device, command_memory, NULL);
    vkDestroyBuffer(context.device, palette_buffer, NULL);
    vkFreeMemory(context.device, palette_memory, NULL);
    vkDestroyBuffer(context.device, base_buffer, NULL);
    vkFreeMemory(context.device, base_memory, NULL);
    vkDestroyBuffer(context.device, glyph_buffer, NULL);
    vkFreeMemory(context.device, glyph_memory, NULL);
    vkDestroyBuffer(context.device, art_buffer, NULL);
    vkFreeMemory(context.device, art_memory, NULL);
    vkDestroyBuffer(context.device, blit_buffer, NULL);
    vkFreeMemory(context.device, blit_memory, NULL);
    vkDestroyCommandPool(context.device, context.command_pool, NULL);
    vkDestroyDevice(context.device, NULL);
    vkDestroyInstance(context.instance, NULL);
    free(command_bytes);
    free(palette_bytes);
    free(glyph_bytes);
    free(art_bytes);
    free(blit_bytes);
    free(base_bytes);
    free(packed);
    return 0;
}
