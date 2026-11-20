#pragma once

namespace k3::screen {
// Both the virtual screen and all consumers are built for the same native mode.
#ifdef K3_FULL_HD
inline constexpr int width = 1920;
inline constexpr int height = 1080;
inline constexpr const char *ffmpegSize = "1920x1080";
#else
inline constexpr int width = 1280;
inline constexpr int height = 720;
inline constexpr const char *ffmpegSize = "1280x720";
#endif
}
