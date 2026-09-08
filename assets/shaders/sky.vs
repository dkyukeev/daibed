#version 330
in vec3 vertexPosition;
uniform mat4 mvp;
out vec3 worldPosition;
void main()
{
    // DrawSphereEx is CPU-transformed by rlgl's immediate batch.
    worldPosition = vertexPosition;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
