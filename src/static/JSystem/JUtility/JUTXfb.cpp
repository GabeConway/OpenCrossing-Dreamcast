#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include "JSystem/JUtility/JUTAssertion.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JUtility/JUTVideo.h"
#include "JSystem/JUtility/JUTXfb.h"

JUTXfb* JUTXfb::sManager;

void JUTXfb::clearIndex() {
    mDrawingXfbIndex = -1;
    mDrawnXfbIndex = -1;
    mDisplayingXfbIndex = -1;
}

void JUTXfb::common_init(int xfbNum) {
    mBufferNum = xfbNum;
    clearIndex();
    mSDrawingFlag = 99;
}

JUTXfb::JUTXfb(const GXRenderModeObj* rmode, JKRHeap* heap, JUTXfb::EXfbNumber number) {
    common_init(number);

    if (rmode) {
        initiate(rmode->fbWidth, rmode->xfbHeight, heap, number);
    } else {
        u16 efbWidth = JUTVideo::getManager()->getRenderMode()->fbWidth;
        u16 xfbHeight = JUTVideo::getManager()->getRenderMode()->xfbHeight;
        u16 efbHeight = JUTVideo::getManager()->getRenderMode()->efbHeight;

        initiate(efbWidth, xfbHeight, heap, number);
    }
}

JUTXfb::~JUTXfb() {
    for (int i = 0; i < 3; i++) {
        delXfb(i);
    }
    sManager = nullptr;
}

void JUTXfb::delXfb(int xfbIdx) {
    if (mXfbAllocated[xfbIdx] && mBuffer[xfbIdx]) {
        delete mBuffer[xfbIdx];
    }
}

JUTXfb* JUTXfb::createManager(const GXRenderModeObj* rmode, JKRHeap* heap, JUTXfb::EXfbNumber number) {
    JUT_CONFIRM_MESSAGE(sManager == 0);
    if (sManager == nullptr) {
        sManager = new JUTXfb(rmode, heap, number);
    }
    return sManager;
}

void JUTXfb::destroyManager() {
    JUT_CONFIRM_MESSAGE(sManager);
    delete sManager;
    sManager = nullptr;
}

void JUTXfb::initiate(u16 w, u16 h, JKRHeap* heap, JUTXfb::EXfbNumber number) {
    if (heap == nullptr) {
        heap = JKRGetSystemHeap();
    }

    u32 size = (u16)ALIGN_NEXT((u16)w, 16) * h;

#if defined(TARGET_DC)
    /* Dreamcast: the external framebuffers are allocated, zeroed and never
     * read. Every consumer of these pointers terminates at
     * VISetNextFrameBuffer() (dc/src/dc_vi.c), GXCopyDisp() (dc/src/dc_gx.c,
     * ignores `dest`) or JUTChangeFrameBuffer() -- the PVR owns the real
     * framebuffer and it lives in VRAM. At GXNtsc480IntDf that is
     * 2 * 640 * 480 * 2 = 1,228,800 B of system heap for nothing.
     *
     * NULL is a state the code already handles by construction: SingleBuffer
     * mode leaves mBuffer[1]/[2] NULL, JUTXfb::getDrawnXfb() already returns
     * nullptr when the index is negative, and JUTDirectPrint's constructor
     * calls changeFrameBuffer(nullptr, 0, 0) -- so a NULL frame memory simply
     * disables direct print, which cannot work on DC anyway. The buffer
     * *indices* (which drive JFWDisplay's rotation) are untouched.
     * kb/research-budget-premises.md 2.2(a). */
    (void)size;
    (void)number;
    mBuffer[0] = nullptr;
    mBuffer[1] = nullptr;
    mBuffer[2] = nullptr;
    mXfbAllocated[0] = false;
    mXfbAllocated[1] = false;
    mXfbAllocated[2] = false;
    return;
#else
    mBuffer[0] = new (heap, 32) u16[size];
    mXfbAllocated[0] = true;
    if (number >= DoubleBuffer) {
        mBuffer[1] = new (heap, 32) u16[size];
        mXfbAllocated[1] = true;
    } else {
        mBuffer[1] = nullptr;
        mXfbAllocated[1] = false;
    }

    if (number >= TripleBuffer) {
        mBuffer[2] = new (heap, 32) u16[size];
        mXfbAllocated[2] = true;
    } else {
        mBuffer[2] = nullptr;
        mXfbAllocated[2] = false;
    }
#endif
}

u32 JUTXfb::accumeXfbSize() {
    JUTVideo* video = JUTVideo::getManager();
    u16 height = video->getXfbHeight();
    u16 width = video->getFbWidth();
    return (u16)ALIGN_NEXT(width, 16) * height * 2;
}
