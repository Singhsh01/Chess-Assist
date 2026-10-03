// pbr_shaders.glsl — scene shaders for Chess Assist.
//
// One file, several programs. Renderer::load_shader_sections() splits it on
// "//@stage <tag>" lines and links pairs of stages:
//
//   pbr        = scene.vert    + pbr.frag        lit, shadowed, reflective surfaces
//   shadow     = depth.vert    + depth.frag      directional-light shadow map
//   highlight  = scene.vert    + highlight.frag  glowing best-move squares / path
//
// Lighting model: Cook-Torrance GGX (direct) + split-sum image-based lighting
// from an analytic studio environment, an optional lacquer clear-coat lobe,
// planar mirror reflections on the board, PCF soft shadows (16 rotated Poisson
// taps over hardware 2x2 comparisons) and analytic contact ambient occlusion.
// Output is linear HDR; tone mapping happens in post_fx.glsl.

//@stage scene.vert
#version 330 core

layout (location = 0) in vec3 a_position;
layout (location = 1) in vec3 a_normal;
layout (location = 2) in vec2 a_tex_coord;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat3 normal_mat;
uniform vec4 clip_plane;  // world-space plane, used by the mirrored pass

out vec3 world_pos;
out vec3 normal;
out vec2 tex_coord;

void main() {
    vec4 wp = model * vec4(a_position, 1.0);
    world_pos = wp.xyz;
    normal = normal_mat * a_normal;
    tex_coord = a_tex_coord;
    gl_ClipDistance[0] = dot(wp, clip_plane);
    gl_Position = projection * view * wp;
}

//@stage pbr.frag
#version 330 core

const float PI = 3.14159265;
const int MAX_OCCLUDERS = 32;

in vec3 world_pos;
in vec3 normal;
in vec2 tex_coord;

out vec4 frag_color;

uniform sampler2D albedo_tex;
uniform sampler2D roughness_tex;
uniform sampler2D normal_tex;
uniform sampler2DShadow shadow_map;
uniform sampler2D reflection_tex;
uniform int has_normal_map;

uniform vec3 view_pos;
uniform vec3 light_dir;        // direction TOWARDS the key light (normalised)
uniform vec3 light_color;
uniform mat4 light_space;
uniform float shadow_softness; // PCF radius in shadow-map texels
uniform vec2 viewport_size;

// Material controls
uniform int material_kind;       // 0 = textured asset, 1 = procedural table
uniform float roughness_scale;   // < 1 makes the surface glossier
uniform float roughness_bias;
uniform float clearcoat;         // 0..1 strength of the lacquer layer
uniform float clearcoat_roughness;
uniform float reflection_strength; // planar reflection mix (board only)
uniform vec4 tint;                 // rgb emissive tint, a = strength (hover)

// Ambient occlusion
uniform vec4 occluders[MAX_OCCLUDERS]; // xyz piece base, w radius
uniform int occluder_count;
uniform float receive_contact_ao;      // board/table: soft blobs under pieces
uniform float piece_base_height;       // pieces: darken towards their base
uniform float fog_start;
uniform float fog_end;        // fog is written to alpha; post_fx blends the backdrop in
uniform float env_mode;       // 0 = dark photo studio, 1 = white gallery hall
uniform float floor_tile;     // gallery floor tile size in world units

// ---------------------------------------------------------------- utilities
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float value_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x),
               mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; ++i) {
        v += a * value_noise(p);
        p = p * 2.03 + vec2(17.1, 3.7);
        a *= 0.5;
    }
    return v;
}

vec3 perturb_normal(vec3 N) {
    if (has_normal_map == 0) {
        return N;
    }
    vec3 tangent_normal = texture(normal_tex, tex_coord).xyz * 2.0 - 1.0;
    vec3 q1 = dFdx(world_pos);
    vec3 q2 = dFdy(world_pos);
    vec2 st1 = dFdx(tex_coord);
    vec2 st2 = dFdy(tex_coord);
    vec3 T = normalize(q1 * st2.t - q2 * st1.t);
    vec3 B = -normalize(cross(N, T));
    return normalize(mat3(T, B, N) * tangent_normal);
}

// ---------------------------------------------------------------- BRDF
float distribution_ggx(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float geometry_smith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

vec3 fresnel_schlick(float cos_theta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

vec3 fresnel_schlick_roughness(float cos_theta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) *
                pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// Karis' analytic fit of the split-sum environment BRDF (UE4 mobile).
vec2 env_brdf_approx(float NdotV, float roughness) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// ---------------------------------------------------------------- environment
// Analytic photo-studio: dark cyclorama, two large overhead softboxes and a
// warm rim strip. Roughness widens the light edges, approximating a
// pre-filtered radiance map without any cubemap textures. The high contrast
// between black walls and bright boxes is what gives lacquered pieces their
// crisp "product shot" reflections.
float softbox(vec2 q, vec2 center, vec2 half_size, float soft) {
    vec2 d = abs(q - center) - half_size;
    float outside = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
    return 1.0 - smoothstep(-soft, soft, outside);
}

vec3 dark_studio_env(vec3 dir, float roughness) {
    float soft = mix(0.015, 0.9, roughness);
    float spread = mix(1.0, 0.18, roughness);  // energy conservation-ish
    vec3 col = mix(vec3(0.010, 0.011, 0.014), vec3(0.055, 0.058, 0.066),
                   smoothstep(-0.3, 0.8, dir.y));
    if (dir.y > 0.02) {
        vec2 q = dir.xz / dir.y;  // project onto the ceiling plane y = 1
        col += vec3(5.2, 5.0, 4.7) * spread *
               softbox(q, vec2(-0.55, 0.10), vec2(0.38, 0.85), soft);
        col += vec3(2.1, 2.3, 2.7) * spread *
               softbox(q, vec2(0.85, -0.35), vec2(0.22, 0.60), soft);
    }
    // Warm rim strip near the horizon behind Black's side.
    float rim = smoothstep(0.07 + soft, 0.07 - soft, abs(dir.y - 0.16)) *
                smoothstep(0.45, 0.85, dir.z);
    col += vec3(1.5, 0.9, 0.5) * rim * spread * 0.8;
    return col;
}

// Analytic white gallery hall (matches the backdrop photo): black/white
// checker floor below the horizon, white walls with a rhythm of pilasters,
// a bright plaster ceiling and daylight from tall windows on one side.
vec3 gallery_env(vec3 dir, float roughness) {
    float soft = mix(0.02, 0.9, roughness);
    float spread = mix(1.0, 0.25, roughness);
    vec3 floor_c = vec3(0.30, 0.30, 0.31);
    vec3 wall_c = vec3(0.80, 0.79, 0.77);
    vec3 ceil_c = vec3(0.92, 0.91, 0.89);
    vec3 col = dir.y < 0.0 ? mix(wall_c, floor_c, smoothstep(0.0, -0.30 - soft * 0.3, dir.y))
                           : mix(wall_c, ceil_c, smoothstep(0.15, 0.85, dir.y));
    // Pilasters: azimuthal light/shadow rhythm on the walls (sharp only in
    // glossy reflections, averaged away for rough surfaces).
    float az = atan(dir.x, dir.z);
    float band = smoothstep(-0.05, 0.25, dir.y) * (1.0 - smoothstep(0.45, 0.85, dir.y));
    float pil = smoothstep(-0.2 - soft, 0.2 + soft, cos(az * 14.0));
    col *= mix(1.0, mix(0.72, 1.0, pil), band * (1.0 - roughness));
    // Daylight from tall windows on the -X side.
    float win = smoothstep(-0.55 + soft, -0.80 - soft * 0.2, dir.x) *
                smoothstep(-0.05 - soft, 0.10, dir.y) * (1.0 - smoothstep(0.55, 0.85 + soft, dir.y));
    col += vec3(2.6, 2.55, 2.4) * win * spread;
    return col;
}

vec3 studio_env(vec3 dir, float roughness) {
    if (env_mode <= 0.0) return dark_studio_env(dir, roughness);
    if (env_mode >= 1.0) return gallery_env(dir, roughness);
    return mix(dark_studio_env(dir, roughness), gallery_env(dir, roughness), env_mode);
}

// Box-filtered checker (Inigo Quilez): anti-aliased at grazing angles.
float filtered_checker(vec2 p) {
    vec2 w = fwidth(p) + 1e-4;
    vec2 i = 2.0 * (abs(fract((p - 0.5 * w) * 0.5) - 0.5) -
                    abs(fract((p + 0.5 * w) * 0.5) - 0.5)) / w;
    return 0.5 - 0.5 * i.x * i.y;
}

// ---------------------------------------------------------------- shadows
const vec2 POISSON[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
    vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));

float shadow_factor(vec3 N, vec3 L) {
    float NdotL = max(dot(N, L), 0.0);
    // Normal-offset in world space + slope-scaled depth bias against acne.
    vec3 offset_pos = world_pos + N * mix(0.10, 0.02, NdotL);
    vec4 ls = light_space * vec4(offset_pos, 1.0);
    vec3 p = ls.xyz / ls.w * 0.5 + 0.5;
    if (p.z > 1.0 || any(lessThan(p.xy, vec2(0.0))) || any(greaterThan(p.xy, vec2(1.0)))) {
        return 1.0;
    }
    float bias = max(0.0009 * (1.0 - NdotL), 0.00015);
    vec2 texel = 1.0 / vec2(textureSize(shadow_map, 0));
    float angle = 6.2831853 * hash12(gl_FragCoord.xy);
    mat2 rot = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    float sum = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 o = rot * POISSON[i] * texel * shadow_softness;
        sum += texture(shadow_map, vec3(p.xy + o, p.z - bias));
    }
    return sum / 16.0;
}

// ---------------------------------------------------------------- AO
float contact_ao() {
    float ao = 1.0;
    if (receive_contact_ao > 0.0) {
        for (int i = 0; i < MAX_OCCLUDERS; ++i) {
            if (i >= occluder_count) break;
            vec4 o = occluders[i];
            float h = max(world_pos.y - o.y + 0.6, 0.0);
            float d = length(world_pos.xz - o.xz);
            float r = o.w * (1.0 + h * 0.5);
            ao *= 1.0 - 0.62 * receive_contact_ao * exp(-(d * d) / (r * r)) / (1.0 + h);
        }
    }
    if (piece_base_height > -999.0) {
        float h = world_pos.y - piece_base_height;
        ao *= mix(0.42, 1.0, smoothstep(0.0, 2.2, h));
    }
    return ao;
}

// ---------------------------------------------------------------- main
void main() {
    vec3 albedo;
    float roughness;
    vec3 N = normalize(normal);

    if (material_kind == 1) {
        // Procedural slate table: dark, faintly veined, satin finish.
        vec2 p = world_pos.xz * 0.045;
        float veins = fbm(p * 3.0 + fbm(p * 1.5) * 2.0);
        albedo = mix(vec3(0.0055, 0.0058, 0.0068), vec3(0.020, 0.019, 0.018), veins);
        roughness = mix(0.32, 0.55, fbm(p * 9.0));
    } else if (material_kind == 2) {
        // Polished marble floor laid on the diagonal, like the gallery photo.
        vec2 p = world_pos.xz / floor_tile;
        vec2 q = vec2(p.x + p.y, p.x - p.y) * 0.70710678;
        float white = filtered_checker(q);
        vec2 f = abs(fract(q) - 0.5);
        float edge = 0.5 - max(f.x, f.y);
        float lod = clamp(length(fwidth(q)) * 6.0, 0.0, 1.0);
        float grout = mix(smoothstep(0.004, 0.012, edge), 1.0, lod);
        float veins = fbm(world_pos.xz * 0.06 + fbm(world_pos.xz * 0.02) * 3.0);
        vec3 white_m = vec3(0.80, 0.79, 0.76) * (0.90 + 0.10 * veins);
        vec3 black_m = vec3(0.010, 0.010, 0.012) + vec3(0.012) * veins;
        albedo = mix(black_m, white_m, white);
        albedo = mix(vec3(0.18), albedo, grout);
        roughness = mix(0.10, 0.16, white) + 0.06 * veins;
    } else {
        albedo = pow(texture(albedo_tex, tex_coord).rgb, vec3(2.2));
        roughness = texture(roughness_tex, tex_coord).g;
        N = perturb_normal(N);
    }
    roughness = clamp(roughness * roughness_scale + roughness_bias, 0.04, 1.0);

    vec3 V = normalize(view_pos - world_pos);
    vec3 L = normalize(light_dir);
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    vec3 F0 = vec3(0.04);
    float shadow = shadow_factor(normalize(normal), L);
    float ao = contact_ao();

    // --- direct key light (Cook-Torrance)
    vec3 F = fresnel_schlick(HdotV, F0);
    float D = distribution_ggx(NdotH, roughness);
    float G = geometry_smith(NdotV, NdotL, roughness);
    vec3 spec = D * G * F / (4.0 * NdotV * max(NdotL, 1e-4) + 1e-4);
    vec3 kD = (1.0 - F);
    vec3 direct = (kD * albedo / PI + spec) * light_color * NdotL * shadow;

    // --- cool fill light from the opposite side (unshadowed, diffuse only)
    vec3 Lf = normalize(vec3(-light_dir.x, 0.55, -light_dir.z));
    direct += albedo / PI * vec3(0.20, 0.24, 0.32) * max(dot(N, Lf), 0.0);

    // --- image-based lighting from the studio environment
    vec3 R = reflect(-V, N);
    vec3 Fr = fresnel_schlick_roughness(NdotV, F0, roughness);
    vec2 brdf = env_brdf_approx(NdotV, roughness);
    vec3 spec_ibl = studio_env(R, roughness) * (Fr * brdf.x + brdf.y);
    vec3 diff_ibl = studio_env(N, 1.0) * albedo * (1.0 - Fr) * 0.9;
    // Environment light is partly blocked by the board shadow too.
    float env_shadow = mix(0.45, 1.0, shadow);
    vec3 ambient = (diff_ibl + spec_ibl * env_shadow) * ao;

    vec3 color = direct * mix(0.75, 1.0, ao) + ambient;

    // --- lacquer clear-coat: a second, very glossy specular layer
    if (clearcoat > 0.0) {
        vec3 Ng = normalize(normal);  // clear-coat ignores the wood normal map
        float cNdotV = max(dot(Ng, V), 1e-4);
        float cNdotL = max(dot(Ng, L), 0.0);
        float cNdotH = max(dot(Ng, H), 0.0);
        float Fc = fresnel_schlick(cNdotV, vec3(0.04)).x * clearcoat;
        float Dc = distribution_ggx(cNdotH, clearcoat_roughness);
        float Gc = geometry_smith(cNdotV, cNdotL, clearcoat_roughness);
        float cc_direct = Dc * Gc * fresnel_schlick(HdotV, vec3(0.04)).x /
                          (4.0 * cNdotV * max(cNdotL, 1e-4) + 1e-4);
        vec3 Rc = reflect(-V, Ng);
        vec3 env = studio_env(Rc, clearcoat_roughness) * env_shadow;
        if (reflection_strength > 0.0) {
            // Planar mirror of the pieces, slightly rippled by the wood normal.
            vec2 suv = gl_FragCoord.xy / viewport_size + (N.xz - Ng.xz) * 0.03;
            vec4 mirror = texture(reflection_tex, suv);
            env = mix(env, mirror.rgb, mirror.a * reflection_strength);
        }
        color = color * (1.0 - Fc) +
                (cc_direct * light_color * cNdotL * shadow * clearcoat + env * Fc) * ao;
    }

    color += tint.rgb * tint.a;

    // Distance fog: alpha carries surface coverage so post_fx can dissolve the
    // floor into the backdrop (gradient or gallery photo) behind it.
    float fog = smoothstep(fog_start, fog_end, length(world_pos.xz));
    frag_color = vec4(color, 1.0 - fog);
}

//@stage depth.vert
#version 330 core

layout (location = 0) in vec3 a_position;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main() {
    gl_Position = projection * view * model * vec4(a_position, 1.0);
}

//@stage depth.frag
#version 330 core

void main() {}

//@stage highlight.frag
#version 330 core

in vec3 world_pos;
in vec3 normal;
in vec2 tex_coord;

out vec4 frag_color;

uniform vec3 highlight_color;
uniform vec3 highlight_center;   // world-space centre of the square / dot
uniform float highlight_half;    // half extent in world units
uniform float highlight_time;
uniform int highlight_mode;      // 0 = square frame, 1 = round dot, 2 = soft fill
uniform float highlight_intensity;

void main() {
    vec2 local = (world_pos.xz - highlight_center.xz) / highlight_half;
    float pulse = 0.78 + 0.22 * sin(highlight_time * 4.0);
    float a;
    if (highlight_mode == 1) {
        float r = length(local);
        a = (1.0 - smoothstep(0.35, 1.0, r)) * pulse;
    } else if (highlight_mode == 2) {
        float edge = max(abs(local.x), abs(local.y));
        a = 0.35 * (1.0 - smoothstep(0.6, 1.0, edge));
    } else {
        float edge = max(abs(local.x), abs(local.y));
        float frame = smoothstep(0.58, 0.80, edge) * (1.0 - smoothstep(0.90, 0.99, edge));
        float fill = 0.10 * (1.0 - smoothstep(0.2, 1.0, edge));
        a = (frame + fill) * pulse;
    }
    // Premultiplied "over" blending (GL_ONE, GL_ONE_MINUS_SRC_ALPHA): reads on
    // both light and dark squares; intensity > 1 adds an HDR glow on top.
    a = clamp(a, 0.0, 1.0);
    frag_color = vec4(highlight_color * a * highlight_intensity, a * min(highlight_intensity, 1.0));
}
