#version 460

layout(location = 0) out vec2 screen;

void main() {
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    screen = corner * 2.0 - 1.0;
    gl_Position = vec4(screen, 0.0, 1.0);
}
