#ifndef SPINE_SOLAR2D_H_
#define SPINE_SOLAR2D_H_

#include <spine/spine.h>

#include <spine/SkeletonRenderer.h>

// #if SOLAR2D_VERSION < 0x00040000
// #include <spine/v3/SkeletonBatch.h>
// #include <spine/v3/SkeletonTwoColorBatch.h>
// #else
#include <spine/v4/SkeletonBatch.h>
#include <spine/v4/SkeletonTwoColorBatch.h>
// #endif

#include <spine/SkeletonAnimation.h>

namespace spine {
	class Solar2dAtlasAttachmentLoader: public AtlasAttachmentLoader {
	public:
		Solar2dAtlasAttachmentLoader(Atlas* atlas);
		virtual ~Solar2dAtlasAttachmentLoader();
		virtual void configureAttachment(Attachment* attachment);
	};

	class Solar2dTextureLoader: public TextureLoader {
	public:
		Solar2dTextureLoader();
		
		virtual ~Solar2dTextureLoader();
		
		virtual void load(AtlasPage& page, const String& path);

		virtual void unload(void* texture);
	};

	class Solar2dExtension: public DefaultSpineExtension {
	public:
		Solar2dExtension();
		
		virtual ~Solar2dExtension();
		
	protected:
		virtual char *_readFile(const String &path, int *length);
	};
}

#endif /* SPINE_Solar2DX_H_ */
