#include "ReShade.fxh"

// The add-on replaces this semantic with its latest GPU-only SA frame.
texture SaFrame : SAFRAME;
sampler SaSampler { Texture = SaFrame; AddressU = CLAMP; AddressV = CLAMP; MinFilter = LINEAR; MagFilter = LINEAR; };

uniform bool SAActive = false;
uniform bool DepthReady < source = "bufready_depth"; >;
uniform float3 CameraPosition = 0.0;
uniform float3 CameraRotation = 0.0;
uniform float CameraFov = 60.0;
uniform float2 CameraPlanes = float2(0.15, 10000.0);
uniform float3 PlaneCenter = 0.0;
uniform float PlaneYaw = 0.0;
uniform float2 PlaneSize = float2(8.0, 8.0);
uniform int PlaneShape = 2;
uniform float CornerRadius = 0.16;
uniform float EdgeFeather = 0.008;
uniform float4 PlaneCrop = float4(0.21875, 0.0, 0.78125, 1.0);
uniform float2 SAFrameSize = float2(1920.0, 1080.0);
uniform bool HostReversedZ < ui_label = "GTA depth is reversed"; > = true;
uniform float DepthBias < ui_type = "drag"; ui_min = 0.0; ui_max = 0.5; ui_step = 0.005; > = 0.03;

float HostLinearDepth(float depth)
{
    const float n = CameraPlanes.x, f = CameraPlanes.y;
    if (HostReversedZ)
        return depth > 0.0 ? n * f / (n + depth * (f - n)) : 1e9;
    return depth < 1.0 ? n * f / (f - depth * (f - n)) : 1e9;
}

float4 PS_Portal(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float4 host = tex2D(ReShade::BackBuffer, uv);
    // Never draw the pane without a host depth buffer: occluders would be ignored.
    if (!SAActive || !DepthReady)
        return host;

    const float yaw = radians(CameraRotation.z);
    const float pitch = radians(CameraRotation.x);
    const float3 forward = float3(-sin(yaw) * cos(pitch), cos(yaw) * cos(pitch), sin(pitch));
    const float3 right = float3(cos(yaw), sin(yaw), 0.0);
    const float3 up = cross(right, forward);
    const float tanHalf = tan(radians(CameraFov) * 0.5);
    const float aspect = BUFFER_WIDTH * BUFFER_RCP_HEIGHT;
    const float2 screen = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 ray = forward + right * screen.x * tanHalf * aspect + up * screen.y * tanHalf;

    const float anchorYaw = radians(PlaneYaw);
    const float3 planeNormal = float3(-sin(anchorYaw), cos(anchorYaw), 0.0);
    const float denom = dot(ray, planeNormal);
    // The visual disc billboards toward the local player, while a third-person
    // camera may view either mathematical side. Keep it genuinely two-sided.
    if (abs(denom) < 1e-5)
        return host;
    const float distance = dot(PlaneCenter - CameraPosition, planeNormal) / denom;
    if (distance <= 0.0)
        return host;

    const float3 hit = CameraPosition + ray * distance;
    const float3 planeRight = float3(cos(anchorYaw), sin(anchorYaw), 0.0);
    const float2 local = float2(dot(hit - PlaneCenter, planeRight), hit.z - PlaneCenter.z);
    const float2 portalUv = float2(local.x / PlaneSize.x + 0.5, 0.5 - local.y / PlaneSize.y);
    if (any(portalUv < 0.0) || any(portalUv > 1.0))
        return host;

    const float2 centered = abs(portalUv * 2.0 - 1.0);
    float inside = min(1.0 - centered.x, 1.0 - centered.y);
    if (PlaneShape == 2)
        inside = 1.0 - length(centered);
    if (PlaneShape == 1)
    {
        const float radius = saturate(CornerRadius) * 2.0;
        const float2 corner = centered - (1.0 - radius);
        const float signedDistance = length(max(corner, 0.0)) +
            min(max(corner.x, corner.y), 0.0) - radius;
        inside = -signedDistance;
    }
    if (inside < 0.0)
        return host;
    const float edgeAlpha = EdgeFeather > 0.00001 ?
        smoothstep(0.0, EdgeFeather, inside) : 1.0;

    const float2 saUv = lerp(PlaneCrop.xy, PlaneCrop.zw, portalUv);
    const float2 dx = ddx(saUv), dy = ddy(saUv);

    // Depth is compared in camera-forward metres, matching the host depth linearization.
    const float planeDepth = dot(hit - CameraPosition, forward);
    const float hostDepth = HostLinearDepth(tex2D(ReShade::DepthBuffer, uv).x);
    if (planeDepth > hostDepth + DepthBias)
        return host;
    // Four bilinear taps approximate a box filter only when the pane shrinks on screen.
    const float footprint = max(length(dx * SAFrameSize), length(dy * SAFrameSize));
    float3 color = tex2D(SaSampler, saUv).rgb;
    if (footprint > 1.5)
    {
        const float2 a = dx * 0.25, b = dy * 0.25;
        color = (tex2D(SaSampler, saUv - a - b).rgb +
                 tex2D(SaSampler, saUv + a - b).rgb +
                 tex2D(SaSampler, saUv - a + b).rgb +
                 tex2D(SaSampler, saUv + a + b).rgb) * 0.25;
    }
    return float4(lerp(host.rgb, color, edgeAlpha), 1.0);
}

technique PortalSA
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_Portal;
        BlendEnable = false;
        SRGBWriteEnable = false;
    }
}
