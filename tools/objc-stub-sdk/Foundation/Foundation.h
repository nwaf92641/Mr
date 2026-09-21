/* Minimal stand-ins for the parts of Apple's headers this project touches.
 * See the README next to this file for what a check against them is worth. */
#ifndef MR_STUB_FOUNDATION_H
#define MR_STUB_FOUNDATION_H

#include <stddef.h>
#include <stdint.h>

typedef signed char BOOL;
#ifndef YES
#define YES ((BOOL)1)
#endif
#ifndef NO
#define NO ((BOOL)0)
#endif
#ifndef nil
#define nil ((id)0)
#endif

typedef long NSInteger;
typedef unsigned long NSUInteger;
typedef double CGFloat;

@class NSString;
@class NSError;

/* NSObject is both a class and a protocol in Foundation, and Metal's protocols
 * are declared as refining the protocol. Declaring only the class makes every
 * `@protocol MTLFoo <NSObject>` unreachable, which is exactly the mistake this
 * file would otherwise have hidden. */
@protocol NSObject
- (id)init;
- (id)retain;
- (oneway void)release;
- (id)autorelease;
- (NSUInteger)hash;
- (BOOL)isEqual:(id)object;
@end

@interface NSObject <NSObject>
+ (id)alloc;
+ (id)new;
@end

@interface NSString : NSObject
+ (instancetype)stringWithUTF8String:(const char *)bytes;
+ (instancetype)stringWithFormat:(NSString *)format, ...;
- (const char *)UTF8String;
- (NSUInteger)length;
@end

@interface NSError : NSObject
- (NSString *)localizedDescription;
- (NSInteger)code;
@end

typedef struct {
  CGFloat x, y;
} CGPoint;

typedef struct {
  CGFloat width, height;
} CGSize;

/* CoreGraphics, via Foundation, provides these as inline functions. */
static inline CGSize CGSizeMake(CGFloat width, CGFloat height) {
  CGSize size;
  size.width = width;
  size.height = height;
  return size;
}

static inline CGPoint CGPointMake(CGFloat x, CGFloat y) {
  CGPoint point;
  point.x = x;
  point.y = y;
  return point;
}

typedef struct {
  CGPoint origin;
  CGSize size;
} CGRect;

typedef struct {
  NSUInteger location, length;
} NSRange;

#endif /* MR_STUB_FOUNDATION_H */
