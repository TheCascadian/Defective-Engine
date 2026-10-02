layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col;
uniform mat4 u_viewproj;
out vec4 v_col;
void main() {
    gl_Position = u_viewproj * vec4(a_pos, 1.0);
    v_col = a_col;
}
