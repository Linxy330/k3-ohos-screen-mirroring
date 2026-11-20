#pragma once
#include <string>
#include <sys/types.h>

namespace k3::control {
int spawnTextInput(pid_t* child, const std::string& text);
int insertTextFromStdin();
}
