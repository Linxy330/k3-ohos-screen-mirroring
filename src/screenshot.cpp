#include "screen_geometry.h"
#include "screenshot.h"
#include "display_manager.h"
#include "screen_manager.h"
#include <cstdio>
#include <vector>

bool getK3Desktop(uint64_t &displayId)
{
    using namespace OHOS::Rosen;
    auto d = DisplayManager::GetInstance().GetDefaultDisplaySync();
    if (d == nullptr || d->GetWidth() != k3::screen::width || d->GetHeight() != k3::screen::height) return false;
    auto s = ScreenManager::GetInstance().GetScreenById(d->GetScreenId());
    if (s == nullptr || s->GetName() != "K3 USB desktop") return false;
    displayId = d->GetId();
    return true;
}

bool k3DisplayReadable(uint64_t id)
{
    bool privateWindow = true;
    return OHOS::Rosen::DisplayManager::GetInstance().HasPrivateWindow(id, privateWindow) ==
        OHOS::Rosen::DMError::DM_OK && !privateWindow;
}

const uint8_t* k3BlackRgba()
{
    static const std::vector<uint8_t> black = [] {
        std::vector<uint8_t> rgba(size_t(k3::screen::width) * k3::screen::height * 4, 0);
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        return rgba;
    }();
    return black.data();
}

static bool privateFrame(CaptureFrame& frame)
{
    frame = {nullptr, k3BlackRgba(), k3::screen::width * 4, 4, false};
    return true;
}

bool captureK3(uint64_t displayId, CaptureFrame &frame)
{
    using namespace OHOS::Rosen;
    using OHOS::Media::PixelFormat;
    if (!k3DisplayReadable(displayId)) return privateFrame(frame);
    DmErrorCode error = DmErrorCode::DM_OK;
    auto p = DisplayManager::GetInstance().GetScreenshot(displayId, &error, false);
    if (!k3DisplayReadable(displayId)) return privateFrame(frame);
    if (!p || error != DmErrorCode::DM_OK || p->GetWidth() != k3::screen::width || p->GetHeight() != k3::screen::height) {
        std::fprintf(stderr, "invalid screenshot error=%d\n", int(error));
        return false;
    }
    auto fmt = p->GetPixelFormat();
    if (fmt != PixelFormat::RGBA_8888 && fmt != PixelFormat::BGRA_8888 && fmt != PixelFormat::RGB_888) {
        std::fprintf(stderr, "unsupported pixel format %d\n", int(fmt));
        return false;
    }
    int bpp = fmt == PixelFormat::RGB_888 ? 3 : 4;
    int stride = p->GetRowStride();
    if (!stride) stride = p->GetRowBytes();
    auto *bytes = p->GetPixels();
    if (!bytes || stride < k3::screen::width * bpp || (p->GetCapacity() != 0 && p->GetCapacity() < (k3::screen::height - 1u) * stride + k3::screen::width * static_cast<unsigned>(bpp))) {
        std::fprintf(stderr, "invalid image layout stride=%d capacity=%u\n", stride, p->GetCapacity());
        return false;
    }
    frame = {std::move(p), bytes, stride, bpp, fmt == PixelFormat::BGRA_8888};
    return true;
}
