#pragma once

// Minimal gzip container over miniz's raw deflate.
//
// The bundled Poco in the ArkApi SDK is a trimmed subset with no zlib support,
// so compression is vendored instead. Writing a real gzip wrapper (rather than
// miniz's zlib framing) means a .sbp save is an ordinary gzip file that
// `gunzip`, Python's `gzip` module or 7-Zip can open directly.

#include <string>

namespace sb::gz
{
	bool Compress(const std::string& in, std::string& out);
	bool Decompress(const std::string& in, std::string& out);
} // namespace sb::gz
