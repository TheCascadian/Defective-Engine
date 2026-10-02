layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
uniform vec2 u_screen;
out vec2 v_uv;
out vec4 v_col;
void main() {
    vec2 p = a_pos / u_screen * 2.0 - 1.0;
    gl_Position = vec4(p.x, -p.y, 0.0, 1.0);
    v_uv = a_uv;
    v_col = a_col;
}
