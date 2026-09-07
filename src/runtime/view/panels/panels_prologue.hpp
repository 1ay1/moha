#pragma once
// Shared prologue for the split picker view files (model_picker.cpp,
// nav_pickers.cpp, tool_pickers.cpp, misc_pickers.cpp).
//
// pickers.cpp used to be one TU with a single big include block feeding every
// picker. When the views were split into sibling files they all still need
// that same set of headers (maya widgets, the provider/auth surface, workspace
// lookups) plus the `namespace ov` alias and the shared picker_detail helpers
// in scope. Rather than copy 40 include lines into four files (and risk them
// drifting), each split file includes THIS once. Over-including is harmless;
// the win is one place to edit when the dependency set changes.

#include "agentty/runtime/view/panels.hpp"
#include "panels_common.hpp"
#include "agentty/domain/smart_mode.hpp"   // kSmartModeRows

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <variant>
#include <vector>

#include <maya/widget/panel.hpp>
#include <maya/widget/plan_view.hpp>
#include <maya/widget/tool_body_preview.hpp>
#include <maya/platform/io.hpp>

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/auth/vault.hpp"   // vault::signed_in — uniform OAuth status
#include "agentty/runtime/view/hints.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/panel/code_blocks.hpp"  // extract_code_blocks (palette gating)
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/kimi/kimi_oauth.hpp"
#include "agentty/runtime/provider_rows.hpp"
#include "agentty/runtime/fused_models.hpp"
#include "agentty/provider/auth_state.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/runtime/app/deps.hpp"   // deps().auth for the live auth badge
#include "agentty/auth/auth.hpp"          // auth::is_empty
#include "agentty/workspace/files.hpp"
#include "agentty/workspace/symbols.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;

// The shared picker helpers live in pickers_common.hpp; pull the ones the
// split files use into scope so call sites read exactly as they did in the
// original single-TU pickers.cpp.
using picker_detail::split_name_dir;
using picker_detail::parent_segment;
using picker_detail::kViewportH;
using picker_detail::kPanelNarrow;
using picker_detail::kPanelStandard;
using picker_detail::kPanelWide;
using picker_detail::kPickerChromeRows;
using picker_detail::picker_terminal_rows;
using picker_detail::picker_viewport_h;
using picker_detail::picker_terminal_cols;
using picker_detail::picker_badge_max_cols;
using picker_detail::tier_hue;
using picker_detail::active_provider_id;
using picker_detail::reasoning_effort_footer;
using picker_detail::section_header;
using picker_detail::SectionHeader;

}  // namespace agentty::ui
