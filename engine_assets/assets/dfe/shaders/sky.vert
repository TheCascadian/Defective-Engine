#version 330 core
// Fullscreen triangle generated from gl_VertexID, so the pass needs no vertex buffer. It is placed at the far
// plane: drawn after the opaque terrain with a LEQUAL test, it is rejected early everywhere terrain already is.
out vec2 v_ndc;

void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_ndc = p * 2.0 - 1.0;
    gl_Position = vec4(v_ndc, 1.0, 1.0);
}
