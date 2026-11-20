#include "screen_geometry.h"
#include "touch_backend.h"

#include "input_manager.h"
#include "pointer_event.h"

#include <ctime>

namespace k3 {
bool injectTouch(TouchAction action, int x, int y, int64_t downTimeUs)
{
    using OHOS::MMI::InputManager;
    using OHOS::MMI::PointerEvent;
    if (x < 0 || y < 0 || x >= k3::screen::width || y >= k3::screen::height || downTimeUs <= 0) return false;

    int eventAction;
    switch (action) {
        case TouchAction::Down: eventAction = PointerEvent::POINTER_ACTION_DOWN; break;
        case TouchAction::Move: eventAction = PointerEvent::POINTER_ACTION_MOVE; break;
        case TouchAction::Up: eventAction = PointerEvent::POINTER_ACTION_UP; break;
        case TouchAction::Cancel: eventAction = PointerEvent::POINTER_ACTION_CANCEL; break;
        default: return false;
    }

    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return false;
    const int64_t timeUs = static_cast<int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
    if (timeUs < downTimeUs) return false;
    auto* manager = InputManager::GetInstance();
    if (!manager) return false;
    auto event = PointerEvent::Create();
    if (!event) return false;

    constexpr int finger = 0;
    PointerEvent::PointerItem item;
    item.SetPointerId(finger);
    item.SetDownTime(downTimeUs);
    item.SetPressed(action == TouchAction::Down || action == TouchAction::Move);
    item.SetToolType(PointerEvent::TOOL_TYPE_FINGER);
    item.SetDisplayX(x);
    item.SetDisplayY(y);
    item.SetRawDisplayX(x);
    item.SetRawDisplayY(y);
    event->SetSourceType(PointerEvent::SOURCE_TYPE_TOUCHSCREEN);
    event->SetPointerId(finger);
    event->SetTargetDisplayId(0);
    event->SetActionStartTime(downTimeUs);
    event->SetActionTime(timeUs);
    event->SetPointerAction(eventAction);
    event->AddPointerItem(item);
    manager->SimulateInputEvent(event);
    return true;
}
}
