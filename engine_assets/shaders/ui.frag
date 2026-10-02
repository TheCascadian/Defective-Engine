uniform sampler2D u_tex;
uniform int u_mode; // 0: single channel glyph atlas used as alpha, 1: plain RGBA
in vec2 v_uv;
in vec4 v_col;
out vec4 o_col;
void main() {
    vec4 t = texture(u_tex, v_uv);
    if (u_mode == 0) t = vec4(1.0, 1.0, 1.0, t.r);
    o_col = t * v_col;
}
