#version 440
// Selsearch lens backdrop, one pass: the frozen screen, darkened outside a
// crisp rounded selection, with a reactive halftone dot field ("signal").

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
    float idleDim;   // darkening with no selection
    float dim;       // darkening outside a focused selection
    float rimAlpha;  // base rim opacity
    float rimWidth;  // rim thickness in pixels
    float wave;      // 0..1 progress of the release ripple, < 0 = off
    float pulse;     // 0..1 progress of the "text is ready" edge flash, < 0 = off
    float time;      // seconds, drives shimmer / edge glow
    float dots;      // 0 = no dot field (mono), 1 = full
    float spacing;   // dot grid pitch in pixels
    float tintMix;   // aurora: tint of the darkened area
    vec4 rimColor;
    vec4 rimColor2;  // aurora second colour (== rimColor otherwise)
    vec4 dotColor;
};
layout(binding = 1) uniform sampler2D source;

const float PI = 3.14159265;

float sdRoundRect(vec2 p, vec2 halfSize, float r)
{
    vec2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float hash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

void main()
{
    vec2 px = qt_TexCoord0 * size;
    vec3 crisp = texture(source, qt_TexCoord0).rgb;

    vec2 c = sel.xy + sel.zw * 0.5;
    float r = min(radius, 0.5 * min(sel.z, sel.w));
    float d = hasSel > 0.5 ? sdRoundRect(px - c, sel.zw * 0.5, r) : 1e6;
    float inside = 1.0 - smoothstep(-0.75, 0.75, d);
    float ang = atan(px.y - c.y, px.x - c.x);
    float angMix = 0.5 + 0.5 * sin(ang + 0.6);

    // Darken outside (no blur).
    vec3 outside = crisp;
    vec3 tint = mix(rimColor.rgb, rimColor2.rgb, angMix);
    outside = mix(outside, outside * 0.6 + tint * 0.4, tintMix * focusAmt);
    outside *= 1.0 - mix(idleDim, dim, focusAmt);
    vec3 col = outside;

    // ---- dot field ---------------------------------------------------------
    if (dots > 0.0) {
        vec2 cell = floor(px / spacing);
        vec2 cc = (cell + 0.5) * spacing;  // dot centre
        float h = hash(cell);
        // Distance of the whole dot (not the pixel) to the selection, so dots
        // light up as units.
        float dd = hasSel > 0.5 ? sdRoundRect(cc - c, sel.zw * 0.5, r) : 1e6;

        float shimmer = 0.6 + 0.4 * sin(time * 2.2 + h * 2.0 * PI);
        float intensity = 0.0;

        // Edge glow: dots light up along the screen border, with soft bright
        // bands travelling around it. Leaves the middle of the screen clear.
        float edgeDist = min(min(cc.x, cc.y), min(size.x - cc.x, size.y - cc.y));
        float edge = exp(-edgeDist / (spacing * 5.0));
        float around = atan(cc.y - 0.5 * size.y, cc.x - 0.5 * size.x);
        float bands = 0.5 + 0.5 * sin(around * 3.0 - time * 0.9);
        intensity += edge * (0.3 + 0.75 * bands * bands) * shimmer * (1.0 - 0.45 * focusAmt);
        if (pulse >= 0.0) {
            // Quick rise, slower fall; reaches a little further in than the glow.
            float env = sin(PI * sqrt(pulse)) * (1.0 - pulse * 0.3);
            float reach = exp(-edgeDist / (spacing * 9.0));
            intensity += reach * env * 1.3;
        }

        if (hasSel > 0.5 && dd > 0.0) {
            // Halo hugging the selection edge.
            float near = exp(-dd / (spacing * 6.0));
            intensity += near * 0.75 * shimmer * max(focusAmt, 0.4);
            // Sparks: random dots near the edge flash briefly.
            float spark = step(0.965, hash(cell + floor(time * 4.0 + h * 7.0)));
            intensity += spark * near * 0.9;
            // Release ripple travelling outwards.
            if (wave >= 0.0) {
                float reach = wave * max(size.x, size.y) * 0.55;
                float ring = exp(-pow((dd - reach) / (spacing * 3.0), 2.0));
                intensity += ring * (1.0 - wave) * 1.2;
            }
        }
        intensity = clamp(intensity * dots, 0.0, 1.0);

        // Dot grows a little with intensity; anti-aliased disc.
        float rr = length(px - cc);
        float dotR = spacing * (0.14 + 0.16 * intensity);
        float disc = 1.0 - smoothstep(dotR - 0.7, dotR + 0.7, rr);
        // Soft glow around bright dots.
        float glow = exp(-rr * rr / (spacing * spacing * 0.35)) * intensity * 0.35;
        float a = (disc * intensity + glow) * (1.0 - inside);
        col = mix(col, dotColor.rgb, clamp(a, 0.0, 1.0));
    }

    col = mix(col, crisp, inside);

    if (hasSel > 0.5) {
        // Soft contact shadow hugging the selection from outside.
        float shadow = exp(-max(d, 0.0) / 22.0) * (1.0 - inside) * 0.4 * focusAmt;
        col *= 1.0 - shadow;
        // Rim, just inside the edge.
        float rim = 1.0 - smoothstep(rimWidth * 0.5, rimWidth * 0.5 + 1.0, abs(d + rimWidth * 0.5));
        vec3 rimCol = mix(rimColor.rgb, rimColor2.rgb, angMix);
        float a = rimAlpha;
        if (wave >= 0.0)
            a = clamp(a + (1.0 - wave) * 0.4, 0.0, 1.0);
        col = mix(col, rimCol, rim * a);
    }
    fragColor = vec4(col, 1.0) * qt_Opacity;
}
