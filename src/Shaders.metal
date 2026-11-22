#include <metal_stdlib>
using namespace metal;

// A MÁGICA ACONTECE AQUI: usando "packed_" para alinhar a memória com o C
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
    // Precisamos converter para float4 normal aqui dentro
    out.position = float4(vertices[vid].position);
    out.texCoord = float2(vertices[vid].texCoord);
    return out;
}

fragment float4 fragment_main(VertexOut in [[stage_in]]) {
    // Retorna a cor Azul (Jacket do Rock)
    return float4(0.2, 0.4, 0.8, 1.0);
}