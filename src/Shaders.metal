#include <metal_stdlib>
using namespace metal;

// Estrutura que recebe os dados do vértice direto do buffer de memória
struct VertexIn {
    float4 position;
    float2 texCoord;
};

struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
};

vertex VertexOut vertex_main(const device VertexIn *vertices [[buffer(0)]],
                             uint vid [[vertex_id]]) {
    VertexOut out;
    out.position = vertices[vid].position;
    out.texCoord = vertices[vid].texCoord;
    return out;
}

fragment float4 fragment_main(VertexOut in [[stage_in]]) {
    // Retorna a cor Azul (Jacket do Rock)
    return float4(0.2, 0.4, 0.8, 1.0);
}