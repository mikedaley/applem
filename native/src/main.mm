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
#import <objc/runtime.h>

#include "app.hpp"
#include "ui_theme.hpp"
#include "native_menu.hpp"
#include "native_toolbar.hpp"
#include "platform_paths.hpp"
#include "screen_renderer_metal.hpp"

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"

#include <memory>

using a2e::native::App;

// Files dropped on any of the app's windows: the main one and every window
// ImGui makes for a window dragged out of it. Each drop and each move of a
// drag over a window carries where it is, in ImGui's coordinates, so a disk
// dropped on a drive's card goes into that drive and the card can light up
// while the drag is over it.
namespace {

App *g_dropApp = nullptr;

// A point in a window's own coordinates, in ImGui's: the primary screen's top
// left, y down, as the Cocoa backend places viewports.
ImVec2 imguiPoint(NSWindow *window, NSPoint inWindow) {
  const NSRect screen = [window convertRectToScreen:NSMakeRect(inWindow.x, inWindow.y, 0, 0)];
  return ImVec2(static_cast<float>(screen.origin.x),
                static_cast<float>(NSScreen.screens[0].frame.size.height - screen.origin.y));
}

std::vector<std::string> droppedPaths(id<NSDraggingInfo> info) {
  NSArray<NSURL *> *urls = [info.draggingPasteboard
      readObjectsForClasses:@[ NSURL.class ]
                    options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
  std::vector<std::string> paths;
  for (NSURL *url in urls) paths.push_back(url.path.UTF8String);
  return paths;
}

// Refused, with the cursor saying so, when nothing in it is a disk image.
NSDragOperation dragMoved(NSWindow *window, id<NSDraggingInfo> info) {
  if (!g_dropApp) return NSDragOperationNone;
  return g_dropApp->dragHover(imguiPoint(window, info.draggingLocation), droppedPaths(info)) ? NSDragOperationCopy
                                                                                           : NSDragOperationNone;
}

void dragLeft() {
  if (g_dropApp) g_dropApp->dragHover(std::nullopt);
}

BOOL dropped(NSWindow *window, id<NSDraggingInfo> info) {
  const std::vector<std::string> paths = droppedPaths(info);
  dragLeft();
  if (paths.empty() || !g_dropApp) return NO;
  g_dropApp->filesDropped(paths, imguiPoint(window, info.draggingLocation));
  return YES;
}

// The same, added to the view ImGui's backend puts in each window it makes,
// which is not a class of ours.
NSDragOperation viewportDragEntered(NSView *self, SEL, id<NSDraggingInfo> info) { return dragMoved(self.window, info); }
NSDragOperation viewportDragUpdated(NSView *self, SEL, id<NSDraggingInfo> info) { return dragMoved(self.window, info); }
void viewportDragExited(NSView *, SEL, id<NSDraggingInfo>) { dragLeft(); }
BOOL viewportPerformDrag(NSView *self, SEL, id<NSDraggingInfo> info) { return dropped(self.window, info); }

void (*g_platformCreateWindow)(ImGuiViewport *) = nullptr;

void createViewportWindow(ImGuiViewport *viewport) {
  g_platformCreateWindow(viewport);
  void *handle = viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle;
  NSWindow *window = handle ? (__bridge NSWindow *)handle : nil;
  NSView *view = window.contentView;
  if (!view) return;
  Class viewClass = view.class;
  // Added once per class; a class that already answers keeps its own.
  class_addMethod(viewClass, @selector(draggingEntered:), (IMP)viewportDragEntered, "Q@:@");
  class_addMethod(viewClass, @selector(draggingUpdated:), (IMP)viewportDragUpdated, "Q@:@");
  class_addMethod(viewClass, @selector(draggingExited:), (IMP)viewportDragExited, "v@:@");
  class_addMethod(viewClass, @selector(performDragOperation:), (IMP)viewportPerformDrag, "B@:@");
  [view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
}

// Where the pointer is and which of ImGui's windows it is over, as macOS
// stacks them. Left
// to itself ImGui guesses from which window was focused last, so where two
// overlap it can send a click to the one underneath: a SmartPort window
// opened over the Disk Drives window ignored its own Insert button. A window
// ImGui marks as taking no input (one being dragged, so the window under it
// can be found) is looked through, and another app's window on top means
// none of ours.
void reportHoveredViewport() {
  ImGuiIO &io = ImGui::GetIO();
  const NSPoint mouse = NSEvent.mouseLocation;
  // Where the pointer is, too, every frame. macOS sends movement only to the
  // key window, so over any other window ImGui went on believing the pointer
  // was wherever it last saw it, and a first click in a window just opened
  // (the SmartPort window's Insert) landed there instead. In ImGui's
  // coordinates: the primary screen's top left, y down.
  io.AddMousePosEvent(static_cast<float>(mouse.x),
                      static_cast<float>(NSScreen.screens[0].frame.size.height - mouse.y));
  ImGuiID hovered = 0;
  NSInteger below = 0;
  for (int depth = 0; depth < 8; depth++) {
    const NSInteger number = [NSWindow windowNumberAtPoint:mouse belowWindowWithWindowNumber:below];
    if (number <= 0) break;
    ImGuiViewport *match = nullptr;
    for (ImGuiViewport *viewport : ImGui::GetPlatformIO().Viewports) {
      void *handle = viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle;
      if (handle && ((__bridge NSWindow *)handle).windowNumber == number) {
        match = viewport;
        break;
      }
    }
    if (!match) break; // another app's window, or one of ours ImGui does not own
    if (match->Flags & ImGuiViewportFlags_NoInputs) {
      below = number;
      continue;
    }
    hovered = match->ID;
    break;
  }
  io.AddMouseViewportEvent(hovered);
}

// Call once, after the Cocoa backend is initialised.
void acceptDropsOnViewports() {
  ImGuiPlatformIO &io = ImGui::GetPlatformIO();
  if (g_platformCreateWindow || !io.Platform_CreateWindow) return;
  g_platformCreateWindow = io.Platform_CreateWindow;
  io.Platform_CreateWindow = createViewportWindow;
}

} // namespace

// The Metal view, the main window's.
@interface ApplEmView : MTKView
@end

@implementation ApplEmView

- (instancetype)initWithFrame:(NSRect)frame {
  self = [super initWithFrame:frame];
  if (self) [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
  return self;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
  return dragMoved(self.window, sender);
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
  return dragMoved(self.window, sender);
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
  dragLeft();
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
  return dropped(self.window, sender);
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
- (BOOL)commandKeysToWindow;
- (void)attachToolbarTo:(NSWindow *)window;
- (NSSize)contentSizeFor:(NSSize)proposed current:(NSSize)current;
- (NSSize)contentSizeWithin:(NSSize)limit;
- (void)beginLiveResize:(BOOL)widthLeads;
- (void)endLiveResize;
@end

@implementation AppViewController {
  std::unique_ptr<App> _app;
  NativeMenu *_menu;
  NativeToolbar *_toolbar;
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
  platform.setMainContentSize = [weakSelf](float width, float height) {
    NSWindow *window = weakSelf.view.window;
    if (!window || (window.styleMask & NSWindowStyleMaskFullScreen)) return;
    // Grown from the top left, as a window keeps its title bar in place,
    // then kept on its screen.
    const NSRect content = [window contentRectForFrameRect:window.frame];
    NSRect frame = [window frameRectForContentRect:NSMakeRect(content.origin.x, NSMaxY(content) - height,
                                                              width, height)];
    const NSRect visible = (window.screen ?: NSScreen.mainScreen).visibleFrame;
    if (NSMinY(frame) < NSMinY(visible)) frame.origin.y = NSMinY(visible);
    if (NSMaxX(frame) > NSMaxX(visible)) frame.origin.x = std::max(NSMinX(visible), NSMaxX(visible) - frame.size.width);
    [window setFrame:frame display:YES animate:NO];
  };
  platform.mainContentLimit = [weakSelf]() -> ImVec2 {
    NSWindow *window = weakSelf.view.window;
    const NSRect visible = (window.screen ?: NSScreen.mainScreen).visibleFrame;
    const NSRect content = window ? [window contentRectForFrameRect:visible] : visible;
    return ImVec2(content.size.width, content.size.height);
  };
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
      pad.name = controller.vendorName ? controller.vendorName.UTF8String : "Gamepad";
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
  a2e::native::ui::roundViewportWindows();

  App *app = _app.get();
  _menu = [[NativeMenu alloc] initWithChosen:^(NSString *action) {
    app->menuChosen(action.UTF8String);
  }];
  _toolbar = [[NativeToolbar alloc] initWithChosen:^(NSString *action) {
    app->menuChosen(action.UTF8String);
  }];
  return self;
}

- (MTKView *)mtkView {
  return (MTKView *)self.view;
}

- (void)loadView {
  self.view = [[ApplEmView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 860)];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.mtkView.device = self.device;
  self.mtkView.delegate = self;
  self.mtkView.preferredFramesPerSecond = 60;
  ImGui_ImplOSX_Init(self.view);
  acceptDropsOnViewports();
  // The app says which window the pointer is over (reportHoveredViewport).
  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasMouseHoveredViewport;
  g_dropApp = _app.get();
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
  reportHoveredViewport();
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
  [_toolbar update:_app->toolbarState()];
  if (_app->quitRequested()) [NSApp terminate:nil];
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
}


- (void)attachToolbarTo:(NSWindow *)window {
  [_toolbar attachToWindow:window];
}

- (BOOL)commandKeysToWindow {
  return _app && _app->commandKeysToWindow();
}

// The content size a resize should land on to keep the picture's shape, or
// the proposal when the picture is not what fills the window.
- (NSSize)contentSizeFor:(NSSize)proposed current:(NSSize)current {
  float width = 0;
  float height = 0;
  if (!_app || !_app->mainContentSizeFor(proposed.width, proposed.height, current.width, current.height,
                                         width, height)) {
    return proposed;
  }
  return NSMakeSize(width, height);
}

- (void)beginLiveResize:(BOOL)widthLeads {
  if (_app) _app->beginLiveResize(widthLeads);
}

- (void)endLiveResize {
  if (_app) _app->endLiveResize();
}

- (NSSize)contentSizeWithin:(NSSize)limit {
  float width = 0;
  float height = 0;
  if (!_app || !_app->mainContentSizeWithin(limit.width, limit.height, width, height)) return limit;
  return NSMakeSize(width, height);
}

// Another app took the keyboard: key-ups for anything held will never come.
- (void)releaseKeys {
  if (_app) _app->releaseKeys();
}

- (void)shutdown {
  if (!_app) return;
  g_dropApp = nullptr;
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

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) AppViewController *controller;
@end

@implementation AppDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
  return YES;
}

// Closing the main window closes everything and quits. The windows ImGui
// makes for windows dragged out of the main one are windows too, as are a
// file panel and a dialog, so the app would otherwise never close its last
// window; and they are put away at once rather than left on the screen
// while the app shuts down.
- (void)windowWillClose:(NSNotification *)notification {
  if (notification.object != self.window) return;
  for (NSWindow *window in NSApp.windows) {
    if (window == self.window) continue;
    if ([window isKindOfClass:NSSavePanel.class]) [(NSSavePanel *)window cancel:nil];
    [window orderOut:nil];
  }
  dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
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
  self.window.delegate = self;
  self.window.title = @"ApplEm";
  [self.window center];
  [self.window setFrameAutosaveName:@"ApplEmMainWindow"];
  self.window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
  [self.controller attachToolbarTo:self.window];
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

// A resize keeps the picture at the machine's shape. Full screen is the
// system's to size, and the picture is letterboxed there instead.
- (NSSize)windowWillResize:(NSWindow *)window toSize:(NSSize)frameSize {
  if (window.styleMask & NSWindowStyleMaskFullScreen) return frameSize;
  const NSRect frame = window.frame;
  const NSSize proposed = [window contentRectForFrameRect:NSMakeRect(0, 0, frameSize.width, frameSize.height)].size;
  const NSSize current = [window contentRectForFrameRect:frame].size;
  const NSSize content = [self.controller contentSizeFor:proposed current:current];
  return [window frameRectForContentRect:NSMakeRect(0, 0, content.width, content.height)].size;
}

// Which edge is being dragged decides which dimension leads, for the whole
// drag: the side edges and the corners lead with the width, the top and
// bottom with the height. Deciding afresh at each step by which changed
// more flips between the two in a corner drag, and the window jumps.
- (void)windowWillStartLiveResize:(NSNotification *)notification {
  NSWindow *window = notification.object;
  const NSPoint mouse = window.mouseLocationOutsideOfEventStream;
  const NSSize size = window.frame.size;
  const CGFloat fromSide = std::min(mouse.x, size.width - mouse.x);
  const CGFloat fromTopOrBottom = std::min(mouse.y, size.height - mouse.y);
  [self.controller beginLiveResize:fromSide <= fromTopOrBottom + 8];
}

- (void)windowDidEndLiveResize:(NSNotification *)notification {
  [self.controller endLiveResize];
}

// The zoom button: as large as the screen allows at the picture's shape.
- (NSRect)windowWillUseStandardFrame:(NSWindow *)window defaultFrame:(NSRect)newFrame {
  const NSSize limit = [window contentRectForFrameRect:newFrame].size;
  const NSSize content = [self.controller contentSizeWithin:limit];
  NSRect frame = [window frameRectForContentRect:NSMakeRect(0, 0, content.width, content.height)];
  frame.origin.x = NSMidX(newFrame) - frame.size.width / 2;
  frame.origin.y = NSMaxY(newFrame) - frame.size.height;
  return frame;
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
