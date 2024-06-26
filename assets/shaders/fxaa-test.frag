#version 450

precision highp float;
precision highp int;

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform mediump sampler2D uInput;
layout(set = 0, binding = 1) uniform mediump sampler2D mvTexture;

const float FXAA_REDUCE_MIN = 1.0 / 128.0;
const float FXAA_REDUCE_MUL = 1.0 / 8.0;
const float FXAA_SPAN_MAX = 8.0;

layout(std140, set = 0, binding = 2) uniform Registers
{
    vec2 inv_resolution;
} registers;

void main()
{
    // FragColor = vec4(vUV, 0, 1);
    
//     mediump vec2 texColor2 = textureLod(mvTexture, vUV, 0.0).rg;
//     FragColor = vec4(abs(texColor2), 0, 1);    
    
//    return;
    mediump vec3 rgbNW = textureLodOffset(uInput, vUV, 0.0, ivec2(-1, -1)).rgb;
    mediump vec3 rgbNE = textureLodOffset(uInput, vUV, 0.0, ivec2(+1, -1)).rgb;
    mediump vec3 rgbSW = textureLodOffset(uInput, vUV, 0.0, ivec2(-1, +1)).rgb;
    mediump vec3 rgbSE = textureLodOffset(uInput, vUV, 0.0, ivec2(+1, +1)).rgb;
    mediump vec3 texColor = textureLod(uInput, vUV, 0.0).rgb;

    const mediump vec3 luma = vec3(0.299, 0.587, 0.114);
    mediump float lumaNW = dot(rgbNW, luma);
    mediump float lumaNE = dot(rgbNE, luma);
    mediump float lumaSW = dot(rgbSW, luma);
    mediump float lumaSE = dot(rgbSE, luma);
    mediump float lumaM  = dot(texColor, luma);
    mediump float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    mediump float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    mediump vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));
    
    vec2 ddir = dir;
    mediump float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) *
                                  (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);

    mediump float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -FXAA_SPAN_MAX, FXAA_SPAN_MAX) * registers.inv_resolution;

    mediump vec3 rgbA = 0.5 * (
        textureLod(uInput, vUV + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
        textureLod(uInput, vUV + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    mediump vec3 rgbB = rgbA * 0.5 + 0.25 * (
        textureLod(uInput, vUV + dir * -0.5, 0.0).rgb +
        textureLod(uInput, vUV + dir * 0.5, 0.0).rgb);

    mediump float lumaB = dot(rgbB, luma);
    mediump vec3 color;
    if ((lumaB < lumaMin) || (lumaB > lumaMax))
        color = rgbA;
    else
        color = rgbB;
        
    FragColor.rgb = color;
}