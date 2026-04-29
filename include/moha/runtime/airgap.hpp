#pragma once
// `moha airgap` — run moha on an air-gapped host through an SSH reverse
// tunnel.  See README and the implementation for the full UX; the contract
// here is: parse subcommand args, optionally copy credentials, then exec
// into ssh.  On success the process is replaced by ssh and never returns.

namespace moha::airgap {

// `argc`/`argv` start at the args *after* the `airgap` subcommand token,
// so argv[0] is the first user-supplied flag/positional.
int cmd_airgap(int argc, char** argv);

} // namespace moha::airgap
