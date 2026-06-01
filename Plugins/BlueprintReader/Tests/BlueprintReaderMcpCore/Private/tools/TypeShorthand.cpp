#include "tools/TypeShorthand.h"

#include <fmt/core.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bpr::tools {

namespace type_shorthand_detail {

// Map a leading <name> to canonical (Category, SubCategory). UE5.7 uses
// "real" as the float category; we accept the human-friendly "float" /
// "double" aliases too. Anything that takes a sub-ref returns an empty
// SubCategory and the caller fills SubCategoryObject from after the colon.
struct Resolved {
	std::string category;
	std::string subCategory;
	bool needsSubRef = false;  // true => rest of string is a class/struct/enum ref
};

// Returns nullopt if `name` isn't a recognized shorthand element name.
std::optional<Resolved> ResolveName(std::string_view name) {
	// Aliases first.
	if (name == "bool")     return Resolved{"bool",  "",       false};
	if (name == "byte")     return Resolved{"byte",  "",       false};
	if (name == "int")      return Resolved{"int",   "",       false};
	if (name == "int64")    return Resolved{"int64", "",       false};
	if (name == "float")    return Resolved{"real",  "float",  false};
	if (name == "real")     return Resolved{"real",  "float",  false};
	if (name == "double")   return Resolved{"real",  "double", false};
	if (name == "string")   return Resolved{"string","",       false};
	if (name == "name")     return Resolved{"name",  "",       false};
	if (name == "text")     return Resolved{"text",  "",       false};
	if (name == "exec")     return Resolved{"exec",  "",       false};
	// Sub-ref carriers — caller must supply the colon-suffix.
	if (name == "object")    return Resolved{"object",    "", true};
	if (name == "class")     return Resolved{"class",     "", true};
	if (name == "soft_object") return Resolved{"softobject","", true};
	if (name == "soft_class")  return Resolved{"softclass", "", true};
	if (name == "interface") return Resolved{"interface", "", true};
	if (name == "struct")    return Resolved{"struct",    "", true};
	if (name == "enum")      return Resolved{"byte",      "", true};  // UE wires
																	   // enums as
																	   // byte+enum-ref
	return std::nullopt;
}

[[noreturn]] void Bad(std::string_view input, std::string_view why) {
	throw std::invalid_argument(fmt::format(
		"type shorthand '{}' is invalid: {} — accepted forms include "
		"'float', 'int', 'bool', 'string', 'object:Actor', "
		"'struct:FVector', '[]float', '{{string:int}}'",
		input, why));
}

}    // namespace type_shorthand_detail
using namespace type_shorthand_detail;

BPPinType ParseTypeShorthand(std::string_view input) {
	if (input.empty())
	{
		Bad(input, "empty string");
	}

	BPPinType out;

	// Container prefix.
	std::string_view body = input;
	if (body.size() >= 2 && body[0] == '[' && body[1] == ']') {
		out.IsArray = true;
		body.remove_prefix(2);
	} else if (body.size() >= 2 && body[0] == '{' && body.back() == '}') {
		// Map vs Set: a colon inside the braces => map.
		std::string_view inner = body.substr(1, body.size() - 2);
		auto colon = inner.find(':');
		if (colon == std::string_view::npos) {
			// Set: {}T  written as {T} or {}T. Accept both.
			out.IsSet = true;
			body.remove_prefix(1);
			body.remove_suffix(1);
		} else {
			// Map: {K:V} — the KEY goes in the main fields and the VALUE in the
			// Value* fields, IsMap=true. This matches the editor's wire model
			// (PinCategory/PinSubCategoryObject = key, PinValueType = value).
			// Note: object keys/values via shorthand are best-effort — the split
			// is on the FIRST ':', so an object subref in the key (e.g.
			// {object:Pawn:int}) is ambiguous; use the canonical object form with
			// a nested `value_type` for object-typed map keys/values.
			std::string_view keyPart   = inner.substr(0, colon);
			std::string_view valuePart = inner.substr(colon + 1);
			BPPinType key   = ParseTypeShorthand(keyPart);
			BPPinType value = ParseTypeShorthand(valuePart);
			key.IsMap = true;
			key.ValueCategory          = value.Category;
			key.ValueSubCategory       = value.SubCategory;
			key.ValueSubCategoryObject = value.SubCategoryObject;
			return key;
		}
	} else if (body.size() >= 2 && body[0] == '{' && body[1] == '}') {
		out.IsSet = true;
		body.remove_prefix(2);
	}

	if (body.empty())
	{
		Bad(input, "missing element type after container prefix");
	}

	// Element: <name> or <name>:<subref>
	auto colon = body.find(':');
	std::string_view name = colon == std::string_view::npos ? body : body.substr(0, colon);
	std::string_view sub  = colon == std::string_view::npos
								? std::string_view{} : body.substr(colon + 1);

	auto resolved = ResolveName(name);
	if (!resolved) Bad(input, fmt::format("unknown element '{}'", name));

	if (resolved->needsSubRef) {
		if (sub.empty()) {
			Bad(input, fmt::format(
				"'{}' requires a colon-suffixed reference, e.g. '{}:Actor'", name, name));
		}
		out.SubCategoryObject = std::string(sub);
	} else if (!sub.empty()) {
		Bad(input, fmt::format("'{}' does not take a colon-suffixed reference", name));
	}

	out.Category = resolved->category;
	// Only set SubCategory if non-empty — empty string would round-trip as
	// present-but-blank in the optional, which the commandlet treats as a
	// typed-but-unset sub-category. Match the legacy object-form parser.
	if (!resolved->subCategory.empty()) {
		out.SubCategory = resolved->subCategory;
	}
	return out;
}

BPPinType ParseTypeArg(const nlohmann::json& value) {
	if (value.is_string()) {
		return ParseTypeShorthand(value.get<std::string>());
	}
	if (!value.is_object()) {
		throw std::invalid_argument(
			R"(argument "type" must be a string shorthand (e.g. "float", )"
			R"("object:Actor", "[]float") or a BPPinType object)");
	}

	// Object form — same code path as the legacy buildBPPinType.
	BPPinType type;
	auto catIt = value.find("category");
	if (catIt == value.end() || !catIt->is_string()) {
		throw std::invalid_argument(R"(BPPinType object requires a string "category" field)");
	}
	type.Category = catIt->get<std::string>();
	if (auto it = value.find("sub_category"); it != value.end() && it->is_string()) {
		type.SubCategory = it->get<std::string>();
	}
	if (auto it = value.find("sub_category_object"); it != value.end() && it->is_string()) {
		type.SubCategoryObject = it->get<std::string>();
	}
	type.IsArray = value.value("is_array", false);
	type.IsSet   = value.value("is_set",   false);
	type.IsMap   = value.value("is_map",   false);
	// Map value terminal type (nested object). Mirrors the editor wire shape.
	if (auto it = value.find("value_type"); it != value.end() && it->is_object()) {
		if (auto c = it->find("category"); c != it->end() && c->is_string()) {
			type.ValueCategory = c->get<std::string>();
		}
		if (auto sc = it->find("sub_category"); sc != it->end() && sc->is_string()) {
			type.ValueSubCategory = sc->get<std::string>();
		}
		if (auto so = it->find("sub_category_object"); so != it->end() && so->is_string()) {
			type.ValueSubCategoryObject = so->get<std::string>();
		}
	}
	return type;
}

}    // namespace bpr::tools
