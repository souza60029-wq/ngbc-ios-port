#import "ViewController.h"

@interface ViewController ()
@property (nonatomic, strong) id<MTLRenderPipelineState> pipelineState;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property (nonatomic, strong) id<MTLBuffer> vertexBuffer;
@property (nonatomic, strong) UILabel *debugLabel;
@end

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    
    // Se a tela ficar VERMELHA, significa que a MTKView não está cobrindo ela.
    self.view.backgroundColor = [UIColor redColor];
    
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    self.metalView = [[MTKView alloc] initWithFrame:self.view.bounds device:device];
    self.metalView.delegate = self;
    
    // Se o Metal estiver funcionando, a tela vai ficar ROSA (Magenta).
    self.metalView.clearColor = MTLClearColorMake(1.0, 0.0, 1.0, 1.0); 
    self.metalView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    [self.view addSubview:self.metalView];
    
    // Cria a caixa de debug na tela
    self.debugLabel = [[UILabel alloc] initWithFrame:CGRectMake(10, 60, self.view.bounds.size.width - 20, 200)];
    self.debugLabel.numberOfLines = 0;
    self.debugLabel.textColor = [UIColor whiteColor];
    self.debugLabel.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.8];
    self.debugLabel.font = [UIFont systemFontOfSize:14];
    self.debugLabel.text = @"Iniciando Motor...";
    [self.view addSubview:self.debugLabel];
    
    self.commandQueue = [device newCommandQueue];
    
    NSError *error = nil;
    // Tenta carregar a biblioteca de shaders
    id<MTLLibrary> library = [device newDefaultLibrary];
    if (!library) {
        self.debugLabel.text = @"ERRO FATAL: O arquivo default.metallib nao foi encontrado dentro do app!";
        return;
    }
    
    MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDesc.vertexFunction = [library newFunctionWithName:@"vertex_main"];
    pipelineDesc.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
    pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    
    self.pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
    if (error || !self.pipelineState) {
        // Se der erro, imprime na tela!
        self.debugLabel.text = [NSString stringWithFormat:@"ERRO NO PIPELINE:\n%@", error.localizedDescription];
        return;
    }
    
    float vertices[] = {
        -0.5,  0.5, 0, 1,  0, 0,
         0.5,  0.5, 0, 1,  1, 0,
        -0.5, -0.5, 0, 1,  0, 1,
         0.5, -0.5, 0, 1,  1, 1
    };
    self.vertexBuffer = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
    
    self.debugLabel.text = @"Motor OK! Renderizando quadrado...";
    
    // Força a renderização contínua (Loop de 60 FPS)
    self.metalView.paused = NO;
    self.metalView.enableSetNeedsDisplay = NO;
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {}

- (void)drawInMTKView:(MTKView *)view {
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