#version 330 core
// Entities, drawn instanced. The mesh carries a part id: 0 body, 1 head, 2 one merged box (far LOD), 3 an impostor
// quad that faces the camera (farthest LOD). Everything that differs per entity arrives as per-instance attributes.
layout(location = 0) in vec3 a_pos;     // -0.5 to 0.5; the impostor quad spans x -0.5..0.5, y 0..1
layout(location = 1) in vec3 a_normal;
layout(location = 2) in float a_part;
layout(location = 3) in vec3 i_origin;  // feet position relative to the camera
layout(location = 4) in vec2 i_yaw;     // sin, cos
layout(location = 5) in vec2 i_size;    // width, height
layout(location = 6) in vec3 i_color;
layout(location = 7) in vec3 i_accent;
layout(location = 8) in vec4 i_light;   // sky, r, g, b block light, 0 to 1

uniform mat4 u_viewproj;                // projection * rotation-only view, so positions are camera relative
uniform vec3 u_cam_right;
uniform vec3 u_cam_forward;

out vec3 v_normal;
out vec3 v_rel;
flat out vec3 v_color;
flat out vec4 v_light;

const float BODY_FRACTION = 0.62;
const float HEAD_WIDTH_FRACTION = 0.7;
const float HEAD_FORWARD_FRACTION = 0.25;

vec3 turn(vec3 v) { return vec3(v.x * i_yaw.y + v.z * i_yaw.x, v.y, -v.x * i_yaw.x + v.z * i_yaw.y); }

void main() {
    float w = i_size.x, h = i_size.y;
    vec3 rel;
    if (a_part > 2.5) {
        rel = i_origin + vec3(0.0, a_pos.y * h, 0.0) + u_cam_right * (a_pos.x * w * 1.25);
        v_normal = -u_cam_forward;
        v_color = mix(i_color, i_accent, 0.25);
    } else {
        vec3 size, offset;
        if (a_part < 0.5) {
            size = vec3(w, h * BODY_FRACTION, w);
            offset = vec3(0.0, h * BODY_FRACTION * 0.5, 0.0);
            v_color = i_color;
        } else if (a_part < 1.5) {
            float head_h = h * (1.0 - BODY_FRACTION);
            size = vec3(w * HEAD_WIDTH_FRACTION, head_h, w * HEAD_WIDTH_FRACTION);
            offset = vec3(0.0, h * BODY_FRACTION + head_h * 0.5, -w * HEAD_FORWARD_FRACTION);
            v_color = i_accent;
        } else {
            size = vec3(w, h, w);
            offset = vec3(0.0, h * 0.5, 0.0);
            v_color = mix(i_color, i_accent, 0.25);
        }
        rel = i_origin + turn(a_pos * size + offset);
        v_normal = turn(a_normal);
    }
    v_rel = rel;
    v_light = i_light;
    gl_Position = u_viewproj * vec4(rel, 1.0);
}
