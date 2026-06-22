#pragma once

#include "raylib.h"

class SceneShader
{
public:
    bool Initialize();
    void Shutdown();
    void Begin(const Camera3D& camera, Color fogColor, float fogDensity) const;
    void End() const;
    Shader GetShader() const;
    bool IsReady() const;

private:
    Shader shader_ {};
    int viewPositionLocation_ = -1;
    int lightDirectionLocation_ = -1;
    int lightColorLocation_ = -1;
    int ambientLocation_ = -1;
    int fogColorLocation_ = -1;
    int fogDensityLocation_ = -1;
    int timeLocation_ = -1;
    bool ready_ = false;
};
