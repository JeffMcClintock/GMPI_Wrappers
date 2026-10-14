#pragma once

#include <algorithm>
#include <string>
#include "pluginterfaces/base/ibstream.h"

// Reading a VST3 state chunk (int32 size, then that many bytes). Same code as SE16's se_vst3/source/StateStreamRead.h.
namespace gmpi_vst3
{
// IBStream may return fewer bytes than asked; loop until all arrive. False if the stream ends early.
inline bool readAll(Steinberg::IBStream* state, void* dest, Steinberg::int32 size)
{
	auto* at = static_cast<char*>(dest);
	Steinberg::int32 remaining = size;

	while (remaining > 0)
	{
		Steinberg::int32 bytesRead{};
		const auto r = state->read(at, remaining, &bytesRead);
		if (r != Steinberg::kResultOk || bytesRead <= 0)
			return false;

		at += bytesRead;
		remaining -= bytesRead;
	}

	return true;
}

// Reads the int32 size prefix and that many bytes. Grows 'chunk' only as bytes arrive, so a corrupt size fails as a short stream instead of allocating it.
inline bool readSizedChunk(Steinberg::IBStream* state, std::string& chunk)
{
	Steinberg::int32 chunkSize{};
	if (!readAll(state, &chunkSize, sizeof(chunkSize)) || chunkSize < 0)
		return false;

	constexpr Steinberg::int32 pieceSize = 0x10000;

	chunk.clear();
	while (static_cast<Steinberg::int32>(chunk.size()) < chunkSize)
	{
		const auto have = static_cast<Steinberg::int32>(chunk.size());
		const auto want = (std::min)(pieceSize, chunkSize - have);

		chunk.resize(have + want);
		if (!readAll(state, chunk.data() + have, want))
			return false;
	}

	return true;
}
} // namespace gmpi_vst3
