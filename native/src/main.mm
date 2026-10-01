/*
 * main.mm - The native front end's Cocoa and Metal host
 *
 * One window with a Metal view, Dear ImGui's Cocoa and Metal backends, and
 * multi-viewport: a window dragged out of the main one becomes a window of its
 * own, which the backends create and draw. Everything drawn inside is App's;
 * this file only owns the platform.
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "app.hpp"
#include "ui_theme.hpp"
#include "native_menu.hpp"
#include "platform_paths.hpp"
#include "screen_renderer_metal.hpp"

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"

#include <memory>

using a2e::native::App;

// The Metal view, taking files dropped on the window.
@interface ApplEmView : MTKView
@property(nonatomic, copy) void (^onDrop)(NSArray<NSString *> *paths);
@end

@implementation ApplEmView

- (instancetype)initWithFrame:(NSRect)frame {
  self = [super initWithFrame:frame];
  if (self) [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
  return self;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
  return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
  NSArray<NSURL *> *urls = [sender.draggingPasteboard
      readObjectsForClasses:@[ NSURL.class ]
                    options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
  NSMutableArray<NSString *> *paths = [NSMutableArray array];
  for (NSURL *url in urls) [paths addObject:url.path];
  if (paths.count && self.onDrop) self.onDrop(paths);
  return paths.count > 0;
}

@end

namespace {

NSArray<UTType *> *contentTypes(const std::vector<std::string> &extensions) {
  NSMutableArray<UTType *> *types = [NSMutableArray array];
  for (const std::string &extension : extensions) {
    UTType *type = [UTType typeWithFilenameExtension:@(extension.c_str())];
    if (type) [types addObject:type];
  }
  return types;
}

} // namespace

@interface AppViewController : NSViewController <MTKViewDelegate>
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> commandQueue;
- (void)shutdown;
- (void)releaseKeys;
- (void)filesDropped:(const std::vector<std::string> &)paths;
- (BOOL)commandKeysToWindow;
@end

@implementation AppViewController {
  std::unique_ptr<App> _app;
  NativeMenu *_menu;
}

- (instancetype)init {
  self = [super initWithNibName:nil bundle:nil];
  if (!self) return nil;

  _device = MTLCreateSystemDefaultDevice();
  if (!_device) {
    NSLog(@"Metal is not supported on this machine");
    abort();
  }
  _commandQueue = [_device newCommandQueue];

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  // No keyboard navigation: with it, Option alone moves focus to the menu
  // bar, and Option is an Apple key while the machine has the keyboard.
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

  // SF Pro for the interface and SF Mono for numbers, and the system's own
  // colours, following light and dark and the accent as they change.
  a2e::native::ui::loadFonts();
  a2e::native::ui::followSystemAppearance();

  // Key repeat at the rate the user set for the system, not ImGui's own.
  io.KeyRepeatDelay = static_cast<float>(NSEvent.keyRepeatDelay);
  io.KeyRepeatRate = static_cast<float>(NSEvent.keyRepeatInterval);

  a2e::native::Platform platform;
  platform.screen = a2e::native::makeMetalScreenRenderer((__bridge void *)_device,
                                                         (__bridge void *)_commandQueue);
  platform.capsLockOn = [] {
    return (NSEvent.modifierFlags & NSEventModifierFlagCapsLock) != 0;
  };
  __weak AppViewController *weakSelf = self;
  platform.setWindowTitle = [weakSelf](const std::string &title) {
    weakSelf.view.window.title = [NSString stringWithUTF8String:title.c_str()];
  };
  platform.toggleFullScreen = [weakSelf] { [weakSelf.view.window toggleFullScreen:nil]; };
  platform.setAppearance = [](int choice) {
    NSApp.appearance = choice == 1   ? [NSAppearance appearanceNamed:NSAppearanceNameAqua]
                       : choice == 2 ? [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]
                                     : nil;
  };
  // The panels run on their own, and answer on the main thread between
  // frames: a modal loop inside a frame would re-enter ImGui.
  platform.openFile = [](const std::string &title, const std::vector<std::string> &extensions,
                         a2e::native::Platform::FileChosen done) {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = @(title.c_str());
    panel.allowedContentTypes = contentTypes(extensions);
    panel.allowsMultipleSelection = NO;
    [panel beginWithCompletionHandler:^(NSModalResponse result) {
      done(result == NSModalResponseOK ? std::string(panel.URL.path.UTF8String) : std::string());
    }];
  };
  platform.saveFile = [](const std::string &title, const std::string &suggestedName,
                         const std::vector<std::string> &extensions,
                         a2e::native::Platform::FileChosen done) {
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.message = @(title.c_str());
    panel.nameFieldStringValue = @(suggestedName.c_str());
    panel.allowedContentTypes = contentTypes(extensions);
    panel.allowsOtherFileTypes = YES;
    [panel beginWithCompletionHandler:^(NSModalResponse result) {
      done(result == NSModalResponseOK ? std::string(panel.URL.path.UTF8String) : std::string());
    }];
  };
  platform.resourceDirectory = NSBundle.mainBundle.resourcePath.UTF8String;
  id<MTLDevice> device = _device;
  platform.makeTexture = [device](const uint8_t *rgba, int width, int height) -> ImTextureID {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
               mipmapLevel:0
                 withBytes:rgba
               bytesPerRow:width * 4];
    // Held until releaseTexture, which hands the reference back to ARC.
    return (ImTextureID)(intptr_t)CFBridgingRetain(texture);
  };
  // GameController's extended gamepad, in the W3C standard layout the
  // browser reads: a thumbstick's up is positive here and negative there.
  platform.gamepads = [] {
    std::vector<a2e::native::Pad> pads;
    for (GCController *controller in GCController.controllers) {
      GCExtendedGamepad *gamepad = controller.extendedGamepad;
      if (!gamepad) continue;
      a2e::native::Pad pad;
      pad.axes = {gamepad.leftThumbstick.xAxis.value, -gamepad.leftThumbstick.yAxis.value,
                  gamepad.rightThumbstick.xAxis.value, -gamepad.rightThumbstick.yAxis.value};
      pad.buttons[0] = gamepad.buttonA.isPressed;
      pad.buttons[1] = gamepad.buttonB.isPressed;
      pad.buttons[2] = gamepad.buttonX.isPressed;
      pad.buttons[3] = gamepad.buttonY.isPressed;
      pad.buttons[12] = gamepad.dpad.up.isPressed;
      pad.buttons[13] = gamepad.dpad.down.isPressed;
      pad.buttons[14] = gamepad.dpad.left.isPressed;
      pad.buttons[15] = gamepad.dpad.right.isPressed;
      pads.push_back(pad);
    }
    return pads;
  };
  platform.releaseTexture = [](ImTextureID texture) {
    if (texture != ImTextureID_Invalid) CFBridgingRelease((void *)(intptr_t)texture);
  };
  _app = std::make_unique<App>(a2e::native::appSupportDirectory(), std::move(platform));
  io.IniFilename = _app->iniPath();

  ImGui_ImplMetal_Init(_device);

  App *app = _app.get();
  _menu = [[NativeMenu alloc] initWithChosen:^(NSString *action) {
    app->menuChosen(action.UTF8String);
  }];
  return self;
}

- (MTKView *)mtkView {
  return (MTKView *)self.view;
}

- (void)loadView {
  ApplEmView *view = [[ApplEmView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 860)];
  __weak AppViewController *weakSelf = self;
  view.onDrop = ^(NSArray<NSString *> *paths) {
    std::vector<std::string> files;
    for (NSString *path in paths) files.push_back(path.UTF8String);
    [weakSelf filesDropped:files];
  };
  self.view = view;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.mtkView.device = self.device;
  self.mtkView.delegate = self;
  self.mtkView.preferredFramesPerSecond = 60;
  ImGui_ImplOSX_Init(self.view);
}

- (void)drawInMTKView:(MTKView *)view {
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(view.bounds.size.width, view.bounds.size.height);
  const CGFloat scale = view.window.screen.backingScaleFactor
                            ?: NSScreen.mainScreen.backingScaleFactor;
  io.DisplayFramebufferScale = ImVec2(scale, scale);

  id<MTLCommandBuffer> commandBuffer = [self.commandQueue commandBuffer];
  MTLRenderPassDescriptor *pass = view.currentRenderPassDescriptor;
  if (pass == nil) {
    [commandBuffer commit];
    return;
  }

  a2e::native::ui::followSystemAppearance();
  ImGui_ImplMetal_NewFrame(pass);
  ImGui_ImplOSX_NewFrame(view);
  ImGui::NewFrame();

  _app->frame();

  ImGui::Render();
  const ImVec4 background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
  pass.colorAttachments[0].clearColor = MTLClearColorMake(background.x, background.y, background.z, 1.0);
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:pass];
  ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, encoder);
  [encoder endEncoding];
  [commandBuffer presentDrawable:view.currentDrawable];
  [commandBuffer commit];

  if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
  }

  [_menu update:_app->menuBar()];
  if (_app->quitRequested()) [NSApp terminate:nil];
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
}

- (void)filesDropped:(const std::vector<std::string> &)paths {
  if (_app) _app->filesDropped(paths);
}

- (BOOL)commandKeysToWindow {
  return _app && _app->commandKeysToWindow();
}

// Another app took the keyboard: key-ups for anything held will never come.
- (void)releaseKeys {
  if (_app) _app->releaseKeys();
}

- (void)shutdown {
  if (!_app) return;
  _app->shutdown();
  // Written before the context goes, while the settings handler can still
  // reach the App.
  ImGui::SaveIniSettingsToDisk(_app->iniPath());
  ImGui::GetIO().IniFilename = nullptr;
  ImGui_ImplMetal_Shutdown();
  ImGui_ImplOSX_Shutdown();
  ImGui::DestroyContext();
  _app.reset();
}

@end

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) AppViewController *controller;
@end

@implementation AppDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
  return YES;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {

  self.controller = [[AppViewController alloc] init];
  self.window = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(0, 0, 1280, 860)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                          NSWindowStyleMaskResizable |
                          NSWindowStyleMaskMiniaturizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  self.window.contentViewController = self.controller;
  self.window.title = @"ApplEm";
  [self.window center];
  [self.window setFrameAutosaveName:@"ApplEmMainWindow"];
  self.window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
  [self.window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];

  // Command keys skip the menu bar when the machine takes Command as Open
  // Apple, or an ImGui text field is being typed into; Command-Q always
  // reaches the menu, as the Tauri build does it.
  __weak AppDelegate *weakSelf = self;
  [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                        handler:^NSEvent *(NSEvent *event) {
    if (!(event.modifierFlags & NSEventModifierFlagCommand)) return event;
    if ([event.charactersIgnoringModifiers.lowercaseString isEqualToString:@"q"]) return event;
    if (![weakSelf.controller commandKeysToWindow]) return event;
    NSResponder *responder = event.window.firstResponder;
    if (!responder) return event;
    [responder keyDown:event];
    return nil;
  }];
}

- (void)applicationDidResignActive:(NSNotification *)notification {
  [self.controller releaseKeys];
}

- (void)applicationWillTerminate:(NSNotification *)notification {
  [self.controller shutdown];
}

@end

int main(int argc, const char *argv[]) {
  (void)argc;
  (void)argv;
  @autoreleasepool {
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    AppDelegate *delegate = [[AppDelegate alloc] init];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
