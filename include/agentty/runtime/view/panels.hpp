#pragma once
// The panel views — one function per panel, named <name>_panel, defined in
// src/runtime/view/panels/<name>.cpp. Uniform rule: the panel's name (the
// same word as its state header, reducer file, slot alternative and Msg
// family) + _panel. view.cpp's pick_panel() is the only caller for most.
#include "agentty/runtime/model.hpp"

#include <maya/element/element.hpp>

namespace agentty::ui {

[[nodiscard]] maya::Element models_panel(const Model& m);
[[nodiscard]] maya::Element providers_panel(const Model& m);
[[nodiscard]] maya::Element thread_list_panel(const Model& m);
[[nodiscard]] maya::Element smart_mode_panel(const Model& m);
[[nodiscard]] maya::Element palette_panel(const Model& m);
[[nodiscard]] maya::Element mention_panel(const Model& m);
[[nodiscard]] maya::Element symbol_panel(const Model& m);
[[nodiscard]] maya::Element code_blocks_panel(const Model& m);
[[nodiscard]] maya::Element code_block_result_panel(const Model& m);
[[nodiscard]] maya::Element tool_output_panel(const Model& m);
[[nodiscard]] maya::Element checkpoints_panel(const Model& m);
[[nodiscard]] maya::Element rag_panel(const Model& m);
[[nodiscard]] maya::Element settings_list_panel(const Model& m);
[[nodiscard]] maya::Element fork_panel(const Model& m);
[[nodiscard]] maya::Element todo_panel(const Model& m);

} // namespace agentty::ui
