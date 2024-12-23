#include <core/SkSurface.h>
#include <core/SkCanvas.h>
#include <core/SkPaint.h>
#include <core/SkData.h>
#include <codec/SkCodec.h>
#include <core/SkTypeface.h>
#include <core/SkFont.h>
#include <core/SkFontMgr.h>
#include <core/SkStream.h>
#include <ports/SkTypeface_win.h>
#include "runtime.h"
#include "blob.h"
#include "skia-bind.h"

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
inline const sk_sp<SkImage>& get_image(AgSkImage* me) {
	return reinterpret_cast<sk_sp<SkImage>&>(me->rc_image);
}
void ag_fn_guiPlatform_disposeImage(AgSkImage* me) {
	get_image(me).~sk_sp();
}

void ag_m_guiPlatform_Image_guiPlatform_fromBlob(AgSkImage* me, AgBlob* data) {
	ag_fn_guiPlatform_disposeImage(me);
	auto [itmp, unused] = SkCodec::MakeFromStream(
		std::make_unique<SkMemoryStream>(data->bytes, data->bytes_count))
		->getImage();
	new(me->rc_image) sk_sp<SkImage>(std::move(itmp));
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
		std::make_unique<SkMemoryStream>(data->bytes, data->bytes_count));
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
inline const sk_sp<SkCanvas>& get_canvas(AgSkCanvas* me) {
	return reinterpret_cast<sk_sp<SkTypeface>&>(me->rc_font);
}
void ag_fn_guiPlatform_disposeCanvas(AgSkCanvas* me) {
	get_canvas(me).~sk_sp();
}
void ag_m_guiPlatform_Canvas_guiPlatform_clear(AgSkCanvas* me, int32_t color) {
	if (auto& c = get_canvas(me))
		c->clear(color);
}

void ag_m_guiPlatform_Canvas_guiPlatform_drawLine(AgSkCanvas* me, float x0, float y0, float x1, float y1, AgSkPaint* p) {
	if (me->canvas)
		me->canvas->drawLine(x0, y0, x1, y1, *get_paint(p));
}

void ag_m_guiPlatform_Canvas_guiPlatform_drawRect(AgSkCanvas* me, AgSkRect* rect, AgSkPaint* paint) {
	if (auto& c = get_canvas(me))
		c->drawRect(rect->rect, *get_paint(paint));
}

void ag_m_guiPlatform_Canvas_guiPlatform_drawImage(AgSkCanvas* me, float x, float y, AgSkImage* image) {
	if (auto& c = get_canvas(me)) {
		if (auto& i = get_image(image))
			c->drawImage(i, x, y);
	}
}

void ag_m_guiPlatform_Canvas_guiPlatform_drawSimpleText(AgSkCanvas* me, float x, float y, AgString* s, AgSkFont* font, float size, AgSkPaint* paint) {
	if (auto& c = get_canvas(me)) {
		if (auto& f = get_font(font)) {
			c->drawSimpleText(
				s->chars, strlen(s->chars),
				SkTextEncoding::kUTF8,
				x, y,
				SkFont(f, size),
				*get_paint(paint));
		}
	}
}
