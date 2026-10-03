// post_fx.glsl — screen-space passes run after the HDR scene is resolved.
//
//   ssao      = fullscreen.vert + ssao.frag       half-res ambient occlusion
//   ao_blur   = fullscreen.vert + ao_blur.frag    depth-aware 4x4 blur
//   composite = fullscreen.vert + composite.frag  depth of field, AO, backdrop,
//                                                 ACES tone map, vignette, grain

//@stage fullscreen.vert
#version 330 core

out vec2 uv;

void main() {
    // One oversized triangle covering the screen; no vertex buffer needed.
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

//@stage ssao.frag
#version 330 core

in vec2 uv;
out vec4 frag_color;

uniform sampler2D depth_tex;
uniform mat4 inv_projection;
uniform mat4 projection;
uniform vec2 depth_texel;   // 1 / full-res size
uniform float ao_radius;    // world units
uniform float ao_intensity;

vec3 view_pos_at(vec2 t) {
    float d = texture(depth_tex, t).r;
    vec4 ndc = vec4(t * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 v = inv_projection * ndc;
    return v.xyz / v.w;
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    float d = texture(depth_tex, uv).r;
    if (d >= 1.0) {
        frag_color = vec4(1.0);
        return;
    }
    vec3 P = view_pos_at(uv);
    vec3 px = view_pos_at(uv + vec2(depth_texel.x * 2.0, 0.0));
    vec3 py = view_pos_at(uv + vec2(0.0, depth_texel.y * 2.0));
    vec3 N = normalize(cross(px - P, py - P));

    // Project the world-space radius to a screen-space radius.
    float screen_r = ao_radius * projection[1][1] / max(-P.z, 0.1) * 0.5;
    const int SAMPLES = 14;
    float angle = hash12(gl_FragCoord.xy) * 6.2831853;
    float occlusion = 0.0;
    for (int i = 0; i < SAMPLES; ++i) {
        float t = (float(i) + 0.5) / float(SAMPLES);
        float a = angle + t * 6.2831853 * 3.0;  // three-turn spiral
        vec2 offs = vec2(cos(a), sin(a)) * t * screen_r;
        vec3 Q = view_pos_at(uv + offs);
        vec3 v = Q - P;
        float vv = dot(v, v);
        float vn = dot(v, N);
        float falloff = max(0.0, 1.0 - vv / (ao_radius * ao_radius));
        occlusion += max(0.0, vn - 0.015 * -P.z) / (vv + 0.05) * falloff;
    }
    float ao = max(0.0, 1.0 - ao_intensity * occlusion / float(SAMPLES));
    frag_color = vec4(vec3(ao), 1.0);
}

//@stage ao_blur.frag
#version 330 core

in vec2 uv;
out vec4 frag_color;

uniform sampler2D ao_tex;
uniform sampler2D depth_tex;
uniform vec2 ao_texel;

void main() {
    float center_d = texture(depth_tex, uv).r;
    float sum = 0.0;
    float wsum = 0.0;
    for (int y = -2; y <= 1; ++y) {
        for (int x = -2; x <= 1; ++x) {
            vec2 o = (vec2(x, y) + 0.5) * ao_texel;
            float d = texture(depth_tex, uv + o).r;
            float w = 1.0 / (1e-5 + abs(d - center_d) * 400.0);
            w = min(w, 1.0);
            sum += texture(ao_tex, uv + o).r * w;
            wsum += w;
        }
    }
    frag_color = vec4(vec3(sum / max(wsum, 1e-4)), 1.0);
}

//@stage composite.frag
#version 330 core

in vec2 uv;
out vec4 frag_color;

uniform sampler2D scene_tex;
uniform sampler2D depth_tex;
uniform sampler2D ao_tex;
uniform vec2 texel;
uniform float near_plane;
uniform float far_plane;
uniform float focus_distance;  // world units from the camera
uniform float focus_range;     // in-focus band half width
uniform float dof_scale;       // blur growth outside the band (pixels)
uniform float max_coc;         // pixels
uniform float ao_strength;
uniform float exposure;
uniform float time;
uniform vec3 backdrop_top;
uniform vec3 backdrop_bottom;
uniform sampler2D backdrop_tex;   // gallery photo (top row first)
uniform int backdrop_mode;        // 0 = studio gradient, 1 = photo
uniform float backdrop_aspect;    // photo width / height
uniform float screen_aspect;      // framebuffer width / height
uniform vec2 backdrop_vp;         // photo vanishing point (u, v from top)
uniform float horizon_y;          // screen-space height of the horizon (0..1, may be off-screen)
uniform float backdrop_shift_u;   // horizontal pan that follows camera yaw
uniform float backdrop_gain;
uniform float vignette;

float linear_depth(float d) {
    float z = d * 2.0 - 1.0;
    return 2.0 * near_plane * far_plane / (far_plane + near_plane - z * (far_plane - near_plane));
}

vec3 backdrop(vec2 t) {
    if (backdrop_mode == 1) {
        // Photo scaled to the screen width; its vanishing point is pinned to
        // the 3D horizon so the corridor recedes in the same direction as the
        // floor. Mirrored-repeat in U keeps yaw panning seamless.
        float scale = backdrop_aspect / screen_aspect;
        vec2 p = vec2(t.x + backdrop_shift_u, backdrop_vp.y - (t.y - horizon_y) * scale);
        return pow(texture(backdrop_tex, p).rgb, vec3(2.2)) * backdrop_gain;
    }
    vec2 c = t - vec2(0.5, 0.62);
    float glow = exp(-dot(c, c) * 3.2);
    return mix(backdrop_bottom, backdrop_top, glow);
}

vec3 scene_at(vec2 t, float d) {
    if (d >= 1.0) {
        return backdrop(t);
    }
    vec4 s = texture(scene_tex, t);
    float ao = mix(1.0, texture(ao_tex, t).r, ao_strength);
    // Soft-clamp very bright specular samples ("fireflies") so the bokeh
    // gather doesn't splat them into visible discs.
    float lum = dot(s.rgb, vec3(0.2126, 0.7152, 0.0722));
    vec3 c = s.rgb * min(1.0, 6.0 / max(lum, 1e-4));
    // Alpha < 1 = distance fog: dissolve the surface into the backdrop.
    return mix(backdrop(t), c * ao, s.a);
}

float coc_px(float d) {
    if (d >= 1.0) {
        return max_coc;
    }
    float z = linear_depth(d);
    float off = max(abs(z - focus_distance) - focus_range, 0.0);
    return clamp(off / z * dof_scale, 0.0, max_coc);
}

// ACES filmic curve (Narkowicz 2015 fit).
vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    float d0 = texture(depth_tex, uv).r;
    float z0 = d0 >= 1.0 ? far_plane : linear_depth(d0);
    float c0 = coc_px(d0);
    vec3 color = scene_at(uv, d0);

    // Gather depth of field: golden-angle disc, each tap contributes if its
    // own circle of confusion would reach this pixel. Taps farther away than
    // the centre are clamped so blurry background never bleeds over sharp
    // foreground pieces.
    if (max_coc > 0.5) {
        const int TAPS = 40;
        const float GOLDEN = 2.39996323;
        vec3 acc = color;
        float wsum = 1.0;
        for (int i = 1; i < TAPS; ++i) {
            float r = sqrt(float(i) / float(TAPS)) * max_coc;
            float a = float(i) * GOLDEN;
            vec2 t = uv + vec2(cos(a), sin(a)) * r * texel;
            float d = texture(depth_tex, t).r;
            float z = d >= 1.0 ? far_plane : linear_depth(d);
            float c = coc_px(d);
            if (z > z0) {
                c = min(c, max(c0, 0.0) * 2.0);
            }
            float w = smoothstep(r - 1.5, r + 0.5, c);
            acc += scene_at(t, d) * w;
            wsum += w;
        }
        color = acc / wsum;
    }

    color *= exposure;
    color = aces(color);

    // Vignette and a touch of film grain (also hides 8-bit banding).
    vec2 c = uv - 0.5;
    color *= mix(1.0, vignette, smoothstep(0.25, 0.85, dot(c, c) * 2.0));
    color = pow(color, vec3(1.0 / 2.2));
    color += (hash12(gl_FragCoord.xy + fract(time) * 100.0) - 0.5) / 255.0;

    frag_color = vec4(color, 1.0);
}
