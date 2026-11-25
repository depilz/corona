//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////


#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include "AndroidSystemImageDecoder.h"
#include "AndroidImageData.h"
#include "AndroidImageSniffer.h"
#include "AndroidOperationResult.h"
#include "AndroidZipFileEntry.h"
#include "Core/Rtt_Build.h"


// ----------------------------------------------------------------------------
// Private helpers
// ----------------------------------------------------------------------------

namespace
{

typedef AndroidImageSniffer Sniffer;

/// Opaque AImageDecoder types; the NDK this engine builds with has no "android/imagedecoder.h".
struct SystemDecoder;
struct SystemHeaderInfo;

/// Values from the NDK's "android/imagedecoder.h" and "android/bitmap.h".
enum
{
	kMinimumApiLevel = 30,
	kDecoderSuccess = 0,
	kBitmapFormatRgba8888 = 1,
	kAlphaFlagsOpaque = 1,
	kDataSpaceUnknown = 0,
	kBytesPerPixel = 4
};

/// The AImageDecoder functions this decoder calls, resolved from "libjnigraphics.so".
/// Usable only if "isValid" is set: on API level 30 or newer with every function resolved.
struct SystemDecoderApi
{
	bool isValid;
	int apiLevel;
	int (*createFromBuffer)(const void *buffer, size_t length, SystemDecoder **decoder);
	int (*createFromFd)(int fd, SystemDecoder **decoder);
	void (*destroy)(SystemDecoder *decoder);
	const SystemHeaderInfo* (*getHeaderInfo)(const SystemDecoder *decoder);
	int32_t (*getWidth)(const SystemHeaderInfo *info);
	int32_t (*getHeight)(const SystemHeaderInfo *info);
	const char* (*getMimeType)(const SystemHeaderInfo *info);
	int (*getAlphaFlags)(const SystemHeaderInfo *info);
	int32_t (*getDataSpace)(const SystemHeaderInfo *info);
	int (*setAndroidBitmapFormat)(SystemDecoder *decoder, int32_t format);
	int (*setUnpremultipliedRequired)(SystemDecoder *decoder, bool isRequired);
	size_t (*getMinimumStride)(SystemDecoder *decoder);
	int (*decodeImage)(SystemDecoder *decoder, void *pixels, size_t stride, size_t size);
};

/// Reads the device's API level as a decimal number.
/// @return Returns 0 if the property is missing or not a decimal number.
int ReadApiLevel()
{
	char value[PROP_VALUE_MAX] = { 0 };
	int length = __system_property_get("ro.build.version.sdk", value);
	if ((length <= 0) || (length > 9))
	{
		return 0;
	}
	int apiLevel = 0;
	for (int index = 0; index < length; index++)
	{
		if ((value[index] < '0') || (value[index] > '9'))
		{
			return 0;
		}
		apiLevel = (apiLevel * 10) + (value[index] - '0');
	}
	return apiLevel;
}

template<typename Function>
bool Resolve(void *library, const char *name, Function &function)
{
	function = reinterpret_cast<Function>(dlsym(library, name));
	return (function != NULL);
}

SystemDecoderApi LoadApi()
{
	SystemDecoderApi api;
	memset(&api, 0, sizeof(api));
	api.apiLevel = ReadApiLevel();
	if (api.apiLevel < kMinimumApiLevel)
	{
		return api;
	}
	void *library = dlopen("libjnigraphics.so", RTLD_NOW | RTLD_LOCAL);
	if (!library)
	{
		return api;
	}
	api.isValid =
			Resolve(library, "AImageDecoder_createFromBuffer", api.createFromBuffer) &&
			Resolve(library, "AImageDecoder_createFromFd", api.createFromFd) &&
			Resolve(library, "AImageDecoder_delete", api.destroy) &&
			Resolve(library, "AImageDecoder_getHeaderInfo", api.getHeaderInfo) &&
			Resolve(library, "AImageDecoderHeaderInfo_getWidth", api.getWidth) &&
			Resolve(library, "AImageDecoderHeaderInfo_getHeight", api.getHeight) &&
			Resolve(library, "AImageDecoderHeaderInfo_getMimeType", api.getMimeType) &&
			Resolve(library, "AImageDecoderHeaderInfo_getAlphaFlags", api.getAlphaFlags) &&
			Resolve(library, "AImageDecoderHeaderInfo_getDataSpace", api.getDataSpace) &&
			Resolve(library, "AImageDecoder_setAndroidBitmapFormat", api.setAndroidBitmapFormat) &&
			Resolve(library, "AImageDecoder_setUnpremultipliedRequired", api.setUnpremultipliedRequired) &&
			Resolve(library, "AImageDecoder_getMinimumStride", api.getMinimumStride) &&
			Resolve(library, "AImageDecoder_decodeImage", api.decodeImage);
	if (!api.isValid)
	{
		dlclose(library);
	}
	return api;
}

/// Gets the AImageDecoder functions, loaded once per process on first use, from any thread.
const SystemDecoderApi& GetApi()
{
	static const SystemDecoderApi sApi = LoadApi();
	return sApi;
}

/// Owns a file descriptor and closes it when it goes out of scope.
class ScopedFile
{
	public:
		explicit ScopedFile(const char *path) : fDescriptor(open(path, O_RDONLY | O_CLOEXEC)) {}
		~ScopedFile() { if (fDescriptor >= 0) { close(fDescriptor); } }

		int GetDescriptor() const { return fDescriptor; }

		/// Gets the size of a regular file.
		/// @return Returns false if the file is not open or not a regular file.
		bool GetSize(off_t &size) const
		{
			struct stat status;
			if ((fDescriptor < 0) || (fstat(fDescriptor, &status) != 0) || !S_ISREG(status.st_mode))
			{
				return false;
			}
			size = status.st_size;
			return true;
		}

	private:
		ScopedFile(const ScopedFile&);
		void operator=(const ScopedFile&);

		int fDescriptor;
};

/// Maps a byte range of a file read-only, and unmaps it when it goes out of scope.
class ScopedMapping
{
	public:
		ScopedMapping(int descriptor, off_t offset, size_t length)
		:	fAddress(MAP_FAILED),
			fMappedLength(0),
			fBytes(NULL)
		{
			off_t pageOffset = offset - (offset % (off_t)sysconf(_SC_PAGESIZE));
			size_t lead = (size_t)(offset - pageOffset);
			fAddress = mmap(NULL, lead + length, PROT_READ, MAP_PRIVATE, descriptor, pageOffset);
			if (fAddress != MAP_FAILED)
			{
				fMappedLength = lead + length;
				fBytes = (const unsigned char *)fAddress + lead;
			}
		}
		~ScopedMapping() { if (fAddress != MAP_FAILED) { munmap(fAddress, fMappedLength); } }

		/// @return Returns NULL if the mapping failed.
		const unsigned char* GetBytes() const { return fBytes; }

	private:
		ScopedMapping(const ScopedMapping&);
		void operator=(const ScopedMapping&);

		void *fAddress;
		size_t fMappedLength;
		const unsigned char *fBytes;
};

/// Owns a system decoder and deletes it when it goes out of scope.
class ScopedDecoder
{
	public:
		explicit ScopedDecoder(const SystemDecoderApi &api) : fApi(api), fDecoder(NULL) {}
		~ScopedDecoder() { if (fDecoder) { fApi.destroy(fDecoder); } }

		SystemDecoder* Get() const { return fDecoder; }
		SystemDecoder** GetAddress() { return &fDecoder; }

	private:
		ScopedDecoder(const ScopedDecoder&);
		void operator=(const ScopedDecoder&);

		const SystemDecoderApi &fApi;
		SystemDecoder *fDecoder;
};

/// Byte source over a byte range of an open file, read with pread() so a file changed while it is read cannot crash.
/// Reads ahead into a small window, since an image's structure is read in many small pieces.
class FileSource : public Sniffer::ByteSource
{
	public:
		FileSource(int descriptor, off_t start, size_t length)
		:	fDescriptor(descriptor), fStart(start), fLength(length), fWindowOffset(0), fWindowLength(0)
		{
		}

		virtual size_t GetLength() const
		{
			return fLength;
		}

		virtual bool ReadAt(size_t offset, void *buffer, size_t count) const
		{
			if ((offset > fLength) || (count > fLength - offset))
			{
				return false;
			}
			if (count > sizeof(fWindow))
			{
				return ReadFile(offset, buffer, count);
			}
			if ((offset < fWindowOffset) || (offset + count > fWindowOffset + fWindowLength))
			{
				size_t length = (fLength - offset < sizeof(fWindow)) ? (fLength - offset) : sizeof(fWindow);
				fWindowLength = 0;
				if (!ReadFile(offset, fWindow, length))
				{
					return false;
				}
				fWindowOffset = offset;
				fWindowLength = length;
			}
			memcpy(buffer, fWindow + (offset - fWindowOffset), count);
			return true;
		}

	private:
		bool ReadFile(size_t offset, void *buffer, size_t count) const
		{
			for (size_t done = 0; done < count;)
			{
				ssize_t readCount = pread(fDescriptor, (char *)buffer + done, count - done, fStart + (off_t)(offset + done));
				if (readCount > 0)
				{
					done += (size_t)readCount;
				}
				else if ((readCount == 0) || (errno != EINTR))
				{
					return false;
				}
			}
			return true;
		}

		int fDescriptor;
		off_t fStart;
		size_t fLength;
		mutable size_t fWindowOffset;
		mutable size_t fWindowLength;
		mutable unsigned char fWindow[1024];
};

/// Multiplies each RGBA pixel's colour by its alpha the way the engine's libpng decoder does, (c * a) >> 8.
void PremultiplyByAlpha(unsigned char *pixels, size_t pixelCount)
{
	for (unsigned char *pixel = pixels, *end = pixels + (pixelCount * kBytesPerPixel); pixel < end; pixel += kBytesPerPixel)
	{
		int alpha = pixel[3];
		pixel[0] = (unsigned char)((pixel[0] * alpha) >> 8);
		pixel[1] = (unsigned char)((pixel[1] * alpha) >> 8);
		pixel[2] = (unsigned char)((pixel[2] * alpha) >> 8);
	}
}

/// Checks the image's size against the decode settings: never downscaled, and small enough for the engine's pixel buffer.
bool IsSizeAccepted(int32_t width, int32_t height, const AndroidBaseImageDecoder &settings)
{
	if ((width <= 0) || (height <= 0))
	{
		return false;
	}
	if (((settings.GetMaxWidth() > 0) && ((U32)width > settings.GetMaxWidth())) ||
	    ((settings.GetMaxHeight() > 0) && ((U32)height > settings.GetMaxHeight())))
	{
		return false;
	}
	return ((uint64_t)width * (uint64_t)height * kBytesPerPixel <= UINT32_MAX);
}

/// Checks the system decoder's header against what the sniffer read and against the decode settings.
bool IsHeaderAccepted(
	const SystemDecoderApi &api, const SystemHeaderInfo &info, const Sniffer::Result &sniffed,
	const AndroidBaseImageDecoder &settings)
{
	bool isJpeg = (Sniffer::kFormatJpeg == sniffed.format);
	const char *mimeType = api.getMimeType(&info);
	if (!mimeType || strcmp(mimeType, isJpeg ? "image/jpeg" : "image/png"))
	{
		return false;
	}
	if (isJpeg && (kDataSpaceUnknown == api.getDataSpace(&info)))
	{
		return false;
	}

	// The size must be the sniffed one, which the header pass gave the target.
	int32_t width = api.getWidth(&info);
	int32_t height = api.getHeight(&info);
	return (width == sniffed.width) && (height == sniffed.height) && IsSizeAccepted(width, height, settings);
}

/// Fills the settings' target with the image's information as the sniffer read it from the file:
/// its full size, a scale of 1, upright orientation and RGBA format.
bool ReadInfo(const Sniffer::Result &sniffed, const AndroidBaseImageDecoder &settings)
{
	if (!IsSizeAccepted(sniffed.width, sniffed.height, settings))
	{
		return false;
	}
	AndroidImageData &target = *settings.GetTarget();
	target.SetWidth((U32)sniffed.width);
	target.SetHeight((U32)sniffed.height);
	target.SetScale(Rtt_REAL_1);
	target.SetOrientation(Rtt::PlatformBitmap::kUp);
	target.SetPixelFormatToRGBA();
	return true;
}

/// Decodes the pixels into a new buffer of the target, which must already have the decoder's size.
/// Destroys the buffer if the decode fails.
bool DecodePixels(
	const SystemDecoderApi &api, SystemDecoder *decoder, bool isPng, bool isOpaque, AndroidImageData &target)
{
	size_t stride = (size_t)target.GetWidth() * kBytesPerPixel;
	size_t size = stride * target.GetHeight();
	if ((api.setAndroidBitmapFormat(decoder, kBitmapFormatRgba8888) != kDecoderSuccess) ||
	    (isPng && (api.setUnpremultipliedRequired(decoder, true) != kDecoderSuccess)) ||
	    (api.getMinimumStride(decoder) != stride) ||
	    !target.CreateImageByteBuffer())
	{
		return false;
	}
	if (api.decodeImage(decoder, target.GetImageByteBuffer(), stride, size) != kDecoderSuccess)
	{
		target.DestroyImageByteBuffer();
		return false;
	}
	if (isPng && !isOpaque)
	{
		PremultiplyByAlpha(target.GetImageByteBuffer(), size / kBytesPerPixel);
	}
	return true;
}

/// Decodes the image's pixels into the settings' target.
/// Taken only if the target already has the size the header pass gave it.
bool DecodeWith(
	const SystemDecoderApi &api, SystemDecoder *decoder, const Sniffer::Result &sniffed,
	const AndroidBaseImageDecoder &settings)
{
	const SystemHeaderInfo *info = api.getHeaderInfo(decoder);
	if (!info || !IsHeaderAccepted(api, *info, sniffed, settings))
	{
		return false;
	}

	AndroidImageData &target = *settings.GetTarget();
	if ((target.GetWidth() != (U32)sniffed.width) || (target.GetHeight() != (U32)sniffed.height) ||
	    !target.IsPixelFormatRGBA())
	{
		return false;
	}
	bool isPng = (Sniffer::kFormatPng == sniffed.format);
	return DecodePixels(api, decoder, isPng, (kAlphaFlagsOpaque == api.getAlphaFlags(info)), target);
}

/// Determines if the sniffed file may be taken.
/// @param acceptsPng Set to false to refuse PNG content.
bool IsFileAccepted(const Sniffer::Result &sniffed, bool acceptsPng)
{
	return sniffed.IsEligible() && (acceptsPng || (Sniffer::kFormatPng != sniffed.format));
}

/// Decodes a plain file, read through its own descriptor.
bool DecodePlainFile(
	const SystemDecoderApi &api, const char *filePath, bool acceptsPng, const AndroidBaseImageDecoder &settings)
{
	ScopedFile file(filePath);
	off_t fileSize = 0;
	if (!file.GetSize(fileSize))
	{
		return false;
	}
	Sniffer::Result sniffed = Sniffer::Sniff(FileSource(file.GetDescriptor(), 0, (size_t)fileSize), api.apiLevel);
	if (!IsFileAccepted(sniffed, acceptsPng))
	{
		return false;
	}
	if (settings.IsDecodingImageInfoOnly())
	{
		return ReadInfo(sniffed, settings);
	}

	// The system decoder reads from the descriptor's offset, which pread() left at the start of the file.
	ScopedDecoder decoder(api);
	if (api.createFromFd(file.GetDescriptor(), decoder.GetAddress()) != kDecoderSuccess)
	{
		return false;
	}
	return DecodeWith(api, decoder.Get(), sniffed, settings);
}

/// Decodes an uncompressed entry of the APK or expansion file: its information is read with pread(),
/// its pixels from a memory mapping of its bytes.
bool DecodePackageEntry(
	const SystemDecoderApi &api, const AndroidZipFileEntry &entry, bool acceptsPng,
	const AndroidBaseImageDecoder &settings)
{
	long offset = entry.GetByteOffsetInPackage();
	long length = entry.GetByteCountInPackage();
	if (entry.IsCompressed() || (offset < 0) || (length <= 0))
	{
		return false;
	}

	// Map only bytes inside the file, since touching a mapped page past its end would crash.
	ScopedFile file(entry.GetPackageFilePath());
	off_t fileSize = 0;
	if (!file.GetSize(fileSize) || ((off_t)length > fileSize - (off_t)offset))
	{
		return false;
	}
	if (settings.IsDecodingImageInfoOnly())
	{
		Sniffer::Result sniffed = Sniffer::Sniff(FileSource(file.GetDescriptor(), (off_t)offset, (size_t)length), api.apiLevel);
		return IsFileAccepted(sniffed, acceptsPng) && ReadInfo(sniffed, settings);
	}
	ScopedMapping mapping(file.GetDescriptor(), (off_t)offset, (size_t)length);
	if (!mapping.GetBytes())
	{
		return false;
	}
	Sniffer::Result sniffed = Sniffer::Sniff(Sniffer::MemorySource(mapping.GetBytes(), (size_t)length), api.apiLevel);
	if (!IsFileAccepted(sniffed, acceptsPng))
	{
		return false;
	}

	// The system decoder reads the mapping without copying it, so it must be deleted first.
	ScopedDecoder decoder(api);
	if (api.createFromBuffer(mapping.GetBytes(), (size_t)length, decoder.GetAddress()) != kDecoderSuccess)
	{
		return false;
	}
	return DecodeWith(api, decoder.Get(), sniffed, settings);
}

} // namespace


// ----------------------------------------------------------------------------
// Constructors/Destructors
// ----------------------------------------------------------------------------

/// Creates a new image decoder with settings copied from the given decoder.
/// @param decoder The decoder to copy settings from.
/// @param packageEntry The uncompressed APK or expansion file entry holding the image's bytes,
///                     as resolved by NativeToJavaBridge::GetAssetFileLocation(), which must outlive this decoder.
///                     <br>
///                     Set to NULL if the file path given to DecodeFromFile() is a plain file.
/// @param acceptsPng Set to false for a file named as a JPEG. Java decodes PNG content in such a file,
///                   multiplying by alpha differently and not loading 16-bit channels.
AndroidSystemImageDecoder::AndroidSystemImageDecoder(
	const AndroidBaseImageDecoder &decoder, const AndroidZipFileEntry *packageEntry, bool acceptsPng)
:	AndroidBaseImageDecoder(decoder),
	fPackageEntry(packageEntry),
	fAcceptsPng(acceptsPng)
{
}

/// Destroys this decoder and its resources.
AndroidSystemImageDecoder::~AndroidSystemImageDecoder()
{
}


// ----------------------------------------------------------------------------
// Public Static Functions
// ----------------------------------------------------------------------------

/// Determines if this Android version has the system image decoder.
/// @return Returns false if every decode would fail, whatever the file.
bool AndroidSystemImageDecoder::IsAvailable()
{
	return GetApi().isValid;
}


// ----------------------------------------------------------------------------
// Protected Member Functions
// ----------------------------------------------------------------------------

/// Reads the image's information, or decodes its pixels, into the targeted image data object.
/// <br>
/// The information is the image's full size, a scale of 1, upright orientation and RGBA format.
/// The pixels are decoded only if the target already holds the size of that information.
/// @param filePath The name and path of the image file. Read only if no package entry was given.
/// @return Returns a success result if the image was read with the system image decoder.
///         <br>
///         Returns a failed result if the system image decoder did not take the file, in which case
///         the target holds no new pixel buffer.
AndroidOperationResult AndroidSystemImageDecoder::OnDecodeFromFile(const char *filePath)
{
	const SystemDecoderApi &api = GetApi();
	bool isDecoded = false;
	if (api.isValid && IsPixelFormatRGBA())
	{
		isDecoded = fPackageEntry ?
				DecodePackageEntry(api, *fPackageEntry, fAcceptsPng, *this) :
				DecodePlainFile(api, filePath, fAcceptsPng, *this);
	}
	if (!isDecoded)
	{
		return AndroidOperationResult::FailedWith(GetAllocator(), "The system image decoder did not take the file.");
	}
	return AndroidOperationResult::Succeeded(GetAllocator());
}
