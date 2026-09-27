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

// Tribes 2 shape lighting (SceneObject::installLights), gamma space and
// per vertex like the engine's fixed-function GL lights.
uniform int uShapeLightMode = 0;          // 0 sun, 1 under a roof, 2 terrain
uniform vec3 uShapeLightColor = vec3(1.0); // probed, slewed colour
uniform float uShapeBoundRadius = 1.0;
uniform vec3 uLightDir = vec3(0.5, 0.8, 0.6);
uniform vec3 uSunColor = vec3(1.0);
uniform vec3 uAmbient = vec3(0.3);
uniform vec3 uShapeInteriorDir = vec3(0.0, 1.0, 0.0); // toward the indoor light
uniform int uPointLightCount = 0;
uniform vec3 uPointLightPos[8];
uniform vec3 uPointLightColor[8];
uniform vec3 uPointLightParams[8]; // radius, falloff, unused

out vec3 vNormal;
out vec2 vUV;
out vec2 vUV2;
out vec4 vColor;
out vec3 vWorldPos;
out vec3 vShapeLight;

vec3 shapeLighting(vec3 n, vec3 position) {
    vec3 lighting;
    if (uShapeLightMode == 1) {
        lighting = uShapeLightColor * 0.7
            + uShapeLightColor * 0.3 * max(dot(n, uShapeInteriorDir), 0.0);
    } else {
        float brightness = uShapeLightMode == 2
            ? clamp((uShapeLightColor.r + uShapeLightColor.g + uShapeLightColor.b) / 3.0, 0.0, 1.0)
            : 1.0;
        float ambientAverage = (uAmbient.r + uAmbient.g + uAmbient.b) / 3.0;
        vec3 diffuse = uSunColor * clamp(brightness - ambientAverage, 0.0, 1.0);
        lighting = uAmbient + diffuse * max(dot(n, normalize(uLightDir)), 0.0);
    }
    // Point lights contribute colour x radius / distance.
    for (int i = 0; i < uPointLightCount; ++i) {
        float radius = uPointLightParams[i].x;
        if (radius <= 0.0) continue;
        vec3 toLight = uPointLightPos[i] - position;
        float dist = length(toLight);
        if (dist > radius + uShapeBoundRadius) continue;
        float d = max(dist, 1e-3);
        lighting += uPointLightColor[i] * (radius / d) * max(dot(n, toLight / d), 0.0);
    }
    return clamp(lighting, 0.0, 1.0);
}

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    gl_Position = uProjection * uView * worldPos;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vUV2 = aUV2;
    vColor = aColor;
    vWorldPos = worldPos.xyz;
    vec3 n = length(vNormal) > 1e-6 ? normalize(vNormal) : vec3(0.0, 1.0, 0.0);
    vShapeLight = shapeLighting(n, worldPos.xyz);
}
)";

static const char* defaultFrag = R"(
#version 330 core
in vec3 vNormal;
in vec2 vUV;
in vec2 vUV2;
in vec4 vColor;
in vec3 vWorldPos;
in vec3 vShapeLight;

uniform sampler2D uTexture;
uniform sampler2D uLightmap;
uniform sampler2D uEnvMap;
uniform bool uUseTexture;
uniform bool uUseLightmap = false;
uniform bool uInterior = false;
uniform bool uInteriorOutsideVisible = false;
uniform bool uUseEnvMap = false;
uniform bool uSelfIlluminated = false;
uniform vec3 uLightDir = vec3(0.5, 0.8, 0.6);
uniform vec3 uSunColor = vec3(1.0);
uniform vec3 uAmbient = vec3(0.3);
uniform vec3 uCamPos = vec3(0);
uniform vec4 uTint = vec4(1.0);
uniform int uPointLightCount = 0;
uniform vec3 uPointLightPos[8];
uniform vec3 uPointLightColor[8];
uniform vec3 uPointLightParams[8]; // radius, falloff, unused

uniform bool uFogEnabled = false;
uniform vec3 uFogColor = vec3(0.75, 0.8, 0.85);
uniform float uFogDensity = 0.01;
uniform float uFogStart = -1.0;
uniform float uFogEnd = -1.0;
uniform vec4 uFogVolume0 = vec4(0.0);
uniform vec4 uFogVolume1 = vec4(0.0);
uniform vec4 uFogVolume2 = vec4(0.0);
uniform float uScreenDoor = 0.0;

uniform sampler2DShadow uShadowMap;
uniform mat4 uShadowMatrix;
uniform float uShadowStrength = 0.5;

uniform float uMetallic = 0.0;
uniform float uRoughness = 0.5;
uniform bool uAlphaTest = false;
uniform float uAlphaTestThreshold = 0.0001;
uniform float uReflectionAmount = 0.0;
uniform bool uDebugInterior = false;
uniform bool uDebugLightmap = false;
uniform bool uDebugTex = false;
uniform bool uDebugTexColor = false;
uniform bool uDebugTexOnly = false;
uniform bool uDebugLightmapContent = false;

out vec4 FragColor;

uniform float uFogRowBase = 0.0;
uniform float uFogRowStep = 0.0;
// Volume fog at one height: the distance travelled through each volume's
// height band (similar triangles), times percentage / visibleDistance.
float fogVolumesAt(float height, float dist) {
    float result = 0.0;
    vec4 volumes[3] = vec4[3](uFogVolume0, uFogVolume1, uFogVolume2);
    float deltaY = abs(height - uCamPos.y);
    for (int i = 0; i < 3; ++i) {
        vec4 volume = volumes[i];
        if (volume.x <= 0.0 || volume.z <= volume.y) continue;
        if (deltaY > 0.01) {
            float low = max(min(uCamPos.y, height), volume.y);
            float high = min(max(uCamPos.y, height), volume.z);
            if (high > low) result += dist * (high - low) / deltaY * volume.x * volume.w;
        } else if (uCamPos.y >= volume.y && uCamPos.y <= volume.z) {
            result += dist * volume.x * volume.w;
        }
    }
    return min(result, 1.0);
}
// SceneGraph::buildFogTexture samples volume fog into 64 rows spanning the
// terrain's height range and filters bilinearly: blend the two nearest rows.
float torqueVolumeFog(float height, float dist) {
    if (uFogRowStep <= 0.0) return fogVolumesAt(height, dist);
    float rowF = (height - uFogRowBase) / uFogRowStep;
    float row0 = floor(rowF);
    float h0 = uFogRowBase + row0 * uFogRowStep;
    return mix(fogVolumesAt(h0, dist), fogVolumesAt(h0 + uFogRowStep, dist), rowF - row0);
}

float authoredVolumeFog(vec3 point) {
    return torqueVolumeFog(point.y, length(point - uCamPos));
}

// Torque SceneState::getHaze: none within fogDistance, then a quadratic
// ramp to visibleDistance (geometry beyond it is not drawn).
float torqueHaze(float distance) {
    if (uFogStart < 0.0 || uFogEnd <= uFogStart || distance <= uFogStart) return 0.0;
    float f = (distance - uFogStart) / (uFogEnd - uFogStart) - 1.0;
    return clamp(1.0 - f * f, 0.0, 1.0);
}

uniform bool uFogAdditive = false;

vec3 applyAuthoredFog(vec3 color, float distance) {
    if (uFogStart >= 0.0 && uFogEnd > uFogStart && distance >= uFogEnd) discard;
    float haze = torqueHaze(distance);
    float volume = authoredVolumeFog(vWorldPos);
    float factor = volume + min(haze, 1.0 - volume);
    // Additive and subtractive materials fade by 1 - fog instead of mixing
    // toward the fog colour (tsMesh.cc disables their fog stage).
    if (uFogAdditive) return color * (1.0 - factor);
    return mix(color, uFogColor, factor);
}

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
    if (uDebugInterior) {
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }
    if (uDebugLightmap) {
        // Show lightmap UVs as color: red=U, green=V, blue=0
        // Output raw UV2 (not fract) to see actual values
        FragColor = vec4(clamp(vUV2.x, 0.0, 1.0), clamp(vUV2.y, 0.0, 1.0), 0.0, 1.0);
        return;
    }
    if (uDebugTex) {
        // Show raw UV1 values: red=U, green=V (wrapped to [0,1] for visualization)
        float u = fract(vUV.x);
        float vv = fract(vUV.y);
        FragColor = vec4(u, vv, 0.0, 1.0);
        return;
    }
    if (uDebugTexColor) {
        vec4 texColor = uUseTexture ? texture(uTexture, vUV) : vec4(1.0);
        if (uUseTexture) {
            vec2 ts = textureSize(uTexture, 0);
            // Encode: R = fract(UV.x)*255/256, G = fract(UV.y)*255/256, B = texColor.r
            FragColor = vec4(fract(vUV.x), fract(vUV.y), texColor.r, 1.0);
        } else
            FragColor = vec4(0.5, 0.5, 0.5, 1.0);
        return;
    }
    if (uDebugTexOnly) {
        // Render texture only, no lightmap
        vec4 texColor = uUseTexture ? texture(uTexture, vUV) : vec4(1.0);
        FragColor = vec4(texColor.rgb * 0.3, texColor.a);
        return;
    }
    if (uDebugLightmapContent) {
        vec3 lm = texture(uLightmap, vUV2).rgb;
        if (uUseLightmap)
            FragColor = vec4(lm, 1.0);
        else
            FragColor = vec4(0.5, 0.5, 0.5, 1.0);
        return;
    }
    if (uSelfIlluminated) {
        FragColor = vec4(col.rgb, col.a);
    } else if (uInterior) {
        // Torque interior (.dif) formula — output = clamp(lighting * texture).
        // lighting = clamp(sceneLighting) + lightmap  (sceneLighting in gamma
        // space: sunColor*NdotL + ambient). Matches t2-mapper interiorMaterial.ts.
        vec3 N = normalize(vNormal);
        vec3 L = normalize(uLightDir);
        float NdotL = max(dot(N, L), 0.0);

        // Scene lighting, clamped to [0,1] BEFORE adding the lightmap
        vec3 sceneLighting = clamp(uSunColor * NdotL + uAmbient, 0.0, 1.0);
        for (int i = 0; i < uPointLightCount; ++i) {
            vec3 toLight = uPointLightPos[i] - vWorldPos;
            float distance = length(toLight);
            vec3 Lp = distance > 0.0001 ? toLight / distance : N;
            float edge = clamp(1.0 - distance / uPointLightParams[i].x, 0.0, 1.0);
            float attenuation = pow(edge, max(0.1, uPointLightParams[i].y));
            sceneLighting += uPointLightColor[i] * max(dot(N, Lp), 0.0) * attenuation;
        }
        sceneLighting = clamp(sceneLighting, 0.0, 1.0);

        float shadowFactor = 1.0;
        if (uShadowStrength > 0.0) {
            vec4 shadowCoord = uShadowMatrix * vec4(vWorldPos, 1.0);
            shadowFactor = mix(shadowPCF(shadowCoord), 1.0, 1.0 - uShadowStrength);
        }
        sceneLighting *= shadowFactor;

        vec3 lighting = uInteriorOutsideVisible ? sceneLighting : vec3(0.0);
        if (uUseLightmap)
            lighting = clamp(lighting + texture(uLightmap, vUV2).rgb, 0.0, 1.0);
        vec3 lit = clamp(lighting * col.rgb, 0.0, 1.0);

        if (uFogEnabled) {
            float dist = length(vWorldPos - uCamPos);
            lit = applyAuthoredFog(lit, dist);
        }
        FragColor = vec4(lit, col.a);
    } else {
        // Tribes 2 shapes: interpolated gamma-space vertex lighting times
        // the texture, clamped. Shapes are never shadow-mapped.
        vec3 N = normalize(vNormal);
        vec3 lit = clamp(vShapeLight * col.rgb, 0.0, 1.0);
        if (uUseEnvMap) {
            vec3 V = normalize(uCamPos - vWorldPos);
            vec3 R = reflect(-V, N);
            float m = 2.0 * sqrt(R.x*R.x + R.y*R.y + (R.z + 1.0)*(R.z + 1.0));
            vec2 envUV = vec2(R.x / m + 0.5, R.y / m + 0.5);
            vec4 env = texture(uEnvMap, envUV);
            float baseAlpha = uUseTexture ? texture(uTexture, vUV).a : 1.0;
            float factor = clamp(baseAlpha * uReflectionAmount, 0.0, 1.0);
            lit = mix(lit, env.rgb, factor);
        }
        if (uFogEnabled) {
            float dist = length(vWorldPos - uCamPos);
            lit = applyAuthoredFog(lit, dist);
        }
        FragColor = vec4(lit, col.a);
    }
    if (uAlphaTest && FragColor.a < uAlphaTestThreshold) discard;
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
// TerrainBlock detail texture fade (drawn over the terrain near the eye).
uniform bool uUseOverlayDetail = false;
uniform float uSquareSize = 8.0;
uniform float uViewportHeight = 1080.0;
uniform vec3 uCamPos;
uniform bool uFogEnabled = false;
uniform float uFogStart = -1.0;
uniform float uFogEnd = -1.0;
uniform vec4 uFogVolume0 = vec4(0.0);
uniform vec4 uFogVolume1 = vec4(0.0);
uniform vec4 uFogVolume2 = vec4(0.0);

out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out vec3 vWorldPos;
out float vDetailFade;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    gl_Position = uProjection * uView * worldPos;
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vColor = aColor;
    vWorldPos = worldPos.xyz;
    vDetailFade = 0.0;
    if (uUseOverlayDetail) {
        // dglProjectRadius(1, 1) with the viewport and projection scale.
        float detailDistance = uSquareSize * uViewportHeight * uProjection[1][1] / 128.0
            - floor(uSquareSize / 2.0);
        float dist = length(worldPos.xyz - uCamPos);
        float fade = detailDistance > 0.0 ? clamp(1.0 - dist / detailDistance, 0.0, 1.0) : 0.0;
        if (uFogEnabled) {
            float haze = (uFogEnd > uFogStart && dist >= uFogEnd) ? 1.0 : 0.0;
            if (uFogStart >= 0.0 && uFogEnd > uFogStart && dist > uFogStart && dist < uFogEnd) {
                float f = (dist - uFogStart) / (uFogEnd - uFogStart) - 1.0;
                haze = 1.0 - f * f;
            }
            vec4 volumes[3] = vec4[3](uFogVolume0, uFogVolume1, uFogVolume2);
            float height = worldPos.y;
            float deltaHeight = abs(height - uCamPos.y);
            for (int i = 0; i < 3; ++i) {
                vec4 v = volumes[i];
                if (v.x <= 0.0) continue;
                if (deltaHeight > 0.01) {
                    float overlap = max(0.0, min(max(height, uCamPos.y), v.z) - max(min(height, uCamPos.y), v.y));
                    haze += dist * overlap / deltaHeight * v.x * v.w;
                } else if (uCamPos.y >= v.y && uCamPos.y <= v.z) {
                    haze += dist * v.x * v.w;
                }
            }
            fade *= 1.0 - min(haze, 1.0);
        }
        // The original detail pass interpolates a byte-valued vertex colour.
        vDetailFade = floor(fade * 255.0 + 0.5) / 255.0;
    }
}
)";

static const char* terrainFrag = R"(
#version 330 core
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
in vec3 vWorldPos;

in float vDetailFade;
uniform bool uUseOverlayDetail = false;
uniform sampler2D uOverlayDetail;
uniform vec3 uOverlayDetailTiling = vec3(1.0);
uniform sampler2D uSplatMap;
uniform sampler2D uSplatMap2;
uniform bool uUseSplatMap2 = false;
uniform sampler2D uDetail0;
uniform sampler2D uDetail1;
uniform sampler2D uDetail2;
uniform sampler2D uDetail3;
uniform sampler2D uDetail4;
uniform sampler2D uDetail5;
uniform sampler2D uLightmap;
uniform bool uUseLightmap = false;
uniform bool uUseVertexColor = false;
uniform vec3 uLightDir = vec3(0.5, 0.8, 0.6);
uniform vec3 uSunColor = vec3(1.0);
uniform vec3 uAmbient = vec3(0.3);
uniform float uDetailTiling = 32.0;
uniform float uDetailTiling0 = 32.0;
uniform float uDetailTiling1 = 32.0;
uniform float uDetailTiling2 = 32.0;
uniform float uDetailTiling3 = 32.0;
uniform float uDetailTiling4 = 32.0;
uniform float uDetailTiling5 = 32.0;

uniform bool uFogEnabled = false;
uniform vec3 uFogColor = vec3(0.75, 0.8, 0.85);
uniform float uFogDensity = 0.01;
uniform float uFogStart = -1.0;
uniform float uFogEnd = -1.0;
uniform vec4 uFogVolume0 = vec4(0.0);
uniform vec4 uFogVolume1 = vec4(0.0);
uniform vec4 uFogVolume2 = vec4(0.0);
uniform vec3 uCamPos = vec3(0);
uniform int uPointLightCount = 0;
uniform vec3 uPointLightPos[8];
uniform vec3 uPointLightColor[8];
uniform vec3 uPointLightParams[8];

uniform sampler2DShadow uShadowMap;
uniform mat4 uShadowMatrix;
uniform float uShadowStrength = 0.5;

uniform bool uUseNormalMap = false;
uniform sampler2D uNormal0;

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

uniform float uFogRowBase = 0.0;
uniform float uFogRowStep = 0.0;
// Volume fog at one height: the distance travelled through each volume's
// height band (similar triangles), times percentage / visibleDistance.
float fogVolumesAt(float height, float dist) {
    float result = 0.0;
    vec4 volumes[3] = vec4[3](uFogVolume0, uFogVolume1, uFogVolume2);
    float deltaY = abs(height - uCamPos.y);
    for (int i = 0; i < 3; ++i) {
        vec4 volume = volumes[i];
        if (volume.x <= 0.0 || volume.z <= volume.y) continue;
        if (deltaY > 0.01) {
            float low = max(min(uCamPos.y, height), volume.y);
            float high = min(max(uCamPos.y, height), volume.z);
            if (high > low) result += dist * (high - low) / deltaY * volume.x * volume.w;
        } else if (uCamPos.y >= volume.y && uCamPos.y <= volume.z) {
            result += dist * volume.x * volume.w;
        }
    }
    return min(result, 1.0);
}
// SceneGraph::buildFogTexture samples volume fog into 64 rows spanning the
// terrain's height range and filters bilinearly: blend the two nearest rows.
float torqueVolumeFog(float height, float dist) {
    if (uFogRowStep <= 0.0) return fogVolumesAt(height, dist);
    float rowF = (height - uFogRowBase) / uFogRowStep;
    float row0 = floor(rowF);
    float h0 = uFogRowBase + row0 * uFogRowStep;
    return mix(fogVolumesAt(h0, dist), fogVolumesAt(h0 + uFogRowStep, dist), rowF - row0);
}

void main() {
    vec4 base;
    if (uUseVertexColor) {
        base = vColor;
    } else {
        vec2 splatUv = vUV + vec2(0.5 / 256.0);
        vec4 weights = texture(uSplatMap, splatUv);
        vec4 c0 = texture(uDetail0, vUV * uDetailTiling0);
        vec4 c1 = texture(uDetail1, vUV * uDetailTiling1);
        vec4 c2 = texture(uDetail2, vUV * uDetailTiling2);
        vec4 c3 = texture(uDetail3, vUV * uDetailTiling3);
        base = c0 * weights.r + c1 * weights.g + c2 * weights.b + c3 * weights.a;
        // Layers 4-5 via second splat map
        vec4 c4 = texture(uDetail4, vUV * uDetailTiling4);
        vec4 c5 = texture(uDetail5, vUV * uDetailTiling5);
        if (uUseSplatMap2) {
            vec4 weights2 = texture(uSplatMap2, splatUv);
            base = base + c4 * weights2.r + c5 * weights2.g;
        }
        // Clamp the blended result back into range
        base = clamp(base, 0.0, 1.0);
    }

    vec3 N = normalize(vNormal);
    if (uUseNormalMap) {
        vec3 nMap = texture(uNormal0, vUV).rgb * 2.0 - 1.0;
        N = normalize(N + nMap * 0.5);
    }
    float ndotl = max(dot(N, normalize(uLightDir)), 0.0);
    float shadowFactor = 1.0;
    if (uShadowStrength > 0.0) {
        vec4 shadowCoord = uShadowMatrix * vec4(vWorldPos, 1.0);
        shadowFactor = mix(terrainShadowPCF(shadowCoord), 1.0, 1.0 - uShadowStrength);
    }
    // Dynamic sun (live NdotL + shadow map) combined with the baked self-shadowing
    // lightmap. The baked lightmap (2px/square) provides soft, ray-marched self
    // shadowing and a smooth normal light; the live term keeps the sun reacting
    // to time-of-day and object shadows.
    vec3 lighting;
    if (uUseLightmap) {
        vec4 lm = texture(uLightmap, vUV + vec2(0.5 / 512.0));
        // The baked value already contains NdotL and self-shadowing. V12 adds
        // its sun contribution to ambient rather than darkening ambient too.
        lighting = uAmbient + lm.r * uSunColor * shadowFactor;
    } else {
        lighting = uAmbient + uSunColor * ndotl * shadowFactor;
    }
    for (int i = 0; i < uPointLightCount; ++i) {
        vec3 toLight = uPointLightPos[i] - vWorldPos;
        float distance = length(toLight);
        vec3 Lp = distance > 0.0001 ? toLight / distance : N;
        float edge = clamp(1.0 - distance / uPointLightParams[i].x, 0.0, 1.0);
        lighting += uPointLightColor[i] * max(dot(N, Lp), 0.0) *
                    pow(edge, max(0.1, uPointLightParams[i].y));
    }
    vec3 lit = base.rgb * lighting;
    if (uFogEnabled) {
        float dist = length(vWorldPos - uCamPos);
        float volumeFog = torqueVolumeFog(vWorldPos.y, dist);
        if (uFogStart >= 0.0 && uFogEnd > uFogStart && dist >= uFogEnd) discard;
        float hazeRamp = uFogStart >= 0.0 && uFogEnd > uFogStart && dist > uFogStart
            ? (dist - uFogStart) / (uFogEnd - uFogStart) - 1.0 : -1.0;
        float haze = uFogStart >= 0.0 && uFogEnd > uFogStart && dist > uFogStart
            ? clamp(1.0 - hazeRamp * hazeRamp, 0.0, 1.0) : 0.0;
        float volume = min(volumeFog, 1.0);
        lit = mix(lit, uFogColor, volume + min(haze, 1.0 - volume));
    }
    // The detail pass draws after fog with DST_COLOR, ONE_MINUS_SRC_ALPHA.
    if (uUseOverlayDetail) {
        vec4 detail = texture(uOverlayDetail, vUV * uOverlayDetailTiling.xy);
        lit *= detail.rgb * vDetailFade + vec3(1.0 - detail.a * vDetailFade);
    }
    FragColor = vec4(lit, 1.0);
}
)";

static const char* skyVert = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uInvViewProj;
uniform vec3 uCameraPos;
out vec3 vWorldDir;

void main() {
    // Fullscreen triangle: aPos spans [-1,3] in NDC, so gl_Position covers the
    // entire screen at the far plane regardless of FOV/aspect. Unproject each
    // vertex through the inverse view-projection to recover a world-space ray,
    // which is interpolated across the screen and sampled as a cube direction.
    vec4 p = uInvViewProj * vec4(aPos.xy, 1.0, 1.0);
    // The inverse matrix returns a world-space point on the far plane.  A
    // cubemap needs the view ray, not that point relative to world origin.
    vWorldDir = p.xyz / p.w - uCameraPos;
    gl_Position = vec4(aPos.xy, 1.0, 1.0);
}
)";

static const char* skyFrag = R"(
#version 330 core
in vec3 vWorldDir;
uniform samplerCube uSkybox;
uniform bool uUseGradient = false;
uniform vec3 uGradTop = vec3(0.3, 0.5, 0.8);
uniform vec3 uGradBot = vec3(0.7, 0.8, 0.9);
uniform vec3 uFogColor = vec3(0.5);
uniform vec4 uFogBands = vec4(0.0, 60.0, 0.0, 0.0);
uniform float uSkyRadius = 1.0;
out vec4 FragColor;

void main() {
    vec3 dir = normalize(vWorldDir);
    if (uUseGradient) {
        float t = abs(dir.y) * 0.8 + 0.1;
        FragColor = vec4(mix(uGradBot, uGradTop, t), 1.0);
    } else {
        FragColor = texture(uSkybox, dir);
    }
    float s = dir.y * uSkyRadius;
    float fogAlpha = s <= uFogBands.x ? 1.0 :
        (s <= uFogBands.y ? mix(1.0, uFogBands.z,
            (s - uFogBands.x) / max(uFogBands.y - uFogBands.x, 0.001)) :
         mix(uFogBands.z, uFogBands.w,
            clamp((s - uFogBands.y) / max(uSkyRadius - uFogBands.y, 0.001), 0.0, 1.0)));
    FragColor.rgb = mix(FragColor.rgb, uFogColor, fogAlpha);
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
uniform vec4 uColor;
out vec4 vColor;

void main() {
    gl_Position = uProjection * uView * vec4(aPos, 1.0);
    vColor = uColor;
}
)";

static const char* lineFrag = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;

void main() {
    FragColor = vColor;
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
layout(location = 1) in vec2 aUV;
uniform mat4 uProjection;
uniform mat4 uView;
uniform mat4 uModel;
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = vec3(0.0, 1.0, 0.0);
    vUV = aUV;
    gl_Position = uProjection * uView * worldPos;
}
)";

static const char* waterFrag = R"(
#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uWaterColor;
uniform float uWaterOpacity;
uniform sampler2D uSurfaceTexture;
uniform bool uUseSurfaceTexture;
uniform sampler2D uShoreTexture;
uniform bool uUseShoreTexture;
uniform sampler2D uEnvMap;
uniform bool uUseEnvMap;
uniform float uEnvIntensity;
uniform float uTexOffset;
uniform float uShoreFactor;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform float uFogStart;
uniform float uFogEnd;
uniform vec4 uFogVolume0 = vec4(0.0);
uniform vec4 uFogVolume1 = vec4(0.0);
uniform vec4 uFogVolume2 = vec4(0.0);
uniform bool uFogEnabled;
out vec4 FragColor;
uniform float uFogRowBase = 0.0;
uniform float uFogRowStep = 0.0;
// Volume fog at one height: the distance travelled through each volume's
// height band (similar triangles), times percentage / visibleDistance.
float fogVolumesAt(float height, float dist) {
    float result = 0.0;
    vec4 volumes[3] = vec4[3](uFogVolume0, uFogVolume1, uFogVolume2);
    float deltaY = abs(height - uCamPos.y);
    for (int i = 0; i < 3; ++i) {
        vec4 volume = volumes[i];
        if (volume.x <= 0.0 || volume.z <= volume.y) continue;
        if (deltaY > 0.01) {
            float low = max(min(uCamPos.y, height), volume.y);
            float high = min(max(uCamPos.y, height), volume.z);
            if (high > low) result += dist * (high - low) / deltaY * volume.x * volume.w;
        } else if (uCamPos.y >= volume.y && uCamPos.y <= volume.z) {
            result += dist * volume.x * volume.w;
        }
    }
    return min(result, 1.0);
}
// SceneGraph::buildFogTexture samples volume fog into 64 rows spanning the
// terrain's height range and filters bilinearly: blend the two nearest rows.
float torqueVolumeFog(float height, float dist) {
    if (uFogRowStep <= 0.0) return fogVolumesAt(height, dist);
    float rowF = (height - uFogRowBase) / uFogRowStep;
    float row0 = floor(rowF);
    float h0 = uFogRowBase + row0 * uFogRowStep;
    return mix(fogVolumesAt(h0, dist), fogVolumesAt(h0 + uFogRowStep, dist), rowF - row0);
}

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
    vec4 surface = uUseSurfaceTexture
        ? texture(uSurfaceTexture, fract(vUV + vec2(uTexOffset, 0.0)))
        : vec4(1.0);
    if (uUseShoreTexture && uShoreFactor < 1.0)
        surface = mix(texture(uShoreTexture, fract(vUV + vec2(uTexOffset, 0.0))), surface,
                      clamp(uShoreFactor, 0.0, 1.0));
    vec3 waterBase = uWaterColor * surface.rgb * (1.0 - fresnel);
    vec3 reflColor = skyReflect * fresnel;
    if (uUseEnvMap) {
        vec3 R = reflect(-V, N);
        vec2 envUV = vec2(0.5 + 0.5 * R.x, 0.5 - 0.5 * R.y);
        reflColor = mix(reflColor, texture(uEnvMap, envUV).rgb, clamp(uEnvIntensity, 0.0, 1.0));
    }
    vec3 color = waterBase + reflColor + specular;
    // Fog
    if (uFogEnabled) {
        float dist = length(vWorldPos - uCamPos);
        float volumeFog = torqueVolumeFog(vWorldPos.y, dist);
        if (uFogStart >= 0.0 && uFogEnd > uFogStart && dist >= uFogEnd) discard;
        float hazeRamp = uFogStart >= 0.0 && uFogEnd > uFogStart && dist > uFogStart
            ? (dist - uFogStart) / (uFogEnd - uFogStart) - 1.0 : -1.0;
        float haze = uFogStart >= 0.0 && uFogEnd > uFogStart && dist > uFogStart
            ? clamp(1.0 - hazeRamp * hazeRamp, 0.0, 1.0) : 0.0;
        float volume = min(volumeFog, 1.0);
        color = mix(color, uFogColor, volume + min(haze, 1.0 - volume));
    }
    FragColor = vec4(color, uWaterOpacity * surface.a);
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
