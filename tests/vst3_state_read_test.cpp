// vst3_state_read_test - the VST3 wrapper's state reads (Processor_VST3::setState, Controller_VST3::setComponentState).
//
// An IBStream may deliver a chunk in pieces, end early, or carry a corrupt size. readSizedChunk must reassemble
// pieces, fail on a short stream, and never allocate a size the stream doesn't actually hold.
//
// No CMake needed (paths relative to C:\SE or the parent of the repos):
//
//   g++ -std=c++20 -I SDKs/VST3_SDK GMPI_Wrappers/tests/vst3_state_read_test.cpp -o vst3_state_read_test
//
// (one command line; MSVC: the same with cl /std:c++20 /EHsc /DNOMINMAX.) Exit code 0 = pass.

#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../wrapper/VST3/StateStreamRead.h"

using namespace Steinberg;

namespace
{
int failures = 0;

void check(bool ok, const char* what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		++failures;
}

// An in-memory IBStream that hands out at most 'maxPerRead' bytes per read() call.
class PieceStream : public IBStream
{
public:
	PieceStream(std::vector<char> bytes, int32 maxPerRead) : data(std::move(bytes)), maxPerRead(maxPerRead) {}

	tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override
	{
		if (failReads)
			return kResultFalse;

		const auto n = static_cast<int32>((std::min)({ static_cast<size_t>(numBytes), static_cast<size_t>(maxPerRead), data.size() - pos }));
		std::memcpy(buffer, data.data() + pos, n);
		pos += n;
		if (numBytesRead)
			*numBytesRead = n;
		return kResultOk;
	}
	tresult PLUGIN_API write(void*, int32, int32*) override { return kNotImplemented; }
	tresult PLUGIN_API seek(int64, int32, int64*) override { return kNotImplemented; }
	tresult PLUGIN_API tell(int64* p) override { if (p) *p = static_cast<int64>(pos); return kResultOk; }

	tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
	uint32 PLUGIN_API addRef() override { return 1; }
	uint32 PLUGIN_API release() override { return 1; }

	size_t pos = 0;
	bool failReads = false;

private:
	std::vector<char> data;
	int32 maxPerRead;
};

std::vector<char> sizedChunk(int32 declaredSize, const std::string& body)
{
	std::vector<char> bytes(sizeof(declaredSize));
	std::memcpy(bytes.data(), &declaredSize, sizeof(declaredSize));
	bytes.insert(bytes.end(), body.begin(), body.end());
	return bytes;
}

std::string patternBody(size_t size)
{
	std::string body(size, '\0');
	for (size_t i = 0; i < size; ++i)
		body[i] = static_cast<char>('A' + i % 26);
	return body;
}
} // namespace

int main()
{
	using gmpi_vst3::readSizedChunk;

	{
		const auto body = patternBody(150000); // spans several 64 KB pieces
		PieceStream stream(sizedChunk(static_cast<int32>(body.size()), body), 1000);
		std::string chunk;
		check(readSizedChunk(&stream, chunk) && chunk == body, "reassembles a chunk delivered 1000 bytes at a time");
	}
	{
		const auto body = patternBody(5000);
		PieceStream stream(sizedChunk(static_cast<int32>(body.size()), body), INT_MAX);
		std::string chunk;
		check(readSizedChunk(&stream, chunk) && chunk == body, "reads a whole chunk delivered in one go");
	}
	{
		const auto body = patternBody(100);
		PieceStream stream(sizedChunk(60, body), 7);
		std::string chunk;
		check(readSizedChunk(&stream, chunk) && chunk == body.substr(0, 60) && stream.pos == sizeof(int32) + 60, "stops at the declared size");
	}
	{
		PieceStream stream(sizedChunk(0, ""), 1000);
		std::string chunk = "stale";
		check(readSizedChunk(&stream, chunk) && chunk.empty(), "accepts an empty chunk");
	}
	{
		PieceStream stream(sizedChunk(5000, patternBody(3000)), 1000);
		std::string chunk;
		check(!readSizedChunk(&stream, chunk), "fails when the stream ends early");
	}
	{
		PieceStream stream({ 'x', 'y' }, 1000);
		std::string chunk;
		check(!readSizedChunk(&stream, chunk), "fails when the size prefix is cut");
	}
	{
		PieceStream stream(sizedChunk(-1, patternBody(10)), 1000);
		std::string chunk;
		check(!readSizedChunk(&stream, chunk), "rejects a negative size");
	}
	{
		PieceStream stream(sizedChunk(INT_MAX, patternBody(10)), 1000);
		std::string chunk;
		const bool ok = readSizedChunk(&stream, chunk);
		check(!ok && chunk.capacity() <= 0x20000u, "a 2 GB size fails without allocating it (one 64 KB piece at most)");
	}
	{
		PieceStream stream(sizedChunk(10, patternBody(10)), 1000);
		stream.failReads = true;
		std::string chunk;
		check(!readSizedChunk(&stream, chunk), "fails when the stream reports an error");
	}

	std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
