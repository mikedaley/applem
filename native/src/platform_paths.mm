/*
 * platform_paths.mm - Where the native app keeps what it remembers
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "platform_paths.hpp"

#import <Foundation/Foundation.h>

namespace a2e::native {

std::string appSupportDirectory() {
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

} // namespace a2e::native
