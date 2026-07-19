#version 330

in vec2 fragTexCoord;
in vec3 fragPosition;
in vec3 fragNormal;
in vec4 fragColor;

uniform sampler2D texture0;
// Raylib binds MATERIAL_MAP_NORMAL to the "texture2" sampler from DrawMesh,
// so chunk materials get their normal map without SetShaderValueTexture and
// without fighting the batch system for texture slots.  RGB packs the
// tangent-space normal, alpha packs the material mask (0 = matte dirt,
// 1 = polished metal/ice).
uniform sampler2D texture2;
uniform sampler2D shadowMap;
uniform vec4 colDiffuse;
uniform vec3 viewPos;
uniform vec3 lightDirection;
uniform vec4 lightColor;
uniform vec4 ambientColor;
uniform vec4 fogColor;
uniform float fogDensity;
uniform float fogStart;
uniform float fogEnd;
uniform float time;
// Both gates are 1.0 only while chunk meshes draw.  Batched geometry
// (players, devices, transparent blocks) carries no normal map and stores
// real transparency in the vertex alpha, so both features must stay off
// there to keep the current look byte-for-byte.
uniform float normalMapGate;
uniform float aoGate;
uniform mat4 lightViewProj;
uniform float shadowStrength;
uniform vec2 shadowTexel;
uniform float shadowDistance;
uniform float shadowPcfExtra;
const int kMaxPointLights = 12;
uniform int pointLightCount;
uniform vec4 pointLightPositionRadius[kMaxPointLights];
uniform vec4 pointLightColorIntensity[kMaxPointLights];

out vec4 finalColor;

float SampleShadow(vec2 uv, float compare)
{
    return step(texture(shadowMap, uv).r, compare);
}

void main()
{
    // During the chunk pass the vertex alpha carries baked ambient occlusion
    // instead of coverage, so it must feed lighting rather than blending.
    float vertexAo = mix(1.0, fragColor.a, aoGate);
    float coverage = mix(fragColor.a, 1.0, aoGate);
    vec4 albedo = texture(texture0, fragTexCoord) * vec4(colDiffuse.rgb * fragColor.rgb, colDiffuse.a * coverage);
    if (albedo.a < 0.015) discard;

    // Albedo textures and vertex tints are sRGB-authored; lighting math runs
    // in linear space and the result is re-encoded before fog.  This is what
    // keeps sunlit grass from clipping into neon and shadowed faces from
    // crushing to black.
    vec3 albedoLinear = pow(albedo.rgb, vec3(2.2));

    vec3 cameraDelta = viewPos - fragPosition;
    float distanceToCamera = length(cameraDelta);
    vec3 viewDirection = cameraDelta / max(distanceToCamera, 0.0001);

    // rlgl's batch pipeline (DrawCube/DrawSphere/... on raylib 5.0) uploads
    // NO vertex normals: the attribute reads as (0,0,0) and normalize() of it
    // is NaN, which used to paint items, generators and cores solid black.
    // Reconstruct a flat per-face normal from position derivatives instead.
    vec3 geoNormal;
    float normalLength = length(fragNormal);
    if (normalLength > 0.25)
    {
        geoNormal = fragNormal / normalLength;
    }
    else
    {
        geoNormal = cross(dFdx(fragPosition), dFdy(fragPosition));
        float flatLength = length(geoNormal);
        geoNormal = flatLength > 1.0e-9 ? geoNormal / flatLength : vec3(0.0, 1.0, 0.0);
        if (dot(geoNormal, viewDirection) < 0.0)
        {
            geoNormal = -geoNormal;
        }
    }

    vec3 normal = geoNormal;
    float materialMask = 0.0;
    if (normalMapGate > 0.5)
    {
        // Chunk faces are axis-aligned, so the tangent basis follows from the
        // face normal alone and matches the meshing UV layout on all six
        // faces; no per-vertex tangents are needed.
        vec3 tangent = abs(geoNormal.y) > 0.5 ? vec3(1.0, 0.0, 0.0) : normalize(vec3(-geoNormal.z, 0.0, geoNormal.x));
        vec3 bitangent = cross(geoNormal, tangent);
        vec4 packedNormal = texture(texture2, fragTexCoord);
        vec3 tangentNormal = packedNormal.rgb * 2.0 - 1.0;
        normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y + geoNormal * tangentNormal.z);
        materialMask = packedNormal.a;
    }

    // SceneShader uploads a normalized directional vector, so no per-pixel
    // normalization is required here.
    vec3 sunDirection = -lightDirection;
    float sunFacing = dot(normal, sunDirection);

    // Sun-space occlusion from the depth pass.  Fragments outside the ortho
    // volume or past the fade distance fall back to fully lit, which keeps
    // the horizon stable when the shadow camera pans with the player.
    float sunVisibility = 1.0;
    if (shadowStrength > 0.001)
    {
        vec4 lightClip = lightViewProj * vec4(fragPosition, 1.0);
        vec3 lightNdc = lightClip.xyz / lightClip.w;
        float edge = max(abs(lightNdc.x), abs(lightNdc.y));
        vec3 shadowCoord = lightNdc * 0.5 + 0.5;
        if (edge < 1.0 && shadowCoord.z < 1.0 && distanceToCamera < shadowDistance)
        {
            float geoFacing = clamp(dot(geoNormal, sunDirection), 0.0, 1.0);
            // Depth range spans 1000 world units (raylib ortho cull planes),
            // so 0.0001 here is 0.1 of a block: enough to stop acne without
            // the shadow visibly detaching from the wall that casts it.
            float bias = 0.00012 + 0.0005 * (1.0 - geoFacing);
            float compare = shadowCoord.z - bias;
            float occlusion =
                SampleShadow(shadowCoord.xy + shadowTexel * vec2(-0.5, -0.5), compare)
                + SampleShadow(shadowCoord.xy + shadowTexel * vec2(0.5, -0.5), compare)
                + SampleShadow(shadowCoord.xy + shadowTexel * vec2(-0.5, 0.5), compare)
                + SampleShadow(shadowCoord.xy + shadowTexel * vec2(0.5, 0.5), compare);
            if (shadowPcfExtra > 0.5)
            {
                occlusion +=
                    SampleShadow(shadowCoord.xy + shadowTexel * vec2(-1.5, 0.0), compare)
                    + SampleShadow(shadowCoord.xy + shadowTexel * vec2(1.5, 0.0), compare)
                    + SampleShadow(shadowCoord.xy + shadowTexel * vec2(0.0, -1.5), compare)
                    + SampleShadow(shadowCoord.xy + shadowTexel * vec2(0.0, 1.5), compare);
                occlusion *= 0.125;
            }
            else
            {
                occlusion *= 0.25;
            }
            float distanceFade = smoothstep(shadowDistance * 0.82, shadowDistance, distanceToCamera);
            float edgeFade = smoothstep(0.84, 1.0, edge);
            sunVisibility = 1.0 - occlusion * shadowStrength * (1.0 - max(distanceFade, edgeFade));
        }
    }

    // A wrapped key light preserves readable detail on low-poly and voxel
    // silhouettes while still giving the sun a clear direction.  This is
    // deliberately a single directional-light evaluation per pixel.
    float wrappedSun = clamp((sunFacing + 0.20) / 1.20, 0.0, 1.0);
    float skyFacing = normal.y * 0.5 + 0.5;
    vec3 skyAmbient = ambientColor.rgb * mix(0.55, 1.05, skyFacing) * vertexAo;
    vec3 light = skyAmbient + lightColor.rgb * (0.16 + wrappedSun * 0.84 * sunVisibility) * 0.72;
    vec3 lit = albedoLinear * light;

    // Torches and lamps are chosen on the CPU by distance to the camera.
    // A smooth quadratic falloff keeps their warm pools visible without the
    // hard sphere edge typical of a cheap voxel light implementation.
    for (int i = 0; i < kMaxPointLights; ++i)
    {
        if (i >= pointLightCount) break;
        vec3 toLight = pointLightPositionRadius[i].xyz - fragPosition;
        float lightDistance = length(toLight);
        float radius = max(pointLightPositionRadius[i].w, 0.001);
        float falloff = clamp(1.0 - lightDistance / radius, 0.0, 1.0);
        falloff *= falloff;
        vec3 localDirection = toLight / max(lightDistance, 0.0001);
        float localFacing = max(dot(normal, localDirection), 0.0);
        vec3 localColor = pointLightColorIntensity[i].rgb;
        float localIntensity = pointLightColorIntensity[i].w;
        lit += albedoLinear * localColor * localIntensity * falloff * (0.20 + localFacing * 0.80);
    }

    // Cheap sun-edge lift: reuse the distance needed for fog instead of a
    // second normalize/length path.  The material mask widens it into a
    // gloss response on metal, ice and polished stone.
    float rim = 1.0 - clamp(dot(normal, viewDirection), 0.0, 1.0);
    rim *= rim;
    lit += albedoLinear * lightColor.rgb * rim * wrappedSun * sunVisibility * (0.075 + materialMask * 0.22);

    // Directional gloss for shiny materials only; gated to the chunk pass so
    // batched geometry with reconstructed normals stays matte.
    if (normalMapGate > 0.5 && materialMask > 0.01)
    {
        vec3 halfDir = normalize(sunDirection + viewDirection);
        float gloss = pow(max(dot(normal, halfDir), 0.0), mix(16.0, 48.0, materialMask));
        lit += lightColor.rgb * gloss * materialMask * wrappedSun * sunVisibility * 0.5;
    }

    // Bright, saturated materials (energy, lava and team devices) feed the
    // compact bloom pass.  Neutral white blocks stay stable and do not glow;
    // the material mask lets lava and glow blocks push harder into bloom.
    float highest = max(albedo.r, max(albedo.g, albedo.b));
    float lowest = min(albedo.r, min(albedo.g, albedo.b));
    float coloredEnergy = smoothstep(0.13, 0.42, highest - lowest)
        * smoothstep(0.58, 0.92, highest);
    float materialPulse = 0.88 + 0.12 * sin(time * 3.2 + fragPosition.x * 0.72 + fragPosition.z * 0.61);
    lit += albedoLinear * coloredEnergy * (0.16 + 0.18 * materialPulse) * (1.0 + materialMask * 1.3);

    // Back to display space before mixing with the sRGB-authored fog color.
    vec3 encoded = pow(max(lit, vec3(0.0)), vec3(1.0 / 2.2));

    // Two-part fog: a light atmospheric haze that grows with distance, plus
    // a sky veil that dissolves geometry right before the draw-distance cull
    // so far chunks fade instead of popping.  Mid-range combat stays clear.
    float atmosphere = 1.0 - exp(-fogDensity * fogDensity * distanceToCamera * distanceToCamera);
    float skyVeil = fogEnd > fogStart + 1.0 ? smoothstep(fogStart, fogEnd, distanceToCamera) : 0.0;
    float fogAmount = clamp(atmosphere * 0.55 + skyVeil, 0.0, 0.96);
    finalColor = vec4(mix(encoded, fogColor.rgb, fogAmount), albedo.a);
}
