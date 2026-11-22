#import "ViewController.h"

@interface ViewController ()
@property (nonatomic, strong) id<MTLRenderPipelineState> pipelineState;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property (nonatomic, strong) id<MTLBuffer> vertexBuffer;
@property (nonatomic, strong) id<MTLTexture> texture;
@property (nonatomic, strong) id<MTLSamplerState> samplerState;
@property (nonatomic, strong) UILabel *debugLabel;
@end

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    
    self.view.backgroundColor = [UIColor blackColor];
    
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    self.metalView = [[MTKView alloc] initWithFrame:self.view.bounds device:device];
    self.metalView.delegate = self;
    self.metalView.clearColor = MTLClearColorMake(0.1, 0.1, 0.1, 1.0);
    self.metalView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    [self.view addSubview:self.metalView];
    
    self.debugLabel = [[UILabel alloc] initWithFrame:CGRectMake(10, 60, self.view.bounds.size.width - 20, 80)];
    self.debugLabel.numberOfLines = 0;
    self.debugLabel.textColor = [UIColor whiteColor];
    self.debugLabel.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.8];
    self.debugLabel.font = [UIFont systemFontOfSize:14];
    self.debugLabel.text = @"Motor OK! Carregando textura...";
    [self.view addSubview:self.debugLabel];
    
    self.commandQueue = [device newCommandQueue];
    
    NSError *error = nil;
    id<MTLLibrary> library = [device newDefaultLibrary];
    if (!library) {
        self.debugLabel.text = @"ERRO: default.metallib nao encontrado.";
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
    
    // 1. Carrega a textura gerada pelo Python
    NSString *texPath = [[NSBundle mainBundle] pathForResource:@"texture" ofType:@"png" inDirectory:@"converted_assets"];
    if (texPath) {
        MTKTextureLoader *loader = [[MTKTextureLoader alloc] initWithDevice:device];
        self.texture = [loader newTextureWithContentsOfURL:[NSURL fileURLWithPath:texPath] options:nil error:&error];
    }
    
    if (!self.texture) {
        self.debugLabel.text = @"ERRO: Falha ao carregar texture.png!";
        return;
    }
    
    // 2. Configura o Sampler (como a textura é lida)
    MTLSamplerDescriptor *samplerDesc = [[MTLSamplerDescriptor alloc] init];
    samplerDesc.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
    self.samplerState = [device newSamplerStateWithDescriptor:samplerDesc];
    
    // 3. Ajusta os vértices para manter a proporção da tela (evitar quadrado esticado)
    float aspect = self.view.bounds.size.width / self.view.bounds.size.height;
    float s = 0.5f;
    float vertices[] = {
        -s * aspect,  s, 0, 1,  0, 0,
         s * aspect,  s, 0, 1,  1, 0,
        -s * aspect, -s, 0, 1,  0, 1,
         s * aspect, -s, 0, 1,  1, 1
    };
    self.vertexBuffer = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
    
    self.debugLabel.text = @"Motor OK! Textura carregada. Renderizando...";
    self.metalView.paused = NO;
    self.metalView.enableSetNeedsDisplay = NO;
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {}

- (void)drawInMTKView:(MTKView *)view {
    if (!self.pipelineState || !self.texture) return;

    id<CAMetalDrawable> drawable = view.currentDrawable;
    MTLRenderPassDescriptor *passDesc = view.currentRenderPassDescriptor;
    
    if (drawable && passDesc) {
        id<MTLCommandBuffer> commandBuffer = [self.commandQueue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:passDesc];
        
        MTLViewport viewport = { 0.0, 0.0, (double)view.drawableSize.width, (double)view.drawableSize.height, 0.0, 1.0 };
        [encoder setViewport:viewport];
        
        [encoder setRenderPipelineState:self.pipelineState];
        [encoder setVertexBuffer:self.vertexBuffer offset:0 atIndex:0];
        [encoder setFragmentTexture:self.texture atIndex:0];
        [encoder setFragmentSamplerState:self.samplerState atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        
        [encoder endEncoding];
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];
    }
}

@end