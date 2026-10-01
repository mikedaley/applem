/*
 * ui_theme.mm - The macOS look: system colours, SF Pro and SF Mono
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "ui_theme.hpp"

#include "platform_paths.hpp"

#import <Cocoa/Cocoa.h>

#include "imgui.h"

#include "imgui_internal.h" // ImGuiViewportP, for the window a viewport holds

#import <QuartzCore/QuartzCore.h>

#include <map>
#include <string>

namespace a2e::native::ui {

namespace {

ImFont *g_mono = nullptr;
std::string g_appliedKey;
bool g_dark = true;

// An AppKit colour resolved under the current appearance, in sRGB.
ImVec4 resolve(NSColor *color, float alphaScale = 1.0f) {
  __block ImVec4 out(0, 0, 0, 1);
  [NSApp.effectiveAppearance performAsCurrentDrawingAppearance:^{
    NSColor *srgb = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    if (srgb) {
      out = ImVec4(static_cast<float>(srgb.redComponent), static_cast<float>(srgb.greenComponent),
                   static_cast<float>(srgb.blueComponent), static_cast<float>(srgb.alphaComponent) * alphaScale);
    }
  }];
  return out;
}

ImVec4 withAlpha(ImVec4 colour, float alpha) {
  colour.w = alpha;
  return colour;
}

ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
  return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// What decides the palette: light or dark, and which accent.
std::string appearanceKey() {
  NSAppearanceName name = [NSApp.effectiveAppearance
      bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
  const ImVec4 accent = resolve(NSColor.controlAccentColor);
  char key[96];
  std::snprintf(key, sizeof(key), "%s/%.3f/%.3f/%.3f", name.UTF8String, accent.x, accent.y, accent.z);
  return key;
}

void apply() {
  g_dark = [[NSApp.effectiveAppearance
      bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]]
      isEqualToString:NSAppearanceNameDarkAqua];

  ImGuiStyle &style = ImGui::GetStyle();

  // Shapes: rounded like AppKit's controls, roomier than ImGui's defaults.
  // Floating windows match a macOS window's corners. A window that has been
  // dragged out of the main one is its own macOS window, which ImGui draws
  // square; roundViewportWindows() rounds that one at the window level.
  style.WindowRounding = WINDOW_RADIUS;
  style.ChildRounding = 6.0f;
  style.FrameRounding = 5.0f;
  style.PopupRounding = 7.0f;
  style.GrabRounding = 5.0f;
  style.TabRounding = 5.0f;
  style.ScrollbarRounding = 8.0f;
  style.WindowPadding = ImVec2(12, 10);
  style.FramePadding = ImVec2(8, 4);
  style.ItemSpacing = ImVec2(8, 6);
  style.ItemInnerSpacing = ImVec2(6, 4);
  style.CellPadding = ImVec2(6, 4);
  style.IndentSpacing = 18;
  style.ScrollbarSize = 11;
  style.GrabMinSize = 10;
  style.WindowBorderSize = 1;
  style.ChildBorderSize = 1;
  style.PopupBorderSize = 1;
  style.FrameBorderSize = 0;
  style.TabBorderSize = 0;
  style.SeparatorTextBorderSize = 1;
  style.SeparatorTextPadding = ImVec2(10, 4);
  style.WindowTitleAlign = ImVec2(0.5f, 0.5f); // centred, as macOS titles are
  style.WindowMenuButtonPosition = ImGuiDir_None;
  style.DockingSeparatorSize = 1;

  // Colours: the system's own, under the current appearance.
  const ImVec4 window = resolve(NSColor.windowBackgroundColor);
  const ImVec4 control = resolve(NSColor.controlBackgroundColor);
  const ImVec4 text = resolve(NSColor.labelColor);
  const ImVec4 secondary = resolve(NSColor.secondaryLabelColor);
  const ImVec4 tertiary = resolve(NSColor.tertiaryLabelColor);
  const ImVec4 separator = resolve(NSColor.separatorColor);
  const ImVec4 accent = resolve(NSColor.controlAccentColor);
  const ImVec4 selection = resolve(NSColor.selectedContentBackgroundColor);
  const ImVec4 unemphasised = resolve(NSColor.unemphasizedSelectedContentBackgroundColor);
  // A control's fill is a tint of the label colour, as AppKit draws a
  // push button or a text field's bezel in either appearance.
  const ImVec4 fill = withAlpha(text, g_dark ? 0.10f : 0.06f);
  const ImVec4 fillHover = withAlpha(text, g_dark ? 0.16f : 0.10f);
  const ImVec4 fillActive = withAlpha(text, g_dark ? 0.22f : 0.15f);
  const ImVec4 titleBar = g_dark ? mix(window, ImVec4(0, 0, 0, 1), 0.25f) : mix(window, ImVec4(0, 0, 0, 1), 0.05f);

  ImVec4 *c = style.Colors;
  c[ImGuiCol_Text] = text;
  c[ImGuiCol_TextDisabled] = tertiary;
  c[ImGuiCol_WindowBg] = window;
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = g_dark ? mix(window, ImVec4(1, 1, 1, 1), 0.06f) : control;
  c[ImGuiCol_Border] = separator;
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = fill;
  c[ImGuiCol_FrameBgHovered] = fillHover;
  c[ImGuiCol_FrameBgActive] = fillActive;
  c[ImGuiCol_TitleBg] = titleBar;
  c[ImGuiCol_TitleBgActive] = titleBar;
  c[ImGuiCol_TitleBgCollapsed] = titleBar;
  c[ImGuiCol_MenuBarBg] = titleBar;
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = withAlpha(secondary, 0.35f);
  c[ImGuiCol_ScrollbarGrabHovered] = withAlpha(secondary, 0.55f);
  c[ImGuiCol_ScrollbarGrabActive] = withAlpha(secondary, 0.75f);
  c[ImGuiCol_CheckMark] = accent;
  c[ImGuiCol_SliderGrab] = accent;
  c[ImGuiCol_SliderGrabActive] = mix(accent, ImVec4(1, 1, 1, 1), 0.2f);
  c[ImGuiCol_Button] = fill;
  c[ImGuiCol_ButtonHovered] = fillHover;
  c[ImGuiCol_ButtonActive] = accent;
  c[ImGuiCol_Header] = unemphasised;
  c[ImGuiCol_HeaderHovered] = withAlpha(selection, 0.65f);
  c[ImGuiCol_HeaderActive] = selection;
  c[ImGuiCol_Separator] = separator;
  c[ImGuiCol_SeparatorHovered] = withAlpha(accent, 0.7f);
  c[ImGuiCol_SeparatorActive] = accent;
  c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ResizeGripHovered] = withAlpha(accent, 0.5f);
  c[ImGuiCol_ResizeGripActive] = accent;
  c[ImGuiCol_InputTextCursor] = text;
  c[ImGuiCol_Tab] = titleBar;
  c[ImGuiCol_TabHovered] = fillHover;
  c[ImGuiCol_TabSelected] = window;
  c[ImGuiCol_TabSelectedOverline] = accent;
  c[ImGuiCol_TabDimmed] = titleBar;
  c[ImGuiCol_TabDimmedSelected] = window;
  c[ImGuiCol_TabDimmedSelectedOverline] = withAlpha(secondary, 0.5f);
  c[ImGuiCol_DockingPreview] = withAlpha(accent, 0.45f);
  c[ImGuiCol_DockingEmptyBg] = window;
  c[ImGuiCol_PlotLines] = accent;
  c[ImGuiCol_PlotLinesHovered] = mix(accent, ImVec4(1, 1, 1, 1), 0.3f);
  c[ImGuiCol_PlotHistogram] = accent;
  c[ImGuiCol_PlotHistogramHovered] = mix(accent, ImVec4(1, 1, 1, 1), 0.3f);
  c[ImGuiCol_TableHeaderBg] = fill;
  c[ImGuiCol_TableBorderStrong] = separator;
  c[ImGuiCol_TableBorderLight] = withAlpha(separator, 0.5f);
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  // Alternating rows, as an NSTableView draws them.
  c[ImGuiCol_TableRowBgAlt] = withAlpha(text, g_dark ? 0.035f : 0.03f);
  c[ImGuiCol_TextLink] = accent;
  c[ImGuiCol_TextSelectedBg] = withAlpha(selection, 0.6f);
  c[ImGuiCol_DragDropTarget] = accent;
  c[ImGuiCol_NavCursor] = accent;
  c[ImGuiCol_NavWindowingHighlight] = withAlpha(accent, 0.7f);
  c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.3f);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, g_dark ? 0.45f : 0.25f);
}

} // namespace

void loadFonts() {
  ImGuiIO &io = ImGui::GetIO();
  ImFontConfig config;
  config.OversampleH = 2;

  // SF Pro is the system's own face, and every Mac has it.
  const char *sfPro = "/System/Library/Fonts/SFNS.ttf";
  if ([NSFileManager.defaultManager fileExistsAtPath:@(sfPro)]) {
    io.Fonts->AddFontFromFileTTF(sfPro, 13.0f, &config);
  }
  const std::string mono = uiFontPath();
  if (!mono.empty()) g_mono = io.Fonts->AddFontFromFileTTF(mono.c_str(), 12.5f, &config);
}

ImFont *monoFont() { return g_mono ? g_mono : ImGui::GetFont(); }

void followSystemAppearance() {
  const std::string key = appearanceKey();
  if (key == g_appliedKey) return;
  g_appliedKey = key;
  apply();
  // The dragged-out windows' edges are the separator colour of the
  // appearance they were made in; give them this one's.
  ImGuiPlatformIO &io = ImGui::GetPlatformIO();
  for (ImGuiViewport *viewport : io.Viewports) {
    if (viewport == ImGui::GetMainViewport()) continue;
    void *handle = viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle;
    NSWindow *window = handle ? (__bridge NSWindow *)handle : nil;
    CALayer *layer = window.contentView.layer;
    if (!layer || layer.borderWidth <= 0) continue;
    [NSApp.effectiveAppearance performAsCurrentDrawingAppearance:^{
      layer.borderColor = NSColor.separatorColor.CGColor;
    }];
  }
}

bool isDark() { return g_dark; }

namespace {

void (*g_createWindow)(ImGuiViewport *) = nullptr;
void (*g_renderWindow)(ImGuiViewport *, void *) = nullptr;
std::map<ImGuiViewport *, int> g_shadowFrames;

NSWindow *windowOf(ImGuiViewport *viewport) {
  void *handle = viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle;
  return handle ? (__bridge NSWindow *)handle : nil;
}

// A popup or tooltip gets the popup radius; anything else a window's.
float radiusFor(ImGuiViewport *viewport) {
  ImGuiWindow *window = static_cast<ImGuiViewportP *>(viewport)->Window;
  if (window && (window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip))) {
    return ImGui::GetStyle().PopupRounding;
  }
  return WINDOW_RADIUS;
}

void createWindow(ImGuiViewport *viewport) {
  g_createWindow(viewport);
  if (!(viewport->Flags & ImGuiViewportFlags_NoDecoration)) return;
  NSWindow *window = windowOf(viewport);
  NSView *view = window.contentView;
  if (!window || !view.layer) return;
  // Transparent outside the rounded shape, which the layer clips to.
  window.opaque = NO;
  window.backgroundColor = NSColor.clearColor;
  window.hasShadow = YES;
  CALayer *layer = view.layer;
  layer.cornerRadius = radiusFor(viewport);
  layer.cornerCurve = kCACornerCurveContinuous;
  layer.masksToBounds = YES;
  // A hairline edge, as a macOS window has, in the separator colour.
  layer.borderWidth = 1.0 / std::max<CGFloat>(window.backingScaleFactor, 1.0);
  [NSApp.effectiveAppearance performAsCurrentDrawingAppearance:^{
    layer.borderColor = NSColor.separatorColor.CGColor;
  }];
  g_shadowFrames[viewport] = 0;
}

// The shadow follows the window's shape, which it learns only once there is
// something drawn, and again if the window is resized.
void renderWindow(ImGuiViewport *viewport, void *arg) {
  g_renderWindow(viewport, arg);
  auto it = g_shadowFrames.find(viewport);
  if (it == g_shadowFrames.end()) return;
  NSWindow *window = windowOf(viewport);
  if (!window) return;
  static std::map<ImGuiViewport *, NSSize> sizes;
  const NSSize size = window.frame.size;
  NSSize &last = sizes[viewport];
  if (it->second < 3 || !NSEqualSizes(last, size)) {
    [window invalidateShadow];
    if (it->second < 3) it->second++;
    last = size;
  }
}

} // namespace

void roundViewportWindows() {
  ImGuiPlatformIO &io = ImGui::GetPlatformIO();
  if (g_createWindow || !io.Renderer_CreateWindow || !io.Renderer_RenderWindow) return;
  g_createWindow = io.Renderer_CreateWindow;
  g_renderWindow = io.Renderer_RenderWindow;
  io.Renderer_CreateWindow = createWindow;
  io.Renderer_RenderWindow = renderWindow;
}

} // namespace a2e::native::ui
