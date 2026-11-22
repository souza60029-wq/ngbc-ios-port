#import "ViewController.h"

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    
    // 1. Inicializa a GPU do iPhone (Metal)
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    self.metalView = [[MTKView alloc] initWithFrame:self.view.bounds device:device];
    self.metalView.delegate = self;
    self.metalView.clearColor = MTLClearColorMake(0.1, 0.1, 0.1, 1.0); // Fundo cinza escuro
    [self.view addSubview:self.metalView];
    
    // 2. Simula a leitura dos assets traduzidos pela Fábrica AOT
    [self loadConvertedAssets];
}

- (void)loadConvertedAssets {
    // Aqui o motor do jogo leria os arquivos gerados pelo GitHub Actions
    NSLog(@"[NGBC Engine] Iniciando o Motor Gráfico...");
    NSLog(@"[NGBC Engine] GPU: %@", self.metalView.device.name);
    NSLog(@"[NGBC Engine] Carregando Shaders AOT (MetalLib)...");
    NSLog(@"[NGBC Engine] Carregando Texturas ASTC...");
    NSLog(@"[NGBC Engine] Pronto para receber o código recompilado do Xbox 360!");
}

// Métodos obrigatórios do MTKViewDelegate (Loop de renderização)
- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {}
- (void)drawInMTKView:(MTKView *)view {
    // Futuramente, aqui o jogo vai desenhar os sprites do Rock e do Terry
}

@end