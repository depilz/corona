//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////


#include "AndroidImageSniffer.h"
#include <string.h>
#include <zlib.h>


// ----------------------------------------------------------------------------
// Private helpers
// ----------------------------------------------------------------------------

namespace
{

typedef AndroidImageSniffer Sniffer;

const unsigned char kPngSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
const unsigned char kPngEnd[12] = { 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82 };
const unsigned long kPngMaxValue = 0x7FFFFFFFUL;

bool Fits(size_t length, size_t offset, size_t count)
{
	return (offset <= length) && (count <= length - offset);
}

unsigned int GetU16(const unsigned char *bytes, bool isBigEndian)
{
	return isBigEndian ? ((bytes[0] << 8) | bytes[1]) : ((bytes[1] << 8) | bytes[0]);
}

unsigned long GetU32(const unsigned char *bytes, bool isBigEndian)
{
	unsigned long high = GetU16(isBigEndian ? bytes : bytes + 2, isBigEndian);
	unsigned long low = GetU16(isBigEndian ? bytes + 2 : bytes, isBigEndian);
	return (high << 16) | low;
}

/// Reads bytes of the whole file.
/// @return Returns kEligible when read, kTruncated past the end, kUnreadable if the source failed.
Sniffer::Verdict Read(const Sniffer::ByteSource &source, size_t offset, void *buffer, size_t count)
{
	if (!Fits(source.GetLength(), offset, count))
	{
		return Sniffer::kTruncated;
	}
	return source.ReadAt(offset, buffer, count) ? Sniffer::kEligible : Sniffer::kUnreadable;
}

/// Rejects a file that does not end with the given bytes.
Sniffer::Verdict CheckEnd(const Sniffer::ByteSource &source, const unsigned char *end, size_t count)
{
	unsigned char bytes[12];
	size_t length = source.GetLength();
	Sniffer::Verdict verdict = Read(source, length - count, bytes, count);
	if (Sniffer::kEligible != verdict)
	{
		return verdict;
	}
	return (0 == memcmp(bytes, end, count)) ? Sniffer::kEligible : Sniffer::kTruncated;
}

/// The TIFF block of an Exif APP1 segment, whose offsets start at the block.
struct TiffBlock
{
	const Sniffer::ByteSource *source;
	size_t start;
	size_t length;
	bool isBigEndian;
};

/// Reads bytes of a TIFF block.
/// @return Returns kEligible when read, kExifMalformed past the block, kUnreadable if the source failed.
Sniffer::Verdict ReadTiff(const TiffBlock &tiff, size_t offset, unsigned char *buffer, size_t count)
{
	if (!Fits(tiff.length, offset, count))
	{
		return Sniffer::kExifMalformed;
	}
	return tiff.source->ReadAt(tiff.start + offset, buffer, count) ? Sniffer::kEligible : Sniffer::kUnreadable;
}

/// Rejects an IFD with an orientation other than 1 (tag 0x0112), and checks the Exif sub-IFD
/// (tag 0x8769) the same way when "followExifIfd" is set. IFD1 (the thumbnail) is not read.
Sniffer::Verdict CheckIfd(const TiffBlock &tiff, unsigned long ifdOffset, bool followExifIfd)
{
	unsigned char bytes[12];
	Sniffer::Verdict verdict = ReadTiff(tiff, ifdOffset, bytes, 2);
	if (Sniffer::kEligible != verdict)
	{
		return verdict;
	}
	unsigned int entryCount = GetU16(bytes, tiff.isBigEndian);
	for (unsigned int index = 0; index < entryCount; index++)
	{
		verdict = ReadTiff(tiff, ifdOffset + 2 + (12 * index), bytes, 12);
		if (Sniffer::kEligible != verdict)
		{
			return verdict;
		}
		unsigned int tag = GetU16(bytes, tiff.isBigEndian);
		unsigned int type = GetU16(bytes + 2, tiff.isBigEndian);
		unsigned long count = GetU32(bytes + 4, tiff.isBigEndian);
		if (0x0112 == tag)
		{
			if ((type != 3) || (count != 1))
			{
				return Sniffer::kExifMalformed;
			}
			if (GetU16(bytes + 8, tiff.isBigEndian) != 1)
			{
				return Sniffer::kExifOrientation;
			}
		}
		else if ((0x8769 == tag) && followExifIfd)
		{
			if (((type != 4) && (type != 13)) || (count != 1))
			{
				return Sniffer::kExifMalformed;
			}
			verdict = CheckIfd(tiff, GetU32(bytes + 8, tiff.isBigEndian), false);
			if (Sniffer::kEligible != verdict)
			{
				return verdict;
			}
		}
	}
	return Sniffer::kEligible;
}

/// Checks an APP1 segment's payload; one that is not Exif passes.
Sniffer::Verdict CheckApp1(const Sniffer::ByteSource &source, size_t offset, size_t length)
{
	unsigned char bytes[8];
	if (length < 6)
	{
		return Sniffer::kEligible;
	}
	Sniffer::Verdict verdict = Read(source, offset, bytes, 6);
	if ((Sniffer::kEligible != verdict) || (0 != memcmp(bytes, "Exif\0", 5)))
	{
		return verdict;
	}

	// The TIFF header: byte order, the number 42 and the offset of IFD0.
	TiffBlock tiff = { &source, offset + 6, length - 6, false };
	verdict = ReadTiff(tiff, 0, bytes, 8);
	if (Sniffer::kEligible != verdict)
	{
		return verdict;
	}
	if ((0 == memcmp(bytes, "MM", 2)) || (0 == memcmp(bytes, "II", 2)))
	{
		tiff.isBigEndian = ('M' == bytes[0]);
	}
	else
	{
		return Sniffer::kExifMalformed;
	}
	unsigned long ifd0Offset = GetU32(bytes + 4, tiff.isBigEndian);
	if ((GetU16(bytes + 2, tiff.isBigEndian) != 42) || (ifd0Offset < 8))
	{
		return Sniffer::kExifMalformed;
	}
	return CheckIfd(tiff, ifd0Offset, true);
}

/// Reads a SOF0, SOF1 or SOF2 frame header's size.
Sniffer::Verdict ReadFrame(const Sniffer::ByteSource &source, size_t offset, size_t length, Sniffer::Result &result)
{
	unsigned char bytes[6];
	Sniffer::Verdict verdict = (length < 6) ? Sniffer::kMalformed : Read(source, offset, bytes, 6);
	if (Sniffer::kEligible != verdict)
	{
		return verdict;
	}
	unsigned int height = GetU16(bytes + 1, true);
	unsigned int width = GetU16(bytes + 3, true);
	unsigned int componentCount = bytes[5];
	if ((bytes[0] != 8) || (0 == height))
	{
		return Sniffer::kUnsupportedFrame;
	}
	if ((0 == width) || (componentCount < 1) || (componentCount > 4) || (length != 6 + (3 * componentCount)))
	{
		return Sniffer::kMalformed;
	}
	result.width = (int)width;
	result.height = (int)height;
	return Sniffer::kEligible;
}

/// Walks the JPEG segments from SOI up to SOS: one SOF0/1/2 frame, no doubtful Exif APP1.
Sniffer::Verdict CheckJpeg(const Sniffer::ByteSource &source, Sniffer::Result &result)
{
	bool hasFrame = false;
	size_t offset = 2;
	for (;;)
	{
		unsigned char bytes[4];
		Sniffer::Verdict verdict = Read(source, offset, bytes, 2);
		if (Sniffer::kEligible != verdict)
		{
			return verdict;
		}
		if (bytes[0] != 0xFF)
		{
			return Sniffer::kMalformed;
		}
		unsigned char marker = bytes[1];
		if (0xFF == marker)
		{
			// A fill byte before the marker.
			offset++;
			continue;
		}

		// Before SOS every marker but SOI, EOI, RSTn, TEM and 0x00 is followed by a length.
		if ((marker <= 0x01) || ((marker >= 0xD0) && (marker <= 0xD9)))
		{
			return Sniffer::kMalformed;
		}
		verdict = Read(source, offset + 2, bytes + 2, 2);
		if (Sniffer::kEligible != verdict)
		{
			return verdict;
		}
		size_t segmentLength = GetU16(bytes + 2, true);
		if (segmentLength < 2)
		{
			return Sniffer::kMalformed;
		}
		size_t payloadOffset = offset + 4;
		size_t payloadLength = segmentLength - 2;
		if (!Fits(source.GetLength(), payloadOffset, payloadLength))
		{
			return Sniffer::kTruncated;
		}

		switch (marker)
		{
			case 0xDA:
				if (!hasFrame)
				{
					return Sniffer::kUnsupportedFrame;
				}
				return CheckEnd(source, (const unsigned char *)"\xFF\xD9", 2);
			case 0xC0:
			case 0xC1:
			case 0xC2:
				verdict = hasFrame ? Sniffer::kMalformed : ReadFrame(source, payloadOffset, payloadLength, result);
				hasFrame = true;
				break;
			case 0xC3:
			case 0xC5: case 0xC6: case 0xC7:
			case 0xC9: case 0xCA: case 0xCB:
			case 0xCD: case 0xCE: case 0xCF:
				verdict = Sniffer::kUnsupportedFrame;
				break;
			case 0xE1:
				verdict = CheckApp1(source, payloadOffset, payloadLength);
				break;
			default:
				break;
		}
		if (Sniffer::kEligible != verdict)
		{
			return verdict;
		}
		offset = payloadOffset + payloadLength;
	}
}

bool IsValidPngDepth(unsigned int colorType, unsigned int bitDepth)
{
	switch (colorType)
	{
		case 0:
			return (1 == bitDepth) || (2 == bitDepth) || (4 == bitDepth) || (8 == bitDepth) || (16 == bitDepth);
		case 3:
			return (1 == bitDepth) || (2 == bitDepth) || (4 == bitDepth) || (8 == bitDepth);
		case 2:
		case 4:
		case 6:
			return (8 == bitDepth) || (16 == bitDepth);
		default:
			return false;
	}
}

/// Reads the IHDR chunk that must follow the signature.
Sniffer::Verdict ReadHeader(const Sniffer::ByteSource &source, Sniffer::Result &result)
{
	unsigned char bytes[21];
	Sniffer::Verdict verdict = Read(source, 8, bytes, 21);
	if (Sniffer::kEligible != verdict)
	{
		return verdict;
	}
	const unsigned char *data = bytes + 8;
	unsigned long width = GetU32(data, true);
	unsigned long height = GetU32(data + 4, true);
	if ((GetU32(bytes, true) != 13) || (0 != memcmp(bytes + 4, "IHDR", 4))
		|| (0 == width) || (width > kPngMaxValue) || (0 == height) || (height > kPngMaxValue)
		|| !IsValidPngDepth(data[9], data[8]) || (data[10] != 0) || (data[11] != 0) || (data[12] > 1))
	{
		return Sniffer::kMalformed;
	}
	if (1 == data[12])
	{
		return Sniffer::kInterlaced;
	}
	result.width = (int)width;
	result.height = (int)height;
	result.hasAlpha = (4 == data[9]) || (6 == data[9]);
	return Sniffer::kEligible;
}

/// Rejects a chunk whose CRC does not match its type and data.
Sniffer::Verdict CheckCrc(const Sniffer::ByteSource &source, size_t offset, unsigned long dataLength)
{
	unsigned char bytes[512];
	uLong crc = crc32(0L, Z_NULL, 0);
	size_t end = offset + 8 + (size_t)dataLength;
	for (size_t index = offset + 4; index < end; index += sizeof(bytes))
	{
		size_t count = (end - index < sizeof(bytes)) ? (end - index) : sizeof(bytes);
		Sniffer::Verdict verdict = Read(source, index, bytes, count);
		if (Sniffer::kEligible != verdict)
		{
			return verdict;
		}
		crc = crc32(crc, bytes, (uInt)count);
	}
	Sniffer::Verdict verdict = Read(source, end, bytes, 4);
	if (Sniffer::kEligible != verdict)
	{
		return verdict;
	}
	return (GetU32(bytes, true) == (crc & 0xFFFFFFFFUL)) ? Sniffer::kEligible : Sniffer::kMalformed;
}

/// Walks the PNG chunks from IHDR up to the first IDAT, checking the CRC of each chunk before it.
Sniffer::Verdict CheckPng(const Sniffer::ByteSource &source, int apiLevel, Sniffer::Result &result)
{
	Sniffer::Verdict verdict = ReadHeader(source, result);
	size_t offset = 8;
	unsigned long dataLength = 13;
	while (Sniffer::kEligible == verdict)
	{
		verdict = CheckCrc(source, offset, dataLength);
		if (Sniffer::kEligible != verdict)
		{
			break;
		}
		offset += 12 + (size_t)dataLength;

		unsigned char bytes[8];
		verdict = Read(source, offset, bytes, 8);
		if (Sniffer::kEligible != verdict)
		{
			break;
		}
		dataLength = GetU32(bytes, true);
		const char *type = (const char *)bytes + 4;
		if (dataLength > kPngMaxValue)
		{
			return Sniffer::kMalformed;
		}
		if (!Fits(source.GetLength(), offset, 12 + (size_t)dataLength))
		{
			return Sniffer::kTruncated;
		}
		if (0 == memcmp(type, "IDAT", 4))
		{
			return CheckEnd(source, kPngEnd, sizeof(kPngEnd));
		}
		if ((0 == memcmp(type, "IHDR", 4)) || (0 == memcmp(type, "IEND", 4)))
		{
			verdict = Sniffer::kMalformed;
		}
		else if ((0 == memcmp(type, "iCCP", 4)) || (0 == memcmp(type, "cICP", 4)) || (0 == memcmp(type, "eXIf", 4)))
		{
			verdict = Sniffer::kColorOrExifChunk;
		}
		else if ((0 == memcmp(type, "sBIT", 4)) && (apiLevel <= 30))
		{
			verdict = Sniffer::kSignificantBits;
		}
		else if (0 == memcmp(type, "tRNS", 4))
		{
			result.hasAlpha = true;
		}
	}
	return verdict;
}

} // namespace


// ----------------------------------------------------------------------------
// MemorySource
// ----------------------------------------------------------------------------

AndroidImageSniffer::MemorySource::MemorySource(const void *bytes, size_t length)
:	fBytes((const unsigned char *)bytes),
	fLength(length)
{
}

size_t
AndroidImageSniffer::MemorySource::GetLength() const
{
	return fLength;
}

bool
AndroidImageSniffer::MemorySource::ReadAt(size_t offset, void *buffer, size_t count) const
{
	if (!Fits(fLength, offset, count))
	{
		return false;
	}
	memcpy(buffer, fBytes + offset, count);
	return true;
}


// ----------------------------------------------------------------------------
// Public static functions
// ----------------------------------------------------------------------------

AndroidImageSniffer::Result
AndroidImageSniffer::Sniff(const ByteSource &source, int apiLevel)
{
	Result result = { kFormatUnknown, kEligible, 0, 0, false };
	unsigned char signature[sizeof(kPngSignature)];
	if (source.GetLength() < kMinimumLength)
	{
		result.verdict = kTooShort;
	}
	else if (!source.ReadAt(0, signature, sizeof(signature)))
	{
		result.verdict = kUnreadable;
	}
	else if ((0xFF == signature[0]) && (0xD8 == signature[1]) && (0xFF == signature[2]))
	{
		result.format = kFormatJpeg;
		result.verdict = CheckJpeg(source, result);
	}
	else if (0 == memcmp(signature, kPngSignature, sizeof(kPngSignature)))
	{
		result.format = kFormatPng;
		result.verdict = CheckPng(source, apiLevel, result);
	}
	else
	{
		result.verdict = kUnknownFormat;
	}
	return result;
}
