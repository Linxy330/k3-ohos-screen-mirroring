#pragma once
#include <cstdint>
#include <memory>
#include "pixel_map.h"

struct CaptureFrame {
    std::shared_ptr<OHOS::Media::PixelMap> pixels;
    const uint8_t *data;
    int stride;
    int bytesPerPixel;
    bool blueFirst;
};
bool getK3Desktop(uint64_t &displayId);
bool k3DisplayReadable(uint64_t displayId);
const uint8_t* k3BlackRgba();
bool captureK3(uint64_t displayId, CaptureFrame &frame);
void convertNV12(const CaptureFrame &f, uint8_t *y, uint8_t *uv, int stride);
