#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    packed_float4 position;
    packed_float2 texCoord;
};

struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
};

vertex VertexOut vertex_main(const device VertexIn *vertices [[buffer(0)]],
                             uint vid [[vertex_id]]) {
    VertexOut out;
    out.position = float4(vertices[vid].position);
    out.texCoord = float2(vertices[vid].texCoord);
    return out;
}

fragment float4 fragment_main(VertexOut in [[stage_in]],
                              texture2d<float> tex [[texture(0)]],
                              sampler samp [[sampler(0)]]) {
    // Lê a textura real em vez da cor fixa
    return tex.sample(samp, in.texCoord);
}