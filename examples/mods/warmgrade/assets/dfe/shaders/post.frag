#version 330 core
// Shader pack example: replaces the resolve pass with a warm colour grade and a soft vignette.
// It keeps the engine's uniforms, so dynamic resolution still works. Copy the engine's post.frag as a starting
// point when you want light shafts too.
in vec2 v_uv;

uniform sampler2D u_color;
uniform vec2 u_scale;
uniform vec2 u_texel;

out vec4 o_color;

void main() {
    vec2 q = clamp(v_uv * u_scale, u_texel * 0.5, u_scale - u_texel * 0.5);
    vec3 rgb = texture(u_color, q).rgb;
    float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb = mix(vec3(luma), rgb, 1.15);                        // a little more saturation
    rgb *= vec3(1.06, 1.0, 0.90);                            // warm tint
    float edge = length(v_uv - 0.5);
    rgb *= 1.0 - smoothstep(0.35, 0.85, edge) * 0.45;        // vignette
    o_color = vec4(rgb, 1.0);
}
