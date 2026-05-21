// MCP Prompts primitive — slash-command UX in Claude Code / Cursor / etc.
//
// MCP semantics:
//   - prompts/list: enumerates available prompts with their schemas
//   - prompts/get: takes a name + arguments, returns a messages array
//                  the client feeds to the LLM as a conversation prefix
//
// Each PromptDescriptor declares its name, description, and required
// arguments. The handler receives the substituted args as JSON and
// returns the rendered messages array (typically a single `user`
// message containing the slash command's prompt text).
//
// We ship 8 built-in prompts — see RegisterBuiltinPrompts in Prompts.cpp.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace bpr::tools::prompts {

struct PromptArgument {
	std::string name;
	std::string description;
	bool        required = false;
};

struct PromptDescriptor {
	std::string                 name;
	std::string                 description;
	std::vector<PromptArgument> arguments;
};

// Handler signature: receives the JSON arguments object (per the
// declared `arguments`) and returns the rendered messages array.
// Throws std::invalid_argument on missing required args (the
// dispatcher converts that to a JSON-RPC error).
using PromptFn = std::function<nlohmann::json(const nlohmann::json&)>;

class PromptRegistry {
public:
	void Register(PromptDescriptor desc, PromptFn fn);

	// Spec for prompts/list — array of {name, description, arguments[]}.
	nlohmann::json ListSpec() const;

	// Render a prompt by name. Returns the prompts/get result body:
	//   { description, messages: [{role, content: {type:"text", text:"..."}}] }
	// Throws std::invalid_argument when the name doesn't exist or when
	// the handler reports a required-arg violation.
	nlohmann::json Render(const std::string& name,
						   const nlohmann::json& arguments) const;

	bool        Has(const std::string& name) const;
	std::size_t Size() const;

private:
	std::vector<PromptDescriptor>                            descriptors_;
	std::unordered_map<std::string, PromptFn>                fns_;
};

// Convenience helper for handlers: build a single user message with
// the given text. Wraps the {role, content:{type:"text", text:...}}
// shape so call sites stay readable.
nlohmann::json UserMessage(std::string text);

// Helper to extract a required string argument; throws when missing
// with a message naming the prompt and arg.
std::string RequirePromptArg(const nlohmann::json& args,
							  std::string_view promptName,
							  std::string_view argName);

// Same, but returns a default when the arg is missing or null.
std::string OptionalPromptArg(const nlohmann::json& args,
							   std::string_view argName,
							   std::string fallback = {});

// Register the 8 built-in prompts on `registry`. See Prompts.cpp for
// the prompt bodies. Idempotent: re-registering overwrites prior
// entries with the same name.
void RegisterBuiltinPrompts(PromptRegistry& registry);

}    // namespace bpr::tools::prompts
