#version 450

layout(location = 0) in vec3 position;
layout(location = 0) out vec3 worldPosition;

layout(push_constant) uniform Camera {
    mat4 viewProjection;
} camera;

void main()
{
    worldPosition = position;
    gl_Position = camera.viewProjection * vec4(position, 1.0);
}
