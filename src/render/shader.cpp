#include "render/shader.h"
#include "core/console.h"

Shader* ShaderManager::defaultShader = nullptr;
Shader* ShaderManager::terrainShader = nullptr;
Shader* ShaderManager::skyShader = nullptr;
Shader* ShaderManager::textShader = nullptr;
Shader* ShaderManager::lineShader = nullptr;

static const char* defaultVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

uniform mat4 uProjection;
uniform mat4 uView;
uniform mat4 uModel;

out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;

void main() {
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vColor = aColor;
}
)";

static const char* defaultFrag = R"(
#version 330 core
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;

uniform sampler2D uTexture;
uniform sampler2D uLightmap;
uniform bool uUseTexture;
uniform bool uUseLightmap = false;
uniform bool uSelfIlluminated = false;
uniform vec3 uLightDir = vec3(0.5, 0.8, 0.6);

out vec4 FragColor;

void main() {
    vec4 texColor = uUseTexture ? texture(uTexture, vUV) : vec4(1.0);
    vec4 col = vColor * texColor;
    if (uUseLightmap) {
        vec4 lm = texture(uLightmap, vUV);
        col.rgb = col.rgb * (0.5 + 0.5 * lm.rgb);
    }
    if (uSelfIlluminated) {
        FragColor = vec4(col.rgb, col.a);
    } else {
        vec3 N = normalize(vNormal);
        float diff = max(dot(N, normalize(uLightDir)), 0.0);
        FragColor = vec4(col.rgb * (0.3 + 0.7 * diff), col.a);
    }
}
)";

static const char* terrainVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

uniform mat4 uProjection;
uniform mat4 uView;
uniform mat4 uModel;

out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out float vHeight;

void main() {
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vColor = aColor;
    vHeight = aPos.y;
}
)";

static const char* terrainFrag = R"(
#version 330 core
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
in float vHeight;

uniform sampler2D uSplatMap;
uniform sampler2D uDetail0;
uniform sampler2D uDetail1;
uniform sampler2D uDetail2;
uniform sampler2D uDetail3;
uniform sampler2D uLightmap;
uniform bool uUseLightmap = false;
uniform vec3 uLightDir = vec3(0.5, 0.8, 0.6);

out vec4 FragColor;

void main() {
    vec4 weights = texture(uSplatMap, vUV);
    float total = weights.r + weights.g + weights.b + weights.a;
    if (total > 0.0) weights /= total;

    vec4 c0 = texture(uDetail0, vUV * 32.0);
    vec4 c1 = texture(uDetail1, vUV * 32.0);
    vec4 c2 = texture(uDetail2, vUV * 32.0);
    vec4 c3 = texture(uDetail3, vUV * 32.0);
    vec4 base = c0 * weights.r + c1 * weights.g + c2 * weights.b + c3 * weights.a;

    vec3 N = normalize(vNormal);
    float ndotl = max(dot(N, normalize(uLightDir)), 0.0);
    vec3 lighting = vec3(0.3 + 0.7 * ndotl);
    if (uUseLightmap) {
        vec4 lm = texture(uLightmap, vUV);
        lighting *= lm.r;
    }
    FragColor = vec4(base.rgb * lighting, 1.0);
}
)";

static const char* skyVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uProjection;
uniform mat4 uView;
out vec3 vUV;

void main() {
    vec4 p = uProjection * mat4(mat3(uView)) * vec4(aPos, 1.0);
    gl_Position = p.xyww;
    vUV = aPos;
}
)";

static const char* skyFrag = R"(
#version 330 core
in vec3 vUV;
uniform samplerCube uSkybox;
out vec4 FragColor;

void main() {
    FragColor = texture(uSkybox, vUV);
}
)";

static const char* textVert = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uProjection;
out vec2 vUV;

void main() {
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)";

static const char* textFrag = R"(
#version 330 core
in vec2 vUV;
uniform sampler2D uTexture;
uniform vec4 uColor;
out vec4 FragColor;

void main() {
    FragColor = uColor * texture(uTexture, vUV);
}
)";

static const char* lineVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uProjection;
uniform mat4 uView;
uniform vec3 uColor;
out vec3 vColor;

void main() {
    gl_Position = uProjection * uView * vec4(aPos, 1.0);
    vColor = uColor;
}
)";

static const char* lineFrag = R"(
#version 330 core
in vec3 vColor;
out vec4 FragColor;

void main() {
    FragColor = vec4(vColor, 1.0);
}
)";

void ShaderManager::init() {
    defaultShader = new Shader();
    if (!defaultShader->load(defaultVert, defaultFrag)) {
        Console::instance().printf(LogLevel::Error, "Failed to load default shader");
    }

    terrainShader = new Shader();
    if (!terrainShader->load(terrainVert, terrainFrag)) {
        Console::instance().printf(LogLevel::Error, "Failed to load terrain shader");
    }

    skyShader = new Shader();
    if (!skyShader->load(skyVert, skyFrag)) {
        Console::instance().printf(LogLevel::Error, "Failed to load sky shader");
    }

    textShader = new Shader();
    if (!textShader->load(textVert, textFrag)) {
        Console::instance().printf(LogLevel::Error, "Failed to load text shader");
    }

    lineShader = new Shader();
    if (!lineShader->load(lineVert, lineFrag)) {
        Console::instance().printf(LogLevel::Error, "Failed to load line shader");
    }
}

void ShaderManager::destroy() {
    delete defaultShader; defaultShader = nullptr;
    delete terrainShader; terrainShader = nullptr;
    delete skyShader; skyShader = nullptr;
    delete textShader; textShader = nullptr;
    delete lineShader; lineShader = nullptr;
}

Shader* ShaderManager::getDefaultShader() { return defaultShader; }
Shader* ShaderManager::getTerrainShader() { return terrainShader; }
Shader* ShaderManager::getSkyShader() { return skyShader; }
Shader* ShaderManager::getTextShader() { return textShader; }
Shader* ShaderManager::getLineShader() { return lineShader; }
