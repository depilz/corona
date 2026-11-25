//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////


#ifndef _AndroidImageSniffer_H__
#define _AndroidImageSniffer_H__

#include <stddef.h>


/// Structural checks of a JPEG or PNG file's bytes, made before the file is given to the
/// system image decoder (AImageDecoder).
/// <br>
/// The bytes decide the format, never the file name. Any file the checks doubt is rejected,
/// so the caller decodes it the old way. The checks are pure: they read only through the given
/// byte source, never past its length, hold no state and are safe to run on any thread.
class AndroidImageSniffer
{
	public:
		/// Bounded random-access bytes of one file.
		class ByteSource
		{
			public:
				virtual ~ByteSource() {}

				/// Number of bytes in the source. The sniffer never reads at or past it.
				virtual size_t GetLength() const = 0;

				/// Copies exactly "count" bytes starting at "offset" into "buffer".
				/// @return Returns false if not all of them could be read.
				virtual bool ReadAt(size_t offset, void *buffer, size_t count) const = 0;
		};

		/// Byte source over a buffer the caller owns, such as a memory mapping.
		class MemorySource : public ByteSource
		{
			public:
				MemorySource(const void *bytes, size_t length);

				virtual size_t GetLength() const;
				virtual bool ReadAt(size_t offset, void *buffer, size_t count) const;

			private:
				const unsigned char *fBytes;
				size_t fLength;
		};

		enum Format
		{
			kFormatUnknown,
			kFormatJpeg,
			kFormatPng
		};

		enum Verdict
		{
			/// Every check passed.
			kEligible,
			/// Shorter than the smallest file that can pass the checks.
			kTooShort,
			/// The source failed to read bytes inside its length.
			kUnreadable,
			/// Neither the JPEG nor the PNG signature.
			kUnknownFormat,
			/// The structure runs past the end, or the file lacks its JPEG EOI or PNG IEND end.
			kTruncated,
			/// A segment, chunk or frame header breaks the format's rules or is unknown, or a PNG chunk
			/// before the first IDAT has a wrong CRC.
			kMalformed,
			/// No JPEG SOF0/SOF1/SOF2 frame before SOS, or a frame with an unsupported precision
			/// or component count.
			kUnsupportedFrame,
			/// An Exif orientation other than 1 in IFD0 or the Exif sub-IFD.
			kExifOrientation,
			/// An Exif block whose TIFF structure cannot be read, or a second Exif block or sub-IFD.
			kExifMalformed,
			/// An interlaced PNG.
			kInterlaced,
			/// A PNG iCCP, cICP or eXIf chunk before the first IDAT.
			kColorOrExifChunk,
			/// A PNG sBIT chunk before the first IDAT on API level 30 or lower.
			kSignificantBits,
			/// More than 1 MiB before the image data, which the checks do not read.
			kHeaderTooLong
		};

		/// What the checks found.
		struct Result
		{
			Format format;
			Verdict verdict;
			/// Size from the JPEG SOF or PNG IHDR; 0 until that header was read.
			int width;
			int height;

			bool IsEligible() const { return kEligible == verdict; }
		};

		/// Smallest file length the checks can accept: a PNG signature, IHDR chunk, empty IDAT
		/// chunk and IEND chunk. Every decodable JPEG is longer: one DQT segment alone is 69 bytes.
		static const size_t kMinimumLength = 57;

		/// Runs the checks.
		/// @param source The file's bytes.
		/// @param apiLevel The device's Android API level; on 30 or lower a PNG sBIT chunk is rejected.
		static Result Sniff(const ByteSource &source, int apiLevel);

	private:
		AndroidImageSniffer();
};

#endif // _AndroidImageSniffer_H__
