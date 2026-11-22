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
    self.metalView.clearColor = MTLClearColorMake(0.05, 0.05, 0.05, 1.0);
    
    // Força o formato de cor padrão para evitar incompatibilidade
    self.metalView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    [self.view addSubview:self.metalView];
    
    self.commandQueue = [device newCommandQueue];
    
    NSError *error = nil;
    id<MTLLibrary> library = [device newDefaultLibrary];
    if (!library) {
        NSLog(@"[NGBC Engine] ERRO FATAL: default.metallib não encontrado no app!");
        return;
    }
    
    MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDesc.vertexFunction = [library newFunctionWithName:@"vertex_main"];
    pipelineDesc.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
    pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    
    self.pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
    if (error || !self.pipelineState) {
        NSLog(@"[NGBC Engine] ERRO no Pipeline: %@", error.localizedDescription);
        return;
    }
    
    // Vértices: X, Y, Z, W | U, V
    float vertices[] = {
        -0.5,  0.5, 0, 1,  0, 0,
         0.5,  0.5, 0, 1,  1, 0,
        -0.5, -0.5, 0, 1,  0, 1,
         0.5, -0.5, 0, 1,  1, 1
    };
    self.vertexBuffer = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
    
    NSLog(@"[NGBC Engine] Renderizador 2D inicializado com sucesso!");
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {}

- (void)drawInMTKView:(MTKView *)view {
    // PROTEÇÃO: Se o pipeline não existir, não tenta desenhar e não crasha o app!
    if (!self.pipelineState) return; 

    id<CAMetalDrawable> drawable = view.currentDrawable;
    MTLRenderPassDescriptor *passDesc = view.currentRenderPassDescriptor;
    
    if (drawable && passDesc) {
        id<MTLCommandBuffer> commandBuffer = [self.commandQueue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:passDesc];
        
        [encoder setRenderPipelineState:self.pipelineState];
        [encoder setVertexBuffer:self.vertexBuffer offset:0 atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        
        [encoder endEncoding];
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];
    }
}

@end