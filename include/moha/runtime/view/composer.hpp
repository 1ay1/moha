#pragma once
#include <maya/widget/composer.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::Composer::Config composer_config(const Model& m);

} // namespace moha::ui
