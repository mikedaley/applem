// make-app-icon.swift - ApplEm's icon, drawn from the header's wordmark
//
// Usage: swift scripts/make-app-icon.swift public/assets/applem-logo.png out.png [bleed]
//
// Without "bleed" it is a macOS app icon: a rounded square on the 1024 grid
// with the standard margin, made into native/resources/icon.icns with
// iconutil. With "bleed" the square
// fills the canvas, for the web app's icons, which iOS and Android mask
// themselves.
import AppKit
let bleed = CommandLine.arguments.count > 3 && CommandLine.arguments[3] == "bleed"
let src = NSImage(contentsOfFile: CommandLine.arguments[1])!
let rep = NSBitmapImageRep(data: src.tiffRepresentation!)!
// Cut the apple out of the logo: every colour in it is saturated, and the
// logo's grey box and white lettering are not, so alpha follows saturation.
let cx = 40, cy = 15, cw = 230, ch = 280   // top-left origin in the 827x317 logo
let apple = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: cw, pixelsHigh: ch, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
for y in 0..<ch { for x in 0..<cw {
  guard let c = rep.colorAt(x: cx + x, y: cy + y)?.usingColorSpace(.deviceRGB) else { continue }
  let r = c.redComponent, g = c.greenComponent, b = c.blueComponent
  let mx = max(r, g, b), mn = min(r, g, b)
  let sat = mx > 0 ? (mx - mn) / mx : 0
  let a = min(1, max(0, (sat - 0.15) / 0.25)) * c.alphaComponent
  apple.setColor(NSColor(deviceRed: r, green: g, blue: b, alpha: a), atX: x, y: y)
}}
let size = 1024
let out = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: out)
NSColor.clear.setFill(); NSRect(x: 0, y: 0, width: size, height: size).fill()
// The macOS icon grid: an 824px rounded square centred in 1024.
let body = bleed ? NSRect(x: 0, y: 0, width: size, height: size) : NSRect(x: 100, y: 100, width: 824, height: 824)
let radius: CGFloat = bleed ? 0 : 185
let grad = NSGradient(starting: NSColor(deviceWhite: 0.20, alpha: 1), ending: NSColor(deviceWhite: 0.07, alpha: 1))!
grad.draw(in: NSBezierPath(roundedRect: body, xRadius: radius, yRadius: radius), angle: -90)
let h: CGFloat = bleed ? 640 : 600, w = h * CGFloat(cw) / CGFloat(ch)
let dst = NSRect(x: (CGFloat(size) - w) / 2, y: (CGFloat(size) - h) / 2, width: w, height: h)
apple.draw(in: dst, from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: false, hints: [.interpolation: NSImageInterpolation.high])
NSGraphicsContext.restoreGraphicsState()
try! out.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[2]))
