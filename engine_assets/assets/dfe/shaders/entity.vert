#version 330 core
// One box of an entity. The unit cube is scaled and placed per part, then turned by the entity's yaw about Y.
layout(location = 0) in vec3 a_pos;     // -0.5 to 0.5
layout(location = 1) in vec3 a_normal;

uniform mat4 u_viewproj;                // projection * rotation-only view, so positions are camera relative
uniform vec3 u_origin;                  // feet position relative to the camera
uniform vec2 u_yaw;                     // sin, cos
uniform vec3 u_part_size;
uniform vec3 u_part_offset;             // centre of the part in entity space, before the yaw turn

out vec3 v_normal;
out vec3 v_rel;

vec3 turn(vec3 v) { return vec3(v.x * u_yaw.y + v.z * u_yaw.x, v.y, -v.x * u_yaw.x + v.z * u_yaw.y); }

void main() {
    vec3 rel = u_origin + turn(a_pos * u_part_size + u_part_offset);
    v_normal = turn(a_normal);
    v_rel = rel;
    gl_Position = u_viewproj * vec4(rel, 1.0);
}
