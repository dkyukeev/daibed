#version 330

in vec2 fragTexCoord;
in vec3 fragPosition;
in vec3 fragNormal;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 viewPos;
uniform vec3 lightDirection;
uniform vec4 lightColor;
uniform vec4 ambientColor;
uniform vec4 fogColor;
uniform float fogDensity;
uniform float time;

out vec4 finalColor;

void main()
{
    vec4 albedo = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    if (albedo.a < 0.015) discard;
    vec3 normal = normalize(fragNormal);
    float diffuse = max(dot(normal, normalize(-lightDirection)), 0.0);
    float halfLambert = 0.35 + diffuse * 0.65;
    vec3 lit = albedo.rgb * (ambientColor.rgb + lightColor.rgb * halfLambert * 0.72);
    float emissiveMask = smoothstep(0.68, 1.0, max(albedo.r, max(albedo.g, albedo.b)));
    float energyMask = smoothstep(0.18, 0.62, abs(albedo.b - albedo.r));
    float materialPulse = 0.82 + 0.18 * sin(time * 3.2 + fragPosition.x * 0.72 + fragPosition.z * 0.61);
    lit += albedo.rgb * emissiveMask * (0.18 + energyMask * 0.20 * materialPulse);
    float distanceToCamera = length(viewPos - fragPosition);
    float fogAmount = 1.0 - exp(-fogDensity * fogDensity * distanceToCamera * distanceToCamera);
    finalColor = vec4(mix(lit, fogColor.rgb, clamp(fogAmount, 0.0, 0.86)), albedo.a);
}
