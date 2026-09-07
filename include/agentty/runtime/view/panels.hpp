#pragma once
#include <maya/maya.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::Element fused_picker(const Model& m);
[[nodiscard]] maya::Element provider_picker(const Model& m);
[[nodiscard]] maya::Element thread_list(const Model& m);
[[nodiscard]] maya::Element smart_mode_panel(const Model& m);
[[nodiscard]] maya::Element command_palette(const Model& m);
[[nodiscard]] maya::Element mention(const Model& m);
[[nodiscard]] maya::Element symbol(const Model& m);
[[nodiscard]] maya::Element code_blocks(const Model& m);
[[nodiscard]] maya::Element code_block_result_card(const Model& m);
[[nodiscard]] maya::Element tool_output_viewer(const Model& m);
[[nodiscard]] maya::Element checkpoints(const Model& m);
[[nodiscard]] maya::Element rag_settings_picker(const Model& m);
[[nodiscard]] maya::Element settings_list_picker(const Model& m);
[[nodiscard]] maya::Element fork_picker_view(const Model& m);
[[nodiscard]] maya::Element todo_modal(const Model& m);

} // namespace agentty::ui
