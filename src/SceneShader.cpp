#include "SceneShader.h"

#include <string>

namespace
{
std::string FindAssetFile(const std::string& relative)
{
    const std::string candidates[] { relative, "../" + relative, "../../" + relative };
    for (const std::string& candidate : candidates)
    {
        if (FileExists(candidate.c_str()))
        {
            return candidate;
        }
    }
    return {};
}

void SetVector(Shader shader, int location, const float* value, int type)
{
    if (location >= 0)
    {
        SetShaderValue(shader, location, value, type);
    }
}
}

bool SceneShader::Initialize()
{
    if (ready_)
    {
        return true;
    }
    const std::string vertexPath = FindAssetFile("assets/shaders/lighting.vs");
    const std::string fragmentPath = FindAssetFile("assets/shaders/lighting.fs");
    if (vertexPath.empty() || fragmentPath.empty())
    {
        TraceLog(LOG_WARNING, "SHADER: scene lighting files missing; default renderer enabled");
        return false;
    }

    shader_ = LoadShader(vertexPath.c_str(), fragmentPath.c_str());
    if (shader_.id == 0)
    {
        TraceLog(LOG_WARNING, "SHADER: scene lighting failed; default renderer enabled");
        return false;
    }
    viewPositionLocation_ = GetShaderLocation(shader_, "viewPos");
    lightDirectionLocation_ = GetShaderLocation(shader_, "lightDirection");
    lightColorLocation_ = GetShaderLocation(shader_, "lightColor");
    ambientLocation_ = GetShaderLocation(shader_, "ambientColor");
    fogColorLocation_ = GetShaderLocation(shader_, "fogColor");
    fogDensityLocation_ = GetShaderLocation(shader_, "fogDensity");
    timeLocation_ = GetShaderLocation(shader_, "time");
    ready_ = true;
    return true;
}

void SceneShader::Shutdown()
{
    if (ready_ && shader_.id != 0)
    {
        UnloadShader(shader_);
    }
    shader_ = {};
    ready_ = false;
}

void SceneShader::Begin(const Camera3D& camera, Color fogColor, float fogDensity) const
{
    if (!ready_)
    {
        return;
    }
    const float view[] { camera.position.x, camera.position.y, camera.position.z };
    const float lightDirection[] { -0.48f, -0.82f, 0.31f };
    const float lightColor[] { 1.00f, 0.96f, 0.86f, 1.0f };
    const float ambient[] { 0.43f, 0.49f, 0.58f, 1.0f };
    const float fog[] {
        static_cast<float>(fogColor.r) / 255.0f,
        static_cast<float>(fogColor.g) / 255.0f,
        static_cast<float>(fogColor.b) / 255.0f,
        1.0f
    };
    SetVector(shader_, viewPositionLocation_, view, SHADER_UNIFORM_VEC3);
    SetVector(shader_, lightDirectionLocation_, lightDirection, SHADER_UNIFORM_VEC3);
    SetVector(shader_, lightColorLocation_, lightColor, SHADER_UNIFORM_VEC4);
    SetVector(shader_, ambientLocation_, ambient, SHADER_UNIFORM_VEC4);
    SetVector(shader_, fogColorLocation_, fog, SHADER_UNIFORM_VEC4);
    if (fogDensityLocation_ >= 0)
    {
        SetShaderValue(shader_, fogDensityLocation_, &fogDensity, SHADER_UNIFORM_FLOAT);
    }
    const float time = static_cast<float>(GetTime());
    if (timeLocation_ >= 0)
    {
        SetShaderValue(shader_, timeLocation_, &time, SHADER_UNIFORM_FLOAT);
    }
    BeginShaderMode(shader_);
}

void SceneShader::End() const
{
    if (ready_)
    {
        EndShaderMode();
    }
}

Shader SceneShader::GetShader() const
{
    return ready_ ? shader_ : Shader {};
}

bool SceneShader::IsReady() const
{
    return ready_;
}
