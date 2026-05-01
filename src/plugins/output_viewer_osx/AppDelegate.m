#import "AppDelegate.h"

#import <QuartzCore/QuartzCore.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define FRAME_FILENAME "output_viewer_osx_frame.jpg"

@interface AppDelegate () <NSApplicationDelegate, NSWindowDelegate>

@property (copy, nonatomic) NSString *framePath;
@property (copy, nonatomic) NSString *disableFlagPath;
@property (copy, nonatomic) NSString *windowTitleText;
@property (strong, nonatomic) NSTimer *frameTimer;
@property (strong, nonatomic) NSDate *lastModificationDate;
@property (assign, nonatomic) unsigned long long lastFileLength;
@property (assign, nonatomic) BOOL hasAdjustedWindowSize;
@property (assign, nonatomic) NSSize currentImageSize;
@property (assign, nonatomic) NSSize resizeSessionStartFrameSize;
@property (assign, nonatomic) BOOL resizeSessionUsesWidthDriver;
@property (assign, nonatomic) BOOL resizeSessionDriverLocked;
@property (strong, nonatomic) NSView *titleBarOverlay;
@property (strong, nonatomic) NSTextField *titleLabel;
@property (strong, nonatomic) NSTrackingArea *hoverTrackingArea;
@property (assign, nonatomic) BOOL titleBarVisible;
@property (strong, nonatomic) id keyEventMonitor;
@property (strong, nonatomic) dispatch_source_t fileMonitor;
@property (strong, nonatomic) dispatch_queue_t fileMonitorQueue;
@property (assign, nonatomic) int fileDescriptor;

@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    [[NSApplication sharedApplication] setActivationPolicy:NSApplicationActivationPolicyRegular];

    self.framePath = [self resolveFramePath];
    self.windowTitleText = [self resolveWindowTitle];
    NSString *directoryPath = [self.framePath stringByDeletingLastPathComponent];
    self.disableFlagPath = [directoryPath stringByAppendingPathComponent:@"output_viewer_osx_disabled.flag"];
    self.lastFileLength = 0;
    self.hasAdjustedWindowSize = NO;
    self.currentImageSize = NSZeroSize;
    self.resizeSessionStartFrameSize = NSZeroSize;
    self.resizeSessionUsesWidthDriver = YES;
    self.resizeSessionDriverLocked = NO;
    self.titleBarVisible = NO;
    self.fileDescriptor = -1;

    [self configureWindow];
    [self setupKeyboardShortcuts];
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

- (NSString *)resolveWindowTitle {
    NSArray<NSString *> *arguments = [[NSProcessInfo processInfo] arguments];
    NSDictionary<NSString *, NSString *> *environment = [[NSProcessInfo processInfo] environment];
    NSString *envTitle = environment[@"MJPG_STREAMER_OSX_TITLE"];

    if (arguments.count > 2) {
        NSString *argTitle = [arguments[2] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (argTitle.length > 0) {
            return argTitle;
        }
    }

    if (envTitle.length > 0) {
        NSString *trimmedEnvTitle = [envTitle stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (trimmedEnvTitle.length > 0) {
            return trimmedEnvTitle;
        }
    }

    return @"MJPEG Streamer: output viewer osx";
}

- (void)configureWindow {
    CGFloat initialWidth = 640.0;
    CGFloat initialHeight = 480.0;
    NSRect contentRect = NSMakeRect(0, 0, initialWidth, initialHeight);

    self.window = [[NSWindow alloc] initWithContentRect:contentRect
                                              styleMask:(NSWindowStyleMaskTitled |
                                                         NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskMiniaturizable |
                                                         NSWindowStyleMaskResizable |
                                                         NSWindowStyleMaskFullSizeContentView)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.delegate = self;
    [self.window setTitle:self.windowTitleText ?: @"MJPG-Streamer macOS Viewer"];
    [self.window setBackgroundColor:[NSColor blackColor]];
    self.window.titleVisibility = NSWindowTitleHidden;
    self.window.titlebarAppearsTransparent = YES;
    self.window.movableByWindowBackground = YES;
    [self.window setAcceptsMouseMovedEvents:YES];

    self.imageView = [[NSImageView alloc] initWithFrame:contentRect];
    self.imageView.imageScaling = NSImageScaleProportionallyUpOrDown;
    self.imageView.imageAlignment = NSImageAlignCenter;
    self.imageView.animates = YES;
    self.imageView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    self.window.contentView = self.imageView;
    [self setupTitleBarOverlay];
    [self updateTrackingArea];
    [self setTitleBarOverlayVisible:NO animated:NO];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
}

- (void)setupTitleBarOverlay {
    CGFloat overlayHeight = 36.0;
    NSRect bounds = self.window.contentView.bounds;
    NSRect overlayFrame = NSMakeRect(0.0, bounds.size.height - overlayHeight, bounds.size.width, overlayHeight);

    self.titleBarOverlay = [[NSView alloc] initWithFrame:overlayFrame];
    self.titleBarOverlay.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    self.titleBarOverlay.wantsLayer = YES;
    self.titleBarOverlay.layer.backgroundColor = [[NSColor colorWithWhite:0.0 alpha:0.45] CGColor];

    self.titleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(12.0, 8.0, bounds.size.width - 24.0, 20.0)];
    self.titleLabel.autoresizingMask = NSViewWidthSizable;
    self.titleLabel.stringValue = self.windowTitleText ?: @"MJPEG Streamer: output viewer osx";
    self.titleLabel.editable = NO;
    self.titleLabel.selectable = NO;
    self.titleLabel.bordered = NO;
    self.titleLabel.drawsBackground = NO;
    self.titleLabel.textColor = [NSColor colorWithWhite:1.0 alpha:0.9];
    self.titleLabel.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightSemibold];
    self.titleLabel.alignment = NSTextAlignmentLeft;
    self.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;

    [self.titleBarOverlay addSubview:self.titleLabel];
    [self.window.contentView addSubview:self.titleBarOverlay];
    [self updateTitleBarLayout];
}

- (void)setupKeyboardShortcuts {
    self.keyEventMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent * _Nullable(NSEvent * _Nonnull event) {
        NSEventModifierFlags flags = (event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask);
        if ((flags & NSEventModifierFlagCommand) != 0) {
            NSString *chars = event.charactersIgnoringModifiers.lowercaseString;
            if ([chars isEqualToString:@"q"]) {
                [NSApp terminate:nil];
                return nil;
            }
        }
        return event;
    }];
}

- (void)setWindowButtonsVisible:(BOOL)visible {
    NSButton *closeButton = [self.window standardWindowButton:NSWindowCloseButton];
    NSButton *miniButton = [self.window standardWindowButton:NSWindowMiniaturizeButton];
    NSButton *zoomButton = [self.window standardWindowButton:NSWindowZoomButton];

    closeButton.hidden = !visible;
    miniButton.hidden = !visible;
    zoomButton.hidden = !visible;
}

- (void)updateTitleBarLayout {
    if (!self.window.contentView || !self.titleBarOverlay || !self.titleLabel) {
        return;
    }

    NSRect bounds = self.window.contentView.bounds;
    CGFloat overlayHeight = 36.0;
    self.titleBarOverlay.frame = NSMakeRect(0.0, bounds.size.height - overlayHeight, bounds.size.width, overlayHeight);

    CGFloat leftInset = 84.0;
    NSButton *closeButton = [self.window standardWindowButton:NSWindowCloseButton];
    if (closeButton && closeButton.superview) {
        NSRect closeFrameInContent = [self.window.contentView convertRect:closeButton.frame fromView:closeButton.superview];
        leftInset = MAX(leftInset, NSMaxX(closeFrameInContent) + 12.0);
    }

    CGFloat rightInset = 12.0;
    CGFloat labelWidth = MAX(120.0, bounds.size.width - leftInset - rightInset);
    self.titleLabel.frame = NSMakeRect(leftInset, 8.0, labelWidth, 20.0);
}

- (void)updateTrackingArea {
    if (!self.window.contentView) {
        return;
    }

    if (self.hoverTrackingArea) {
        [self.window.contentView removeTrackingArea:self.hoverTrackingArea];
        self.hoverTrackingArea = nil;
    }

    NSTrackingAreaOptions options = NSTrackingMouseMoved |
                                    NSTrackingMouseEnteredAndExited |
                                    NSTrackingActiveInKeyWindow |
                                    NSTrackingInVisibleRect;
    self.hoverTrackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                           options:options
                                                             owner:self
                                                          userInfo:nil];
    [self.window.contentView addTrackingArea:self.hoverTrackingArea];
}

- (void)setTitleBarOverlayVisible:(BOOL)visible animated:(BOOL)animated {
    if (!self.titleBarOverlay) {
        return;
    }

    if (self.titleBarVisible == visible && self.titleBarOverlay.alphaValue == (visible ? 1.0 : 0.0)) {
        return;
    }

    self.titleBarVisible = visible;
    [self setWindowButtonsVisible:visible];
    [self.titleBarOverlay setHidden:NO];

    if (animated) {
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
            context.duration = 0.12;
            self.titleBarOverlay.animator.alphaValue = visible ? 1.0 : 0.0;
        } completionHandler:^{
            if (!visible) {
                [self.titleBarOverlay setHidden:YES];
            }
        }];
        return;
    }

    self.titleBarOverlay.alphaValue = visible ? 1.0 : 0.0;
    if (!visible) {
        [self.titleBarOverlay setHidden:YES];
    }
}

- (void)updateTitleBarOverlayForMouseLocation:(NSPoint)pointInWindow {
    (void)pointInWindow;
    [self setTitleBarOverlayVisible:YES animated:YES];
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

- (void)updateWindowAspectRatioForImage:(NSImage *)image {
    if (!image || image.size.width <= 0 || image.size.height <= 0) {
        return;
    }

    NSSize newImageSize = NSMakeSize(image.size.width, image.size.height);
    if (NSEqualSizes(self.currentImageSize, newImageSize)) {
        return;
    }

    self.currentImageSize = newImageSize;
}

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize {
    if (!sender || self.currentImageSize.width <= 0 || self.currentImageSize.height <= 0) {
        return frameSize;
    }

    CGFloat ratio = self.currentImageSize.width / self.currentImageSize.height;
    CGFloat deltaWidth;
    CGFloat deltaHeight;
    BOOL useWidthDriver;

    if (ratio <= 0.0) {
        return frameSize;
    }

    if ([sender inLiveResize]) {
        if (!self.resizeSessionDriverLocked) {
            if (self.resizeSessionStartFrameSize.width <= 0.0 || self.resizeSessionStartFrameSize.height <= 0.0) {
                self.resizeSessionStartFrameSize = sender.frame.size;
            }

            deltaWidth = fabs(frameSize.width - self.resizeSessionStartFrameSize.width);
            deltaHeight = fabs(frameSize.height - self.resizeSessionStartFrameSize.height);
            self.resizeSessionUsesWidthDriver = (deltaWidth >= deltaHeight);
            self.resizeSessionDriverLocked = YES;
        }
        useWidthDriver = self.resizeSessionUsesWidthDriver;
    } else {
        NSRect currentFrame = sender.frame;
        deltaWidth = fabs(frameSize.width - currentFrame.size.width);
        deltaHeight = fabs(frameSize.height - currentFrame.size.height);
        useWidthDriver = (deltaWidth >= deltaHeight);
    }

    if (useWidthDriver) {
        NSRect candidateFrame = NSMakeRect(0.0, 0.0, frameSize.width, frameSize.height);
        NSRect candidateContent = [sender contentRectForFrameRect:candidateFrame];
        CGFloat targetContentHeight = floor(candidateContent.size.width / ratio);
        targetContentHeight = MAX(1.0, targetContentHeight);
        NSRect targetContentRect = NSMakeRect(0.0, 0.0, candidateContent.size.width, targetContentHeight);
        NSRect targetFrame = [sender frameRectForContentRect:targetContentRect];
        return NSMakeSize(frameSize.width, targetFrame.size.height);
    }

    NSRect candidateFrame = NSMakeRect(0.0, 0.0, frameSize.width, frameSize.height);
    NSRect candidateContent = [sender contentRectForFrameRect:candidateFrame];
    CGFloat targetContentWidth = floor(candidateContent.size.height * ratio);
    targetContentWidth = MAX(1.0, targetContentWidth);
    NSRect targetContentRect = NSMakeRect(0.0, 0.0, targetContentWidth, candidateContent.size.height);
    NSRect targetFrame = [sender frameRectForContentRect:targetContentRect];
    return NSMakeSize(targetFrame.size.width, frameSize.height);
}

- (void)windowWillStartLiveResize:(NSNotification *)notification {
    self.resizeSessionStartFrameSize = self.window.frame.size;
    self.resizeSessionDriverLocked = NO;
}

- (void)windowDidEndLiveResize:(NSNotification *)notification {
    self.resizeSessionStartFrameSize = NSZeroSize;
    self.resizeSessionDriverLocked = NO;
}

- (void)windowDidResize:(NSNotification *)notification {
    [self updateTrackingArea];
    [self updateTitleBarLayout];
}

- (void)mouseMoved:(NSEvent *)event {
    [self updateTitleBarOverlayForMouseLocation:event.locationInWindow];
}

- (void)mouseEntered:(NSEvent *)event {
    [self updateTitleBarOverlayForMouseLocation:event.locationInWindow];
}

- (void)mouseExited:(NSEvent *)event {
    [self setTitleBarOverlayVisible:NO animated:YES];
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
    [self updateWindowAspectRatioForImage:image];

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
    [self.window setAcceptsMouseMovedEvents:NO];
    if (self.keyEventMonitor) {
        [NSEvent removeMonitor:self.keyEventMonitor];
        self.keyEventMonitor = nil;
    }
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
