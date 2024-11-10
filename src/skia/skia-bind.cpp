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

extern "C" {

typedef struct {
	AgObject header;
	SkRect   rect;
} AgSkRect;

typedef struct {
	AgObject header;
	SkPaint* paint;
} AgSkPaint;

typedef struct {
	AgObject header;
	void*    rc_image;
} AgSkImage;

typedef struct {
	AgObject  header;
	void*     rc_font;
} AgSkFont;

typedef struct {
	AgObject  header;
	SkCanvas* canvas;
} AgSkCanvas;

void ag_fn_skia_disposePaint(AgSkPaint* me) {
	if (me->paint)
		delete me->paint;
}
void ag_fn_skia_disposeImage(AgSkImage* me) {
	if (me->rc_image)
		reinterpret_cast<sk_sp<SkImage>&>(me->rc_image).sk_sp<SkImage>::~sk_sp();
}
void ag_fn_skia_disposeFont(AgSkFont* me) {
	if (me->rc_font)
		reinterpret_cast<sk_sp<SkTypeface>&>(me->rc_font).sk_sp<SkTypeface>::~sk_sp();
}
void ag_fn_skia_disposeCanvas(AgSkCanvas* me) {
	if (me->canvas)
		delete me->canvas;
}

void ag_m_skia_Paint_skia_setColor(AgSkPaint* me, int32_t color);
void ag_m_skia_Image_skia_fromBlob(AgSkImage* me, AgBlob* data);
void ag_m_skia_Font_skia_fromBlob(AgSkFont* me, AgBlob* data);
void ag_m_skia_Font_skia_fromName(AgSkFont* me, AgString* name);
void ag_m_skia_Font_skia_fromName(AgSkFont* me, AgString* name);
void ag_m_skia_Canvas_skia_drawRect(AgSkCanvas* me, AgSkRect* rect, AgSkPaint* paint);
void ag_m_skia_Canvas_skia_drawImage(AgSkCanvas* me, float x, float y, AgSkImage* image);
void ag_m_skia_Canvas_skia_drawSimpleText(AgSkCanvas* me, float x, float y, AgString* str, AgSkFont* font, float size, AgSkPaint* paint);

}

static SkPaint* get_paint(AgSkPaint* me) {
	if (!me->paint)
		me->paint = new SkPaint;
	return me->paint;
}
void ag_m_skia_Paint_skia_setColor(AgSkPaint* me, int32_t color) {
	get_paint(me)->setColor(color);
}

inline const sk_sp<SkImage>& get_image(AgSkImage* me) {
	return reinterpret_cast<sk_sp<SkImage>&>(me->rc_image);
}
void ag_m_skia_Image_skia_fromBlob(AgSkImage* me, AgBlob* data) {
	ag_fn_skia_disposeImage(me);
	auto [itmp, unused] = SkCodec::MakeFromStream(std::make_unique<SkMemoryStream>(data->bytes, data->bytes_count))->getImage();
	new(me->rc_image) sk_sp<SkImage>(std::move(itmp));
}

inline const sk_sp<SkTypeface>& get_font(AgSkFont* me){
	return reinterpret_cast<sk_sp<SkTypeface>&>(me->rc_font);
}
void ag_m_skia_Font_skia_fromBlob(AgSkFont* me, AgBlob* data) {
	ag_fn_skia_disposeFont(me);
	reinterpret_cast<sk_sp<SkTypeface>&>(me->rc_font) =
		SkFontMgr_New_GDI()->makeFromStream(
			std::make_unique<SkMemoryStream>(data->bytes, data->bytes_count));
}
void ag_m_skia_Font_skia_fromName(AgSkFont* me, AgString* name) {
	ag_fn_skia_disposeFont(me);
	reinterpret_cast<sk_sp<SkTypeface>&>(me->rc_font) =
		SkFontMgr_New_GDI()->matchFamilyStyle(name->chars, SkFontStyle::Normal());
}

void ag_m_skia_Canvas_skia_drawRect(AgSkCanvas* me, AgSkRect* rect, AgSkPaint* paint) {
	if (me->canvas)
		me->canvas->drawRect(rect->rect, *get_paint(paint));
}
void ag_m_skia_Canvas_skia_drawImage(AgSkCanvas* me, float x, float y, AgSkImage* image) {
	if (me->canvas && image->rc_image)
		me->canvas->drawImage(get_image(image), x, y);
}
void ag_m_skia_Canvas_skia_drawSimpleText(AgSkCanvas* me, float x, float y, AgString* s, AgSkFont* font, float size, AgSkPaint* paint) {
	if (me->canvas && font->rc_font)
		me->canvas->drawSimpleText(
			s->chars, strlen(s->chars),
			SkTextEncoding::kUTF8,
			x, y,
			SkFont(get_font(font), size),
			*get_paint(paint));
}
