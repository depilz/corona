//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md 
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////


#include <unistd.h>
#include "AndroidImageDecoder.h"
#include "AndroidJavaImageDecoder.h"
#include "AndroidNativePngDecoder.h"
#include "AndroidOperationResult.h"
#include "Core/Rtt_Build.h"
#include "Core/Rtt_String.h"
#include "NativeToJavaBridge.h"


// ----------------------------------------------------------------------------
// Constructors/Destructors
// ----------------------------------------------------------------------------

/// Creates a new image decoder.
/// @param allocatorPointer Allocator this decoder needs to creates its objects. Cannot be NULL.
AndroidImageDecoder::AndroidImageDecoder(Rtt_Allocator *allocatorPointer, NativeToJavaBridge * ntjb)
:	AndroidBaseImageDecoder(allocatorPointer),
	fNativeToJavaBridge(ntjb),
	fFileLocation(kUnresolved),
	fPackageEntry(allocatorPointer)
{
}

/// Destroys this decoder and its resources.
AndroidImageDecoder::~AndroidImageDecoder()
{
}


// ----------------------------------------------------------------------------
// Public Member Functions
// ----------------------------------------------------------------------------

/// Looks up where the given PNG file lives, via JNI, so that later decodes of it can skip that lookup.
/// <br>
/// Binds this decoder to the given file: every later decode is expected to be given the same path.
/// The location is kept only if the native PNG decoder can read the file as is, which is
/// a plain file or an uncompressed entry within the APK or expansion file.
/// Must be called on a thread attached to the Java VM.
/// @param filePath The name and path of the image file. Cannot be NULL or empty.
void AndroidImageDecoder::ResolveFileLocation(const char *filePath)
{
	if (!Rtt_StringEndsWithNoCase(filePath, ".png"))
	{
		return;
	}

	if (fNativeToJavaBridge->GetAssetFileLocation(filePath, fPackageEntry))
	{
		if (!fPackageEntry.IsCompressed())
		{
			fFileLocation = kPackageEntry;
		}
	}
	else if (access(filePath, R_OK) == 0)
	{
		fFileLocation = kPlainFile;
	}
}

/// Determines if DecodeNativelyFromFile() can decode the file given to ResolveFileLocation().
/// @return Returns true if the file is a PNG whose location was resolved to a plain file or
///         an uncompressed package entry.
bool AndroidImageDecoder::CanDecodeNatively() const
{
	return (fFileLocation != kUnresolved);
}

/// Decodes the file given to ResolveFileLocation() with the native PNG decoder only.
/// <br>
/// Makes no JNI calls and has no Java fallback, so it may run on a thread that is not attached to the Java VM.
/// @param filePath The name and path of the image file given to ResolveFileLocation().
/// @return Returns the result of the image decoding operation, as described by OnDecodeFromFile().
///         Fails if CanDecodeNatively() returns false.
AndroidOperationResult AndroidImageDecoder::DecodeNativelyFromFile(const char *filePath)
{
	if (!CanDecodeNatively())
	{
		return AndroidOperationResult::FailedWith(GetAllocator(), "Image file location was not resolved.");
	}

	AndroidNativePngDecoder pngDecoder(*this, fNativeToJavaBridge);
	return pngDecoder.DecodeFromResolvedFile(filePath, (kPackageEntry == fFileLocation) ? &fPackageEntry : NULL);
}


// ----------------------------------------------------------------------------
// Protected Member Functions
// ----------------------------------------------------------------------------

/// Decodes the given image file and copies its data to the targeted image data object
/// assigned to this decoder via the SetTarget() function.
/// @param filePath The name and path of the image file to decode. Cannot be NULL or empty.
/// @return Returns the result of the image decoding operation.
///         <br>
///         If the result object's HasSucceeded() function returns true, then this decoder
///         has successfully loaded the image and copied its data to the target image data object.
///         <br>
///         If the result object's HasSucceed() function returns false, then this decoder
///         has failed to decode the image and the targeted image data object might not have been
///         written to or might have incomplete information. The returned result object's
///         GetErrorMessage() function will typically provide a reason why it failed.
AndroidOperationResult AndroidImageDecoder::OnDecodeFromFile(const char *filePath)
{
	AndroidOperationResult result;

	// First, attempt to load the image file natively on the C/C++ side.
	if (CanDecodeNatively())
	{
		result = DecodeNativelyFromFile(filePath);
	}
	else if (Rtt_StringEndsWithNoCase(filePath, ".png"))
	{
		AndroidNativePngDecoder pngDecoder(*this, fNativeToJavaBridge);
		result = pngDecoder.DecodeFromFile(filePath);
	}

	// If we're unable to decode the image natively, then load it on the Java side.
	if (result.HasFailed())
	{
		AndroidJavaImageDecoder javaImageDecoder(*this, fNativeToJavaBridge);
		result = javaImageDecoder.DecodeFromFile(filePath);
	}

	// Return the result.
	return result;
}
