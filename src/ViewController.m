#import "ViewController.h"

@interface ViewController ()
@property (nonatomic, strong) id<MTLRenderPipelineState> pipelineState;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property (nonatomic, strong) id<MTLBuffer> vertexBuffer;
@end

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    self.metalView = [[MTKView alloc] initWithFrame:self.view.bounds device:device];
    self.metalView.delegate = self;
    self.metalView.clearColor = MTLClearColorMake(0.05, 0.05, 0.05, 1.0); // Quase preto
    [self.view addSubview:self.metalView];
    
    self.commandQueue = [device newCommandQueue];
    
    // 1. Carrega os shaders compilados pelo GitHub Actions
    NSError *error = nil;
    id<MTLLibrary> library = [device newDefaultLibrary];
    if (!library) {
        NSLog(@"[NGBC Engine] ERRO: Biblioteca de shaders não encontrada!");
        return;
    }
    
    // 2. Configura o Pipeline Gráfico
    MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDesc.vertexFunction = [library newFunctionWithName:@"vertex_main"];
    pipelineDesc.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
    pipelineDesc.colorAttachments[0].pixelFormat = self.metalView.colorPixelFormat;
    
    self.pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
    if (error) {
        NSLog(@"[NGBC Engine] ERRO no Pipeline: %@", error.localizedDescription);
    }
    
    // 3. Cria os vértices do nosso "Sprite" (um quadrado no centro da tela)
    // X, Y, Z, W | U, V
    float vertices[] = {
        -0.5,  0.5, 0, 1,  0, 0, // Topo Esquerda
         0.5,  0.5, 0, 1,  1, 0, // Topo Direita
        -0.5, -0.5, 0, 1,  0, 1, // Baixo Esquerda
         0.5, -0.5, 0, 1,  1, 1  // Baixo Direita
    };
    self.vertexBuffer = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
    
    NSLog(@"[NGBC Engine] Renderizador 2D inicializado com sucesso!");
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {}

- (void)drawInMTKView:(MTKView *)view {
    // Pega o próximo frame para desenhar
    id<CAMetalDrawable> drawable = view.currentDrawable;
    MTLRenderPassDescriptor *passDesc = view.currentRenderPassDescriptor;
    
    if (drawable && passDesc) {
        id<MTLCommandBuffer> commandBuffer = [self.commandQueue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:passDesc];
        
        // Manda a GPU desenhar!
        [encoder setRenderPipelineState:self.pipelineState];
        [encoder setVertexBuffer:self.vertexBuffer offset:0 atIndex:0];
        
        // Desenha os 4 vértices como um Triângulo em Tira (Triangle Strip)
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        
        [encoder endEncoding];
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];
    }
}

@end