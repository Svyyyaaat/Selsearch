#version 440
// Selsearch lens backdrop: the frozen screen with a "frosted" outside and a
// crisp, rounded selection. One pass, no offscreen layers.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 sel;        // selection x, y, w, h in item pixels
    vec2 size;       // item size in pixels
    float radius;    // selection corner radius
    float hasSel;    // 0 or 1
    float focusAmt;  // 0 = idle (light dim), 1 = selection focused
    float blurMix;   // how much of the blurred frame shows outside (theme)
    float idleDim;   // darkening with no selection
    float dim;       // darkening outside a focused selection
    float rimAlpha;  // base rim opacity
    float rimWidth;  // rim thickness in pixels
    float sweep;     // 0..1 progress of the light sweep, < 0 = off
    float tintMix;   // aurora: how much the outside is tinted
    vec4 rimColor;
    vec4 rimColor2;  // aurora second colour (== rimColor for single-colour themes)
};
layout(binding = 1) uniform sampler2D source;
layout(binding = 2) uniform sampler2D blurred;

const float PI = 3.14159265;

float sdRoundRect(vec2 p, vec2 halfSize, float r)
{
    vec2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    vec2 px = qt_TexCoord0 * size;
    vec3 crisp = texture(source, qt_TexCoord0).rgb;
    vec3 soft = texture(blurred, qt_TexCoord0).rgb;

    float d = 1e6;
    vec2 c = sel.xy + sel.zw * 0.5;
    if (hasSel > 0.5) {
        float r = min(radius, 0.5 * min(sel.z, sel.w));
        d = sdRoundRect(px - c, sel.zw * 0.5, r);
    }
    float inside = 1.0 - smoothstep(-0.75, 0.75, d);
    float ang = atan(px.y - c.y, px.x - c.x);          // -PI..PI
    float angMix = 0.5 + 0.5 * sin(ang * 1.0 + 0.6);    // aurora gradient position

    // Outside: blend towards the blurred frame and darken.
    vec3 outside = mix(crisp, soft, focusAmt * blurMix);
    vec3 tint = mix(rimColor.rgb, rimColor2.rgb, angMix);
    outside = mix(outside, outside * 0.6 + tint * 0.4, tintMix * focusAmt);
    outside *= 1.0 - mix(idleDim, dim, focusAmt);

    vec3 col = mix(outside, crisp, inside);

    if (hasSel > 0.5) {
        // Soft contact shadow hugging the selection from outside.
        float shadow = exp(-max(d, 0.0) / 22.0) * (1.0 - inside) * 0.45 * focusAmt;
        col *= 1.0 - shadow;

        // Rim, slightly inside the edge so it reads as the selection's border.
        float rim = 1.0 - smoothstep(rimWidth * 0.5, rimWidth * 0.5 + 1.0, abs(d + rimWidth * 0.5));
        vec3 rimCol = mix(rimColor.rgb, rimColor2.rgb, angMix);
        float a = rimAlpha;

        if (sweep >= 0.0) {
            // A highlight travelling once around the rim.
            float t = sweep * 2.0 * PI - PI;
            float da = abs(mod(ang - t + PI, 2.0 * PI) - PI);
            float fade = 1.0 - smoothstep(0.7, 1.0, sweep);
            float glow = exp(-da * da * 10.0) * fade;
            a = clamp(a + glow, 0.0, 1.0);
            float bloom = exp(-abs(d) / 7.0) * glow * 0.55;
            col = mix(col, vec3(1.0), bloom);
        }
        col = mix(col, rimCol, rim * a);
    }
    fragColor = vec4(col, 1.0) * qt_Opacity;
}
