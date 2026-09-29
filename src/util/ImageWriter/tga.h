#include "Common/FileStream.h"
#include <vector>
#include <fstream>
#include <filesystem>
#include <string>
#include <cerrno>
#include <cstring>

static bool tga_write_rgba(const fs::path& path, sint32 width, sint32 height, uint8* pixelData, std::string* errorOut = nullptr)
{
	// Deliberately std::ofstream rather than FileStream. On Android FileStream routes through
	// FileStreamAndroid, which carries the content-URI abstraction and its own descriptor
	// handling; texture dumping writes one file per texture straight to a real path and does not
	// need any of that. Using the stream directly removes a whole class of failure from a path
	// that previously reported nothing when it broke.
	auto fail = [&](const std::string& what) {
		if (errorOut)
			*errorOut = what;
		return false;
	};
	std::error_code _ec;
	fs::create_directories(path.parent_path(), _ec);
	if (_ec)
		return fail("cannot create folder: " + _ec.message());
	errno = 0;
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out.is_open())
		return fail(std::string("cannot open file: ") + (errno ? std::strerror(errno) : "unknown error"));

	uint8_t header[18] = {0,0,2,0,0,0,0,0,0,0,0,0, (uint8)(width % 256), (uint8)(width / 256), (uint8)(height % 256), (uint8)(height / 256), 32, 0x20};
	out.write((const char*)header, sizeof(header));

	std::vector<uint8> tempPixelData;
	tempPixelData.resize(width * height * 4);

	// write one row at a time
	uint8* pOut = tempPixelData.data();
	for (sint32 y = 0; y < height; y++)
	{
		const uint8* rowIn = pixelData + y * width*4;
		for (sint32 x = 0; x < width; x++)
		{
			pOut[0] = rowIn[2];
			pOut[1] = rowIn[1];
			pOut[2] = rowIn[0];
			pOut[3] = rowIn[3];
			pOut += 4;
			rowIn += 4;
		}
	}
	out.write((const char*)tempPixelData.data(), (std::streamsize)width * height * 4);
	out.close();
	if (out.fail())
	{
		std::error_code rmEc;
		fs::remove(path, rmEc); // a truncated TGA would be kept forever by the skip-if-exists check
		return fail("write failed (disk full?)");
	}
	return true;
}
