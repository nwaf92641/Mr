/* Stand-in for AppKit/AppKit.h: the window and view the present test needs. */
#ifndef MR_STUB_APPKIT_H
#define MR_STUB_APPKIT_H

#include <Foundation/Foundation.h>
#include <QuartzCore/CAMetalLayer.h>

typedef CGRect NSRect;
typedef NSUInteger NSWindowStyleMask;
typedef NSUInteger NSBackingStoreType;
typedef NSInteger NSApplicationActivationPolicy;

enum { NSWindowStyleMaskBorderless = 0 };
enum { NSBackingStoreBuffered = 2 };
enum { NSApplicationActivationPolicyAccessory = 1 };

static inline NSRect NSMakeRect(CGFloat x, CGFloat y, CGFloat w, CGFloat h) {
  NSRect r;
  r.origin.x = x;
  r.origin.y = y;
  r.size.width = w;
  r.size.height = h;
  return r;
}

@class NSView;

@interface NSWindow : NSObject
- (instancetype)initWithContentRect:(NSRect)contentRect
                          styleMask:(NSWindowStyleMask)style
                            backing:(NSBackingStoreType)backingStoreType
                              defer:(BOOL)flag;
- (void)setContentView:(NSView *)contentView;
- (void)orderFront:(id)sender;
- (void)close;
@end

@interface NSView : NSObject
- (instancetype)initWithFrame:(NSRect)frameRect;
- (void)setLayer:(id)layer;
- (void)setWantsLayer:(BOOL)wantsLayer;
@end

@interface NSApplication : NSObject
+ (NSApplication *)sharedApplication;
- (void)setActivationPolicy:(NSApplicationActivationPolicy)policy;
@end

#define NSApp ((NSApplication *)0)

#endif /* MR_STUB_APPKIT_H */
