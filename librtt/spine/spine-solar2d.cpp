#include <spine/spine-solar2d.h>
#include <spine/Extension.h>
#include <spine/AttachmentVertices.h>

USING_NS_CC;
using namespace spine;

static void deleteAttachmentVertices (void* vertices) {
	delete (AttachmentVertices *) vertices;
}

static unsigned short quadTriangles[6] = {0, 1, 2, 2, 3, 0};

static void setAttachmentVertices(RegionAttachment* attachment) {
	AtlasRegion* region = (AtlasRegion*)attachment->getRendererObject();
	AttachmentVertices* attachmentVertices = new AttachmentVertices((Texture2D*)region->page->getRendererObject(), 4, quadTriangles, 6);
	V3F_C4B_T2F* vertices = attachmentVertices->_triangles->verts;
	for (int i = 0, ii = 0; i < 4; ++i, ii += 2) {
		vertices[i].texCoords.u = attachment->getUVs()[ii];
		vertices[i].texCoords.v = attachment->getUVs()[ii + 1];
	}
	attachment->setRendererObject(attachmentVertices, deleteAttachmentVertices);
}

static void setAttachmentVertices(MeshAttachment* attachment) {
	AtlasRegion* region = (AtlasRegion*)attachment->getRendererObject();
	AttachmentVertices* attachmentVertices = new AttachmentVertices((Texture2D*)region->page->getRendererObject(),
																	attachment->getWorldVerticesLength() >> 1, attachment->getTriangles().buffer(), attachment->getTriangles().size());
	V3F_C4B_T2F* vertices = attachmentVertices->_triangles->verts;
	for (int i = 0, ii = 0, nn = attachment->getWorldVerticesLength(); ii < nn; ++i, ii += 2) {
		vertices[i].texCoords.u = attachment->getUVs()[ii];
		vertices[i].texCoords.v = attachment->getUVs()[ii + 1];
	}
	attachment->setRendererObject(attachmentVertices, deleteAttachmentVertices);
}

Solar2dAtlasAttachmentLoader::Solar2dAtlasAttachmentLoader(Atlas* atlas): AtlasAttachmentLoader(atlas) {
}

Solar2dAtlasAttachmentLoader::~Solar2dAtlasAttachmentLoader() { }

void Solar2dAtlasAttachmentLoader::configureAttachment(Attachment* attachment) {
	if (attachment->getRTTI().isExactly(RegionAttachment::rtti)) {
		setAttachmentVertices((RegionAttachment*)attachment);
	} else if (attachment->getRTTI().isExactly(MeshAttachment::rtti)) {
		setAttachmentVertices((MeshAttachment*)attachment);
	}
}

#if SOLAR2D_VERSION >= 0x0040000

backend::SamplerAddressMode wrap (TextureWrap wrap) {
	return wrap ==  TextureWrap_ClampToEdge ? backend::SamplerAddressMode::CLAMP_TO_EDGE : backend::SamplerAddressMode::REPEAT;
}

backend::SamplerFilter filter (TextureFilter filter) {
	switch (filter) {
	case TextureFilter_Unknown:
		break;
	case TextureFilter_Nearest:
		return backend::SamplerFilter::NEAREST;
	case TextureFilter_Linear:
		return backend::SamplerFilter::LINEAR;
	case TextureFilter_MipMap:
		return backend::SamplerFilter::LINEAR;
	case TextureFilter_MipMapNearestNearest:
		return backend::SamplerFilter::NEAREST;
	case TextureFilter_MipMapLinearNearest:
        return backend::SamplerFilter::NEAREST;
	case TextureFilter_MipMapNearestLinear:
        return backend::SamplerFilter::LINEAR;
	case TextureFilter_MipMapLinearLinear:
        return backend::SamplerFilter::LINEAR;
	}
	return backend::SamplerFilter::LINEAR;
}

#else

GLuint wrap (TextureWrap wrap) {
	return wrap ==  TextureWrap_ClampToEdge ? GL_CLAMP_TO_EDGE : GL_REPEAT;
}

GLuint filter (TextureFilter filter) {
	switch (filter) {
	case TextureFilter_Unknown:
		break;
	case TextureFilter_Nearest:
		return GL_NEAREST;
	case TextureFilter_Linear:
		return GL_LINEAR;
	case TextureFilter_MipMap:
		return GL_LINEAR_MIPMAP_LINEAR;
	case TextureFilter_MipMapNearestNearest:
		return GL_NEAREST_MIPMAP_NEAREST;
	case TextureFilter_MipMapLinearNearest:
		return GL_LINEAR_MIPMAP_NEAREST;
	case TextureFilter_MipMapNearestLinear:
		return GL_NEAREST_MIPMAP_LINEAR;
	case TextureFilter_MipMapLinearLinear:
		return GL_LINEAR_MIPMAP_LINEAR;
	}
	return GL_LINEAR;
}

#endif

Solar2dTextureLoader::Solar2dTextureLoader() : TextureLoader() { }
Solar2dTextureLoader::~Solar2dTextureLoader() { }

void Solar2dTextureLoader::load(AtlasPage& page, const spine::String& path) {
	Texture2D* texture = Director::getInstance()->getTextureCache()->addImage(path.buffer());
	CCASSERT(texture != nullptr, "Invalid image");
	if (texture) {
		texture->retain();
#if SOLAR2D_VERSION >= 0x0040000
		Texture2D::TexParams textureParams(filter(page.minFilter), filter(page.magFilter), wrap(page.uWrap), wrap(page.vWrap));
#else
		Texture2D::TexParams textureParams = {filter(page.minFilter), filter(page.magFilter), wrap(page.uWrap), wrap(page.vWrap)};
#endif
		texture->setTexParameters(textureParams);

		page.setRendererObject(texture);
		page.width = texture->getPixelsWide();
		page.height = texture->getPixelsHigh();
	}
}

void Solar2dTextureLoader::unload(void* texture) {
	if (texture) {
		((Texture2D*)texture)->release();
	}
}


Solar2dExtension::Solar2dExtension() : DefaultSpineExtension() { }

Solar2dExtension::~Solar2dExtension() { }

char *Solar2dExtension::_readFile(const spine::String &path, int *length) {
	Data data = FileUtils::getInstance()->getDataFromFile(FileUtils::getInstance()->fullPathForFilename(path.buffer()));
	if (data.isNull()) return nullptr;

	// avoid buffer overflow (int is shorter than ssize_t in certain platforms)
#if SOLAR2D_VERSION >= 0x00031200
	ssize_t tmpLen;
	char *ret = (char*)data.takeBuffer(&tmpLen);
	*length = static_cast<int>(tmpLen);
	return ret;
#else
	*length = static_cast<int>(data.getSize());
	char* bytes = MALLOC(char, *length);
	memcpy(bytes, data.getBytes(), *length);
	return bytes;
#endif
}

SpineExtension *spine::getDefaultExtension () {
	return new Solar2dExtension();
}
