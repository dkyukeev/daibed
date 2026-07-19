#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec2 sourceSize;
uniform float quality;

out vec4 finalColor;

float luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

vec3 ExtractBloom(vec3 color)
{
    // The scene target is LDR, so a soft knee starts below pure white.  It
    // retains the warm highlight visible in the references and avoids hard
    // halos around ordinary light-colored blocks.
    float amount = smoothstep(0.54, 0.98, luminance(color));
    return color * amount;
}

void main()
{
    // Bright-pass downsample into the tiny bloom target.  The separable
    // Gaussian passes that follow do the actual spreading, so this stage
    // only needs enough taps to keep the downsample stable in motion.
    vec3 bloom = ExtractBloom(texture(texture0, fragTexCoord).rgb);

    // Quality 0: a single bilinear fetch.  Higher presets average a wider
    // 4x4 footprint to stop small emitters from flickering when the camera
    // pans across them.
    if (quality > 0.5)
    {
        vec2 texel = 1.0 / max(sourceSize, vec2(1.0));
        bloom *= 0.36;
        bloom += ExtractBloom(texture(texture0, fragTexCoord + texel * vec2(-1.0, -1.0)).rgb) * 0.16;
        bloom += ExtractBloom(texture(texture0, fragTexCoord + texel * vec2(1.0, -1.0)).rgb) * 0.16;
        bloom += ExtractBloom(texture(texture0, fragTexCoord + texel * vec2(-1.0, 1.0)).rgb) * 0.16;
        bloom += ExtractBloom(texture(texture0, fragTexCoord + texel * vec2(1.0, 1.0)).rgb) * 0.16;
    }

    finalColor = vec4(bloom, 1.0) * fragColor;
}
