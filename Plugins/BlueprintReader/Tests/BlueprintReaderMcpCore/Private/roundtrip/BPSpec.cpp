#include "BPSpec.h"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace bpr::roundtrip {

namespace {
	// FNV-1a 64-bit hash for stability.
	std::uint64_t FnvHash(std::string_view sv) {
		std::uint64_t h = 14695981039346656037ull;
		for (char c : sv) {
			h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
			h *= 1099511628211ull;
		}
		return h;
	}

	std::string HexHash(std::uint64_t h) {
		std::ostringstream o;
		o << std::hex << std::setw(16) << std::setfill('0') << h;
		return o.str();
	}

	nlohmann::json PinTypeJson(const BPPinType& t) {
		nlohmann::json j;
		j["category"] = t.Category;
		if (t.SubCategory)        j["sub_category"]        = *t.SubCategory;
		if (t.SubCategoryObject)  j["sub_category_object"] = *t.SubCategoryObject;
		if (t.IsArray) j["is_array"] = true;
		if (t.IsSet)   j["is_set"]   = true;
		if (t.IsMap)   j["is_map"]   = true;
		return j;
	}
	BPPinType PinTypeFromJson(const nlohmann::json& j) {
		BPPinType t;
		t.Category = j.value("category", "");
		if (j.contains("sub_category") && j["sub_category"].is_string())
			t.SubCategory       = j["sub_category"].get<std::string>();
		if (j.contains("sub_category_object") && j["sub_category_object"].is_string())
			t.SubCategoryObject = j["sub_category_object"].get<std::string>();
		t.IsArray = j.value("is_array", false);
		t.IsSet   = j.value("is_set", false);
		t.IsMap   = j.value("is_map", false);
		return t;
	}

	nlohmann::json PinJson(const BPPin& p) {
		nlohmann::json j;
		j["id"]         = p.Id;
		j["name"]       = p.Name;
		j["direction"]  = p.Direction;
		j["type"]       = PinTypeJson(p.Type);
		if (p.DefaultValue) j["default_value"] = *p.DefaultValue;
		return j;
	}
	BPPin PinFromJson(const nlohmann::json& j) {
		BPPin p;
		p.Id = j.value("id", "");
		p.Name = j.value("name", "");
		p.Direction = j.value("direction", "");
		p.Type = PinTypeFromJson(j.value("type", nlohmann::json::object()));
		if (j.contains("default_value") && j["default_value"].is_string())
			p.DefaultValue = j["default_value"].get<std::string>();
		return p;
	}

	nlohmann::json NodeJson(const BPNode& n) {
		nlohmann::json j;
		j["id"]        = n.Id;
		j["class"]     = n.Class;
		j["title"]     = n.Title;
		j["position"]  = { {"x", n.Position.X}, {"y", n.Position.Y} };
		if (n.Comment) j["comment"] = *n.Comment;
		j["pins"]      = nlohmann::json::array();
		for (const auto& p : n.Pins) j["pins"].push_back(PinJson(p));
		j["meta"]      = n.Meta;
		return j;
	}
	BPNode NodeFromJson(const nlohmann::json& j) {
		BPNode n;
		n.Id = j.value("id", "");
		n.Class = j.value("class", "");
		n.Title = j.value("title", "");
		if (j.contains("position")) {
			n.Position.X = j["position"].value("x", 0);
			n.Position.Y = j["position"].value("y", 0);
		}
		if (j.contains("comment") && j["comment"].is_string())
			n.Comment = j["comment"].get<std::string>();
		if (j.contains("pins")) {
			for (const auto& p : j["pins"]) n.Pins.push_back(PinFromJson(p));
		}
		if (j.contains("meta")) n.Meta = j["meta"];
		return n;
	}

	nlohmann::json ConnJson(const BPConnection& c) {
		return { {"from_node", c.FromNode}, {"from_pin", c.FromPin},
				 {"to_node", c.ToNode},     {"to_pin", c.ToPin} };
	}
	BPConnection ConnFromJson(const nlohmann::json& j) {
		BPConnection c;
		c.FromNode = j.value("from_node", "");
		c.FromPin  = j.value("from_pin", "");
		c.ToNode   = j.value("to_node", "");
		c.ToPin    = j.value("to_pin", "");
		return c;
	}

	nlohmann::json VarJson(const BPVariable& v) {
		nlohmann::json j;
		j["name"] = v.Name;
		j["type"] = PinTypeJson(v.Type);
		if (v.DefaultValue) j["default_value"] = *v.DefaultValue;
		if (v.Category)     j["category"]      = *v.Category;
		if (v.IsReplicated) j["is_replicated"] = true;
		if (v.IsEditable)   j["is_editable"]   = true;
		return j;
	}
	BPVariable VarFromJson(const nlohmann::json& j) {
		BPVariable v;
		v.Name = j.value("name", "");
		v.Type = PinTypeFromJson(j.value("type", nlohmann::json::object()));
		if (j.contains("default_value") && j["default_value"].is_string())
			v.DefaultValue = j["default_value"].get<std::string>();
		if (j.contains("category") && j["category"].is_string())
			v.Category = j["category"].get<std::string>();
		v.IsReplicated = j.value("is_replicated", false);
		v.IsEditable   = j.value("is_editable", false);
		return v;
	}

	nlohmann::json GraphJson(const SpecGraph& g) {
		nlohmann::json j;
		j["name"] = g.name; j["type"] = g.type;
		j["nodes"] = nlohmann::json::array();
		for (const auto& n : g.nodes) j["nodes"].push_back(NodeJson(n));
		j["connections"] = nlohmann::json::array();
		for (const auto& c : g.connections) j["connections"].push_back(ConnJson(c));
		return j;
	}
	SpecGraph GraphFromJson(const nlohmann::json& j) {
		SpecGraph g;
		g.name = j.value("name", "");
		g.type = j.value("type", "");
		if (j.contains("nodes")) for (const auto& n : j["nodes"]) g.nodes.push_back(NodeFromJson(n));
		if (j.contains("connections")) for (const auto& c : j["connections"]) g.connections.push_back(ConnFromJson(c));
		return g;
	}

	nlohmann::json FnJson(const SpecFunction& f) {
		nlohmann::json j;
		j["name"] = f.name;
		j["inputs"]  = nlohmann::json::array();
		for (const auto& p : f.inputs)  j["inputs"].push_back(PinJson(p));
		j["outputs"] = nlohmann::json::array();
		for (const auto& p : f.outputs) j["outputs"].push_back(PinJson(p));
		j["locals"]  = nlohmann::json::array();
		for (const auto& v : f.locals)  j["locals"].push_back(VarJson(v));
		j["nodes"]   = nlohmann::json::array();
		for (const auto& n : f.nodes)   j["nodes"].push_back(NodeJson(n));
		j["connections"] = nlohmann::json::array();
		for (const auto& c : f.connections) j["connections"].push_back(ConnJson(c));
		return j;
	}
	SpecFunction FnFromJson(const nlohmann::json& j) {
		SpecFunction f;
		f.name = j.value("name", "");
		if (j.contains("inputs"))  for (const auto& p : j["inputs"])  f.inputs.push_back(PinFromJson(p));
		if (j.contains("outputs")) for (const auto& p : j["outputs"]) f.outputs.push_back(PinFromJson(p));
		if (j.contains("locals"))  for (const auto& v : j["locals"])  f.locals.push_back(VarFromJson(v));
		if (j.contains("nodes"))   for (const auto& n : j["nodes"])   f.nodes.push_back(NodeFromJson(n));
		if (j.contains("connections")) for (const auto& c : j["connections"]) f.connections.push_back(ConnFromJson(c));
		return f;
	}

	nlohmann::json CompJson(const SpecComponent& c) {
		return { {"name", c.name}, {"component_class", c.component_class},
				 {"parent_name", c.parent_name}, {"socket", c.socket},
				 {"properties", c.properties} };
	}
	SpecComponent CompFromJson(const nlohmann::json& j) {
		SpecComponent c;
		c.name = j.value("name", "");
		c.component_class = j.value("component_class", "");
		c.parent_name = j.value("parent_name", "");
		c.socket = j.value("socket", "");
		c.properties = j.value("properties", nlohmann::json::object());
		return c;
	}
}

nlohmann::json ToJson(const BPSpec& s) {
	nlohmann::json j;
	j["package_path"] = s.package_path;
	j["parent_class"] = s.parent_class;
	j["interfaces"]   = s.interfaces;
	j["variables"]    = nlohmann::json::array();
	for (const auto& v : s.variables)  j["variables"].push_back(VarJson(v));
	j["components"]   = nlohmann::json::array();
	for (const auto& c : s.components) j["components"].push_back(CompJson(c));
	j["functions"]    = nlohmann::json::array();
	for (const auto& f : s.functions)  j["functions"].push_back(FnJson(f));
	j["macros"]       = nlohmann::json::array();
	for (const auto& g : s.macros)     j["macros"].push_back(GraphJson(g));
	j["event_graph"]  = GraphJson(s.event_graph);
	if (s.incomplete) {
		j["incomplete"] = true;
		j["errors"]     = s.errors;
	}
	return j;
}

BPSpec FromJson(const nlohmann::json& j) {
	BPSpec s;
	s.package_path = j.value("package_path", "");
	s.parent_class = j.value("parent_class", "");
	if (j.contains("interfaces"))
		s.interfaces = j["interfaces"].get<std::vector<std::string>>();
	if (j.contains("variables"))
		for (const auto& v : j["variables"])  s.variables.push_back(VarFromJson(v));
	if (j.contains("components"))
		for (const auto& c : j["components"]) s.components.push_back(CompFromJson(c));
	if (j.contains("functions"))
		for (const auto& f : j["functions"])  s.functions.push_back(FnFromJson(f));
	if (j.contains("macros"))
		for (const auto& g : j["macros"])     s.macros.push_back(GraphFromJson(g));
	if (j.contains("event_graph"))
		s.event_graph = GraphFromJson(j["event_graph"]);
	s.incomplete = j.value("incomplete", false);
	if (j.contains("errors"))
		s.errors = j["errors"].get<std::vector<std::string>>();
	return s;
}

std::string StableNodeId(const BPNode& node, std::size_t positionRank) {
	std::string sig;
	sig.reserve(64);
	sig += node.Class;
	sig += '|';
	sig += node.Title;
	sig += '|';
	for (const auto& p : node.Pins) {
		sig += p.Name;  sig += ':';
		sig += p.Direction; sig += ':';
		sig += p.Type.Category;
		if (p.Type.SubCategoryObject) { sig += '@'; sig += *p.Type.SubCategoryObject; }
		sig += ';';
	}
	sig += '|';
	sig += std::to_string(positionRank);
	return HexHash(FnvHash(sig));
}

}    // namespace bpr::roundtrip
