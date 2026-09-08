#version 330
in vec3 worldPosition;
uniform vec3 eye;
uniform vec3 skyTint;
uniform vec3 sunDirection;
uniform float sunIntensity;
uniform float time;
uniform int quality;
out vec4 finalColor;

float Hash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float Noise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(Hash(i), Hash(i + vec2(1, 0)), f.x),
               mix(Hash(i + vec2(0, 1)), Hash(i + vec2(1, 1)), f.x), f.y);
}
float Clouds(vec2 p)
{
    float n = 0.0, weight = 0.57;
    for (int i = 0; i < 4; ++i)
    {
        n += Noise(p) * weight;
        p = mat2(1.6, 1.2, -1.2, 1.6) * p + 13.7;
        weight *= 0.5;
    }
    return smoothstep(0.48, 0.76, n);
}
void main()
{
    vec3 ray = normalize(worldPosition - eye);
    float altitude = max(ray.y, 0.0);
    vec3 horizon = mix(skyTint, vec3(0.86, 0.89, 0.94), 0.38);
    vec3 zenith = skyTint * vec3(0.48, 0.66, 0.92);
    vec3 color = mix(horizon, zenith, pow(altitude, 0.45));
    float mu = clamp(dot(ray, sunDirection), 0.0, 1.0);
    float halo = pow(mu, 24.0) * 0.18 + pow(mu, 180.0) * 0.16;
    float disk = smoothstep(0.99965, 0.99985, mu);
    float daylight = clamp(dot(skyTint, vec3(0.2126, 0.7152, 0.0722)) * 2.0, 0.0, 1.0);
    color += vec3(1.0, 0.83, 0.56) * (halo + disk * 0.9) * sunIntensity * daylight;
    if (quality >= 2 && ray.y > 0.035)
    {
        // A distant cloud layer anchored in world space, with slow wind.
        vec2 p = (eye.xz + ray.xz * (160.0 / max(ray.y, 0.035))) * 0.006;
        p += vec2(time * 0.003, time * 0.001);
        float density = Clouds(p);
        float sunEdge = max(density - Clouds(p + sunDirection.xz * 0.16), 0.0);
        vec3 cloudColor = mix(skyTint * 0.8, vec3(0.96, 0.94, 0.90), daylight * 0.86);
        cloudColor += sunEdge * vec3(1.0, 0.77, 0.43) * sunIntensity;
        color = mix(color, cloudColor, density * smoothstep(0.035, 0.18, ray.y) * 0.94);
    }
    finalColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
