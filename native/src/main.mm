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
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "app.hpp"
#include "platform_paths.hpp"
#include "screen_renderer_metal.hpp"

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"

#include <memory>

using a2e::native::App;

@interface AppViewController : NSViewController <MTKViewDelegate>
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> commandQueue;
- (void)shutdown;
- (void)releaseKeys;
@end

@implementation AppViewController {
  std::unique_ptr<App> _app;
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

  ImGui::StyleColorsDark();
  // A window that has left the main one is a real OS window, so it is drawn
  // square and opaque like one.
  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowRounding = 0.0f;
  style.Colors[ImGuiCol_WindowBg].w = 1.0f;

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
  _app = std::make_unique<App>(a2e::native::appSupportDirectory(), std::move(platform));
  io.IniFilename = _app->iniPath();

  ImGui_ImplMetal_Init(_device);
  return self;
}

- (MTKView *)mtkView {
  return (MTKView *)self.view;
}

- (void)loadView {
  self.view = [[MTKView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 860)];
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

  ImGui_ImplMetal_NewFrame(pass);
  ImGui_ImplOSX_NewFrame(view);
  ImGui::NewFrame();

  _app->frame();

  ImGui::Render();
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.08, 0.08, 0.09, 1.0);
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

  if (_app->quitRequested()) [NSApp terminate:nil];
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
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
  [self buildMenuBar];

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
}

- (void)applicationDidResignActive:(NSNotification *)notification {
  [self.controller releaseKeys];
}

- (void)applicationWillTerminate:(NSNotification *)notification {
  [self.controller shutdown];
}

// The application menu, so ⌘Q, ⌘H and ⌘M behave as on every other Mac app.
// The emulator's own menus are ImGui's, inside the window.
- (void)buildMenuBar {
  NSMenu *bar = [[NSMenu alloc] init];
  NSMenuItem *appItem = [[NSMenuItem alloc] init];
  [bar addItem:appItem];
  NSMenu *appMenu = [[NSMenu alloc] init];
  [appMenu addItemWithTitle:@"About ApplEm"
                     action:@selector(orderFrontStandardAboutPanel:)
              keyEquivalent:@""];
  [appMenu addItem:[NSMenuItem separatorItem]];
  [appMenu addItemWithTitle:@"Hide ApplEm" action:@selector(hide:) keyEquivalent:@"h"];
  [appMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
  [appMenu addItem:[NSMenuItem separatorItem]];
  [appMenu addItemWithTitle:@"Quit ApplEm" action:@selector(terminate:) keyEquivalent:@"q"];
  appItem.submenu = appMenu;
  NSApp.mainMenu = bar;
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
