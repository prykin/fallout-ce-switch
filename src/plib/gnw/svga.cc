#include "plib/gnw/svga.h"

#include <stdint.h>

#include <algorithm>
#include <vector>

#ifdef __SWITCH__
#include <GLES2/gl2.h>
#endif

#include "plib/gnw/diagnostics.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/winmain.h"

namespace fallout {

static bool createRenderer(int width, int height);
static bool createBilinearRenderTarget(int sourceWidth, int sourceHeight, Uint32 format);
static void destroyRenderer();
static const char* getSdlScaleQualityHint(RenderScaleQuality scaleQuality);
static void setTextureScaleQuality(SDL_Texture* texture);
static void markFullTextureDirty();
static void markTextureDirtyRect(const SDL_Rect* rect);
static void markTextureDirtyRect(int x, int y, int width, int height);
static bool rectsTouchOrIntersect(const SDL_Rect& a, const SDL_Rect& b);
static SDL_Rect sourceRectToBilinearTargetRect(const SDL_Rect& sourceRect);
static int sourceRangeToTargetStart(int sourceStart, int sourceLength, int targetLength);
static int sourceRangeToTargetEnd(int sourceEnd, int sourceLength, int targetLength);
static void buildBilinearScaleMap(int sourceWidth, int sourceHeight, int targetWidth, int targetHeight);
static void scaleSurfaceBilinearRect(SDL_Surface* source, SDL_Surface* target, const SDL_Rect& targetRect);
static int sharpenBilinearFraction(int fraction);
static int lerp8(int from, int to, int fraction);
static Uint32 blendRgb888(Uint32 topLeft, Uint32 topRight, Uint32 bottomLeft, Uint32 bottomRight, int fractionX, int fractionY);

static constexpr int BILINEAR_DIRTY_RECT_CAPACITY = 64;
static constexpr int BILINEAR_DIRTY_FULL_AREA_PERCENT = 60;

#ifdef __SWITCH__
static bool createGlBilinearRenderer(int width, int height);
static void destroyGlRenderer();
static GLuint compileGlShader(GLenum type, const char* source);
static bool linkGlProgram(GLuint vertexShader, GLuint fragmentShader);
static void uploadGlDirtyRects();
static void uploadGlSourceRect(const SDL_Rect& sourceRect);
static void renderPresentGlBilinear();
static bool isGlBilinearRenderer();
#endif

// screen rect
Rect scr_size;

// 0x6ACA18
ScreenBlitFunc* scr_blit = GNW95_ShowRect;

SDL_Window* gSdlWindow = NULL;
SDL_Surface* gSdlSurface = NULL;
SDL_Renderer* gSdlRenderer = NULL;
SDL_Texture* gSdlTexture = NULL;
SDL_Surface* gSdlTextureSurface = NULL;

static RenderScaleQuality gRenderScaleQuality = RENDER_SCALE_QUALITY_LINEAR;
static SDL_Texture* gSdlBilinearTexture = NULL;
static SDL_Surface* gSdlBilinearTextureSurface = NULL;
static std::vector<int> gBilinearSourceX;
static std::vector<int> gBilinearSourceY;
static std::vector<int> gBilinearFractionX;
static std::vector<int> gBilinearFractionY;
static std::vector<SDL_Rect> gBilinearDirtySourceRects;
static bool gBilinearFullDirty = true;

#ifdef __SWITCH__
static SDL_GLContext gSdlGlContext = NULL;
static GLuint gGlProgram = 0;
static GLuint gGlSourceTexture = 0;
static GLuint gGlVertexBuffer = 0;
static GLint gGlPositionAttribute = -1;
static GLint gGlTextureUniform = -1;
static GLint gGlSourceSizeUniform = -1;
static GLint gGlOutputSizeUniform = -1;
static int gGlOutputWidth = 0;
static int gGlOutputHeight = 0;
static std::vector<unsigned char> gGlUploadBuffer;
#endif

// TODO: Remove once migration to update-render cycle is completed.
FpsLimiter sharedFpsLimiter;

// 0x4CB310
void GNW95_SetPaletteEntries(unsigned char* palette, int start, int count)
{
    if (gSdlSurface != NULL && gSdlSurface->format->palette != NULL) {
        SDL_Color colors[256];

        if (count != 0) {
            for (int index = 0; index < count; index++) {
                colors[index].r = palette[index * 3] << 2;
                colors[index].g = palette[index * 3 + 1] << 2;
                colors[index].b = palette[index * 3 + 2] << 2;
                colors[index].a = 255;
            }
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, start, count);
        SDL_BlitSurface(gSdlSurface, NULL, gSdlTextureSurface, NULL);
        markFullTextureDirty();
    }
}

// 0x4CB568
void GNW95_SetPalette(unsigned char* palette)
{
    if (gSdlSurface != NULL && gSdlSurface->format->palette != NULL) {
        SDL_Color colors[256];

        for (int index = 0; index < 256; index++) {
            colors[index].r = palette[index * 3] << 2;
            colors[index].g = palette[index * 3 + 1] << 2;
            colors[index].b = palette[index * 3 + 2] << 2;
            colors[index].a = 255;
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);
        SDL_BlitSurface(gSdlSurface, NULL, gSdlTextureSurface, NULL);
        markFullTextureDirty();
    }
}

// 0x4CB850
void GNW95_ShowRect(unsigned char* src, unsigned int srcPitch, unsigned int a3, unsigned int srcX, unsigned int srcY, unsigned int srcWidth, unsigned int srcHeight, unsigned int destX, unsigned int destY)
{
    buf_to_buf(src + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch, (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destY + destX, gSdlSurface->pitch);

    SDL_Rect srcRect;
    srcRect.x = destX;
    srcRect.y = destY;
    srcRect.w = srcWidth;
    srcRect.h = srcHeight;

    SDL_Rect destRect;
    destRect.x = destX;
    destRect.y = destY;
    SDL_BlitSurface(gSdlSurface, &srcRect, gSdlTextureSurface, &destRect);
    markTextureDirtyRect(&srcRect);
}

bool svga_init(VideoOptions* video_options)
{
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    gRenderScaleQuality = video_options->scaleQuality;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, getSdlScaleQualityHint(gRenderScaleQuality));

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        return false;
    }

    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;

    if (video_options->fullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN;
    }

#ifdef __SWITCH__
    if (video_options->scaleQuality == RENDER_SCALE_QUALITY_BILINEAR) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    }
#endif

    int windowWidth = video_options->width * video_options->scale;
    int windowHeight = video_options->height * video_options->scale;
    if (video_options->fullscreen && video_options->scaleQuality == RENDER_SCALE_QUALITY_BILINEAR) {
        SDL_DisplayMode displayMode;
        if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0 && displayMode.w > 0 && displayMode.h > 0) {
            windowWidth = displayMode.w;
            windowHeight = displayMode.h;
        }
    }

    gSdlWindow = SDL_CreateWindow(GNW95_title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        windowWidth,
        windowHeight,
        windowFlags);
    if (gSdlWindow == NULL) {
        return false;
    }

    if (!createRenderer(video_options->width, video_options->height)) {
        destroyRenderer();

        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;

        return false;
    }

    gSdlSurface = SDL_CreateRGBSurface(0,
        video_options->width,
        video_options->height,
        8,
        0,
        0,
        0,
        0);
    if (gSdlSurface == NULL) {
        destroyRenderer();

        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
    }

    SDL_Color colors[256];
    for (int index = 0; index < 256; index++) {
        colors[index].r = index;
        colors[index].g = index;
        colors[index].b = index;
        colors[index].a = 255;
    }

    SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);

    scr_size.ulx = 0;
    scr_size.uly = 0;
    scr_size.lrx = video_options->width - 1;
    scr_size.lry = video_options->height - 1;

    mouse_blit_trans = NULL;
    scr_blit = GNW95_ShowRect;
    mouse_blit = GNW95_ShowRect;

    return true;
}

void svga_exit()
{
    diagnostics_shutdown();
    destroyRenderer();

    if (gSdlWindow != NULL) {
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int screenGetWidth()
{
    // TODO: Make it on par with _xres;
    return rectGetWidth(&scr_size);
}

int screenGetHeight()
{
    // TODO: Make it on par with _yres.
    return rectGetHeight(&scr_size);
}

static bool createRenderer(int width, int height)
{
#ifdef __SWITCH__
    if (gRenderScaleQuality == RENDER_SCALE_QUALITY_BILINEAR) {
        return createGlBilinearRenderer(width, height);
    }
#endif

    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 0);
    if (gSdlRenderer == NULL) {
        return false;
    }

    if (gRenderScaleQuality != RENDER_SCALE_QUALITY_BILINEAR) {
        if (SDL_RenderSetLogicalSize(gSdlRenderer, width, height) != 0) {
            return false;
        }
    }

    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (gSdlTexture == NULL) {
        return false;
    }
    setTextureScaleQuality(gSdlTexture);

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, NULL, NULL, NULL) != 0) {
        return false;
    }

    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
    if (gSdlTextureSurface == NULL) {
        return false;
    }

    if (gRenderScaleQuality == RENDER_SCALE_QUALITY_BILINEAR) {
        if (!createBilinearRenderTarget(width, height, format)) {
            return false;
        }
    }

    return true;
}

static bool createBilinearRenderTarget(int sourceWidth, int sourceHeight, Uint32 format)
{
    int targetWidth = 0;
    int targetHeight = 0;
    if (SDL_GetRendererOutputSize(gSdlRenderer, &targetWidth, &targetHeight) != 0 || targetWidth <= 0 || targetHeight <= 0) {
        SDL_GetWindowSize(gSdlWindow, &targetWidth, &targetHeight);
    }

    if (targetWidth <= 0 || targetHeight <= 0) {
        targetWidth = sourceWidth;
        targetHeight = sourceHeight;
    }

    gSdlBilinearTexture = SDL_CreateTexture(gSdlRenderer, format, SDL_TEXTUREACCESS_STREAMING, targetWidth, targetHeight);
    if (gSdlBilinearTexture == NULL) {
        return false;
    }
    setTextureScaleQuality(gSdlBilinearTexture);

    gSdlBilinearTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, targetWidth, targetHeight, SDL_BITSPERPIXEL(format), format);
    if (gSdlBilinearTextureSurface == NULL) {
        return false;
    }

    buildBilinearScaleMap(sourceWidth, sourceHeight, targetWidth, targetHeight);
    markFullTextureDirty();

    return true;
}

static void destroyRenderer()
{
#ifdef __SWITCH__
    destroyGlRenderer();
#endif

    if (gSdlBilinearTextureSurface != NULL) {
        SDL_FreeSurface(gSdlBilinearTextureSurface);
        gSdlBilinearTextureSurface = NULL;
    }

    if (gSdlBilinearTexture != NULL) {
        SDL_DestroyTexture(gSdlBilinearTexture);
        gSdlBilinearTexture = NULL;
    }

    gBilinearSourceX.clear();
    gBilinearSourceY.clear();
    gBilinearFractionX.clear();
    gBilinearFractionY.clear();
    gBilinearDirtySourceRects.clear();
    gBilinearFullDirty = true;

    if (gSdlTextureSurface != NULL) {
        SDL_FreeSurface(gSdlTextureSurface);
        gSdlTextureSurface = NULL;
    }

    if (gSdlTexture != NULL) {
        SDL_DestroyTexture(gSdlTexture);
        gSdlTexture = NULL;
    }

    if (gSdlRenderer != NULL) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = NULL;
    }
}

void handleWindowSizeChanged()
{
    destroyRenderer();
    createRenderer(screenGetWidth(), screenGetHeight());
}

void renderPresent()
{
    diagnostics_on_present();

#ifdef __SWITCH__
    if (isGlBilinearRenderer()) {
        renderPresentGlBilinear();
        return;
    }
#endif

    SDL_RenderClear(gSdlRenderer);

    if (gRenderScaleQuality == RENDER_SCALE_QUALITY_BILINEAR && gSdlBilinearTexture != NULL && gSdlBilinearTextureSurface != NULL) {
        if (gBilinearFullDirty) {
            SDL_Rect targetRect = { 0, 0, gSdlBilinearTextureSurface->w, gSdlBilinearTextureSurface->h };
            scaleSurfaceBilinearRect(gSdlTextureSurface, gSdlBilinearTextureSurface, targetRect);
            SDL_UpdateTexture(gSdlBilinearTexture, NULL, gSdlBilinearTextureSurface->pixels, gSdlBilinearTextureSurface->pitch);
            gBilinearFullDirty = false;
            gBilinearDirtySourceRects.clear();
        } else {
            for (size_t index = 0; index < gBilinearDirtySourceRects.size(); index++) {
                SDL_Rect targetRect = sourceRectToBilinearTargetRect(gBilinearDirtySourceRects[index]);
                scaleSurfaceBilinearRect(gSdlTextureSurface, gSdlBilinearTextureSurface, targetRect);

                unsigned char* pixels = static_cast<unsigned char*>(gSdlBilinearTextureSurface->pixels)
                    + targetRect.y * gSdlBilinearTextureSurface->pitch
                    + targetRect.x * gSdlBilinearTextureSurface->format->BytesPerPixel;
                SDL_UpdateTexture(gSdlBilinearTexture, &targetRect, pixels, gSdlBilinearTextureSurface->pitch);
            }
            gBilinearDirtySourceRects.clear();
        }

        SDL_RenderCopy(gSdlRenderer, gSdlBilinearTexture, NULL, NULL);
    } else {
        SDL_UpdateTexture(gSdlTexture, NULL, gSdlTextureSurface->pixels, gSdlTextureSurface->pitch);
        SDL_RenderCopy(gSdlRenderer, gSdlTexture, NULL, NULL);
    }

    SDL_RenderPresent(gSdlRenderer);
}

void renderMarkDirtyRect(const SDL_Rect* rect)
{
    markTextureDirtyRect(rect);
}

static const char* getSdlScaleQualityHint(RenderScaleQuality scaleQuality)
{
    if (scaleQuality == RENDER_SCALE_QUALITY_LINEAR) {
        return "linear";
    }

    return "nearest";
}

static void setTextureScaleQuality(SDL_Texture* texture)
{
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_ScaleMode scaleMode = gRenderScaleQuality == RENDER_SCALE_QUALITY_LINEAR
        ? SDL_ScaleModeLinear
        : SDL_ScaleModeNearest;
    SDL_SetTextureScaleMode(texture, scaleMode);
#else
    (void)texture;
#endif
}

static void markFullTextureDirty()
{
    if (gRenderScaleQuality != RENDER_SCALE_QUALITY_BILINEAR) {
        return;
    }

    gBilinearFullDirty = true;
    gBilinearDirtySourceRects.clear();
}

static void markTextureDirtyRect(const SDL_Rect* rect)
{
    if (rect == NULL) {
        markFullTextureDirty();
        return;
    }

    markTextureDirtyRect(rect->x, rect->y, rect->w, rect->h);
}

static void markTextureDirtyRect(int x, int y, int width, int height)
{
    if (gRenderScaleQuality != RENDER_SCALE_QUALITY_BILINEAR || gSdlTextureSurface == NULL || gBilinearFullDirty) {
        return;
    }

    if (width <= 0 || height <= 0) {
        return;
    }

    SDL_Rect clipped;
    clipped.x = std::max(0, x - 1);
    clipped.y = std::max(0, y - 1);

    int right = std::min(gSdlTextureSurface->w, x + width + 1);
    int bottom = std::min(gSdlTextureSurface->h, y + height + 1);
    clipped.w = right - clipped.x;
    clipped.h = bottom - clipped.y;
    if (clipped.w <= 0 || clipped.h <= 0) {
        return;
    }

    if (clipped.w * clipped.h * 100 >= gSdlTextureSurface->w * gSdlTextureSurface->h * BILINEAR_DIRTY_FULL_AREA_PERCENT) {
        markFullTextureDirty();
        return;
    }

    for (size_t index = 0; index < gBilinearDirtySourceRects.size(); index++) {
        if (rectsTouchOrIntersect(gBilinearDirtySourceRects[index], clipped)) {
            SDL_Rect merged;
            SDL_UnionRect(&(gBilinearDirtySourceRects[index]), &clipped, &merged);
            gBilinearDirtySourceRects[index] = merged;
            return;
        }
    }

    if (gBilinearDirtySourceRects.size() >= BILINEAR_DIRTY_RECT_CAPACITY) {
        markFullTextureDirty();
        return;
    }

    gBilinearDirtySourceRects.push_back(clipped);
}

static bool rectsTouchOrIntersect(const SDL_Rect& a, const SDL_Rect& b)
{
    return a.x <= b.x + b.w
        && b.x <= a.x + a.w
        && a.y <= b.y + b.h
        && b.y <= a.y + a.h;
}

static SDL_Rect sourceRectToBilinearTargetRect(const SDL_Rect& sourceRect)
{
    SDL_Rect targetRect;
    targetRect.x = sourceRangeToTargetStart(sourceRect.x, gSdlTextureSurface->w, gSdlBilinearTextureSurface->w);
    targetRect.y = sourceRangeToTargetStart(sourceRect.y, gSdlTextureSurface->h, gSdlBilinearTextureSurface->h);

    int right = sourceRangeToTargetEnd(sourceRect.x + sourceRect.w - 1, gSdlTextureSurface->w, gSdlBilinearTextureSurface->w);
    int bottom = sourceRangeToTargetEnd(sourceRect.y + sourceRect.h - 1, gSdlTextureSurface->h, gSdlBilinearTextureSurface->h);

    targetRect.w = right - targetRect.x + 1;
    targetRect.h = bottom - targetRect.y + 1;

    return targetRect;
}

static int sourceRangeToTargetStart(int sourceStart, int sourceLength, int targetLength)
{
    if (sourceLength <= 1 || targetLength <= 1) {
        return 0;
    }

    int target = static_cast<int>((static_cast<int64_t>(sourceStart) * (targetLength - 1)) / (sourceLength - 1)) - 2;
    return std::max(0, target);
}

static int sourceRangeToTargetEnd(int sourceEnd, int sourceLength, int targetLength)
{
    if (sourceLength <= 1 || targetLength <= 1) {
        return targetLength - 1;
    }

    int target = static_cast<int>((static_cast<int64_t>(sourceEnd + 1) * (targetLength - 1) + sourceLength - 2) / (sourceLength - 1)) + 2;
    return std::min(targetLength - 1, target);
}

static void buildBilinearScaleAxis(int sourceLength, int targetLength, std::vector<int>& sourceIndexes, std::vector<int>& fractions)
{
    sourceIndexes.resize(targetLength);
    fractions.resize(targetLength);

    if (sourceLength <= 1 || targetLength <= 1) {
        for (int index = 0; index < targetLength; index++) {
            sourceIndexes[index] = 0;
            fractions[index] = 0;
        }
        return;
    }

    int maxSourceIndex = sourceLength - 1;
    int maxTargetIndex = targetLength - 1;
    bool upscaling = targetLength > sourceLength;

    for (int targetIndex = 0; targetIndex < targetLength; targetIndex++) {
        int64_t fixedSource = (static_cast<int64_t>(targetIndex) * maxSourceIndex << 16) / maxTargetIndex;
        int sourceIndex = static_cast<int>(fixedSource >> 16);
        int fraction = static_cast<int>(fixedSource & 0xFFFF);

        if (sourceIndex >= maxSourceIndex) {
            sourceIndex = maxSourceIndex;
            fraction = 0;
        } else if (upscaling) {
            fraction = sharpenBilinearFraction(fraction);
        }

        sourceIndexes[targetIndex] = sourceIndex;
        fractions[targetIndex] = fraction;
    }
}

static void buildBilinearScaleMap(int sourceWidth, int sourceHeight, int targetWidth, int targetHeight)
{
    buildBilinearScaleAxis(sourceWidth, targetWidth, gBilinearSourceX, gBilinearFractionX);
    buildBilinearScaleAxis(sourceHeight, targetHeight, gBilinearSourceY, gBilinearFractionY);
}

static void scaleSurfaceBilinearRect(SDL_Surface* source, SDL_Surface* target, const SDL_Rect& targetRect)
{
    if (source == NULL || target == NULL || source->format->BytesPerPixel != 4 || target->format->BytesPerPixel != 4) {
        return;
    }

    if (source->w == target->w && source->h == target->h) {
        SDL_BlitSurface(source, &targetRect, target, const_cast<SDL_Rect*>(&targetRect));
        return;
    }

    int targetRight = std::min(target->w, targetRect.x + targetRect.w);
    int targetBottom = std::min(target->h, targetRect.y + targetRect.h);

    for (int targetY = std::max(0, targetRect.y); targetY < targetBottom; targetY++) {
        int sourceY = gBilinearSourceY[targetY];
        int sourceY1 = sourceY < source->h - 1 ? sourceY + 1 : sourceY;
        int fractionY = gBilinearFractionY[targetY];

        const Uint32* sourceRow0 = reinterpret_cast<const Uint32*>(static_cast<const unsigned char*>(source->pixels) + sourceY * source->pitch);
        const Uint32* sourceRow1 = reinterpret_cast<const Uint32*>(static_cast<const unsigned char*>(source->pixels) + sourceY1 * source->pitch);
        Uint32* targetRow = reinterpret_cast<Uint32*>(static_cast<unsigned char*>(target->pixels) + targetY * target->pitch);

        for (int targetX = std::max(0, targetRect.x); targetX < targetRight; targetX++) {
            int sourceX = gBilinearSourceX[targetX];
            int sourceX1 = sourceX < source->w - 1 ? sourceX + 1 : sourceX;
            int fractionX = gBilinearFractionX[targetX];

            targetRow[targetX] = blendRgb888(
                sourceRow0[sourceX],
                sourceRow0[sourceX1],
                sourceRow1[sourceX],
                sourceRow1[sourceX1],
                fractionX,
                fractionY);
        }
    }
}

static int sharpenBilinearFraction(int fraction)
{
    int sharpened = (fraction - 32768) * 2 + 32768;
    if (sharpened < 0) {
        return 0;
    }

    if (sharpened > 65535) {
        return 65535;
    }

    return sharpened;
}

static int lerp8(int from, int to, int fraction)
{
    return from + (((to - from) * fraction + 32768) >> 16);
}

static Uint32 blendRgb888(Uint32 topLeft, Uint32 topRight, Uint32 bottomLeft, Uint32 bottomRight, int fractionX, int fractionY)
{
    int topRed = lerp8((topLeft >> 16) & 0xFF, (topRight >> 16) & 0xFF, fractionX);
    int topGreen = lerp8((topLeft >> 8) & 0xFF, (topRight >> 8) & 0xFF, fractionX);
    int topBlue = lerp8(topLeft & 0xFF, topRight & 0xFF, fractionX);

    int bottomRed = lerp8((bottomLeft >> 16) & 0xFF, (bottomRight >> 16) & 0xFF, fractionX);
    int bottomGreen = lerp8((bottomLeft >> 8) & 0xFF, (bottomRight >> 8) & 0xFF, fractionX);
    int bottomBlue = lerp8(bottomLeft & 0xFF, bottomRight & 0xFF, fractionX);

    int red = lerp8(topRed, bottomRed, fractionY);
    int green = lerp8(topGreen, bottomGreen, fractionY);
    int blue = lerp8(topBlue, bottomBlue, fractionY);

    return static_cast<Uint32>((red << 16) | (green << 8) | blue);
}

#ifdef __SWITCH__
static bool createGlBilinearRenderer(int width, int height)
{
    gSdlGlContext = SDL_GL_CreateContext(gSdlWindow);
    if (gSdlGlContext == NULL) {
        return false;
    }

    SDL_GL_SetSwapInterval(0);
    SDL_GL_GetDrawableSize(gSdlWindow, &gGlOutputWidth, &gGlOutputHeight);
    if (gGlOutputWidth <= 0 || gGlOutputHeight <= 0) {
        SDL_GetWindowSize(gSdlWindow, &gGlOutputWidth, &gGlOutputHeight);
    }
    if (gGlOutputWidth <= 0 || gGlOutputHeight <= 0) {
        gGlOutputWidth = width;
        gGlOutputHeight = height;
    }

    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGB888);
    if (gSdlTextureSurface == NULL) {
        return false;
    }

    static const char* vertexShaderSource =
        "attribute vec2 aPosition;\n"
        "void main() {\n"
        "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
        "}\n";

    static const char* fragmentShaderSource =
        "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
        "precision highp float;\n"
        "#else\n"
        "precision mediump float;\n"
        "#endif\n"
        "uniform sampler2D uTexture;\n"
        "uniform vec2 uSourceSize;\n"
        "uniform vec2 uOutputSize;\n"
        "void main() {\n"
        "    vec2 dst = vec2(gl_FragCoord.x - 0.5, uOutputSize.y - gl_FragCoord.y - 0.5);\n"
        "    vec2 src = dst * ((uSourceSize - vec2(1.0)) / (uOutputSize - vec2(1.0)));\n"
        "    vec2 base = floor(src);\n"
        "    vec2 fraction = clamp((fract(src) - vec2(0.5)) * 2.0 + vec2(0.5), 0.0, 1.0);\n"
        "    vec2 base1 = min(base + vec2(1.0), uSourceSize - vec2(1.0));\n"
        "    vec2 uv00 = (base + vec2(0.5)) / uSourceSize;\n"
        "    vec2 uv10 = (vec2(base1.x, base.y) + vec2(0.5)) / uSourceSize;\n"
        "    vec2 uv01 = (vec2(base.x, base1.y) + vec2(0.5)) / uSourceSize;\n"
        "    vec2 uv11 = (base1 + vec2(0.5)) / uSourceSize;\n"
        "    vec4 top = mix(texture2D(uTexture, uv00), texture2D(uTexture, uv10), fraction.x);\n"
        "    vec4 bottom = mix(texture2D(uTexture, uv01), texture2D(uTexture, uv11), fraction.x);\n"
        "    gl_FragColor = mix(top, bottom, fraction.y);\n"
        "}\n";

    GLuint vertexShader = compileGlShader(GL_VERTEX_SHADER, vertexShaderSource);
    if (vertexShader == 0) {
        return false;
    }

    GLuint fragmentShader = compileGlShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    if (fragmentShader == 0) {
        glDeleteShader(vertexShader);
        return false;
    }

    if (!linkGlProgram(vertexShader, fragmentShader)) {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return false;
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    gGlPositionAttribute = glGetAttribLocation(gGlProgram, "aPosition");
    gGlTextureUniform = glGetUniformLocation(gGlProgram, "uTexture");
    gGlSourceSizeUniform = glGetUniformLocation(gGlProgram, "uSourceSize");
    gGlOutputSizeUniform = glGetUniformLocation(gGlProgram, "uOutputSize");
    if (gGlPositionAttribute < 0 || gGlTextureUniform < 0 || gGlSourceSizeUniform < 0 || gGlOutputSizeUniform < 0) {
        return false;
    }

    glGenTextures(1, &gGlSourceTexture);
    glBindTexture(GL_TEXTURE_2D, gGlSourceTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

    const GLfloat vertices[] = {
        -1.0f, -1.0f,
        1.0f, -1.0f,
        -1.0f, 1.0f,
        1.0f, 1.0f,
    };
    glGenBuffers(1, &gGlVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, gGlVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    buildBilinearScaleMap(width, height, gGlOutputWidth, gGlOutputHeight);
    markFullTextureDirty();

    return glGetError() == GL_NO_ERROR;
}

static void destroyGlRenderer()
{
    if (gGlVertexBuffer != 0) {
        glDeleteBuffers(1, &gGlVertexBuffer);
        gGlVertexBuffer = 0;
    }

    if (gGlSourceTexture != 0) {
        glDeleteTextures(1, &gGlSourceTexture);
        gGlSourceTexture = 0;
    }

    if (gGlProgram != 0) {
        glDeleteProgram(gGlProgram);
        gGlProgram = 0;
    }

    if (gSdlGlContext != NULL) {
        SDL_GL_DeleteContext(gSdlGlContext);
        gSdlGlContext = NULL;
    }

    gGlPositionAttribute = -1;
    gGlTextureUniform = -1;
    gGlSourceSizeUniform = -1;
    gGlOutputSizeUniform = -1;
    gGlOutputWidth = 0;
    gGlOutputHeight = 0;
    gGlUploadBuffer.clear();
}

static GLuint compileGlShader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static bool linkGlProgram(GLuint vertexShader, GLuint fragmentShader)
{
    gGlProgram = glCreateProgram();
    if (gGlProgram == 0) {
        return false;
    }

    glAttachShader(gGlProgram, vertexShader);
    glAttachShader(gGlProgram, fragmentShader);
    glBindAttribLocation(gGlProgram, 0, "aPosition");
    glLinkProgram(gGlProgram);

    GLint linked = GL_FALSE;
    glGetProgramiv(gGlProgram, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(gGlProgram);
        gGlProgram = 0;
        return false;
    }

    return true;
}

static void uploadGlDirtyRects()
{
    if (gBilinearFullDirty) {
        SDL_Rect fullRect = { 0, 0, gSdlTextureSurface->w, gSdlTextureSurface->h };
        uploadGlSourceRect(fullRect);
        gBilinearFullDirty = false;
        gBilinearDirtySourceRects.clear();
        return;
    }

    for (size_t index = 0; index < gBilinearDirtySourceRects.size(); index++) {
        uploadGlSourceRect(gBilinearDirtySourceRects[index]);
    }

    gBilinearDirtySourceRects.clear();
}

static void uploadGlSourceRect(const SDL_Rect& sourceRect)
{
    int bytesPerPixel = gSdlTextureSurface->format->BytesPerPixel;
    int packedPitch = sourceRect.w * 3;
    gGlUploadBuffer.resize(static_cast<size_t>(packedPitch) * sourceRect.h);

    for (int y = 0; y < sourceRect.h; y++) {
        const Uint32* sourceRow = reinterpret_cast<const Uint32*>(
            static_cast<const unsigned char*>(gSdlTextureSurface->pixels)
            + (sourceRect.y + y) * gSdlTextureSurface->pitch
            + sourceRect.x * bytesPerPixel);
        unsigned char* destRow = gGlUploadBuffer.data() + y * packedPitch;

        for (int x = 0; x < sourceRect.w; x++) {
            Uint32 pixel = sourceRow[x];
            destRow[x * 3] = static_cast<unsigned char>((pixel >> 16) & 0xFF);
            destRow[x * 3 + 1] = static_cast<unsigned char>((pixel >> 8) & 0xFF);
            destRow[x * 3 + 2] = static_cast<unsigned char>(pixel & 0xFF);
        }
    }

    glBindTexture(GL_TEXTURE_2D, gGlSourceTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, sourceRect.x, sourceRect.y, sourceRect.w, sourceRect.h, GL_RGB, GL_UNSIGNED_BYTE, gGlUploadBuffer.data());
}

static void renderPresentGlBilinear()
{
    uploadGlDirtyRects();

    glViewport(0, 0, gGlOutputWidth, gGlOutputHeight);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(gGlProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gGlSourceTexture);
    glUniform1i(gGlTextureUniform, 0);
    glUniform2f(gGlSourceSizeUniform, static_cast<GLfloat>(gSdlTextureSurface->w), static_cast<GLfloat>(gSdlTextureSurface->h));
    glUniform2f(gGlOutputSizeUniform, static_cast<GLfloat>(gGlOutputWidth), static_cast<GLfloat>(gGlOutputHeight));

    glBindBuffer(GL_ARRAY_BUFFER, gGlVertexBuffer);
    glEnableVertexAttribArray(static_cast<GLuint>(gGlPositionAttribute));
    glVertexAttribPointer(static_cast<GLuint>(gGlPositionAttribute), 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(static_cast<GLuint>(gGlPositionAttribute));

    SDL_GL_SwapWindow(gSdlWindow);
}

static bool isGlBilinearRenderer()
{
    return gSdlGlContext != NULL;
}
#endif

} // namespace fallout
