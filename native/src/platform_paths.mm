/*
 * platform_paths.mm - Where the native app keeps what it remembers
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "platform_paths.hpp"

#import <Foundation/Foundation.h>

#include <cstdlib>

namespace a2e::native {

std::string appSupportDirectory() {
  // A developer's override, so a second copy can run with its own settings
  // (and its own windows open) beside the one in use.
  if (const char *dir = std::getenv("APPLEM_SETTINGS_DIR"); dir && *dir) {
    [NSFileManager.defaultManager createDirectoryAtPath:@(dir)
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
    return dir;
  }
  @autoreleasepool {
    NSFileManager *files = NSFileManager.defaultManager;
    NSURL *base = [files URLsForDirectory:NSApplicationSupportDirectory
                                inDomains:NSUserDomainMask].firstObject;
    NSURL *dir = [base URLByAppendingPathComponent:@"ApplEm Native"
                                       isDirectory:YES];
    [files createDirectoryAtURL:dir
        withIntermediateDirectories:YES
                         attributes:nil
                              error:nil];
    return std::string(dir.path.UTF8String);
  }
}

std::string uiFontPath() {
  const char *candidates[] = {
      "/System/Applications/Utilities/Terminal.app/Contents/Resources/Fonts/SF-Mono-Regular.otf",
      "/Applications/Utilities/Terminal.app/Contents/Resources/Fonts/SF-Mono-Regular.otf",
      "/Library/Fonts/SF-Mono-Regular.otf",
      "/System/Library/Fonts/SFNSMono.ttf",
  };
  for (const char *path : candidates) {
    if ([NSFileManager.defaultManager fileExistsAtPath:@(path)]) return path;
  }
  return "";
}

} // namespace a2e::native
