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
layout(location = 4) in vec2 aUV2;

uniform mat4 uProjection;
uniform mat4 uView;
uniform mat4 uModel;
uniform vec3 uCamPos = vec3(0);

out vec3 vNormal;
out vec2 vUV;
out vec2 vUV2;
out vec4 vColor;
out vec3 vWorldPos;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    gl_Position = uProjection * uView * worldPos;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vUV2 = aUV2;
    vColor = aColor;
    vWorldPos = worldPos.xyz;
}
)";

static const char* defaultFrag = R"(
#version 330 core
in vec3 vNormal;
in vec2 vUV;
in vec2 vUV2;
in vec4 vColor;
in vec3 vWorldPos;

uniform sampler2D uTexture;
uniform sampler2D uLightmap;
uniform sampler2D uEnvMap;
uniform bool uUseTexture;
uniform bool uUseLightmap = false;
uniform bool uUseEnvMap = false;
uniform bool uSelfIlluminated = false;
uniform vec3 uLightDir = vec3(0.5, 0.8, 0.6);
uniform vec3 uCamPos = vec3(0);
uniform vec4 uTint = vec4(1.0);

uniform bool uFogEnabled = false;
uniform vec3 uFogColor = vec3(0.5, 0.6, 0.7);
uniform float uFogDensity = 0.01;

out vec4 FragColor;

void main() {
    vec4 texColor = uUseTexture ? texture(uTexture, vUV) : vec4(1.0);
    vec4 col = vColor * texColor * uTint;
    if (uUseLightmap) {
        vec4 lm = texture(uLightmap, vUV2);
        col.rgb = col.rgb * (0.5 + 0.5 * lm.rgb);
    }
    if (uSelfIlluminated) {
        FragColor = vec4(col.rgb, col.a);
    } else {
        vec3 N = normalize(vNormal);
        float diff = max(dot(N, normalize(uLightDir)), 0.0);
        vec3 lit = col.rgb * (0.3 + 0.7 * diff);
        if (uUseEnvMap) {
            vec3 V = normalize(uCamPos - vWorldPos);
            vec3 R = reflect(-V, N);
            float m = 2.0 * sqrt(R.x*R.x + R.y*R.y + (R.z + 1.0)*(R.z + 1.0));
            vec2 envUV = vec2(R.x / m + 0.5, R.y / m + 0.5);
            vec4 env = texture(uEnvMap, envUV);
            lit = mix(lit, env.rgb, 0.25);
        }
        if (uFogEnabled) {
            float dist = length(vWorldPos - uCamPos);
            float fogFactor = 1.0 - exp(-uFogDensity * uFogDensity * dist * dist);
            fogFactor = clamp(fogFactor, 0.0, 1.0);
            lit = mix(lit, uFogColor, fogFactor);
        }
        FragColor = vec4(lit, col.a);
    }
}
)";

static const char* terrainVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec2 aUV2;

uniform mat4 uProjection;
uniform mat4 uView;
uniform mat4 uModel;

out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out float vHeight;
out vec3 vWorldPos;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    gl_Position = uProjection * uView * worldPos;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vColor = aColor;
    vHeight = aPos.y;
    vWorldPos = worldPos.xyz;
}
)";

static const char* terrainFrag = R"(
#version 330 core
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
in float vHeight;
in vec3 vWorldPos;

uniform sampler2D uSplatMap;
uniform sampler2D uDetail0;
uniform sampler2D uDetail1;
uniform sampler2D uDetail2;
uniform sampler2D uDetail3;
uniform sampler2D uLightmap;
uniform bool uUseLightmap = false;
uniform bool uUseVertexColor = false;
uniform vec3 uLightDir = vec3(0.5, 0.8, 0.6);

uniform bool uFogEnabled = false;
uniform vec3 uFogColor = vec3(0.5, 0.6, 0.7);
uniform float uFogDensity = 0.01;
uniform vec3 uCamPos = vec3(0);

out vec4 FragColor;

void main() {
    vec4 base;
    if (uUseVertexColor) {
        base = vColor;
    } else {
        vec4 weights = texture(uSplatMap, vUV);
        float total = weights.r + weights.g + weights.b + weights.a;
        if (total > 0.0) weights /= total;
        vec4 c0 = texture(uDetail0, vUV * 32.0);
        vec4 c1 = texture(uDetail1, vUV * 32.0);
        vec4 c2 = texture(uDetail2, vUV * 32.0);
        vec4 c3 = texture(uDetail3, vUV * 32.0);
        base = c0 * weights.r + c1 * weights.g + c2 * weights.b + c3 * weights.a;
    }

    vec3 N = normalize(vNormal);
    float ndotl = max(dot(N, normalize(uLightDir)), 0.0);
    vec3 lighting = vec3(0.3 + 0.7 * ndotl);
    if (uUseLightmap) {
        vec4 lm = texture(uLightmap, vUV);
        lighting *= lm.r;
    }
    vec3 lit = base.rgb * lighting;
    if (uFogEnabled) {
        float dist = length(vWorldPos - uCamPos);
        float fogFactor = 1.0 - exp(-uFogDensity * uFogDensity * dist * dist);
        fogFactor = clamp(fogFactor, 0.0, 1.0);
        lit = mix(lit, uFogColor, fogFactor);
    }
    FragColor = vec4(lit, 1.0);
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
uniform bool uUseGradient = false;
uniform vec3 uGradTop = vec3(0.3, 0.5, 0.8);
uniform vec3 uGradBot = vec3(0.7, 0.8, 0.9);
out vec4 FragColor;

void main() {
    if (uUseGradient) {
        float t = abs(vUV.y) * 0.8 + 0.1;
        FragColor = vec4(mix(uGradBot, uGradTop, t), 1.0);
    } else {
        FragColor = texture(uSkybox, vUV);
    }
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
