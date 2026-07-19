#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
// Texel step premultiplied with the blur direction on the CPU:
// (texelWidth, 0) for the horizontal pass, (0, texelHeight) for vertical.
uniform vec2 blurStep;

out vec4 finalColor;

void main()
{
    // 9-tap Gaussian collapsed into 5 fetches via bilinear offsets.  The
    // bloom targets are always bilinear-filtered, which this relies on.
    vec3 sum = texture(texture0, fragTexCoord).rgb * 0.2270270270;
    vec2 offset1 = blurStep * 1.3846153846;
    vec2 offset2 = blurStep * 3.2307692308;
    sum += texture(texture0, fragTexCoord + offset1).rgb * 0.3162162162;
    sum += texture(texture0, fragTexCoord - offset1).rgb * 0.3162162162;
    sum += texture(texture0, fragTexCoord + offset2).rgb * 0.0702702703;
    sum += texture(texture0, fragTexCoord - offset2).rgb * 0.0702702703;
    finalColor = vec4(sum, 1.0) * fragColor;
}
