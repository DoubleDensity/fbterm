/*
 *   Copyright © 2008-2010 dragchan <zgchan317@gmail.com>
 *   This file is part of FbTerm.
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_GLYPH_H
#include "font.h"
#include "screen.h"
#include "fbconfig.h"
#include <map>

#define OFFSET(TYPE, MEMBER) ((size_t)(&(((TYPE *)0)->MEMBER)))
#define SUBS(a, b) ((a) > (b) ? (a) - (b) : (b) - (a))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static FcCharSet *unicodeMap;
static FcFontSet *fontList;
 
static FT_Library ftlib;
static FT_Face *fontFaces;
static u32 *fontFlags;

static std::map<u32, Font::Glyph *> glyphCache;

static void openFont(u32 index);

DEFINE_INSTANCE(Font)

Font *Font::createInstance()
{
	FcInit();

	s8 buf[64];
	Config::instance()->getOption("font-names", buf, sizeof(buf));

	FcPattern *pat = FcNameParse((FcChar8 *)(*buf ? buf : "mono"));

	u32 pixel_size = 12;
	Config::instance()->getOption("font-size", pixel_size);
	FcPatternAddDouble(pat, FC_PIXEL_SIZE, (double)pixel_size);

	FcPatternAddString(pat, FC_LANG, (FcChar8 *)"en");

	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);

	FcResult result;
	FcFontSet *fs = FcFontSort(NULL, pat, FcTrue, &unicodeMap, &result);

	if (fs) {
		fontList = FcFontSetCreate();

		FcObjectSet *family = FcObjectSetCreate();
		FcObjectSetAdd(family, FC_FAMILY);

		for (u32 i = 0; i < fs->nfont; i++) {
			FcPattern *font = FcFontRenderPrepare(NULL, pat, fs->fonts[i]);
			if (!font) continue;

			bool same = false;
			for (u32 j = 0; j < fontList->nfont; j++) {
				if (FcPatternEqualSubset(fontList->fonts[j], font, family)) {
					same = true;
					break;
				}
			}

			if (same) {
				FcPatternDestroy(font);
			} else {
				FcFontSetAdd(fontList, font);
			}
		}

		FcObjectSetDestroy(family);
	}

	FcPatternDestroy(pat);
	if (fs) FcFontSetDestroy(fs);

	if (fontList && fontList->nfont) return new Font();

	if (unicodeMap) FcCharSetDestroy(unicodeMap);
	if (fontList) FcFontSetDestroy(fontList);
	FcFini();
	return 0;
}

Font::Font()
{
	mHeight = mWidth = 0;

	fontFaces = new FT_Face[fontList->nfont];
	fontFlags = new u32[fontList->nfont];
	memset(fontFaces, 0, sizeof(FT_Face) * fontList->nfont);

	// Use dynamic map to support full Unicode range (including above 0xFFFF for Nerd Fonts)

	FT_Init_FreeType(&ftlib);
	openFont(0);

	FT_Face face = fontFaces[0];
	if (face == (FT_Face)-1) return;

	if (face->face_flags & FT_FACE_FLAG_SCALABLE) {
		mHeight = face->size->metrics.height >> 6;
		mWidth = face->size->metrics.max_advance >> 6;
	} else if (face->num_fixed_sizes) {
		double dsize;
		FcPatternGetDouble(fontList->fonts[0], FC_PIXEL_SIZE, 0, &dsize);

		FT_Bitmap_Size *sizes = face->available_sizes;
		u32 index = 0, diffmin = (u32)-1;
		for (u32 i = 0; i < face->num_fixed_sizes; i++) {
			u32 diff = SUBS(sizes[i].size >> 6, (u32)dsize);
			if (diff < diffmin ) {
				index = i;
				diffmin = diff;
			}
		}

		mHeight = sizes[index].height;
		mWidth = sizes[index].width;
	}
	mBaseline = face->size->metrics.ascender >> 6;

	if (!(face->face_flags & FT_FACE_FLAG_FIXED_WIDTH)) mWidth = MIN(mWidth, (mHeight + 1) / 2);

	u32 width = 0;
	Config::instance()->getOption("font-width", width);

	if (width) {
		s8 buf[64];
		Config::instance()->getOption("font-width", buf, sizeof(buf));

		if (buf[0] == '+' || buf[0] == '-') mWidth += (s32)width;
		else mWidth = width;
	}

	u32 height = 0;
	Config::instance()->getOption("font-height", height);

	if (height) {
		s8 buf[64];
		Config::instance()->getOption("font-height", buf, sizeof(buf));

		if (buf[0] == '+' || buf[0] == '-') mHeight += (s32)height;
		else mHeight = height;
	}

	u32 baseline = 0;
	Config::instance()->getOption("font-baseline", baseline);

	if (baseline) {
		s8 buf[64];
		Config::instance()->getOption("font-baseline", buf, sizeof(buf));

		if (buf[0] == '+' || buf[0] == '-') mBaseline += (s32)baseline;
		else mBaseline = baseline;
	}
}

Font::~Font()
{
	for (auto &entry : glyphCache) {
		if (entry.second) delete[] (u8 *)entry.second;
	}
	glyphCache.clear();

	for (u32 i = 0; i < fontList->nfont; i++) {
		if (fontFaces[i] && fontFaces[i] != (FT_Face)-1) {
			FT_Done_Face(fontFaces[i]);
		}
	}

	delete[] fontFaces;
	delete[] fontFlags;

	FT_Done_FreeType(ftlib);
	FcCharSetDestroy(unicodeMap);
	FcFontSetDestroy(fontList);
	FcFini();
}

void Font::showInfo(bool verbose)
{
	if (!verbose) return;

	printf("[font] width: %dpx, height: %dpx, ordered list: ", mWidth, mHeight);

	u32 index;
	FcChar8 *family;
	for (index = 0; index < fontList->nfont - 1; index++) {
		FcPatternGetString(fontList->fonts[index], FC_FAMILY, 0, &family);
		printf("%s, ", family);
	}

	FcPatternGetString(fontList->fonts[index], FC_FAMILY, 0, &family);
	printf("%s\n", family);
}

static void openFont(u32 index)
{
	if (index >= fontList->nfont) return;

	FcPattern *pattern = fontList->fonts[index];

	FcChar8 *name = (FcChar8 *)"";
	FcPatternGetString(pattern, FC_FILE, 0, &name);

	int id = 0;
	FcPatternGetInteger (pattern, FC_INDEX, 0, &id);

	FT_Face face;
	if (FT_New_Face(ftlib, (const char *)name, id, &face)) {
		fontFaces[index] = (FT_Face)-1;
		return;
	}

	double ysize;
	FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &ysize);
	FT_Set_Pixel_Sizes(face, 0, (FT_UInt)ysize);

	int load_flags = FT_LOAD_DEFAULT;

	FcBool scalable, antialias;
	FcPatternGetBool(pattern, FC_SCALABLE, 0, &scalable);
	FcPatternGetBool(pattern, FC_ANTIALIAS, 0, &antialias);

	if (scalable && antialias) load_flags |= FT_LOAD_NO_BITMAP;

	if (antialias) {
		FcBool hinting;
		int hint_style;
		FcPatternGetBool(pattern, FC_HINTING, 0, &hinting);
		FcPatternGetInteger(pattern, FC_HINT_STYLE, 0, &hint_style);

		if (!hinting || hint_style == FC_HINT_NONE) {
			load_flags |= FT_LOAD_NO_HINTING;
		} else {
			load_flags |= FT_LOAD_TARGET_LIGHT;
		}
	} else {
		load_flags |= FT_LOAD_TARGET_MONO;
	}

	fontFaces[index] = face;
	fontFlags[index] = load_flags;
}

static int fontIndex(u32 unicode)
{
	if (!FcCharSetHasChar(unicodeMap, unicode)) return -1;

	FcCharSet *charset;
	for (u32 i = 0; i < fontList->nfont; i++) {
		FcPatternGetCharSet(fontList->fonts[i], FC_CHARSET, 0, &charset);
		if (FcCharSetHasChar(charset, unicode)) return i;
	}

	return -1;
}

Font::Glyph *Font::getGlyph(u32 unicode)
{
	auto it = glyphCache.find(unicode);
	if (it != glyphCache.end()) return it->second;

	int i = fontIndex(unicode);
	if (i == -1) return 0;

	if (!fontFaces[i]) openFont(i);
	if (fontFaces[i] == (FT_Face)-1) return 0;

	FT_Face face = fontFaces[i];
	FT_UInt index = FT_Get_Char_Index(face, (FT_ULong)unicode);
	if (!index) return 0;

	FT_Load_Glyph(face, index, FT_LOAD_RENDER | fontFlags[i] | FT_LOAD_COLOR);
	FT_Bitmap &bitmap = face->glyph->bitmap;

	u32 x, y, w, h, nx, ny, nw, nh;
	x = y = 0;
	w = nw = bitmap.width;
	h = nh = bitmap.rows;
	u8 *buf = bitmap.buffer;
	s32 top = (s32)mBaseline - face->glyph->bitmap_top;

	bool is_color_bitmap = (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA);
	u32 bytes_per_pixel = is_color_bitmap ? 4 : 1;

	u32 target_w = w, target_h = h;
	s32 target_top = top;
	s32 target_left = face->glyph->bitmap_left;

	u32 optimal_h = (u32)(mHeight * 0.75f);
	if (optimal_h == 0) optimal_h = 1;

	u32 max_w = (face->glyph->advance.x > (mWidth << 6) * 1.5) ? mWidth * 2 : mWidth;

	if (is_color_bitmap && (h > optimal_h || w > max_w)) {
		float scale_factor = (float)optimal_h / h;
		if (w * scale_factor > max_w) {
			scale_factor = (float)max_w / w;
		}
		target_w = (u32)(w * scale_factor);
		target_h = (u32)(h * scale_factor);
		if (target_w == 0) target_w = 1;
		if (target_h == 0) target_h = 1;
		target_top = (s32)mBaseline - (s32)(face->glyph->bitmap_top * scale_factor);
		target_left = (s32)(target_left * scale_factor);
	}

	u32 src_y_offset = 0;
	if (target_top < 0) {
		if (is_color_bitmap) {
			u32 crop = -target_top;
			if (crop >= target_h) return 0;
			target_h -= crop;
			src_y_offset = crop;
			target_top = 0;
		} else {
			buf -= target_top * bitmap.pitch;
			target_h += target_top;
			target_top = 0;
		}
	}

	nw = target_w;
	nh = target_h;
	Screen::instance()->rotateRect(x, y, nw, nh);

	Glyph *glyph = (Glyph *)new u8[OFFSET(Glyph, pixmap) + nw * nh * bytes_per_pixel];
	glyph->left = target_left;
	glyph->top = target_top;
	glyph->width = target_w;
	glyph->height = target_h;
	glyph->pitch = nw * bytes_per_pixel;
	glyph->is_color_bitmap = is_color_bitmap;

	for (y = 0; y < target_h; y++) {
		for (x = 0; x < target_w; x++) {
			nx = x, ny = y;
			Screen::instance()->rotatePoint(target_w, target_h, nx, ny);

			if (is_color_bitmap) {
				u32 target_y = y + src_y_offset;
				u32 r = 0, g = 0, b = 0, a = 0;
				u32 target_h_orig = target_h + src_y_offset;
				u32 src_x_start = (x * w) / target_w;
				u32 src_x_end = ((x + 1) * w) / target_w;
				u32 src_y_start = (target_y * h) / target_h_orig;
				u32 src_y_end = ((target_y + 1) * h) / target_h_orig;

				if (src_x_end <= src_x_start) src_x_end = src_x_start + 1;
				if (src_y_end <= src_y_start) src_y_end = src_y_start + 1;

				u32 count = 0;
				for (u32 py = src_y_start; py < src_y_end && py < h; py++) {
					for (u32 px = src_x_start; px < src_x_end && px < w; px++) {
						u8* p = buf + (py * bitmap.pitch) + (px * 4);
						b += p[0]; g += p[1]; r += p[2]; a += p[3];
						count++;
					}
				}
				if (count > 0) { b /= count; g /= count; r /= count; a /= count; }

				u32 dst_idx = ny * nw * 4 + nx * 4;
				glyph->pixmap[dst_idx + 0] = b;
				glyph->pixmap[dst_idx + 1] = g;
				glyph->pixmap[dst_idx + 2] = r;
				glyph->pixmap[dst_idx + 3] = a;
			} else {
				glyph->pixmap[ny * nw + nx] =
					(bitmap.pixel_mode == FT_PIXEL_MODE_MONO) ? ((buf[(x >> 3)] & (0x80 >> (x & 7))) ? 0xff : 0) : buf[x];
			}
		}
		if (!is_color_bitmap) buf += bitmap.pitch;
	}

	glyphCache[unicode] = glyph;
	return glyph;
}
