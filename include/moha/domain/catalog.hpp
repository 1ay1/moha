#pragma once
// moha catalog — describes an LLM the user can select.

#include <string>

#include "moha/domain/id.hpp"

namespace moha {

struct ModelInfo {
    ModelId     id;
    std::string display_name;
    std::string provider;
    int  context_window = 200000;
    bool favorite       = false;
};

} // namespace moha
