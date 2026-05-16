// Setup-state diagnostics.
//
// One canonical place that knows what a "correctly-configured" bp-reader
// install looks like, so:
//   * the server logs actionable warnings on startup instead of silent
//     failures (the historical pain point — three different failure modes
//     all produced "daemon exited" or 60s-timeout symptoms with no hint at
//     the actual problem)
//   * the `doctor` subcommand can run the same checks standalone
//   * the `config` subcommand uses the same auto-discovery to suggest
//     paths
//
// Each check produces a Finding with severity + message + (optional) fix
// hint. Callers decide how to render — startup logs print them, doctor
// returns an exit code based on highest severity.
#pragma once

#include "backends/BackendFactory.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace bpr::diag {

enum class Severity {
	Ok,       // green — found and healthy
	Info,     // blue — auto-discovered, etc. (informational)
	Warning,  // yellow — likely-fine-but-suspicious
	Error,    // red — will not work
};

struct Finding {
	Severity severity = Severity::Ok;
	std::string label;     // short summary line
	std::string detail;    // longer context (file path, etc.)
	std::string fix_hint;  // optional: copy-paste-able next step
};

struct Report {
	std::vector<Finding> findings;
	bool HasError() const;
	bool HasWarning() const;
};

// Run every check that doesn't require driving the daemon (synchronous,
// fast, ~100 ms). Useful from `doctor`, from `--show-config`, and from the
// server's startup banner path.
Report RunSetupChecks(const backends::BackendConfig& cfg);

// Pretty-print to a stream with ANSI colors when `colors=true`.
void PrintReport(const Report& report, std::ostream& out, bool colors = true);

} // namespace bpr::diag
