#pragma once
/*
#include "Vst3ClassIds.h"

The VST3 class IDs of a GMPI plugin. Every factory entry point that reports or
matches a cid goes through vst3ClassIds(), so they cannot disagree.
*/

#include <cassert>
#include <cctype>
#include <cstdint>
#include <string>
#include "pluginterfaces/base/funknown.h"
#include "Hosting/xml_spec_reader.h"

namespace wrapper
{
struct Vst3ClassIds
{
	Steinberg::FUID processor;
	Steinberg::FUID controller;
};

namespace detail
{
// "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}". FUID::fromRegistryString checks only the length.
inline bool isRegistryString(const std::string& s)
{
	if (s.size() != 38 || s.front() != '{' || s.back() != '}')
		return false;

	for (size_t i = 1; i < 37; ++i)
	{
		const bool dash = i == 9 || i == 14 || i == 19 || i == 24;
		if (dash ? s[i] != '-' : !std::isxdigit(static_cast<unsigned char>(s[i])))
			return false;
	}
	return true;
}

// FNV-1a over unsigned bytes, so a non-ASCII id hashes the same whether char is signed or not.
inline uint64_t fnv1a64(const std::string& s, uint64_t h = 0xcbf29ce484222325ull)
{
	for (const unsigned char c : s)
	{
		h ^= c;
		h *= 0x100000001b3ull;
	}
	return h;
}

// splitmix64 finalizer.
inline uint64_t mix64(uint64_t z)
{
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

// Fallback when the descriptor names no ID: 122 hashed bits of role + id, tagged as an
// RFC 9562 version-8 UUID. from4Int (not a raw memcpy) gives the same GUID string on every platform.
inline Steinberg::FUID hashedClassId(const std::string& pluginId, char role)
{
	const std::string key = std::string("GMPI VST3 ") + role + ':' + pluginId;
	const auto a = mix64(fnv1a64(key));
	const auto b = mix64(fnv1a64(key, a));

	const auto w1 = static_cast<uint32_t>(a >> 32);
	const auto w2 = (static_cast<uint32_t>(a) & 0xFFFF0FFFu) | 0x00008000u; // version 8
	const auto w3 = (static_cast<uint32_t>(b >> 32) & 0x3FFFFFFFu) | 0x80000000u; // variant 10
	const auto w4 = static_cast<uint32_t>(b);

	Steinberg::FUID id;
	id.from4Int(w1, w2, w3, w4);
	return id;
}

inline Steinberg::FUID classId(const std::string& registryString, const std::string& pluginId, char role)
{
	if (!registryString.empty())
	{
		Steinberg::FUID id;
		if (isRegistryString(registryString) && id.fromRegistryString(registryString.c_str()))
			return id;

		assert(false && "malformed vst3ProcessorId/vst3ControllerId, using the hashed fallback");
	}
	return hashedClassId(pluginId, role);
}
} // namespace detail

inline Vst3ClassIds vst3ClassIds(const gmpi::hosting::pluginInfo& plugin)
{
#ifdef GMPI_HOSTING_PLUGININFO_HAS_VST3_IDS
	const std::string& processorId = plugin.vst3ProcessorId;
	const std::string& controllerId = plugin.vst3ControllerId;
#else
	const std::string processorId, controllerId; // SDK predates explicit IDs.
#endif
	return {
		detail::classId(processorId, plugin.id, 'P'),
		detail::classId(controllerId, plugin.id, 'C')
	};
}

// The class ID as 16 bytes in canonical GUID-string order, identical on every platform
// (TUID's raw layout is not: COM_COMPATIBLE stores the first three fields little-endian).
inline void canonicalBytes(const Steinberg::FUID& id, unsigned char out[16])
{
	const uint32_t words[4] = { id.getLong1(), id.getLong2(), id.getLong3(), id.getLong4() };
	for (int i = 0; i < 16; ++i)
		out[i] = static_cast<unsigned char>(words[i / 4] >> (8 * (3 - i % 4)));
}
} // namespace wrapper
