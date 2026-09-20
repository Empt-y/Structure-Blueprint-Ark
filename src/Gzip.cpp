#include "Gzip.h"

#include <miniz.h>

#include <cstring>
#include <cstdint>

namespace sb::gz
{
	namespace
	{
		constexpr size_t kHeaderSize = 10;
		constexpr size_t kTrailerSize = 8;

		void PutLE32(std::string& s, uint32_t v)
		{
			s.push_back(static_cast<char>(v & 0xFF));
			s.push_back(static_cast<char>((v >> 8) & 0xFF));
			s.push_back(static_cast<char>((v >> 16) & 0xFF));
			s.push_back(static_cast<char>((v >> 24) & 0xFF));
		}

		uint32_t ReadLE32(const unsigned char* p)
		{
			return static_cast<uint32_t>(p[0])
				| (static_cast<uint32_t>(p[1]) << 8)
				| (static_cast<uint32_t>(p[2]) << 16)
				| (static_cast<uint32_t>(p[3]) << 24);
		}
	} // namespace

	bool Compress(const std::string& in, std::string& out)
	{
		size_t deflated_len = 0;
		void* deflated = tdefl_compress_mem_to_heap(
			in.data(), in.size(), &deflated_len,
			TDEFL_DEFAULT_MAX_PROBES); // raw deflate: no zlib header

		if (deflated == nullptr) return false;

		out.clear();
		out.reserve(kHeaderSize + deflated_len + kTrailerSize);

		// magic, CM=deflate, no flags, no mtime, no extra flags, OS=unknown
		const unsigned char header[kHeaderSize] = {
			0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF
		};
		out.append(reinterpret_cast<const char*>(header), kHeaderSize);
		out.append(static_cast<const char*>(deflated), deflated_len);

		PutLE32(out, static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT,
			reinterpret_cast<const unsigned char*>(in.data()), in.size())));
		PutLE32(out, static_cast<uint32_t>(in.size() & 0xFFFFFFFFu));

		mz_free(deflated);
		return true;
	}

	bool Decompress(const std::string& in, std::string& out)
	{
		if (in.size() < kHeaderSize + kTrailerSize) return false;

		const auto* raw = reinterpret_cast<const unsigned char*>(in.data());
		if (raw[0] != 0x1F || raw[1] != 0x8B || raw[2] != 0x08) return false;

		// Skip the optional members the FLG byte may announce. Our own writer
		// emits none, but be tolerant of files recompressed by other tools.
		const unsigned char flags = raw[3];
		size_t pos = kHeaderSize;

		if (flags & 0x04) // FEXTRA
		{
			if (pos + 2 > in.size()) return false;
			const size_t xlen = static_cast<size_t>(raw[pos]) | (static_cast<size_t>(raw[pos + 1]) << 8);
			pos += 2 + xlen;
		}
		if (flags & 0x08) // FNAME
		{
			while (pos < in.size() && raw[pos] != 0) ++pos;
			++pos;
		}
		if (flags & 0x10) // FCOMMENT
		{
			while (pos < in.size() && raw[pos] != 0) ++pos;
			++pos;
		}
		if (flags & 0x02) pos += 2; // FHCRC

		if (pos + kTrailerSize > in.size()) return false;

		const size_t payload_len = in.size() - pos - kTrailerSize;
		size_t inflated_len = 0;
		void* inflated = tinfl_decompress_mem_to_heap(
			raw + pos, payload_len, &inflated_len, 0); // raw inflate

		if (inflated == nullptr) return false;

		out.assign(static_cast<const char*>(inflated), inflated_len);

		const uint32_t expect_crc = ReadLE32(raw + in.size() - 8);
		const uint32_t actual_crc = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT,
			reinterpret_cast<const unsigned char*>(out.data()), out.size()));

		mz_free(inflated);
		return expect_crc == actual_crc;
	}
} // namespace sb::gz
