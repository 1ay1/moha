#pragma once
// Anthropic-specific OAuth wiring.  Every constant here identifies moha to
// Anthropic's edge — changing any of them will break the flow, so they're
// kept in one provider-scoped place instead of leaking into `moha::auth`.

namespace moha::provider::anthropic {

struct OAuthConfig {
    // Claude Code's public client ID — using it is what lets us ride a
    // user's existing Claude.ai OAuth session. See
    // memory/project_claude_code_wire.md for the rationale.
    static constexpr const char* client_id     = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
    static constexpr const char* authorize_url = "https://claude.ai/oauth/authorize";
    static constexpr const char* token_url     = "https://platform.claude.com/v1/oauth/token";
    static constexpr const char* redirect_uri  = "https://platform.claude.com/oauth/code/callback";
    static constexpr const char* scopes =
        "user:profile user:inference user:sessions:claude_code "
        "user:mcp_servers user:file_upload";
};

} // namespace moha::provider::anthropic
