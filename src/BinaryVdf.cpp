#include "BinaryVdf.h"

namespace
{
	constexpr uint8_t kTypeObject = 0x00;
	constexpr uint8_t kTypeString = 0x01;
	constexpr uint8_t kTypeInt32 = 0x02;
	constexpr uint8_t kTypeEnd = 0x08;

	bool ReadCString(const uint8_t*& p, const uint8_t* end, std::string& out)
	{
		const uint8_t* start = p;
		while (p < end && *p != 0)
			++p;
		if (p >= end)
			return false;
		out.assign((const char*)start, (const char*)p);
		++p; // skip the terminating null
		return true;
	}

	void WriteCString(std::vector<uint8_t>& out, const std::string& s)
	{
		out.insert(out.end(), s.begin(), s.end());
		out.push_back(0);
	}
}

bool VdfNode::ParseObject(const uint8_t*& p, const uint8_t* end, Object& out)
{
	while (true)
	{
		if (p >= end)
			return false; // ran off the end without a terminator - malformed

		uint8_t type = *p++;
		if (type == kTypeEnd)
			return true;

		std::string key;
		if (!ReadCString(p, end, key))
			return false;

		switch (type)
		{
		case kTypeObject:
		{
			Object child;
			if (!ParseObject(p, end, child))
				return false;
			out[key] = VdfNode(std::move(child));
			break;
		}
		case kTypeString:
		{
			std::string value;
			if (!ReadCString(p, end, value))
				return false;
			out[key] = VdfNode(std::move(value));
			break;
		}
		case kTypeInt32:
		{
			if (end - p < 4)
				return false;
			int32_t value;
			memcpy(&value, p, 4);
			p += 4;
			out[key] = VdfNode(value);
			break;
		}
		default:
			return false; // unsupported type tag - shortcuts.vdf never uses others
		}
	}
}

std::optional<VdfNode> VdfNode::Parse(const std::vector<uint8_t>& data)
{
	const uint8_t* p = data.data();
	const uint8_t* end = p + data.size();

	if (p >= end || *p != kTypeObject)
		return std::nullopt;
	++p;

	std::string rootKey;
	if (!ReadCString(p, end, rootKey))
		return std::nullopt;

	Object root;
	if (!ParseObject(p, end, root))
		return std::nullopt;

	return VdfNode(std::move(root));
}

void VdfNode::SerializeObject(std::vector<uint8_t>& out) const
{
	for (auto& [key, node] : AsObject())
	{
		if (node.IsObject())
		{
			out.push_back(kTypeObject);
			WriteCString(out, key);
			node.SerializeObject(out);
		}
		else if (auto* s = std::get_if<std::string>(&node.m_value))
		{
			out.push_back(kTypeString);
			WriteCString(out, key);
			WriteCString(out, *s);
		}
		else if (auto* i = std::get_if<int32_t>(&node.m_value))
		{
			out.push_back(kTypeInt32);
			WriteCString(out, key);
			out.insert(out.end(), (uint8_t*)i, (uint8_t*)i + 4);
		}
	}
	out.push_back(kTypeEnd);
}

std::vector<uint8_t> VdfNode::Serialize() const
{
	std::vector<uint8_t> out;
	out.push_back(kTypeObject);
	WriteCString(out, "shortcuts");
	SerializeObject(out);
	out.push_back(kTypeEnd);
	return out;
}
