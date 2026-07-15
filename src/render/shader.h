#pragma once
#include "render/renderer.h"

class ShaderManager {
public:
    static Shader* getDefaultShader();
    static Shader* getTerrainShader();
    static Shader* getSkyShader();
    static Shader* getTextShader();
    static Shader* getLineShader();
    static Shader* getShadowShader();
    static Shader* getSpriteShader();
    static Shader* getCloudShader();
    static Shader* getWaterShader();

    static void init();
    static void destroy();

private:
    static Shader* defaultShader;
    static Shader* terrainShader;
    static Shader* skyShader;
    static Shader* textShader;
    static Shader* lineShader;
    static Shader* shadowShader;
    static Shader* spriteShader;
    static Shader* cloudShader;
    static Shader* waterShader;
};
