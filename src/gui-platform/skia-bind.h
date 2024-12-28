#ifndef _AG_SKIA_BIND_H_
#define _AG_SKIA_BIND_H_

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

}  // extern "C"

#endif // _AG_SKIA_BIND_H_
