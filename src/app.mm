// app.mm - casca do app iOS: mostra o log do runtime na tela e inicia o jogo.
#import <UIKit/UIKit.h>
#include <pthread.h>
#include <stdio.h>

int ngbc_run(const char* image_path, const char* data_root, int seconds);

static NSString* gLogPath;

@interface LogVC : UIViewController
@property (nonatomic, strong) UITextView* tv;
@end

@implementation LogVC
- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];
    self.tv = [[UITextView alloc] initWithFrame:self.view.bounds];
    self.tv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.tv.backgroundColor = [UIColor blackColor];
    self.tv.textColor = [UIColor greenColor];
    self.tv.font = [UIFont fontWithName:@"Menlo" size:9];
    self.tv.editable = NO;
    [self.view addSubview:self.tv];
    __weak LogVC* weak = self;
    [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer* t) {
        NSData* d = [NSData dataWithContentsOfFile:gLogPath];
        if (!d) return;
        NSUInteger n = d.length, head = 1800, keep = 6000;
        NSString* s;
        if (n > head + keep)
        {
            NSString* h = [[NSString alloc] initWithData:[d subdataWithRange:NSMakeRange(0, head)] encoding:NSISOLatin1StringEncoding];
            NSString* tl = [[NSString alloc] initWithData:[d subdataWithRange:NSMakeRange(n - keep, keep)] encoding:NSISOLatin1StringEncoding];
            s = [NSString stringWithFormat:@"%@\n[...]\n%@", h, tl];
        }
        else
        {
            s = [[NSString alloc] initWithData:d encoding:NSISOLatin1StringEncoding];
        }
        if (!s) s = @"";
        if (![weak.tv.text isEqualToString:s])
        {
            weak.tv.text = s;
            [weak.tv scrollRangeToVisible:NSMakeRange(s.length, 0)];
        }
    }];
}
@end

static void* game_thread(void*)
{
    NSString* res = [[NSBundle mainBundle] resourcePath];
    NSString* img = [res stringByAppendingPathComponent:@"image.bin"];
    NSString* root = [res stringByAppendingPathComponent:@"game"];
    int rc = ngbc_run(img.UTF8String, root.UTF8String, 0);
    fprintf(stderr, "[app] ngbc_run retornou %d\n", rc);
    return NULL;
}

@interface AppDel : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow* window;
@end

@implementation AppDel
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)opts
{
    NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    gLogPath = [docs stringByAppendingPathComponent:@"ngbc_log.txt"];
    freopen(gLogPath.UTF8String, "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [LogVC new];
    [self.window makeKeyAndVisible];
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16 * 1024 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &a, game_thread, NULL);
    return YES;
}
@end

int main(int argc, char* argv[])
{
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, @"AppDel"); }
}