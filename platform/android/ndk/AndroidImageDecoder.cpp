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
#include "AndroidSystemImageDecoder.h"
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
	fIsJpegFile(false),
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

/// Looks up where the given PNG or JPEG file lives, via JNI, so that later decodes of it can skip that lookup.
/// <br>
/// Binds this decoder to the given file: every later decode is expected to be given the same path.
/// The location is kept only if the native decoders can read the file as is, which is
/// a plain file or an uncompressed entry within the APK or expansion file.
/// Must be called on a thread attached to the Java VM.
/// @param filePath The name and path of the image file. Cannot be NULL or empty.
void AndroidImageDecoder::ResolveFileLocation(const char *filePath)
{
	fIsJpegFile = Rtt_StringEndsWithNoCase(filePath, ".jpg") || Rtt_StringEndsWithNoCase(filePath, ".jpeg");
	if (!fIsJpegFile && !Rtt_StringEndsWithNoCase(filePath, ".png"))
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

/// Determines if ResolveFileLocation() found a location the native decoders can read the file from.
/// @return Returns true if the file is a PNG or JPEG whose location was resolved to a plain file or
///         an uncompressed package entry.
bool AndroidImageDecoder::IsFileLocationResolved() const
{
	return (fFileLocation != kUnresolved);
}

/// Determines if DecodeNativelyFromFile() can decode the file given to ResolveFileLocation().
/// @return Returns true if the file is a PNG whose location was resolved to a plain file or
///         an uncompressed package entry.
bool AndroidImageDecoder::CanDecodeNatively() const
{
	return IsFileLocationResolved() && !fIsJpegFile;
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

/// Determines if DecodeOffThreadFromFile() can decode the file given to ResolveFileLocation().
/// @return Returns true if CanDecodeNatively() returns true or if the file is a JPEG.
bool AndroidImageDecoder::CanDecodeOffThread() const
{
	return CanDecodeNatively() || fIsJpegFile;
}

/// Decodes the file given to ResolveFileLocation() on a thread that does not run Lua.
/// <br>
/// The system image decoder is tried first. Otherwise a PNG is decoded as by DecodeNativelyFromFile(),
/// and a JPEG is decoded by Java on the calling thread, which gets attached to the Java VM until it exits;
/// a Java exception fails the decode instead of raising a Lua error.
/// @param filePath The name and path of the image file given to ResolveFileLocation().
/// @return Returns the result of the image decoding operation, as described by OnDecodeFromFile().
///         Fails if CanDecodeOffThread() returns false.
AndroidOperationResult AndroidImageDecoder::DecodeOffThreadFromFile(const char *filePath)
{
	AndroidOperationResult result = DecodeWithSystemDecoder(filePath);
	if (result.HasSucceeded())
	{
		return result;
	}
	if (CanDecodeNatively())
	{
		return DecodeNativelyFromFile(filePath);
	}
	if (!fIsJpegFile || !NativeToJavaBridge::AttachCurrentThread())
	{
		return AndroidOperationResult::FailedWith(GetAllocator(), "Image file cannot be decoded off the Lua thread.");
	}

	AndroidJavaImageDecoder javaImageDecoder(*this, fNativeToJavaBridge, false);
	return javaImageDecoder.DecodeFromFile(filePath);
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
	// First, attempt to load the image file with the system image decoder.
	AndroidOperationResult result = DecodeWithSystemDecoder(filePath);
	if (result.HasSucceeded())
	{
		return result;
	}

	// Next, attempt to load the image file natively on the C/C++ side.
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


// ----------------------------------------------------------------------------
// Private Member Functions
// ----------------------------------------------------------------------------

/// Decodes the file given to ResolveFileLocation() with the system image decoder only.
/// <br>
/// Makes no JNI or Lua calls. The system decoder and every file it opened are released before
/// this returns, so a Java fallback that raises a Lua error cannot skip their release.
/// @param filePath The name and path of the image file given to ResolveFileLocation().
/// @return Returns a success result if the system image decoder took the file.
///         Returns a failed result, leaving no pixel buffer in the target, if it did not
///         or if the file's location was not resolved.
AndroidOperationResult AndroidImageDecoder::DecodeWithSystemDecoder(const char *filePath)
{
	if (!IsFileLocationResolved())
	{
		return AndroidOperationResult::FailedWith(GetAllocator(), "Image file location was not resolved.");
	}

	AndroidSystemImageDecoder systemDecoder(*this, (kPackageEntry == fFileLocation) ? &fPackageEntry : NULL);
	return systemDecoder.DecodeFromFile(filePath);
}
