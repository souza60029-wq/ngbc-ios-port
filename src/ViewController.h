#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>

@interface ViewController : UIViewController <MTKViewDelegate>
@property (nonatomic, strong) MTKView *metalView;
@end