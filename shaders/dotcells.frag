#version 440
// Signal dot field, pass 1: one fragment per dot cell. Renders into a tiny
// texture (screen size / spacing) that glass.frag samples per pixel, so the
// expensive part (hash, trig, exp) runs ~80x less often than per pixel.
// Output: r = dot intensity 0..1.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 sel;        // selection x, y, w, h in window pixels
    vec2 size;       // window size in pixels
    vec2 cellCount;  // cells across, down (this texture's size)
    float radius;    // selection corner radius
    float hasSel;    // 0 or 1
    float focusAmt;  // 0 = idle, 1 = selection focused
    float wave;      // 0..1 release ripple, < 0 = off
    float pulse;     // 0..1 "text is ready" edge flash, < 0 = off
    float time;      // seconds
    float dots;      // 0 = no dot field, 1 = full
    float spacing;   // dot grid pitch in pixels
    float boost;     // user dot brightness, 1 = default
};

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
    vec2 cell = floor(qt_TexCoord0 * cellCount);
    vec2 cc = (cell + 0.5) * spacing;  // dot centre in window pixels
    float h = hash(cell);

    vec2 c = sel.xy + sel.zw * 0.5;
    float r = min(radius, 0.5 * min(sel.z, sel.w));
    // Distance of the whole dot to the selection, so dots light up as units.
    float dd = hasSel > 0.5 ? sdRoundRect(cc - c, sel.zw * 0.5, r) : 1e6;

    float shimmer = 0.6 + 0.4 * sin(time * 2.2 + h * 2.0 * PI);
    float intensity = 0.0;

    // Edge glow: dots light up along the screen border, with soft bright
    // bands travelling around it. Leaves the middle of the screen clear.
    float edgeDist = min(min(cc.x, cc.y), min(size.x - cc.x, size.y - cc.y));
    float edge = exp(-edgeDist / (spacing * 5.0));
    if (edge > 0.002) {
        float around = atan(cc.y - 0.5 * size.y, cc.x - 0.5 * size.x);
        float bands = 0.5 + 0.5 * sin(around * 3.0 - time * 0.9);
        intensity += edge * (0.3 + 0.75 * bands * bands) * shimmer * (1.0 - 0.45 * focusAmt);
    }
    if (pulse >= 0.0) {
        // Quick rise, slower fall; reaches a little further in than the glow.
        float env = sin(PI * sqrt(pulse)) * (1.0 - pulse * 0.3);
        intensity += exp(-edgeDist / (spacing * 9.0)) * env * 1.3;
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
    fragColor = vec4(clamp(intensity * dots * boost, 0.0, 1.0), 0.0, 0.0, 1.0);
}
