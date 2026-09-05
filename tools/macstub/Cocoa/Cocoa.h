// A STUB, not a replacement. Declares only the AppKit/CoreGraphics surface that
// src/platform/macplugview.mm actually touches, so that file can be
// syntax-checked with clang on Linux where no macOS SDK exists.
//
// What this proves and what it does not: it catches structural errors, typos in
// C++ member names, wrong argument counts against the API AS UNDERSTOOD HERE,
// and anything that does not parse. It cannot validate an API fact, because the
// signatures below were written from the same reference trees the .mm was --
// that would be circular. The API facts are grounded at their sites in the .mm.
#pragma once

#include <objc/objc.h>
#include <stddef.h>

typedef double CGFloat;
typedef unsigned short unichar;
typedef unsigned long NSUInteger;
typedef long NSInteger;

struct CGPoint {
    CGFloat x, y;
};
struct CGSize {
    CGFloat width, height;
};
struct CGRect {
    CGPoint origin;
    CGSize size;
};
typedef struct CGPoint NSPoint;
typedef struct CGSize NSSize;
typedef struct CGRect NSRect;

static inline NSRect NSMakeRect(CGFloat x, CGFloat y, CGFloat w, CGFloat h)
{
    NSRect r = {{x, y}, {w, h}};
    return r;
}
static inline NSSize NSMakeSize(CGFloat w, CGFloat h)
{
    NSSize s = {w, h};
    return s;
}
static inline CGRect CGRectMake(CGFloat x, CGFloat y, CGFloat w, CGFloat h)
{
    CGRect r = {{x, y}, {w, h}};
    return r;
}

// --- CoreGraphics ---
typedef struct CGContext *CGContextRef;
typedef struct CGImage *CGImageRef;
typedef struct CGColorSpace *CGColorSpaceRef;
typedef struct CGDataProvider *CGDataProviderRef;
typedef unsigned CGBitmapInfo;
typedef unsigned CGDirectDisplayID;
typedef int CGInterpolationQuality;
typedef int CGColorRenderingIntent;
enum { kCGImageAlphaPremultipliedFirst = 1 };
enum { kCGBitmapByteOrder32Host = 0x2000 };
enum { kCGInterpolationNone = 1 };
enum { kCGRenderingIntentDefault = 0 };
typedef void (*CGDataProviderReleaseDataCallback)(void *, const void *, size_t);
CGDataProviderRef CGDataProviderCreateWithData(void *, const void *, size_t,
                                               CGDataProviderReleaseDataCallback);
void CGDataProviderRelease(CGDataProviderRef);
CGImageRef CGImageCreate(size_t, size_t, size_t, size_t, size_t, CGColorSpaceRef, CGBitmapInfo,
                         CGDataProviderRef, const CGFloat *, bool, CGColorRenderingIntent);
void CGImageRelease(CGImageRef);
CGDirectDisplayID CGMainDisplayID(void);
CGColorSpaceRef CGDisplayCopyColorSpace(CGDirectDisplayID);
void CGColorSpaceRelease(CGColorSpaceRef);
void CGContextSaveGState(CGContextRef);
void CGContextRestoreGState(CGContextRef);
void CGContextScaleCTM(CGContextRef, CGFloat, CGFloat);
void CGContextDrawImage(CGContextRef, CGRect, CGImageRef);
void CGContextSetInterpolationQuality(CGContextRef, CGInterpolationQuality);

// --- CoreFoundation ---
typedef const struct __CFString *CFStringRef;
extern CFStringRef kCFRunLoopCommonModes;

// --- AppKit ---
typedef NSUInteger NSEventModifierFlags;
enum {
    NSEventModifierFlagShift = 1 << 17,
    NSEventModifierFlagControl = 1 << 18,
    NSEventModifierFlagOption = 1 << 19,
    NSEventModifierFlagCommand = 1 << 20
};
typedef NSUInteger NSTrackingAreaOptions;
enum {
    NSTrackingMouseEnteredAndExited = 0x01,
    NSTrackingMouseMoved = 0x02,
    NSTrackingActiveInActiveApp = 0x40,
    NSTrackingInVisibleRect = 0x200
};
enum {
    NSDeleteFunctionKey = 0xF728,
    NSLeftArrowFunctionKey = 0xF702,
    NSRightArrowFunctionKey = 0xF703,
    NSUpArrowFunctionKey = 0xF700,
    NSDownArrowFunctionKey = 0xF701,
    NSHomeFunctionKey = 0xF729,
    NSEndFunctionKey = 0xF72B,
    NSPageUpFunctionKey = 0xF72C,
    NSPageDownFunctionKey = 0xF72D
};

@interface NSObject
- (instancetype)init;
+ (instancetype)alloc;
- (instancetype)retain;
- (void)release;
- (instancetype)autorelease;
- (void)dealloc;
@end

@interface NSString : NSObject
- (NSUInteger)length;
- (unichar)characterAtIndex:(NSUInteger)i;
@end

@interface NSResponder : NSObject
- (NSResponder *)nextResponder;
- (void)keyDown:(id)event;
- (void)keyUp:(id)event;
- (BOOL)acceptsFirstResponder;
- (BOOL)resignFirstResponder;
@end

@interface NSEvent : NSObject
- (NSPoint)locationInWindow;
- (NSEventModifierFlags)modifierFlags;
- (CGFloat)deltaY;
- (NSString *)characters;
- (NSString *)charactersIgnoringModifiers;
@end

@interface NSTrackingArea : NSObject
- (instancetype)initWithRect:(NSRect)r
                     options:(NSTrackingAreaOptions)o
                       owner:(id)owner
                    userInfo:(id)info;
@end

@interface NSGraphicsContext : NSObject
+ (NSGraphicsContext *)currentContext;
- (CGContextRef)CGContext;
@end

@class NSWindow;

@interface NSView : NSResponder
- (instancetype)initWithFrame:(NSRect)frameRect;
- (NSRect)bounds;
- (NSRect)frame;
- (void)setFrameSize:(NSSize)newSize;
- (void)addSubview:(NSView *)v;
- (void)removeFromSuperview;
- (void)addTrackingArea:(NSTrackingArea *)a;
- (NSWindow *)window;
- (NSPoint)convertPoint:(NSPoint)p fromView:(NSView *)v;
- (void)setNeedsDisplay:(BOOL)flag;
- (void)drawRect:(NSRect)r;
- (BOOL)isFlipped;
- (BOOL)isOpaque;
- (BOOL)acceptsFirstMouse:(NSEvent *)e;
- (void)viewDidMoveToWindow;
- (void)viewDidChangeBackingProperties;
- (void)mouseDown:(NSEvent *)e;
- (void)mouseUp:(NSEvent *)e;
- (void)mouseDragged:(NSEvent *)e;
- (void)mouseMoved:(NSEvent *)e;
- (void)mouseExited:(NSEvent *)e;
- (void)rightMouseDown:(NSEvent *)e;
- (void)rightMouseUp:(NSEvent *)e;
- (void)rightMouseDragged:(NSEvent *)e;
- (void)otherMouseDown:(NSEvent *)e;
- (void)otherMouseUp:(NSEvent *)e;
- (void)otherMouseDragged:(NSEvent *)e;
- (void)scrollWheel:(NSEvent *)e;
@end

@interface NSWindow : NSResponder
- (CGFloat)backingScaleFactor;
- (NSResponder *)firstResponder;
- (BOOL)makeFirstResponder:(NSResponder *)r;
@end

@interface NSTimer : NSObject
+ (NSTimer *)timerWithTimeInterval:(double)ti
                            target:(id)t
                          selector:(SEL)s
                          userInfo:(id)ui
                           repeats:(BOOL)rep;
- (void)invalidate;
@end

@interface NSRunLoop : NSObject
+ (NSRunLoop *)currentRunLoop;
- (void)addTimer:(NSTimer *)t forMode:(NSString *)mode;
@end
