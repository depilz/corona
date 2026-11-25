//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////


#ifndef _AndroidSystemImageDecoder_H__
#define _AndroidSystemImageDecoder_H__

#include "AndroidBaseImageDecoder.h"
#include "AndroidOperationResult.h"


// Forward declarations.
class AndroidZipFileEntry;


/// Decodes a JPEG or PNG file with the system image decoder (AImageDecoder), on Android 11 (API level 30)
/// and newer, which it loads from "libjnigraphics.so" at run time.
/// <br>
/// It takes only files the AndroidImageSniffer checks accept, decoded at full size to RGBA with the
/// same pixels as the old decoders. Every other file, older Android versions, and every failure, fail
/// the decode, leaving no pixel buffer behind, so the caller decodes the file the old way.
/// <br>
/// Makes no JNI or Lua calls and raises no Lua error, so it may run on any thread.
/// Every file, mapping and system decoder it opens is closed before the decode returns.
class AndroidSystemImageDecoder : public AndroidBaseImageDecoder
{
	public:
		AndroidSystemImageDecoder(
			const AndroidBaseImageDecoder &decoder, const AndroidZipFileEntry *packageEntry, bool acceptsPng);
		virtual ~AndroidSystemImageDecoder();

		static bool IsAvailable();

	protected:
		AndroidOperationResult OnDecodeFromFile(const char *filePath);

	private:
		const AndroidZipFileEntry *fPackageEntry;
		bool fAcceptsPng;
};

#endif // _AndroidSystemImageDecoder_H__
