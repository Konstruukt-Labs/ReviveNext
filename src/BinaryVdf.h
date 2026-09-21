#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Minimal reader/writer for Valve's binary KeyValues format, as used by
// Steam's shortcuts.vdf (distinct from the human-readable text VDF format
// used elsewhere). Only the type tags shortcuts.vdf actually uses are
// supported: 0x00 nested object, 0x01 string, 0x02 int32 (also how
// booleans are stored - 0 or 1). Format reference: shortcuts.vdf is a
// single top-level object literally named "shortcuts", whose children are
// objects keyed by a 0-based decimal index (Steam renumbers these on every
// write, so they can't be used as a stable identifier across saves).
class VdfNode
{
public:
	using Object = std::map<std::string, VdfNode>;
	using Value = std::variant<Object, std::string, int32_t>;

	VdfNode() : m_value(Object{}) {}
	VdfNode(Object obj) : m_value(std::move(obj)) {}
	VdfNode(std::string str) : m_value(std::move(str)) {}
	VdfNode(int32_t i) : m_value(i) {}
	VdfNode(bool b) : m_value((int32_t)(b ? 1 : 0)) {}

	bool IsObject() const { return std::holds_alternative<Object>(m_value); }
	Object& AsObject() { return std::get<Object>(m_value); }
	const Object& AsObject() const { return std::get<Object>(m_value); }

	std::string AsString(const std::string& fallback = "") const
	{
		if (auto* s = std::get_if<std::string>(&m_value))
			return *s;
		return fallback;
	}

	int32_t AsInt(int32_t fallback = 0) const
	{
		if (auto* i = std::get_if<int32_t>(&m_value))
			return *i;
		return fallback;
	}

	// Child lookup for object nodes; returns nullptr if not an object or
	// the key doesn't exist.
	VdfNode* Find(const std::string& key)
	{
		if (!IsObject())
			return nullptr;
		auto& obj = AsObject();
		auto it = obj.find(key);
		return it == obj.end() ? nullptr : &it->second;
	}

	VdfNode& operator[](const std::string& key)
	{
		if (!IsObject())
			m_value = Object{};
		return AsObject()[key];
	}

	// Parses a full binary VDF document (starting at the outermost object,
	// which for shortcuts.vdf is the "shortcuts" key itself). Returns
	// nullopt on any malformed input.
	static std::optional<VdfNode> Parse(const std::vector<uint8_t>& data);

	// Serializes back to the same binary format Parse() reads, matching
	// what Steam itself writes.
	std::vector<uint8_t> Serialize() const;

private:
	Value m_value;

	static bool ParseObject(const uint8_t*& p, const uint8_t* end, Object& out);
	void SerializeObject(std::vector<uint8_t>& out) const;
};
