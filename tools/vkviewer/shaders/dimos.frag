#version 450
/*
 * The DimOS CRT shader.
 *
 * Input is exactly what the kernel produced: 320x200 bytes of palette
 * indices (R8_UINT) plus the 256 entry VGA DAC palette the kernel wrote
 * through ports 0x3C8/0x3C9. The lookup that a real VGA card does in
 * hardware happens here in the fragment shader instead.
 *
 * On top of that sits the part a 1993 machine could never do: the
 * picture is resampled with a soft scanline profile, the pixel grid is
 * shaded like an aperture mask, and the whole frame is bent slightly to
 * follow the curve of a tube.
 */

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform usampler2D indexed_frame;

layout(set = 0, binding = 1) uniform Palette {
    vec4 color[256];   /* xyz = rgb 0..1, w unused */
} palette;

layout(push_constant) uniform Settings {
    vec2 source_size;   /* 320 x 200                       */
    vec2 target_size;   /* the window, in pixels           */
    float scanline;     /* 0 = off, 1 = full depth         */
    float mask;         /* aperture grille strength        */
    float curvature;    /* barrel distortion               */
    float glow;         /* phosphor bloom                  */
} settings;

/* The colour of one source pixel, straight out of the palette. */
vec3 fetch(ivec2 texel) {
    texel = clamp(texel, ivec2(0), ivec2(settings.source_size) - 1);
    uint index = texelFetch(indexed_frame, texel, 0).r;
    return palette.color[index].rgb;
}

/* Bend the flat image over a tube. */
vec2 curve(vec2 uv) {
    uv = uv * 2.0 - 1.0;
    vec2 offset = abs(uv.yx) / vec2(6.0, 5.0);
    uv += uv * offset * offset * settings.curvature;
    return uv * 0.5 + 0.5;
}

void main() {
    vec2 uv = curve(in_uv);

    /* Outside the tube there is no picture, just the bezel. */
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 texel_position = uv * settings.source_size;
    ivec2 base = ivec2(floor(texel_position - 0.5));
    vec2 fraction = fract(texel_position - 0.5);

    /* Horizontally the pixels stay hard: a 320 wide image should still
     * look like 320 pixels, not like a blurred photograph of one. */
    vec3 top = mix(fetch(base + ivec2(0, 0)), fetch(base + ivec2(1, 0)),
                   smoothstep(0.35, 0.65, fraction.x));
    vec3 bottom = mix(fetch(base + ivec2(0, 1)), fetch(base + ivec2(1, 1)),
                      smoothstep(0.35, 0.65, fraction.x));
    vec3 color = mix(top, bottom, smoothstep(0.35, 0.65, fraction.y));

    /* Phosphor bloom: a wide, cheap blur added back on top, so bright
     * areas bleed into their surroundings the way a real tube does. */
    vec3 bleed = vec3(0.0);
    bleed += fetch(base + ivec2(-2, 0));
    bleed += fetch(base + ivec2(-1, 0));
    bleed += fetch(base + ivec2(1, 0));
    bleed += fetch(base + ivec2(2, 0));
    bleed += fetch(base + ivec2(0, -1));
    bleed += fetch(base + ivec2(0, 1));
    color += (bleed / 6.0) * settings.glow;

    /* Scanlines: the beam is brightest in the middle of a line. Bright
     * pixels spread out, so the darkening is weaker where the picture is
     * light -- that is what keeps the image from going muddy. */
    float line_phase = fract(texel_position.y);
    float beam = sin(line_phase * 3.14159265);
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    float depth = settings.scanline * (1.0 - 0.55 * luma);
    color *= mix(1.0, beam * beam, depth) * 1.15;

    /* Aperture mask: on a slot mask tube every third subpixel column
     * belongs to one phosphor, so a column is never pure white. */
    float column = mod(gl_FragCoord.x, 3.0);
    vec3 mask_tint = vec3(1.0 - settings.mask);
    if (column < 1.0) {
        mask_tint.r = 1.0 + settings.mask * 0.5;
    } else if (column < 2.0) {
        mask_tint.g = 1.0 + settings.mask * 0.5;
    } else {
        mask_tint.b = 1.0 + settings.mask * 0.5;
    }
    color *= mask_tint;

    /* Vignette, and the slight lift a glass screen gives to black. */
    vec2 edge = uv * (1.0 - uv.yx);
    float vignette = pow(clamp(edge.x * edge.y * 24.0, 0.0, 1.0), 0.25);
    color = color * vignette + vec3(0.015);

    out_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
