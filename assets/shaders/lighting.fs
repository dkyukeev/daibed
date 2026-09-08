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
uniform sampler2D shadowMapNear;
uniform sampler2D shadowMapFar;
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
uniform int materialQuality;
uniform int volumeSteps;
uniform sampler2D voxelAtlas;
uniform vec3 voxelOrigin;
uniform int voxelSize;
uniform int giQuality;
uniform float giStrength;
uniform int localShadows;
uniform float shadowSoftness;
uniform vec2 shadowWorldTexel;
// Both gates are 1.0 only while chunk meshes draw.  Batched geometry
// (players, devices, transparent blocks) carries no normal map and stores
// real transparency in the vertex alpha, so both features must stay off
// there to keep the current look byte-for-byte.
uniform float normalMapGate;
uniform float aoGate;
uniform float aoStrength;
uniform float emissiveStrength;
uniform mat4 lightViewProjNear;
uniform mat4 lightViewProjFar;
uniform float shadowStrength;
uniform vec2 shadowTexelNear;
uniform vec2 shadowTexelFar;
uniform float shadowSplitDistance;
uniform float shadowDistance;
uniform float shadowPcfExtra;
const int kMaxPointLights = 16;
uniform int pointLightCount;
uniform vec4 pointLightPositionRadius[kMaxPointLights];
uniform vec4 pointLightColorIntensity[kMaxPointLights];

out vec4 finalColor;

float SampleShadow(sampler2D mapTexture, vec2 uv, float compare)
{
    return step(texture(mapTexture, uv).r, compare);
}

float FilterShadow(sampler2D mapTexture, vec3 shadowCoord, vec2 texel, float compare, float wide, float worldTexel)
{
    if (shadowSoftness <= 0.01) return SampleShadow(mapTexture, shadowCoord.xy, compare);
    const vec2 disk[8] = vec2[8](vec2(-0.61,-0.35), vec2(0.19,-0.88),
        vec2(0.75,-0.24), vec2(0.42,0.63), vec2(-0.22,0.32), vec2(-0.81,0.41),
        vec2(-0.15,-0.27), vec2(0.07,0.96));
    float radius = shadowSoftness;
    if (wide > 0.5)
    {
        // Blocker search: contact stays sharp, penumbra grows with separation.
        float centerDepth = texture(mapTexture, shadowCoord.xy).r;
        float blocker = centerDepth < compare ? centerDepth : 0.0;
        float blockers = centerDepth < compare ? 1.0 : 0.0;
        for (int i = 0; i < 8; ++i)
        {
            float d = texture(mapTexture, clamp(shadowCoord.xy + disk[i] * texel * 5.0,
                                                texel, vec2(1.0) - texel)).r;
            if (d < compare) { blocker += d; blockers += 1.0; }
        }
        if (blockers < 0.5) return 0.0;
        // Orthographic depth is linear; raylib's default depth span is 1000 m.
        float separation = max(compare - blocker / blockers, 0.0) * 1000.0;
        radius = clamp(0.65 + separation * shadowSoftness * 0.01 / max(worldTexel, 0.001), 0.65, 7.0);
    }
    int taps = wide > 0.5 ? 8 : 4;
    float occlusion = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        if (i >= taps) break;
        vec2 uv = clamp(shadowCoord.xy + disk[i] * texel * radius, texel, vec2(1.0) - texel);
        occlusion += SampleShadow(mapTexture, uv, compare);
    }
    return occlusion / float(taps);
}

float CascadeVisibility(sampler2D mapTexture, mat4 lightMatrix, vec2 texel,
                        vec3 worldPosition, float bias, float wide, float worldTexel)
{
    vec4 lightClip = lightMatrix * vec4(worldPosition, 1.0);
    vec3 lightNdc = lightClip.xyz / max(abs(lightClip.w), 0.00001);
    vec3 shadowCoord = lightNdc * 0.5 + 0.5;
    float edge = max(abs(lightNdc.x), abs(lightNdc.y));
    if (edge >= 1.0 || shadowCoord.z >= 1.0 || shadowCoord.z <= 0.0)
    {
        return 1.0;
    }
    float filtered = FilterShadow(mapTexture, shadowCoord, texel, shadowCoord.z - bias, wide, worldTexel);
    // Cascade borders fade independently, avoiding a hard moving rectangle
    // if geometry briefly leaves the snapped orthographic volume.
    float edgeFade = smoothstep(0.88, 0.995, edge);
    return 1.0 - filtered * (1.0 - edgeFade);
}

// One unfiltered depth lookup per march step, using the cascade containing
// that sample. Outside the finite shadow volume the atmosphere is sunlit.
float VolumeVisibility(vec3 p, float distanceFromEye)
{
    bool nearCascade = distanceFromEye < shadowSplitDistance;
    vec4 clip = nearCascade ? lightViewProjNear * vec4(p, 1.0)
                            : lightViewProjFar * vec4(p, 1.0);
    vec3 q = clip.xyz / max(abs(clip.w), 0.00001) * 0.5 + 0.5;
    if (any(lessThanEqual(q, vec3(0.001))) || any(greaterThanEqual(q, vec3(0.999)))) return 1.0;
    float depth = nearCascade ? texture(shadowMapNear, q.xy).r : texture(shadowMapFar, q.xy).r;
    return step(q.z - 0.00025, depth);
}

// Atlas layout: x + z*N columns, y rows; integer fetch avoids filtering solids.
vec4 Voxel(ivec3 cell)
{
    return texelFetch(voxelAtlas, ivec2(cell.x + cell.z * voxelSize, cell.y), 0);
}

bool IntersectLightBox(vec3 p, vec3 direction, vec3 lower, vec3 upper,
                       float minT, float maxT, out float hitT, out vec3 hitNormal)
{
    vec3 nearT, farT;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (abs(direction[axis]) < 0.00001)
        {
            if (p[axis] < lower[axis] || p[axis] > upper[axis]) return false;
            nearT[axis] = -1e20;
            farT[axis] = 1e20;
        }
        else
        {
            float a = (lower[axis] - p[axis]) / direction[axis];
            float b = (upper[axis] - p[axis]) / direction[axis];
            nearT[axis] = min(a, b);
            farT[axis] = max(a, b);
        }
    }
    float enter = max(nearT.x, max(nearT.y, nearT.z));
    float leave = min(farT.x, min(farT.y, farT.z));
    hitT = max(minT, enter);
    if (leave < hitT || hitT >= maxT) return false;
    hitNormal = -direction;
    if (enter >= minT - 0.00001)
    {
        if (nearT.x >= nearT.y && nearT.x >= nearT.z) hitNormal = vec3(-sign(direction.x), 0, 0);
        else if (nearT.y >= nearT.z) hitNormal = vec3(0, -sign(direction.y), 0);
        else hitNormal = vec3(0, 0, -sign(direction.z));
    }
    return true;
}

bool TraceVoxelIgnoring(vec3 origin, vec3 direction, float maxDistance, ivec3 ignoredCell,
                out vec3 hitPosition, out vec3 hitNormal, out vec4 material)
{
    vec3 p = origin - voxelOrigin;
    ivec3 cell = ivec3(floor(p));
    ivec3 stepDir = ivec3(sign(direction));
    vec3 delta = 1.0 / max(abs(direction), vec3(0.00001));
    vec3 boundary = vec3(cell) + step(vec3(0.0), direction);
    vec3 tMax = (boundary - p) / mix(vec3(0.00001), direction, greaterThan(abs(direction), vec3(0.00001)));
    tMax = max(tMax, vec3(0.0));
    // Parallel axes never cross a cell boundary.
    if (abs(direction.x) < 0.00001) tMax.x = 1e20;
    if (abs(direction.y) < 0.00001) tMax.y = 1e20;
    if (abs(direction.z) < 0.00001) tMax.z = 1e20;
    float t = 0.0;
    hitNormal = -direction;
    hitPosition = origin;
    material = vec4(0.0);
    for (int i = 0; i < 48; ++i)
    {
        if (t >= maxDistance || any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(voxelSize)))) return false;
        material = Voxel(cell);
        int mask = int(material.a * 255.0 + 0.5);
        if (mask != 0 && any(notEqual(cell, ignoredCell)))
        {
            if (mask == 255 || mask == 128)
            {
                hitPosition = origin + direction * t;
                return true;
            }
            // Only partial blocks take this path; empty half-cells transmit light.
            float nearest = maxDistance;
            vec3 nearestNormal = hitNormal;
            for (int octant = 0; octant < 8; ++octant)
            {
                if ((mask & (1 << octant)) == 0) continue;
                vec3 lower = vec3(cell) + vec3(float(octant & 1), float((octant >> 2) & 1),
                                              float((octant >> 1) & 1)) * 0.5;
                float boxT; vec3 boxNormal;
                if (IntersectLightBox(p, direction, lower, lower + vec3(0.5), t, nearest, boxT, boxNormal))
                { nearest = boxT; nearestNormal = boxNormal; }
            }
            if (nearest < maxDistance)
            {
                hitPosition = origin + direction * nearest;
                hitNormal = nearestNormal;
                return true;
            }
        }
        if (tMax.x <= tMax.y && tMax.x <= tMax.z)
        { t = tMax.x; tMax.x += delta.x; cell.x += stepDir.x; hitNormal = vec3(-stepDir.x,0,0); }
        else if (tMax.y <= tMax.z)
        { t = tMax.y; tMax.y += delta.y; cell.y += stepDir.y; hitNormal = vec3(0,-stepDir.y,0); }
        else
        { t = tMax.z; tMax.z += delta.z; cell.z += stepDir.z; hitNormal = vec3(0,0,-stepDir.z); }
    }
    return false;
}

bool TraceVoxel(vec3 origin, vec3 direction, float maxDistance,
                out vec3 hitPosition, out vec3 hitNormal, out vec4 material)
{
    return TraceVoxelIgnoring(origin, direction, maxDistance, ivec3(-1), hitPosition, hitNormal, material);
}

float VoxelCoverage(vec3 p)
{
    vec3 q = p - voxelOrigin;
    vec3 edge = min(q, vec3(float(voxelSize)) - q);
    return smoothstep(1.0, 5.0, min(edge.x, min(edge.y, edge.z)));
}

vec4 IndirectLight(vec3 p, vec3 n)
{
    if (giQuality == 0 || giStrength <= 0.001 || voxelSize == 0) return vec4(0,0,0,1);
    float reach = giQuality == 1 ? 6.0 : 10.0;
    vec3 edge = min(p - voxelOrigin, voxelOrigin + vec3(float(voxelSize)) - p);
    // Fade before rays can escape the cache and mistake unknown space for sky.
    float coverage = smoothstep(reach, reach + 3.0, min(edge.x, min(edge.y, edge.z)));
    if (coverage <= 0.0) return vec4(0,0,0,1);
    vec3 tangent = normalize(cross(n, abs(n.y) < 0.9 ? vec3(0,1,0) : vec3(1,0,0)));
    vec3 bitangent = cross(n, tangent);
    int rays = giQuality == 1 ? 4 : 8;
    float sky = 0.0;
    vec3 bounce = vec3(0.0);
    for (int i = 0; i < 8; ++i)
    {
        if (i >= rays) break;
        float u = (float(i) + 0.5) / float(rays);
        float phi = float(i) * 2.39996323;
        vec3 ray = tangent * (sqrt(u) * cos(phi)) + bitangent * (sqrt(u) * sin(phi)) + n * sqrt(1.0 - u);
        vec3 hp, hn; vec4 material;
        if (!TraceVoxel(p + n * 0.035, ray, reach, hp, hn, material)) { sky += 1.0; continue; }
        sky += smoothstep(reach * 0.5, reach, length(hp - p));
        vec3 albedo = pow(material.rgb, vec3(2.2));
        float sun = 0.0;
        if (shadowStrength > 0.001)
            sun = VolumeVisibility(hp + hn * 0.04, length(hp - viewPos));
        else
        {
            vec3 sp, sn; vec4 sm;
            sun = TraceVoxel(hp + hn * 0.04, -lightDirection, 16.0, sp, sn, sm) ? 0.0 : 1.0;
        }
        vec3 outgoing = albedo * lightColor.rgb * max(dot(hn, -lightDirection), 0.0) * sun * 0.65;
        // Byte 128 is reserved for emission; other partial alpha values are shapes.
        if (int(material.a * 255.0 + 0.5) == 128) outgoing += albedo * 0.8;
        bounce += outgoing;
    }
    return vec4(bounce * (coverage * giStrength / float(rays)), mix(1.0, sky / float(rays), coverage));
}

vec3 IntegrateSunlight(vec3 ray, float rayLength)
{
    if (volumeSteps == 0 || shadowStrength <= 0.001 || fogDensity <= 0.0) return vec3(0.0);
    float marchLength = min(rayLength, min(shadowDistance, 100.0));
    float stepLength = marchLength / float(volumeSteps);
    // Stable screen-space interleaved noise avoids animated grain and bands.
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float transmittance = 1.0;
    float scattering = 0.0;
    for (int i = 0; i < 24; ++i)
    {
        if (i >= volumeSteps) break;
        float t = (float(i) + jitter) * stepLength;
        vec3 p = viewPos + ray * t;
        float heightDensity = exp(-max(p.y - 3.0, 0.0) * 0.055);
        float extinction = 1.0 - exp(-fogDensity * heightDensity * stepLength);
        scattering += transmittance * extinction * VolumeVisibility(p, t);
        transmittance *= 1.0 - extinction;
    }
    float mu = dot(ray, -lightDirection);
    // Henyey-Greenstein forward scattering, bounded to preserve visibility.
    const float g = 0.55;
    float phase = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 0.1), 1.5);
    return lightColor.rgb * scattering * min(phase, 4.0) * 0.32;
}

vec3 MaterialReflection(vec3 n, vec3 v, vec3 l, float mask, float visibility)
{
    float roughness = mix(0.72, 0.24, mask);
    // Filter the microfacet lobe using normal derivatives to reduce sparkle.
    float variance = max(dot(dFdx(n), dFdx(n)), dot(dFdy(n), dFdy(n)));
    roughness = clamp(sqrt(roughness * roughness + min(variance, 0.3)), 0.24, 1.0);
    vec3 h = (v + l) / max(length(v + l), 0.0001);
    float nv = max(dot(n, v), 0.001);
    float nl = max(dot(n, l), 0.0);
    float nh = max(dot(n, h), 0.0);
    float vh = max(dot(v, h), 0.0);
    float a2 = pow(roughness, 4.0);
    float d = nh * nh * (a2 - 1.0) + 1.0;
    float distribution = a2 / max(3.14159265 * d * d, 0.0001);
    float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
    float geometry = (nv / (nv * (1.0 - k) + k)) * (nl / (nl * (1.0 - k) + k));
    vec3 fresnel = vec3(0.04) + vec3(0.96) * pow(1.0 - vh, 5.0);
    vec3 direct = lightColor.rgb * distribution * geometry * fresnel / max(4.0 * nv, 0.001) * visibility;
    // Analytic sky reflection: no scene capture, no offscreen history buffer.
    vec3 reflected = reflect(-v, n);
    vec3 sky = mix(ambientColor.rgb * 0.25, pow(fogColor.rgb, vec3(2.2)), smoothstep(-0.2, 0.65, reflected.y));
    float edgeFresnel = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
    return direct + sky * edgeFresnel * mask * (1.0 - roughness * 0.65);
}

void main()
{
    // During the chunk pass the vertex alpha carries baked ambient occlusion
    // instead of coverage, so it must feed lighting rather than blending.
    float vertexAo = mix(1.0, fragColor.a, aoGate * aoStrength);
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
        if (materialQuality > 0)
            normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y + geoNormal * tangentNormal.z);
        materialMask = packedNormal.a;
    }

    // SceneShader uploads a normalized directional vector, so no per-pixel
    // normalization is required here.
    vec3 sunDirection = -lightDirection;
    float sunFacing = dot(normal, sunDirection);

    // Two stable sun cascades. The short blend band hides the split while
    // preserving near-field texel density for combat silhouettes.
    float sunVisibility = 1.0;
    if (shadowStrength > 0.001 && distanceToCamera < shadowDistance)
    {
        float geoFacing = clamp(dot(geoNormal, sunDirection), 0.0, 1.0);
        float bias = 0.000012 + 0.000025 * (1.0 - geoFacing);
        float splitBlend = max(2.0, shadowSplitDistance * 0.12);
        float cascadeBlend = smoothstep(shadowSplitDistance - splitBlend,
                                       shadowSplitDistance + splitBlend, distanceToCamera);
        float nearVisibility = 1.0;
        float farVisibility = 1.0;
        if (cascadeBlend < 1.0)
            nearVisibility = CascadeVisibility(shadowMapNear, lightViewProjNear, shadowTexelNear,
                                               fragPosition + geoNormal * shadowWorldTexel.x * 0.6, bias, shadowPcfExtra, shadowWorldTexel.x);
        if (cascadeBlend > 0.0)
            farVisibility = CascadeVisibility(shadowMapFar, lightViewProjFar, shadowTexelFar,
                                              fragPosition + geoNormal * shadowWorldTexel.y * 0.6, bias * 1.25, shadowPcfExtra, shadowWorldTexel.y);
        float cascadeVisibility = mix(nearVisibility, farVisibility, cascadeBlend);
        if (distanceToCamera < shadowDistance)
        {
            float distanceFade = smoothstep(shadowDistance * 0.88, shadowDistance, distanceToCamera);
            sunVisibility = mix(1.0 - (1.0 - cascadeVisibility) * shadowStrength, 1.0, distanceFade);
        }
    }

    // A wrapped key light preserves readable detail on low-poly and voxel
    // silhouettes while still giving the sun a clear direction.  This is
    // deliberately a single directional-light evaluation per pixel.
    float wrappedSun = clamp((sunFacing + 0.20) / 1.20, 0.0, 1.0);
    float skyFacing = normal.y * 0.5 + 0.5;
    vec4 indirect = IndirectLight(fragPosition, geoNormal);
    vec3 skyAmbient = ambientColor.rgb * mix(0.55, 1.05, skyFacing) * vertexAo;
    skyAmbient *= mix(0.12, 1.0, indirect.a);
    skyAmbient += indirect.rgb * vertexAo;
    vec3 light = skyAmbient + lightColor.rgb * ((giQuality > 0 ? 0.0 : 0.16) + wrappedSun * 0.84 * sunVisibility) * 0.72;
    vec3 lit = albedoLinear * light;

    // Torches and lamps are chosen on the CPU by distance to the camera.
    // Chebyshev distance yields axis-aligned square light contours on floors
    // and walls. Euclidean distance is needed only for actual visibility rays.
    for (int i = 0; i < kMaxPointLights; ++i)
    {
        if (i >= pointLightCount) break;
        vec3 toLight = pointLightPositionRadius[i].xyz - fragPosition;
        vec3 axisDistance = abs(toLight);
        float boxDistance = max(axisDistance.x, max(axisDistance.y, axisDistance.z));
        float radius = max(pointLightPositionRadius[i].w, 0.001);
        if (boxDistance >= radius) continue;
        float visibility = 1.0;
        if (localShadows != 0 && voxelSize > 0 && boxDistance > 0.04)
        {
            vec3 hp, hn; vec4 material;
            vec3 origin = fragPosition + geoNormal * 0.035;
            vec3 ray = pointLightPositionRadius[i].xyz - origin;
            float lightDistance = length(ray);
            ivec3 sourceCell = ivec3(floor(pointLightPositionRadius[i].xyz - voxelOrigin));
            bool blocked = TraceVoxelIgnoring(origin, ray / max(lightDistance, 0.00001),
                max(lightDistance - 0.01, 0.0), sourceCell, hp, hn, material);
            float known = min(VoxelCoverage(fragPosition), VoxelCoverage(pointLightPositionRadius[i].xyz));
            visibility = blocked ? 1.0 - known : 1.0;
        }
        float falloff = clamp(1.0 - boxDistance / radius, 0.0, 1.0);
        falloff *= falloff;
        float localFacing = clamp(dot(normal, toLight) / max(boxDistance, 0.0001), 0.0, 1.0);
        vec3 localColor = pointLightColorIntensity[i].rgb;
        float localIntensity = pointLightColorIntensity[i].w;
        lit += albedoLinear * localColor * localIntensity * falloff * visibility * (0.20 + localFacing * 0.80);
    }

    // Cheap sun-edge lift: reuse the distance needed for fog instead of a
    // second normalize/length path.  The material mask widens it into a
    // gloss response on metal, ice and polished stone.
    float rim = 1.0 - clamp(dot(normal, viewDirection), 0.0, 1.0);
    rim *= rim;
    lit += albedoLinear * lightColor.rgb * rim * wrappedSun * sunVisibility * (0.075 + materialMask * 0.22);

    // Directional gloss for shiny materials only; gated to the chunk pass so
    // batched geometry with reconstructed normals stays matte.
    if (normalMapGate > 0.5 && materialMask > 0.01 && materialQuality > 0)
    {
        if (materialQuality >= 2)
        {
            lit += MaterialReflection(normal, viewDirection, sunDirection, materialMask, sunVisibility) * vertexAo;
        }
        else
        {
        vec3 halfDir = normalize(sunDirection + viewDirection);
        float gloss = pow(max(dot(normal, halfDir), 0.0), mix(16.0, 48.0, materialMask));
        lit += lightColor.rgb * gloss * materialMask * wrappedSun * sunVisibility * 0.5;
        }
    }

    // Filmic shoulder keeps wool, glass and stone below the bloom knee. Only
    // explicitly tagged scene groups and reserved emissive chunk materials
    // (normal-map alpha > .97) are allowed to add radiance above it.
    lit = lit / (vec3(1.0) + lit * 0.34);
    float chunkEmission = normalMapGate > 0.5 ? smoothstep(0.97, 0.995, materialMask) : 0.0;
    float emission = max(emissiveStrength, chunkEmission);
    float materialPulse = 0.88 + 0.12 * sin(time * 3.2 + fragPosition.x * 0.72 + fragPosition.z * 0.61);
    lit += albedoLinear * emission * (0.42 + 0.30 * materialPulse);

    // Back to display space before mixing with the sRGB-authored fog color.
    lit += IntegrateSunlight(-viewDirection, distanceToCamera);
    vec3 encoded = pow(max(lit, vec3(0.0)), vec3(1.0 / 2.2));

    // Two-part fog: a light atmospheric haze that grows with distance, plus
    // a sky veil that dissolves geometry right before the draw-distance cull
    // so far chunks fade instead of popping.  Mid-range combat stays clear.
    float atmosphere = 1.0 - exp(-fogDensity * fogDensity * distanceToCamera * distanceToCamera);
    float skyVeil = fogEnd > fogStart + 1.0 ? smoothstep(fogStart, fogEnd, distanceToCamera) : 0.0;
    float fogAmount = clamp(atmosphere * 0.55 + skyVeil, 0.0, 0.96);
    finalColor = vec4(mix(encoded, fogColor.rgb, fogAmount), albedo.a);
}
