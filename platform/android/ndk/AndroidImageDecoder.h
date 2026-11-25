//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md 
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////


#ifndef _AndroidImageDecoder_H__
#define _AndroidImageDecoder_H__

#include "AndroidBaseImageDecoder.h"
#include "AndroidOperationResult.h"
#include "AndroidZipFileEntry.h"


// Forward declarations.
class AndroidImageData;
class NativeToJavaBridge;
struct Rtt_Allocator;


/// Decodes all image formats supported by Corona by its file extension.
class AndroidImageDecoder : public AndroidBaseImageDecoder
{
	public:
		AndroidImageDecoder(Rtt_Allocator *allocatorPointer, NativeToJavaBridge *ntjb);
		virtual ~AndroidImageDecoder();

		void ResolveFileLocation(const char *filePath);
		bool IsFileLocationResolved() const;
		bool CanDecodeNatively() const;
		AndroidOperationResult DecodeNativelyFromFile(const char *filePath);
		bool CanDecodeOffThread() const;
		AndroidOperationResult DecodeOffThreadFromFile(const char *filePath);

	protected:
		AndroidOperationResult OnDecodeFromFile(const char *filePath);

	private:
		AndroidOperationResult DecodeWithSystemDecoder(const char *filePath);

		enum FileLocation
		{
			kUnresolved,
			kPlainFile,
			kPackageEntry
		};

		NativeToJavaBridge *fNativeToJavaBridge;
		FileLocation fFileLocation;
		bool fIsJpegFile;
		AndroidZipFileEntry fPackageEntry;
};

#endif // _AndroidImageDecoder_H__
