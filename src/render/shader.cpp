#include "render/shader.h"
#include "core/console.h"

Shader* ShaderManager::defaultShader = nullptr;
Shader* ShaderManager::terrainShader = nullptr;
Shader* ShaderManager::skyShader = nullptr;
Shader* ShaderManager::textShader = nullptr;
Shader* ShaderManager::lineShader = nullptr;
Shader* ShaderManager::shadowShader = nullptr;
Shader* ShaderManager::spriteShader = nullptr;
Shader* ShaderManager::cloudShader = nullptr;
Shader* ShaderManager::waterShader = nullptr;

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
uniform vec3 uFogColor = vec3(0.75, 0.8, 0.85);
uniform float uFogDensity = 0.01;
uniform float uScreenDoor = 0.0;

uniform sampler2DShadow uShadowMap;
uniform mat4 uShadowMatrix;
uniform float uShadowStrength = 0.5;

uniform float uMetallic = 0.0;
uniform float uRoughness = 0.5;
uniform bool uAlphaTest = false;

out vec4 FragColor;

float shadowPCF(vec4 shadowCoord) {
    vec3 sc = shadowCoord.xyz / shadowCoord.w;
    if (sc.x < 0 || sc.x > 1 || sc.y < 0 || sc.y > 1 || sc.z < 0 || sc.z > 1) return 1.0;
    float s = 0.0;
    vec2 texelSize = 1.0 / textureSize(uShadowMap, 0);
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            s += texture(uShadowMap, vec3(sc.xy + vec2(x, y) * texelSize, sc.z - 0.002));
        }
    }
    return s / 9.0;
}

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
        vec3 V = normalize(uCamPos - vWorldPos);
        vec3 L = normalize(uLightDir);
        vec3 H = normalize(L + V);

        float NdotL = max(dot(N, L), 0.0);
        float NdotV = max(dot(N, V), 0.001);
        float NdotH = max(dot(N, H), 0.001);
        float HdotV = max(dot(H, V), 0.001);

        float shadowFactor = 1.0;
        if (uShadowStrength > 0.0) {
            vec4 shadowCoord = uShadowMatrix * vec4(vWorldPos, 1.0);
            shadowFactor = mix(shadowPCF(shadowCoord), 1.0, 1.0 - uShadowStrength);
        }

        // PBR: Cook-Torrance BRDF
        float metallic = uMetallic;
        float roughness = clamp(uRoughness, 0.04, 1.0);
        vec3 F0 = mix(vec3(0.04), col.rgb, metallic);

        // Diffuse (Lambertian)
        vec3 diffuse = col.rgb * (1.0 - metallic) / 3.14159;

        // Specular: D (GGX), G (Smith-Schlick), F (Schlick)
        float alpha = roughness * roughness;
        float a2 = alpha * alpha;
        float NdotH2 = NdotH * NdotH;
        float D = a2 / (3.14159 * (NdotH2 * (a2 - 1.0) + 1.0) * (NdotH2 * (a2 - 1.0) + 1.0));

        float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
        float G = NdotL / (NdotL * (1.0 - k) + k) * NdotV / (NdotV * (1.0 - k) + k);

        vec3 F = F0 + (1.0 - F0) * pow(1.0 - HdotV, 5.0);

        vec3 specular = D * G * F / (4.0 * NdotV * NdotL + 0.0001);

        vec3 lit = (diffuse + specular) * (1.0 + 1.0 * NdotL * shadowFactor);
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
            float fogFactor = clamp(uFogDensity * dist, 0.0, 1.0);
            lit = mix(lit, uFogColor, fogFactor);
        }
        FragColor = vec4(lit, col.a);
    }
    if (uAlphaTest && FragColor.a <= 0.0) discard;
    // Screen-door transparency (dithered transparency for cloak effect)
    if (uScreenDoor > 0.01) {
        vec2 screenPos = gl_FragCoord.xy;
        float dither = mod(floor(screenPos.x) + floor(screenPos.y), 2.0);
        if (dither < uScreenDoor) discard;
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
out vec3 vWorldPos;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    gl_Position = uProjection * uView * worldPos;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vColor = aColor;
    vWorldPos = worldPos.xyz;
}
)";

static const char* terrainFrag = R"(
#version 330 core
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
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
uniform float uDetailTiling = 32.0;
uniform float uDetailTiling0 = 32.0;
uniform float uDetailTiling1 = 32.0;
uniform float uDetailTiling2 = 32.0;
uniform float uDetailTiling3 = 32.0;

uniform bool uFogEnabled = false;
uniform vec3 uFogColor = vec3(0.75, 0.8, 0.85);
uniform float uFogDensity = 0.01;
uniform vec3 uCamPos = vec3(0);

uniform sampler2DShadow uShadowMap;
uniform mat4 uShadowMatrix;
uniform float uShadowStrength = 0.5;

out vec4 FragColor;

float terrainShadowPCF(vec4 shadowCoord) {
    vec3 sc = shadowCoord.xyz / shadowCoord.w;
    if (sc.x < 0 || sc.x > 1 || sc.y < 0 || sc.y > 1 || sc.z < 0 || sc.z > 1) return 1.0;
    float s = 0.0;
    vec2 texelSize = 1.0 / textureSize(uShadowMap, 0);
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            s += texture(uShadowMap, vec3(sc.xy + vec2(x, y) * texelSize, sc.z - 0.003));
        }
    }
    return s / 9.0;
}

void main() {
    vec4 base;
    if (uUseVertexColor) {
        base = vColor;
    } else {
        vec4 weights = texture(uSplatMap, vUV);
        float total = weights.r + weights.g + weights.b + weights.a;
        if (total > 0.0) weights /= total;
        vec4 c0 = texture(uDetail0, vUV * uDetailTiling0);
        vec4 c1 = texture(uDetail1, vUV * uDetailTiling1);
        vec4 c2 = texture(uDetail2, vUV * uDetailTiling2);
        vec4 c3 = texture(uDetail3, vUV * uDetailTiling3);
        base = c0 * weights.r + c1 * weights.g + c2 * weights.b + c3 * weights.a;
    }

    vec3 N = normalize(vNormal);
    float ndotl = max(dot(N, normalize(uLightDir)), 0.0);
    float shadowFactor = 1.0;
    if (uShadowStrength > 0.0) {
        vec4 shadowCoord = uShadowMatrix * vec4(vWorldPos, 1.0);
        shadowFactor = mix(terrainShadowPCF(shadowCoord), 1.0, 1.0 - uShadowStrength);
    }
    vec3 lighting = vec3(0.3 + 0.7 * ndotl * shadowFactor);
    if (uUseLightmap) {
        vec4 lm = texture(uLightmap, vUV);
        lighting *= lm.r;
    }
    vec3 lit = base.rgb * lighting;
    if (uFogEnabled) {
        float dist = length(vWorldPos - uCamPos);
        float fogFactor = clamp(uFogDensity * dist, 0.0, 1.0);
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

static const char* cloudVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uMVP;
out vec2 vUV;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
}
)";

static const char* cloudFrag = R"(
#version 330 core
in vec2 vUV;
uniform sampler2D uTexture;
uniform float uOpacity;
uniform float uScrollU;
out vec4 FragColor;
void main() {
    vec2 uv = vec2(vUV.x + uScrollU, vUV.y);
    vec4 tex = texture(uTexture, uv);
    FragColor = vec4(tex.rgb, tex.a * uOpacity);
    if (FragColor.a < 0.01) discard;
}
)";

static const char* waterVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uProjection;
uniform mat4 uView;
uniform mat4 uModel;
out vec3 vWorldPos;
out vec3 vNormal;
void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = vec3(0.0, 1.0, 0.0);
    gl_Position = uProjection * uView * worldPos;
}
)";

static const char* waterFrag = R"(
#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uWaterColor;
uniform float uWaterOpacity;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform bool uFogEnabled;
out vec4 FragColor;
void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCamPos - vWorldPos);
    vec3 L = normalize(uSunDir);
    // Fresnel effect: more reflective at grazing angles
    float cosTheta = max(dot(N, V), 0.0);
    float fresnel = pow(1.0 - cosTheta, 3.0) * 0.95 + 0.05;
    // Sky reflection color (approximation)
    vec3 skyReflect = mix(vec3(0.4, 0.6, 0.8), vec3(0.7, 0.8, 0.9), cosTheta);
    // Specular highlight
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 128.0);
    vec3 specular = uSunColor * spec * 1.5;
    // Combine: reflection + water base + specular
    vec3 waterBase = uWaterColor * (1.0 - fresnel);
    vec3 reflColor = skyReflect * fresnel;
    vec3 color = waterBase + reflColor + specular;
    // Fog
    if (uFogEnabled) {
        float dist = length(vWorldPos - uCamPos);
        float fogFactor = clamp(uFogDensity * dist, 0.0, 1.0);
        color = mix(color, uFogColor, fogFactor);
    }
    FragColor = vec4(color, uWaterOpacity);
}
)";

static const char* spriteVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uProjection;
uniform mat4 uView;
out vec2 vUV;
out vec4 vColor;
void main() {
    gl_Position = uProjection * uView * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
}
)";

static const char* spriteFrag = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTexture;
uniform bool uUseTexture = false;
out vec4 FragColor;
void main() {
    vec4 tex = uUseTexture ? texture(uTexture, vUV) : vec4(1.0);
    FragColor = vColor * tex;
    if (FragColor.a < 0.01) discard;
}
)";

static const char* shadowDepthVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uLightMVP;
void main() {
    gl_Position = uLightMVP * vec4(aPos, 1.0);
}
)";

static const char* shadowDepthFrag = R"(
#version 330 core
void main() {
    // Depth is written automatically
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

    shadowShader = new Shader();
    if (!shadowShader->load(shadowDepthVert, shadowDepthFrag)) {
        Console::instance().printf(LogLevel::Error, "Failed to load shadow shader");
    }

    spriteShader = new Shader();
    if (!spriteShader->load(spriteVert, spriteFrag)) {
        Console::instance().printf(LogLevel::Error, "Failed to load sprite shader");
    }

    cloudShader = new Shader();
    if (!cloudShader->load(cloudVert, cloudFrag)) {
        Console::instance().printf(LogLevel::Error, "Failed to load cloud shader");
    }

    waterShader = new Shader();
    if (!waterShader->load(waterVert, waterFrag)) {
        Console::instance().printf(LogLevel::Error, "Failed to load water shader");
    }
}

void ShaderManager::destroy() {
    delete defaultShader; defaultShader = nullptr;
    delete terrainShader; terrainShader = nullptr;
    delete skyShader; skyShader = nullptr;
    delete textShader; textShader = nullptr;
    delete lineShader; lineShader = nullptr;
    delete shadowShader; shadowShader = nullptr;
    delete spriteShader; spriteShader = nullptr;
    delete cloudShader; cloudShader = nullptr;
    delete waterShader; waterShader = nullptr;
}

Shader* ShaderManager::getDefaultShader() { return defaultShader; }
Shader* ShaderManager::getTerrainShader() { return terrainShader; }
Shader* ShaderManager::getSkyShader() { return skyShader; }
Shader* ShaderManager::getTextShader() { return textShader; }
Shader* ShaderManager::getLineShader() { return lineShader; }
Shader* ShaderManager::getShadowShader() { return shadowShader; }
Shader* ShaderManager::getSpriteShader() { return spriteShader; }
Shader* ShaderManager::getCloudShader() { return cloudShader; }
Shader* ShaderManager::getWaterShader() { return waterShader; }
