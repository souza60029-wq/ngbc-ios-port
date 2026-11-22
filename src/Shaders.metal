#include <metal_stdlib>
using namespace metal;

// Estrutura que recebe os dados de cada vértice (pontos do sprite)
struct VertexIn {
    float4 position [[attribute(0)]];
    float2 texCoord [[attribute(1)]];
};

// O que o Vertex Shader devolve para o Fragment Shader
struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
};

// Vertex Shader: Posiciona o sprite na tela
vertex VertexOut vertex_main(const device VertexIn *vertices [[buffer(0)]],
                             uint vid [[vertex_id]]) {
    VertexOut out;
    out.position = vertices[vid].position;
    out.texCoord = vertices[vid].texCoord;
    return out;
}

// Fragment Shader: Pinta o pixel na tela com a cor/textura
fragment float4 fragment_main(VertexOut in [[stage_in]],
                              texture2d<float> tex [[texture(0)]],
                              sampler s [[sampler(0)]]) {
    // Como ainda não temos a textura do Rock, vamos desenhar uma cor sólida (Azul da jaqueta dele)
    // return tex.sample(s, in.texCoord); <- Isso será usado quando tivermos as texturas reais
    return float4(0.2, 0.4, 0.8, 1.0); // Cor Azul
}