#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec2 sourceSize;
uniform float time;
uniform float bloomStrength;
uniform float vignetteStrength;
uniform float damageFlash;
uniform float scopeBlend;

out vec4 finalColor;

float luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
    vec2 centered = fragTexCoord - vec2(0.5);
    float radial = dot(centered, centered);
    vec2 uv = vec2(0.5) + centered * (1.0 + radial * 0.055 * scopeBlend);
    vec2 texel = 1.0 / max(sourceSize, vec2(1.0));
    vec3 color = texture(texture0, uv).rgb;
    vec3 blur = vec3(0.0);
    blur += texture(texture0, uv + texel * vec2(1.5, 0.0)).rgb;
    blur += texture(texture0, uv + texel * vec2(-1.5, 0.0)).rgb;
    blur += texture(texture0, uv + texel * vec2(0.0, 1.5)).rgb;
    blur += texture(texture0, uv + texel * vec2(0.0, -1.5)).rgb;
    blur += texture(texture0, uv + texel * vec2(1.1, 1.1)).rgb;
    blur += texture(texture0, uv + texel * vec2(-1.1, 1.1)).rgb;
    blur += texture(texture0, uv + texel * vec2(1.1, -1.1)).rgb;
    blur += texture(texture0, uv + texel * vec2(-1.1, -1.1)).rgb;
    blur *= 0.125;
    color += blur * smoothstep(0.62, 1.08, luminance(blur)) * bloomStrength;
    color = pow(max(color, vec3(0.0)), vec3(0.96));
    color *= vec3(1.025, 1.01, 0.985);
    float vignette = smoothstep(0.18, 0.72, radial);
    color *= 1.0 - vignette * vignetteStrength;
    color = mix(color, color * vec3(1.34, 0.45, 0.40) + vec3(0.14, 0.0, 0.0), damageFlash);
    float scopeEdge = smoothstep(0.50, 0.72, length(centered) * 1.42) * scopeBlend;
    color *= 1.0 - scopeEdge * 0.48;
    color += sin(time * 2.0) * 0.0015;
    finalColor = vec4(color, 1.0) * fragColor;
}
