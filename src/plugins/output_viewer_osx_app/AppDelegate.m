#import "AppDelegate.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define FRAME_FILENAME "output_viewer_osx_frame.jpg"

@interface AppDelegate () <NSApplicationDelegate, NSWindowDelegate>

@property (copy, nonatomic) NSString *framePath;
@property (copy, nonatomic) NSString *disableFlagPath;
@property (strong, nonatomic) NSTimer *frameTimer;
@property (strong, nonatomic) NSDate *lastModificationDate;
@property (assign, nonatomic) unsigned long long lastFileLength;
@property (assign, nonatomic) BOOL hasAdjustedWindowSize;
@property (strong, nonatomic) dispatch_source_t fileMonitor;
@property (strong, nonatomic) dispatch_queue_t fileMonitorQueue;
@property (assign, nonatomic) int fileDescriptor;

@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    [[NSApplication sharedApplication] setActivationPolicy:NSApplicationActivationPolicyRegular];

    self.framePath = [self resolveFramePath];
    NSString *directoryPath = [self.framePath stringByDeletingLastPathComponent];
    self.disableFlagPath = [directoryPath stringByAppendingPathComponent:@"output_viewer_osx_disabled.flag"];
    self.lastFileLength = 0;
    self.hasAdjustedWindowSize = NO;
    self.fileDescriptor = -1;

    [self configureWindow];
    [self bringWindowToFront];
    [self startMonitoringFrameFile];
    [self startFrameTimer];

    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
    [self updateImageIfNeeded];
}

- (NSString *)resolveFramePath {
    NSArray<NSString *> *arguments = [[NSProcessInfo processInfo] arguments];
    NSDictionary<NSString *, NSString *> *environment = [[NSProcessInfo processInfo] environment];
    NSString *envPath = environment[@"MJPG_STREAMER_OSX_FRAME"];

    if (arguments.count > 1) {
        return [arguments[1] stringByStandardizingPath];
    }

    if (envPath.length > 0) {
        return [envPath stringByStandardizingPath];
    }

    NSString *defaultPath = [@"~/tmp/" FRAME_FILENAME stringByExpandingTildeInPath];
    NSLog(@"[OSXViewer] Frame path argument missing, falling back to %@", defaultPath);
    return defaultPath;
}

- (void)configureWindow {
    CGFloat initialWidth = 640.0;
    CGFloat initialHeight = 480.0;
    NSRect contentRect = NSMakeRect(0, 0, initialWidth, initialHeight);

    self.window = [[NSWindow alloc] initWithContentRect:contentRect
                                              styleMask:(NSWindowStyleMaskTitled |
                                                         NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskMiniaturizable |
                                                         NSWindowStyleMaskResizable)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.delegate = self;
    [self.window setTitle:@"MJPG-Streamer macOS Viewer"];
    [self.window setBackgroundColor:[NSColor blackColor]];

    self.imageView = [[NSImageView alloc] initWithFrame:contentRect];
    self.imageView.imageScaling = NSImageScaleProportionallyUpOrDown;
    self.imageView.imageAlignment = NSImageAlignCenter;
    self.imageView.animates = YES;

    self.window.contentView = self.imageView;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
}

- (void)bringWindowToFront {
    if (!self.window) {
        return;
    }

    NSRunningApplication *currentApp = [NSRunningApplication currentApplication];
    [currentApp activateWithOptions:NSApplicationActivateAllWindows];
    if (@available(macOS 14.0, *)) {
        // No additional activation options available on macOS 14+
    } else {
        [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
    }
    [self.window makeKeyAndOrderFront:nil];
}

- (void)startMonitoringFrameFile {
    [self stopMonitoringFrameFile];

    NSString *directoryPath = [self.framePath stringByDeletingLastPathComponent];
    if (directoryPath.length == 0) {
        return;
    }

    int fd = open([directoryPath fileSystemRepresentation], O_EVTONLY);
    if (fd == -1) {
        NSLog(@"[OSXViewer] Failed to monitor directory %@: %s", directoryPath, strerror(errno));
        return;
    }

    self.fileDescriptor = fd;
    self.fileMonitorQueue = dispatch_queue_create("com.mjpg-streamer.osxviewer.filemonitor", DISPATCH_QUEUE_SERIAL);
    unsigned long mask = DISPATCH_VNODE_WRITE | DISPATCH_VNODE_EXTEND | DISPATCH_VNODE_ATTRIB | DISPATCH_VNODE_RENAME | DISPATCH_VNODE_DELETE;
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, (uintptr_t)fd, mask, self.fileMonitorQueue);
    if (!source) {
        close(fd);
        self.fileDescriptor = -1;
        self.fileMonitorQueue = nil;
        return;
    }

    dispatch_source_set_event_handler(source, ^{
        dispatch_async(dispatch_get_main_queue(), ^{
            [self updateImageIfNeeded];
        });
    });

    dispatch_source_set_cancel_handler(source, ^{
        close(fd);
    });

    dispatch_resume(source);
    self.fileMonitor = source;
}

- (void)stopMonitoringFrameFile {
    if (self.fileMonitor) {
        dispatch_source_cancel(self.fileMonitor);
        self.fileMonitor = nil;
    }
    self.fileMonitorQueue = nil;
    self.fileDescriptor = -1;
}

- (void)startFrameTimer {
    self.frameTimer = [NSTimer scheduledTimerWithTimeInterval:0.05
                                                       target:self
                                                     selector:@selector(updateImageIfNeeded)
                                                     userInfo:nil
                                                      repeats:YES];
}

- (void)resizeWindowToFitImage:(NSImage *)image {
    if (!image || image.size.width <= 0 || image.size.height <= 0) {
        return;
    }

    NSScreen *screen = self.window.screen ?: [NSScreen mainScreen];
    NSRect visibleFrame = screen.visibleFrame;

    CGFloat maxWidth = visibleFrame.size.width * 0.9;
    CGFloat maxHeight = visibleFrame.size.height * 0.9;
    CGFloat targetWidth = image.size.width;
    CGFloat targetHeight = image.size.height;

    CGFloat scale = MIN(1.0, MIN(maxWidth / targetWidth, maxHeight / targetHeight));
    targetWidth = MAX(320.0, floor(targetWidth * scale));
    targetHeight = MAX(240.0, floor(targetHeight * scale));

    NSRect contentRect = NSMakeRect(0, 0, targetWidth, targetHeight);
    NSRect frameRect = [self.window frameRectForContentRect:contentRect];

    [self.window setFrame:frameRect display:YES animate:YES];
    [self.window center];
    self.hasAdjustedWindowSize = YES;
}

- (void)updateImageIfNeeded {
    NSError *error = nil;
    NSFileManager *fileManager = [NSFileManager defaultManager];
    NSDictionary<NSFileAttributeKey, id> *attributes =
        [fileManager attributesOfItemAtPath:self.framePath error:&error];

    if (!attributes) {
        return;
    }

    NSDate *modificationDate = attributes[NSFileModificationDate];
    unsigned long long fileLength = [attributes[NSFileSize] unsignedLongLongValue];

    if (self.lastModificationDate &&
        [modificationDate isEqualToDate:self.lastModificationDate] &&
        self.lastFileLength == fileLength) {
        return;
    }

    NSData *jpegData = [NSData dataWithContentsOfFile:self.framePath options:NSDataReadingMappedIfSafe error:&error];
    if (!jpegData || jpegData.length == 0) {
        return;
    }

    NSImage *image = [[NSImage alloc] initWithData:jpegData];
    if (!image || image.size.width <= 0 || image.size.height <= 0) {
        return;
    }

    self.imageView.image = image;

    if (!self.hasAdjustedWindowSize) {
        [self resizeWindowToFitImage:image];
    }

    if (![self.window isKeyWindow]) {
        [self bringWindowToFront];
    }

    self.lastModificationDate = modificationDate;
    self.lastFileLength = fileLength;
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    [self stopMonitoringFrameFile];
    [self.frameTimer invalidate];
    self.frameTimer = nil;
}

- (void)createDisableFlag {
    if (self.disableFlagPath.length == 0) {
        return;
    }
    [[NSFileManager defaultManager] removeItemAtPath:self.disableFlagPath error:nil];
    [[NSFileManager defaultManager] createFileAtPath:self.disableFlagPath contents:[NSData data] attributes:nil];
}

- (void)windowWillClose:(NSNotification *)notification {
    [self stopMonitoringFrameFile];
    [self createDisableFlag];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

@end
