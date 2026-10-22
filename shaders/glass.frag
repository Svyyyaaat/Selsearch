#version 440
// Selsearch lens backdrop, pass 2 (per pixel): the frozen screen, darkened
// outside a crisp rounded selection, with the halftone dot field whose
// per-dot intensity comes from dotcells.frag (one texel per dot).

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 sel;        // selection x, y, w, h in item pixels
    vec2 size;       // item size in pixels
    vec2 cellCount;  // dot cells across, down (size of `cells`)
    float radius;    // selection corner radius
    float hasSel;    // 0 or 1
    float focusAmt;  // 0 = idle (light dim), 1 = selection focused
    float idleDim;   // darkening with no selection
    float dim;       // darkening outside a focused selection
    float rimAlpha;  // base rim opacity
    float rimWidth;  // rim thickness in pixels
    float wave;      // 0..1 progress of the release ripple, < 0 = off
    float dots;      // 0 = no dot field (mono)
    float spacing;   // dot grid pitch in pixels
    float tintMix;   // aurora: tint of the darkened area
    float boost;     // user dot brightness, 1 = default
    vec4 rimColor;
    vec4 rimColor2;  // aurora second colour (== rimColor otherwise)
    vec4 dotColor;
};
layout(binding = 1) uniform sampler2D source;
layout(binding = 2) uniform sampler2D cells;

float sdRoundRect(vec2 p, vec2 halfSize, float r)
{
    vec2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    vec2 px = qt_TexCoord0 * size;
    vec3 crisp = texture(source, qt_TexCoord0).rgb;

    vec2 c = sel.xy + sel.zw * 0.5;
    float r = min(radius, 0.5 * min(sel.z, sel.w));
    float d = hasSel > 0.5 ? sdRoundRect(px - c, sel.zw * 0.5, r) : 1e6;
    float inside = 1.0 - smoothstep(-0.75, 0.75, d);

    // Angle around the selection, only needed for the two-hue aurora look.
    bool twoHue = tintMix > 0.0;
    float angMix = twoHue ? 0.5 + 0.5 * sin(atan(px.y - c.y, px.x - c.x) + 0.6) : 0.0;
    vec3 tint = mix(rimColor.rgb, rimColor2.rgb, angMix);

    // Darken outside (no blur).
    vec3 col = crisp;
    if (twoHue)
        col = mix(col, col * 0.6 + tint * 0.4, tintMix * focusAmt);
    col *= 1.0 - mix(idleDim, dim, focusAmt);

    // ---- dot field ---------------------------------------------------------
    if (dots > 0.0 && inside < 1.0) {
        vec2 cell = floor(px / spacing);
        float intensity = texture(cells, (cell + 0.5) / cellCount).r;
        if (intensity > 0.002) {
            vec2 cc = (cell + 0.5) * spacing;
            // Dot grows a little with intensity; anti-aliased disc.
            float rr = length(px - cc);
            float dotR = spacing * (0.14 + 0.16 * intensity);
            float disc = 1.0 - smoothstep(dotR - 0.7, dotR + 0.7, rr);
            // Soft glow around bright dots (polynomial falloff, no exp per pixel).
            float g = max(0.0, 1.0 - rr * rr / (spacing * spacing * 0.9));
            float glow = g * g * intensity * 0.35;
            float a = (disc * intensity + glow) * (1.0 - inside);
            // Brighter than default also lifts dark accents towards white.
            vec3 dc = mix(dotColor.rgb, vec3(1.0), clamp((boost - 1.0) * 0.2, 0.0, 0.4));
            col = mix(col, dc, clamp(a, 0.0, 1.0));
        }
    }

    col = mix(col, crisp, inside);

    if (hasSel > 0.5) {
        // Soft contact shadow hugging the selection from outside.
        if (d < 110.0 && focusAmt > 0.0) {
            float shadow = exp(-max(d, 0.0) / 22.0) * (1.0 - inside) * 0.4 * focusAmt;
            col *= 1.0 - shadow;
        }
        // Rim, just inside the edge.
        float rim = 1.0 - smoothstep(rimWidth * 0.5, rimWidth * 0.5 + 1.0, abs(d + rimWidth * 0.5));
        float a = rimAlpha;
        if (wave >= 0.0)
            a = clamp(a + (1.0 - wave) * 0.4, 0.0, 1.0);
        col = mix(col, tint, rim * a);
    }
    fragColor = vec4(col, 1.0) * qt_Opacity;
}
