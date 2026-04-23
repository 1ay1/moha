# Streaming state machine

`StreamState` (see `include/moha/domain/session.hpp`) owns the per-turn
lifetime of a single in-flight LLM request.  It is pure domain (no sockets,
no curl) but strictly more volatile than the `Thread` / `Message` types:
`phase` flips on every delta and the cancel handle tears down the live
HTTP/2 stream.

## Phase

`Phase` is a `std::variant` of four empty structs — `Idle`, `Streaming`,
`AwaitingPermission`, `ExecutingTool`.  Modeling it as a variant rather
than an `enum class` lets us express "exactly one of these is true" in
the type system and get exhaustiveness via `std::visit` (instead of a
switch warning).

## Truncation retries

`truncation_retries` counts how many times the current user turn has been
transparently retried because the upstream stream cut off mid-tool-input.
Reset on every fresh user submit.  Capped (see `kMaxTruncationRetries`
in `update.cpp`) so a persistently broken upstream surfaces as a real
error eventually instead of looping forever.

## Live tok/s speedometer

Anthropic only emits `message_delta.usage.output_tokens` rarely — often
just once, right before `message_stop` — so the official `tokens_out`
field is stale for nearly the whole stream.

Instead we accumulate the byte length of every text/json delta as it
arrives (`live_delta_bytes`) and divide by ~4 (the Claude tokenizer
averages ~3.5–4 bytes per token) to get the live rate.

`first_delta_at` is stamped on the first non-empty delta so the divisor
excludes time-to-first-token (TTFT).  Both reset on every
`StreamStarted` so each sub-turn after a tool exec measures cleanly.

## Sparkline ring buffer

`rate_history` is a cheap fixed-size ring buffer (`kRateSamples = 12`,
sampled every ~500 ms) that the `Tick` handler appends to.  The status
bar renders those samples as a row of ▁▂▃▄▅▆▇█ glyphs next to the
numeric rate, giving the user:

- a visual "wire is alive" cue, and
- a glance-readable trend (rising / steady / falling) without parsing
  numbers.

`rate_history_pos` and `rate_history_full` track ring-buffer state;
`rate_last_sample_at` and `rate_last_sample_bytes` are the previous
sample point used to compute the next delta.  Reset on every
`StreamStarted` so each sub-turn measures its own pace.

## Cancel handle

`cancel` is a `shared_ptr<http::CancelToken>` for the in-flight HTTP/2
stream — set when `launch_stream` dispatches the worker, nulled when the
terminal `Msg` lands.  Tripping it from the UI thread
(`Msg::CancelStream`) tears the stream down within a few hundred ms.
