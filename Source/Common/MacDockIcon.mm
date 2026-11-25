#import <AppKit/AppKit.h>
#include "MacDockIcon.h"

namespace tracks {

void setMacDockIconFromPngData(const void* data, size_t size) {
    if (!data || size == 0) return;
    @autoreleasepool {
        NSData* nsData = [NSData dataWithBytes:data length:size];
        NSImage* img = [[NSImage alloc] initWithData:nsData];
        if (img != nil) {
            [[NSApplication sharedApplication] setApplicationIconImage:img];
        }
    }
}

} // namespace tracks
