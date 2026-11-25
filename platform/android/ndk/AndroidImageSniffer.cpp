//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////


#include "AndroidImageSniffer.h"
#include <stdint.h>
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

/// Largest width and height the system's decoders read.
const unsigned int kJpegMaxSize = 65500;
const unsigned long kPngMaxSize = 1000000;

/// Largest length of a PNG chunk that every libpng version takes, whatever the image's size.
const unsigned long kPngMaxChunkLength = 8000000;

/// The checks stop at this many bytes before the image data, so that their cost has a bound.
const size_t kMaxHeaderLength = 1024 * 1024;

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
/// (tag 0x8769) the same way when "followExifIfd" is set. An IFD may have only one of those,
/// so every entry is read at most twice. IFD1 (the thumbnail) is not read.
Sniffer::Verdict CheckIfd(const TiffBlock &tiff, unsigned long ifdOffset, bool followExifIfd)
{
	bool hasExifIfd = false;
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
			if (hasExifIfd || ((type != 4) && (type != 13)) || (count != 1))
			{
				return Sniffer::kExifMalformed;
			}
			hasExifIfd = true;
			verdict = CheckIfd(tiff, GetU32(bytes + 8, tiff.isBigEndian), false);
			if (Sniffer::kEligible != verdict)
			{
				return verdict;
			}
		}
	}
	return Sniffer::kEligible;
}

/// Checks an APP1 segment's payload; one that is not Exif passes. A file may have only one that is.
Sniffer::Verdict CheckApp1(const Sniffer::ByteSource &source, size_t offset, size_t length, bool &hasExif)
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
	if (hasExif)
	{
		return Sniffer::kExifMalformed;
	}
	hasExif = true;

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

const unsigned int kJpegMaxComponents = 4;

/// The component IDs of a JPEG frame; a component count of 0 means it was not read yet.
struct JpegFrame
{
	unsigned int componentCount;
	unsigned char componentIds[kJpegMaxComponents];

	bool HasComponent(unsigned char id) const
	{
		return (componentCount > 0) && (NULL != memchr(componentIds, id, componentCount));
	}
};

/// Reads a SOF0, SOF1 or SOF2 frame header: its size and its components, whose IDs must differ.
Sniffer::Verdict ReadFrame(
	const Sniffer::ByteSource &source, size_t offset, size_t length, Sniffer::Result &result, JpegFrame &frame)
{
	unsigned char bytes[6 + (3 * kJpegMaxComponents)];
	if (frame.componentCount > 0)
	{
		return Sniffer::kMalformed;
	}
	Sniffer::Verdict verdict = (length < 6) ? Sniffer::kMalformed : Read(source, offset, bytes, 6);
	if (Sniffer::kEligible != verdict)
	{
		return verdict;
	}
	unsigned int height = GetU16(bytes + 1, true);
	unsigned int width = GetU16(bytes + 3, true);
	unsigned int componentCount = bytes[5];
	if ((bytes[0] != 8) || (0 == height) || (2 == componentCount))
	{
		return Sniffer::kUnsupportedFrame;
	}
	if ((0 == width) || (width > kJpegMaxSize) || (height > kJpegMaxSize)
		|| (componentCount < 1) || (componentCount > kJpegMaxComponents) || (length != 6 + (3 * componentCount)))
	{
		return Sniffer::kMalformed;
	}
	verdict = Read(source, offset + 6, bytes + 6, 3 * componentCount);
	if (Sniffer::kEligible != verdict)
	{
		return verdict;
	}
	for (unsigned int index = 0; index < componentCount; index++)
	{
		const unsigned char *component = bytes + 6 + (3 * index);
		unsigned int horizontalSampling = component[1] >> 4;
		unsigned int verticalSampling = component[1] & 0x0F;
		if (frame.HasComponent(component[0]) || (horizontalSampling < 1) || (horizontalSampling > 4)
			|| (verticalSampling < 1) || (verticalSampling > 4))
		{
			return Sniffer::kMalformed;
		}
		frame.componentIds[frame.componentCount++] = component[0];
	}
	result.width = (int)width;
	result.height = (int)height;
	return Sniffer::kEligible;
}

/// Checks a DHT segment: Huffman tables with a valid index and length that fill the segment.
Sniffer::Verdict CheckHuffmanTables(const Sniffer::ByteSource &source, size_t offset, size_t length)
{
	while (length > 0)
	{
		unsigned char bytes[17];
		Sniffer::Verdict verdict = (length < sizeof(bytes)) ? Sniffer::kMalformed : Read(source, offset, bytes, sizeof(bytes));
		if (Sniffer::kEligible != verdict)
		{
			return verdict;
		}
		size_t symbolCount = 0;
		for (int index = 1; index <= 16; index++)
		{
			symbolCount += bytes[index];
		}
		if (((bytes[0] & 0xEC) != 0) || (symbolCount > 256) || (symbolCount > length - sizeof(bytes)))
		{
			return Sniffer::kMalformed;
		}
		offset += sizeof(bytes) + symbolCount;
		length -= sizeof(bytes) + symbolCount;
	}
	return Sniffer::kEligible;
}

/// Checks a DQT segment: 8-bit or 16-bit quantization tables with a valid index that fill the segment.
Sniffer::Verdict CheckQuantizationTables(const Sniffer::ByteSource &source, size_t offset, size_t length)
{
	while (length > 0)
	{
		unsigned char header;
		Sniffer::Verdict verdict = Read(source, offset, &header, 1);
		if (Sniffer::kEligible != verdict)
		{
			return verdict;
		}
		size_t tableLength = (header & 0x10) ? 129 : 65;
		if (((header & 0xEC) != 0) || (tableLength > length))
		{
			return Sniffer::kMalformed;
		}
		offset += tableLength;
		length -= tableLength;
	}
	return Sniffer::kEligible;
}

/// Checks a SOS segment: its components are in the frame, in the frame's order.
Sniffer::Verdict CheckScan(const Sniffer::ByteSource &source, size_t offset, size_t length, const JpegFrame &frame)
{
	unsigned char bytes[4 + (2 * kJpegMaxComponents)];
	if (0 == frame.componentCount)
	{
		return Sniffer::kUnsupportedFrame;
	}
	if ((length < 6) || (length > sizeof(bytes)))
	{
		return Sniffer::kMalformed;
	}
	Sniffer::Verdict verdict = Read(source, offset, bytes, length);
	if (Sniffer::kEligible != verdict)
	{
		return verdict;
	}
	unsigned int componentCount = bytes[0];
	if ((componentCount < 1) || (componentCount > frame.componentCount) || (length != 4 + (2 * componentCount)))
	{
		return Sniffer::kMalformed;
	}
	unsigned int frameIndex = 0;
	for (unsigned int index = 0; index < componentCount; index++)
	{
		unsigned char id = bytes[1 + (2 * index)];
		while ((frameIndex < frame.componentCount) && (frame.componentIds[frameIndex] != id))
		{
			frameIndex++;
		}
		if (frameIndex == frame.componentCount)
		{
			return Sniffer::kMalformed;
		}
		frameIndex++;
	}
	return Sniffer::kEligible;
}

/// Walks the JPEG segments from SOI up to SOS: one SOF0/1/2 frame, valid tables, no doubtful Exif APP1
/// and no marker that the system's decoder rejects or that the checks do not know.
Sniffer::Verdict CheckJpeg(const Sniffer::ByteSource &source, Sniffer::Result &result)
{
	JpegFrame frame = JpegFrame();
	bool hasExif = false;
	size_t offset = 2;
	for (;;)
	{
		if (offset > kMaxHeaderLength)
		{
			return Sniffer::kHeaderTooLong;
		}
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

		// Markers below SOF0 are unknown, and RSTn, SOI and EOI have no length and may not come before SOS.
		if ((marker < 0xC0) || ((marker >= 0xD0) && (marker <= 0xD9)))
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
				verdict = CheckScan(source, payloadOffset, payloadLength, frame);
				return (Sniffer::kEligible == verdict) ? CheckEnd(source, (const unsigned char *)"\xFF\xD9", 2) : verdict;
			case 0xC0:
			case 0xC1:
			case 0xC2:
				verdict = ReadFrame(source, payloadOffset, payloadLength, result, frame);
				break;
			case 0xC4:
				verdict = CheckHuffmanTables(source, payloadOffset, payloadLength);
				break;
			case 0xDB:
				verdict = CheckQuantizationTables(source, payloadOffset, payloadLength);
				break;
			case 0xDD:
				verdict = (2 == payloadLength) ? Sniffer::kEligible : Sniffer::kMalformed;
				break;
			case 0xE1:
				verdict = CheckApp1(source, payloadOffset, payloadLength, hasExif);
				break;
			case 0xC3:
			case 0xC5: case 0xC6: case 0xC7:
			case 0xC9: case 0xCA: case 0xCB: case 0xCC:
			case 0xCD: case 0xCE: case 0xCF:
				verdict = Sniffer::kUnsupportedFrame;
				break;
			default:
				// APPn and COM are skipped.
				verdict = (((marker & 0xF0) == 0xE0) || (0xFE == marker)) ? Sniffer::kEligible : Sniffer::kMalformed;
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

/// What the PNG chunks before the first IDAT hold.
struct PngChunks
{
	unsigned int colorType;
	unsigned int bitDepth;
	unsigned long paletteLength;
	bool hasTransparency;
};

/// Reads the IHDR chunk that must follow the signature.
Sniffer::Verdict ReadHeader(const Sniffer::ByteSource &source, Sniffer::Result &result, PngChunks &chunks)
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
		|| (0 == width) || (width > kPngMaxSize) || (0 == height) || (height > kPngMaxSize)
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
	chunks.colorType = data[9];
	chunks.bitDepth = data[8];
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

/// Checks a PLTE chunk: at most one, of whole entries, as many as the image's pixels can use, in a colour image.
Sniffer::Verdict CheckPalette(unsigned long dataLength, PngChunks &chunks)
{
	unsigned long maxEntryCount = (3 == chunks.colorType) ? (1UL << chunks.bitDepth) : 256;
	if ((0 == chunks.colorType) || (4 == chunks.colorType) || (chunks.paletteLength > 0) || chunks.hasTransparency
		|| (0 == dataLength) || ((dataLength % 3) != 0) || (dataLength > 3 * maxEntryCount))
	{
		return Sniffer::kMalformed;
	}
	chunks.paletteLength = dataLength;
	return Sniffer::kEligible;
}

/// Checks a tRNS chunk: at most one, of the length its colour type needs, after the palette.
Sniffer::Verdict CheckTransparency(unsigned long dataLength, PngChunks &chunks)
{
	bool isValid = false;
	switch (chunks.colorType)
	{
		case 0:
			isValid = (2 == dataLength);
			break;
		case 2:
			isValid = (6 == dataLength);
			break;
		case 3:
			isValid = (dataLength > 0) && (3 * dataLength <= chunks.paletteLength);
			break;
		default:
			break;
	}
	if (!isValid || chunks.hasTransparency)
	{
		return Sniffer::kMalformed;
	}
	chunks.hasTransparency = true;
	return Sniffer::kEligible;
}

/// Checks the first IDAT chunk: after the palette that its colour type needs, and not longer than libpng takes.
Sniffer::Verdict CheckImageData(unsigned long dataLength, const Sniffer::Result &result, const PngChunks &chunks)
{
	static const unsigned int kChannelCounts[] = { 1, 0, 3, 1, 2, 0, 4 };
	uint64_t rowLength = ((uint64_t)result.width * kChannelCounts[chunks.colorType] * ((16 == chunks.bitDepth) ? 2 : 1)) + 1;
	if ((3 == chunks.colorType) && (0 == chunks.paletteLength))
	{
		return Sniffer::kMalformed;
	}
	if ((dataLength > kPngMaxChunkLength) && (dataLength > rowLength * (uint64_t)result.height))
	{
		return Sniffer::kMalformed;
	}
	return Sniffer::kEligible;
}

/// Determines if all 4 bytes of a chunk type are ASCII letters, the third one uppercase.
bool IsValidChunkType(const char *type)
{
	if ((type[2] & 0x20) != 0)
	{
		return false;
	}
	for (int index = 0; index < 4; index++)
	{
		char letter = (char)(type[index] | 0x20);
		if ((letter < 'a') || (letter > 'z'))
		{
			return false;
		}
	}
	return true;
}

/// Walks the PNG chunks from IHDR up to the first IDAT, checking the CRC of each chunk before it.
/// Rejects every file that libpng would not read up to there, and every chunk order or length
/// that its versions may read differently.
Sniffer::Verdict CheckPng(const Sniffer::ByteSource &source, int apiLevel, Sniffer::Result &result)
{
	PngChunks chunks = PngChunks();
	Sniffer::Verdict verdict = ReadHeader(source, result, chunks);
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
		if (offset > kMaxHeaderLength)
		{
			return Sniffer::kHeaderTooLong;
		}

		unsigned char bytes[8];
		verdict = Read(source, offset, bytes, 8);
		if (Sniffer::kEligible != verdict)
		{
			break;
		}
		dataLength = GetU32(bytes, true);
		const char *type = (const char *)bytes + 4;
		if ((dataLength > kPngMaxValue) || !IsValidChunkType(type))
		{
			return Sniffer::kMalformed;
		}
		if (!Fits(source.GetLength(), offset, 12 + (size_t)dataLength))
		{
			return Sniffer::kTruncated;
		}
		if (0 == memcmp(type, "IDAT", 4))
		{
			verdict = CheckImageData(dataLength, result, chunks);
			return (Sniffer::kEligible == verdict) ? CheckEnd(source, kPngEnd, sizeof(kPngEnd)) : verdict;
		}
		if (offset + (size_t)dataLength > kMaxHeaderLength)
		{
			return Sniffer::kHeaderTooLong;
		}
		if (0 == memcmp(type, "PLTE", 4))
		{
			verdict = CheckPalette(dataLength, chunks);
		}
		else if (0 == (type[0] & 0x20))
		{
			// A critical chunk that may not come here, or that is unknown.
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
			verdict = CheckTransparency(dataLength, chunks);
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
	Result result = { kFormatUnknown, kEligible, 0, 0 };
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
