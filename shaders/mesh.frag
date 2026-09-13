#version 450

layout(location = 0) in vec3 worldPosition;
layout(location = 0) out vec4 color;

void main()
{
    vec3 n = normalize(cross(dFdx(worldPosition), dFdy(worldPosition)));
    if (!gl_FrontFacing) {
        n = -n;
    }

    float light = 0.32 + 0.55 * abs(dot(n, normalize(vec3(0.4, -0.6, 0.8))))
        + 0.13 * abs(dot(n, normalize(vec3(-0.8, 0.2, 0.3))));

    color = vec4(vec3(0.64, 0.72, 0.79) * light, 1.0);
}
