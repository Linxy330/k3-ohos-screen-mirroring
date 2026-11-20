#include "focused_text.h"
#include "accessibility_constants.h"
#include "accessibility_ui_test_ability.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace k3::control {
namespace {
namespace accessibility = OHOS::Accessibility;

class FocusListener final : public accessibility::AccessibleAbilityListener {
public:
    void OnAbilityConnected() override { setConnected(true); }
    void OnAbilityDisconnected() override { setConnected(false); }
    void OnAccessibilityEvent(const accessibility::AccessibilityEventInfo&) override {}
    bool OnKeyPressEvent(const std::shared_ptr<OHOS::MMI::KeyEvent>&) override { return false; }

    bool waitFor(bool value) {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(3), [&] { return connected == value; });
    }

private:
    void setConnected(bool value) {
        std::lock_guard<std::mutex> lock(mutex);
        connected = value;
        condition.notify_all();
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool connected = false;
};

bool insertAtCursor(accessibility::AccessibilityUITestAbility& client, const std::string& text) {
    accessibility::AccessibilityElementInfo field;
    if (client.GetFocus(accessibility::FOCUS_TYPE_INPUT, field) != accessibility::RET_OK ||
        !field.IsFocused() || !field.IsEditable()) return false;
    accessibility::AccessibilityWindowInfo window;
    if (client.GetWindow(field.GetWindowId(), window) != accessibility::RET_OK ||
        window.GetDisplayId() != 0 || !window.IsFocused()) return false;
    // Standard ArkUI fields route this action to InsertValue, preserving the
    // caret and selection. Custom implementations may instead replace a document.
    const auto& type = field.GetComponentType();
    if (type != "TextInput" && type != "TextArea" && type != "Search" && type != "TextField") return false;
    const auto& actions = field.GetActionList();
    const auto setText = accessibility::ACCESSIBILITY_ACTION_SET_TEXT;
    if (std::none_of(actions.begin(), actions.end(), [setText](const auto& action) {
        return action.GetActionType() == setText;
    })) return false;
    return client.ExecuteAction(field, setText, {{accessibility::ACTION_ARGU_SET_TEXT, text}}) ==
        accessibility::RET_OK;
}
}

int spawnTextInput(pid_t* child, const std::string& text) {
    // An anonymous file keeps submitted text out of process arguments and storage.
    const int fd = memfd_create("hongjing-text", MFD_CLOEXEC);
    if (fd < 0) return errno;
    int error = 0;
    size_t at = 0;
    while (at < text.size()) {
        const auto n = write(fd, text.data() + at, text.size() - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = n == 0 ? EIO : errno; break; }
        at += n;
    }
    if (!error && lseek(fd, 0, SEEK_SET) < 0) error = errno;
    posix_spawn_file_actions_t actions;
    if (!error) {
        error = posix_spawn_file_actions_init(&actions);
        if (!error) {
            error = posix_spawn_file_actions_adddup2(&actions, fd, STDIN_FILENO);
            if (!error) error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
            if (!error) error = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
            const char* args[] = {"k3-text-input", "--insert-focused-text", nullptr};
            if (!error) error = posix_spawn(child, "/proc/self/exe", &actions, nullptr,
                                            const_cast<char* const*>(args), environ);
            posix_spawn_file_actions_destroy(&actions);
        }
    }
    close(fd);
    return error;
}

int insertTextFromStdin() {
    struct stat payload {};
    if (fstat(STDIN_FILENO, &payload) < 0 || payload.st_size <= 0 || payload.st_size > 65535) return 2;
    std::string text(static_cast<size_t>(payload.st_size), '\0');
    if (fread(text.data(), 1, text.size(), stdin) != text.size()) return 2;

    auto client = accessibility::AccessibilityUITestAbility::GetInstance();
    auto listener = std::make_shared<FocusListener>();
    if (!client || client->RegisterAbilityListener(listener) != accessibility::RET_OK ||
        client->Connect() != accessibility::RET_OK) return 3;
    const bool inserted = listener->waitFor(true) && insertAtCursor(*client, text);
    if (client->Disconnect() == accessibility::RET_OK) listener->waitFor(false);
    return inserted ? 0 : 4;
}
}
