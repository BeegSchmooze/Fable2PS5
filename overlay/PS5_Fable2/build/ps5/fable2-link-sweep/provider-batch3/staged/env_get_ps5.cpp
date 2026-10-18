
#include <rex/platform/env.h>
#include <cstdlib>
#include <string>

namespace rex::platform::env {

std::optional<std::string> get(std::string_view name) {
    const std::string key(name);
    const char* value = std::getenv(key.c_str());

    if (!value) {
        return std::nullopt;
    }

    return std::string(value);
}

}
