#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform sampler2D bloomTexture;
uniform float bloomStrength;
uniform float vignetteStrength;
uniform float damageFlash;
uniform float scopeBlend;
// 1/target size; zero disables FXAA (low preset / tiny fallback targets).
uniform vec2 texelSize;

out vec4 finalColor;

vec3 ShoulderToneMap(vec3 color)
{
    // The scene shader already outputs display-encoded color, so the frame
    // must pass through untouched below the shoulder; only bloom overshoot
    // above 0.82 rolls off smoothly instead of hard-clipping.  (The previous
    // ACES curve on encoded input lifted midtones ~20% and dulled highlights,
    // which read as a washed-out, over-bright frame.)
    vec3 base = min(color, vec3(0.82));
    vec3 over = max(color - vec3(0.82), vec3(0.0));
    return clamp(base + over / (1.0 + 2.2 * over), 0.0, 1.0);
}

float FxaaLuma(vec3 color)
{
    return dot(color, vec3(0.299, 0.587, 0.114));
}

// Compact FXAA: smooths block-edge staircases the render-scale bilinear
// upsample cannot, at five extra texture reads.  Runs before tonemapping on
// the LDR scene target.
vec3 FxaaSample(vec2 uv)
{
    vec3 rgbM = texture(texture0, uv).rgb;
    if (texelSize.x <= 0.0)
    {
        return rgbM;
    }

    vec3 rgbNW = texture(texture0, uv + texelSize * vec2(-1.0, -1.0)).rgb;
    vec3 rgbNE = texture(texture0, uv + texelSize * vec2(1.0, -1.0)).rgb;
    vec3 rgbSW = texture(texture0, uv + texelSize * vec2(-1.0, 1.0)).rgb;
    vec3 rgbSE = texture(texture0, uv + texelSize * vec2(1.0, 1.0)).rgb;
    float lumaM = FxaaLuma(rgbM);
    float lumaNW = FxaaLuma(rgbNW);
    float lumaNE = FxaaLuma(rgbNE);
    float lumaSW = FxaaLuma(rgbSW);
    float lumaSE = FxaaLuma(rgbSE);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    if (lumaMax - lumaMin < max(0.0312, lumaMax * 0.125))
    {
        return rgbM;
    }

    vec2 dir = vec2(
        -((lumaNW + lumaNE) - (lumaSW + lumaSE)),
        ((lumaNW + lumaSW) - (lumaNE + lumaSE)));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 0.0078125);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * texelSize;

    vec3 rgbA = 0.5 * (
        texture(texture0, uv + dir * (1.0 / 3.0 - 0.5)).rgb
        + texture(texture0, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(texture0, uv + dir * -0.5).rgb
        + texture(texture0, uv + dir * 0.5).rgb);
    float lumaB = FxaaLuma(rgbB);
    if (lumaB < lumaMin || lumaB > lumaMax)
    {
        return rgbA;
    }
    return rgbB;
}

void main()
{
    vec2 centered = fragTexCoord - vec2(0.5);
    float radial = dot(centered, centered);
    vec2 uv = vec2(0.5) + centered * (1.0 + radial * 0.055 * scopeBlend);
    vec3 color = FxaaSample(uv);

    // Bloom is prefiltered at 1/4-1/16 of the scene area.  The uniform branch
    // lets the low preset skip this texture read entirely.
    if (bloomStrength > 0.001)
    {
        color += texture(bloomTexture, uv).rgb * bloomStrength;
    }

    color = ShoulderToneMap(max(color, vec3(0.0)));
    color *= vec3(1.012, 1.004, 0.992);
    float vignette = smoothstep(0.20, 0.70, radial);
    color *= 1.0 - vignette * vignette * vignetteStrength;
    color = mix(color, color * vec3(1.34, 0.45, 0.40) + vec3(0.14, 0.0, 0.0), damageFlash);
    float scopeEdge = smoothstep(0.50, 0.72, length(centered) * 1.42) * scopeBlend;
    color *= 1.0 - scopeEdge * 0.48;
    finalColor = vec4(color, 1.0) * fragColor;
}
