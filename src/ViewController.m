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
    
    self.view.backgroundColor = [UIColor redColor];
    
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    self.metalView = [[MTKView alloc] initWithFrame:self.view.bounds device:device];
    self.metalView.delegate = self;
    self.metalView.clearColor = MTLClearColorMake(0.1, 0.1, 0.1, 1.0); // Fundo cinza escuro
    self.metalView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    [self.view addSubview:self.metalView];
    
    self.debugLabel = [[UILabel alloc] initWithFrame:CGRectMake(10, 60, self.view.bounds.size.width - 20, 100)];
    self.debugLabel.numberOfLines = 0;
    self.debugLabel.textColor = [UIColor whiteColor];
    self.debugLabel.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.8];
    self.debugLabel.font = [UIFont systemFontOfSize:14];
    self.debugLabel.text = @"Motor OK! Renderizando quadrado...";
    [self.view addSubview:self.debugLabel];
    
    self.commandQueue = [device newCommandQueue];
    
    NSError *error = nil;
    id<MTLLibrary> library = [device newDefaultLibrary];
    if (!library) {
        self.debugLabel.text = @"ERRO FATAL: default.metallib nao encontrado!";
        return;
    }
    
    MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDesc.vertexFunction = [library newFunctionWithName:@"vertex_main"];
    pipelineDesc.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
    pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    
    self.pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
    if (error || !self.pipelineState) {
        self.debugLabel.text = [NSString stringWithFormat:@"ERRO PIPELINE:\n%@", error.localizedDescription];
        return;
    }
    
    float vertices[] = {
        -0.5,  0.5, 0, 1,  0, 0,
         0.5,  0.5, 0, 1,  1, 0,
        -0.5, -0.5, 0, 1,  0, 1,
         0.5, -0.5, 0, 1,  1, 1
    };
    self.vertexBuffer = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
    
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
        
        // FORÇANDO O VIEWPORT (Área de desenho)
        MTLViewport viewport = { 0.0, 0.0, (double)view.drawableSize.width, (double)view.drawableSize.height, 0.0, 1.0 };
        [encoder setViewport:viewport];
        
        MTLScissorRect scissor = { 0, 0, (NSUInteger)view.drawableSize.width, (NSUInteger)view.drawableSize.height };
        [encoder setScissorRect:scissor];
        
        [encoder setRenderPipelineState:self.pipelineState];
        [encoder setVertexBuffer:self.vertexBuffer offset:0 atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        
        [encoder endEncoding];
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];
    }
}

@end