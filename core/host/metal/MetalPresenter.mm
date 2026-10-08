#include "MetalPresenter.hpp"
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace AnyPS5::Metal {

namespace {

void mainThread() {
    if (![NSThread isMainThread]) throw std::runtime_error("Metal presentation requires the main macOS thread");
}

std::runtime_error error(const char* operation, NSError* detail) {
    return std::runtime_error(std::string(operation) + ": " + (detail ? detail.localizedDescription.UTF8String : "Metal returned no object"));
}

void finish(id<MTLCommandBuffer> command) {
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) throw error("Metal presentation command failed", command.error);
}

NSString* source() {
    return @R"msl(#include <metal_stdlib>
using namespace metal;
struct Raster { float4 position [[position]]; float2 uv; };
vertex Raster fullscreen(uint index [[vertex_id]]) {
    const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    Raster result;
    result.position = float4(positions[index], 0, 1);
    result.uv = positions[index] * float2(0.5, -0.5) + float2(0.5);
    return result;
}
fragment float4 framebuffer(Raster input [[stage_in]], texture2d<float> pixels [[texture(0)]]) {
    constexpr sampler nearest(coord::normalized, address::clamp_to_edge, filter::nearest);
    return float4(pixels.sample(nearest, input.uv).rgb, 1);
}
)msl";
}

}

struct Presenter::Impl {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> pipeline;
    NSWindow* window;
    CAMetalLayer* layer;
    id<MTLTexture> lastFrame;
    std::array<bool, 256> keys{};
    std::uint32_t width, height;
    bool closed = false;
};

Presenter::Presenter(std::uint32_t width, std::uint32_t height, std::string title) : impl(std::make_unique<Impl>()) {
    mainThread();
    if (width == 0 || height == 0 || width > 16384 || height > 16384) throw std::runtime_error("Metal presentation dimensions are invalid");
    @autoreleasepool {
        impl->width = width;
        impl->height = height;
        impl->device = MTLCreateSystemDefaultDevice();
        if (!impl->device || !impl->device.hasUnifiedMemory || ![impl->device supportsFamily:MTLGPUFamilyApple7]) throw std::runtime_error("Metal presentation requires an Apple Silicon GPU");
        impl->queue = [impl->device newCommandQueue];
        if (!impl->queue) throw std::runtime_error("Metal presentation queue creation failed");
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_0;
        options.mathMode = MTLMathModeSafe;
        options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        NSError* detail = nil;
        id<MTLLibrary> library = [impl->device newLibraryWithSource:source() options:options error:&detail];
        if (!library) throw error("Compile Metal presentation shader", detail);
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"fullscreen"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"framebuffer"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        impl->pipeline = [impl->device newRenderPipelineStateWithDescriptor:descriptor error:&detail];
        if (!impl->pipeline) throw error("Create Metal presentation pipeline", detail);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        NSString* windowTitle = [[NSString alloc] initWithBytes:title.data() length:title.size() encoding:NSUTF8StringEncoding];
        if (!windowTitle) throw std::runtime_error("Metal presentation title must be UTF-8");
        impl->window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        impl->window.releasedWhenClosed = NO;
        impl->window.title = windowTitle;
        impl->window.contentAspectRatio = NSMakeSize(width, height);
        impl->layer = [CAMetalLayer layer];
        impl->layer.device = impl->device;
        impl->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        impl->layer.framebufferOnly = NO;
        impl->layer.opaque = YES;
        impl->layer.displaySyncEnabled = YES;
        impl->window.contentView.wantsLayer = YES;
        impl->window.contentView.layer = impl->layer;
        [impl->window center];
        [impl->window makeKeyAndOrderFront:nil];
    }
}

Presenter::~Presenter() {
    NSWindow* window = impl->window;
    if (!window) return;
    if ([NSThread isMainThread]) [window close];
    else dispatch_async(dispatch_get_main_queue(), ^{ [window close]; });
}

std::string Presenter::DeviceName() const { return impl->device.name.UTF8String; }

bool Presenter::Poll() {
    mainThread();
    @autoreleasepool {
        for (;;) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
                inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!event) break;
            if (event.type == NSEventTypeKeyDown || event.type == NSEventTypeKeyUp) {
                if (event.keyCode < impl->keys.size()) impl->keys[event.keyCode] = event.type == NSEventTypeKeyDown;
            }
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
        if (!impl->window.visible) impl->closed = true;
        if (!impl->window.keyWindow) impl->keys.fill(false);
        return !impl->closed;
    }
}

bool Presenter::KeyPressed(std::uint16_t key) const {
    mainThread();
    if (key >= impl->keys.size()) throw std::runtime_error("Metal presentation key code is invalid");
    return impl->keys[key];
}

bool Presenter::Present(std::span<const std::byte> pixels, std::size_t bytesPerRow, FrameFormat format) {
    mainThread();
    if (bytesPerRow < static_cast<std::size_t>(impl->width) * 4 || bytesPerRow % 4 != 0 ||
        bytesPerRow > std::numeric_limits<std::size_t>::max() / impl->height || pixels.size() != bytesPerRow * impl->height) {
        throw std::runtime_error("Metal presentation frame byte range is invalid");
    }
    if (format != FrameFormat::Rgba8 && format != FrameFormat::Bgra8) throw std::runtime_error("Metal presentation frame format is invalid");
    if (!Poll()) return false;
    @autoreleasepool {
        const auto bounds = impl->window.contentView.bounds;
        const auto scale = impl->window.backingScaleFactor;
        impl->layer.contentsScale = scale;
        impl->layer.drawableSize = CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
        id<CAMetalDrawable> drawable = [impl->layer nextDrawable];
        if (!drawable) throw std::runtime_error("Metal presentation drawable is unavailable");
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            format == FrameFormat::Rgba8 ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatBGRA8Unorm
            width:impl->width height:impl->height mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [impl->device newTextureWithDescriptor:descriptor];
        if (!texture) throw std::runtime_error("Metal presentation input texture allocation failed");
        [texture replaceRegion:MTLRegionMake2D(0, 0, impl->width, impl->height) mipmapLevel:0
            withBytes:pixels.data() bytesPerRow:bytesPerRow];
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> command = [impl->queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!command || !encoder) throw std::runtime_error("Metal presentation render encoder creation failed");
        [encoder setRenderPipelineState:impl->pipeline];
        [encoder setFragmentTexture:texture atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [command presentDrawable:drawable];
        finish(command);
        impl->lastFrame = drawable.texture;
        return true;
    }
}

std::array<std::uint32_t, 2> Presenter::FrameSize() const {
    mainThread();
    if (!impl->lastFrame) throw std::runtime_error("Metal presentation has no completed frame");
    return {static_cast<std::uint32_t>(impl->lastFrame.width), static_cast<std::uint32_t>(impl->lastFrame.height)};
}

std::vector<std::byte> Presenter::Readback() const {
    const auto size = FrameSize();
    @autoreleasepool {
        const auto row = (static_cast<std::size_t>(size[0]) * 4 + 255) & ~std::size_t{255};
        id<MTLBuffer> buffer = [impl->device newBufferWithLength:row * size[1] options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [impl->queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
        if (!buffer || !command || !encoder) throw std::runtime_error("Metal presentation readback allocation failed");
        [encoder copyFromTexture:impl->lastFrame sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(size[0], size[1], 1) toBuffer:buffer destinationOffset:0
            destinationBytesPerRow:row destinationBytesPerImage:row * size[1]];
        [encoder endEncoding];
        finish(command);
        std::vector<std::byte> result(static_cast<std::size_t>(size[0]) * size[1] * 4);
        for (std::size_t y = 0; y < size[1]; ++y) std::memcpy(result.data() + y * size[0] * 4, static_cast<const std::byte*>(buffer.contents) + y * row, size[0] * 4);
        return result;
    }
}

void Presenter::Close() {
    mainThread();
    [impl->window close];
    impl->keys.fill(false);
    impl->closed = true;
}

}
