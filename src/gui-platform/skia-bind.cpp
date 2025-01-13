#include "skia/codec/SkCodec.h"
#include "core/SkStream.h"
#include "skia/core/SkCanvas.h"
#include "skia/core/SkColorSpace.h"
#include "skia/core/SkFont.h"
#include "skia/core/SkFontMgr.h"
#include "skia/core/SkSurface.h"
#include "skia/private/base/SkTArray.h"
#include "skia/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "skia/gpu/ganesh/gl/GrGLDirectContext.h"
#include "skia/gpu/ganesh/SkSurfaceGanesh.h"
#include "skia/gpu/GrRecordingContext.h"
#include "skia/gpu/GrDirectContext.h"
#include "skia/gpu/GrBackendSurface.h"
#include "skia/src/base/SkRandom.h"
#include "skia/src/gpu/ganesh/gl/GrGLUtil.h"

#if defined(SK_BUILD_FOR_ANDROID)
#   include <GLES/gl.h>
#elif defined(SK_BUILD_FOR_UNIX)
#   include <GL/gl.h>
#elif defined(SK_BUILD_FOR_MAC)
#   include <OpenGL/gl.h>
#elif defined(SK_BUILD_FOR_IOS)
#   include <OpenGLES/ES2/gl.h>
#endif

#ifdef __linux__
#include "skia/ports/SkFontConfigInterface.h"
#include "skia/ports/SkFontMgr_FontConfigInterface.h"
#elif __APPLE__
#include "skia/ports/SkFontMgr_mac_ct.h"
#elif WIN32
#include <skia/ports/SkTypeface_win.h>
#endif

#include "runtime.h"
#include "blob.h"
#include "skia-bind.h"

extern "C" {
	void ag_fn_guiPlatform_disposePaint(AgSkPaint* me);
	void ag_m_guiPlatform_Paint_guiPlatform_color(AgSkPaint* me, int32_t color);
	void ag_m_guiPlatform_Paint_guiPlatform_fill(AgSkPaint* me);
	void ag_m_guiPlatform_Paint_guiPlatform_stroke(AgSkPaint* me, int32_t width);

	void ag_fn_guiPlatform_disposeImage(AgSkImage* me);
	void ag_m_guiPlatform_Image_guiPlatform_fromBlob(AgSkImage* me, AgBlob* data);

	void ag_fn_guiPlatform_disposeFont(AgSkFont* me);
	void ag_m_guiPlatform_Font_guiPlatform_fromBlob(AgSkFont* me, AgBlob* data);
	void ag_m_guiPlatform_Font_guiPlatform_fromName(AgSkFont* me, AgString* name);

	void ag_m_guiPlatform_Canvas_guiPlatform_clear(AgSkCanvas* me, int32_t color);
	void ag_m_guiPlatform_Canvas_guiPlatform_drawLine(AgSkCanvas* me, float x0, float y0, float x1, float y1, AgSkPaint* p);
	void ag_m_guiPlatform_Canvas_guiPlatform_drawRect(AgSkCanvas* me, AgSkRect* rect, AgSkPaint* paint);
	void ag_m_guiPlatform_Canvas_guiPlatform_drawImage(AgSkCanvas* me, float x, float y, AgSkImage* image);
	void ag_m_guiPlatform_Canvas_guiPlatform_drawSimpleText(AgSkCanvas* me, float x, float y, AgString* s, AgSkFont* font, float size, AgSkPaint* paint);
}

//
// Paint
//
static SkPaint* get_paint(AgSkPaint* me) {
	if (!me->paint)
		me->paint = new SkPaint;
	return me->paint;
}
void ag_fn_guiPlatform_disposePaint(AgSkPaint* me) {
	delete me->paint;
}

void ag_m_guiPlatform_Paint_guiPlatform_color(AgSkPaint* me, int32_t color) {
	get_paint(me)->setColor(color);
}
void ag_m_guiPlatform_Paint_guiPlatform_fill(AgSkPaint* me) {
	get_paint(me)->setStroke(false);
}
void ag_m_guiPlatform_Paint_guiPlatform_stroke(AgSkPaint* me, int32_t width) {
	auto p = get_paint(me);
	p->setStroke(true);
	p->setStrokeWidth(width);
}

//
// Image
//
static inline sk_sp<SkImage>& get_image(AgSkImage* me) {
	return reinterpret_cast<sk_sp<SkImage>&>(me->rc_image);
}
void ag_fn_guiPlatform_disposeImage(AgSkImage* me) {
	get_image(me).~sk_sp();
}

void ag_m_guiPlatform_Image_guiPlatform_fromBlob(AgSkImage* me, AgBlob* data) {
	if (!data->bytes_count)
		return;
	std::unique_ptr<SkCodec> codec = SkCodec::MakeFromStream(
		SkMemoryStream::MakeDirect(data->bytes, data->bytes_count));
	auto [itmp, unused] = codec->getImage();
	get_image(me) = std::move(itmp);
}

//
// Font
//
inline const sk_sp<SkTypeface>& get_font(AgSkFont* me){
	return reinterpret_cast<sk_sp<SkTypeface>&>(me->rc_font);
}
void ag_fn_guiPlatform_disposeFont(AgSkFont* me) {
	get_font(me).~sk_sp();
}

void ag_m_guiPlatform_Font_guiPlatform_fromBlob(AgSkFont* me, AgBlob* data) {
	ag_fn_guiPlatform_disposeFont(me);
#ifdef __linux__
	auto font_mgr = SkFontMgr_New_FCI(SkFontConfigInterface::RefGlobal());
#elif __APPLE__
	auto font_mgr = SkFontMgr_New_CoreText(nullptr);
#else
	auto font_mgr = SkFontMgr_New_GDI();
#endif
	reinterpret_cast<sk_sp<SkTypeface>&>(me->rc_font) = font_mgr->makeFromStream(
		SkMemoryStream::MakeDirect(data->bytes, data->bytes_count));
}
void ag_m_guiPlatform_Font_guiPlatform_fromName(AgSkFont* me, AgString* name) {
	ag_fn_guiPlatform_disposeFont(me);
	auto& font = reinterpret_cast<sk_sp<SkTypeface>&>(me->rc_font);
#ifdef __linux__
	font = SkFontMgr_New_FCI(SkFontConfigInterface::RefGlobal())->legacyMakeTypeface("", SkFontStyle());
#elif __APPLE__
	font = SkFontMgr_New_CoreText(nullptr)->legacyMakeTypeface("", SkFontStyle());
#else
	font = SkFontMgr_New_GDI()->matchFamilyStyle(name->chars, SkFontStyle::Normal());
#endif
}

//
// Canvas
//
void ag_m_guiPlatform_Canvas_guiPlatform_clear(AgSkCanvas* me, int32_t color) {
	if (me->canvas)
		me->canvas->clear(color);
}

void ag_m_guiPlatform_Canvas_guiPlatform_drawLine(AgSkCanvas* me, float x0, float y0, float x1, float y1, AgSkPaint* p) {
	if (me->canvas)
		me->canvas->drawLine(x0, y0, x1, y1, *get_paint(p));
}

void ag_m_guiPlatform_Canvas_guiPlatform_drawRect(AgSkCanvas* me, AgSkRect* rect, AgSkPaint* paint) {
	if (me->canvas)
		me->canvas->drawRect(rect->rect, *get_paint(paint));
}

void ag_m_guiPlatform_Canvas_guiPlatform_drawImage(AgSkCanvas* me, float x, float y, AgSkImage* image) {
	if (me->canvas) {
		if (auto& i = get_image(image))
			me->canvas->drawImage(i, x, y);
	}
}

void ag_m_guiPlatform_Canvas_guiPlatform_drawSimpleText(AgSkCanvas* me, float x, float y, AgString* s, AgSkFont* font, float size, AgSkPaint* paint) {
	if (me->canvas) {
		if (auto& f = get_font(font)) {
			me->canvas->drawSimpleText(
				s->chars, strlen(s->chars),
				SkTextEncoding::kUTF8,
				x, y,
				SkFont(f, size),
				*get_paint(paint));
		}
	}
}
