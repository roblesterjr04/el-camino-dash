#include "art_variants.h"

#include <stdlib.h>
#include <string.h>

#if defined(__has_include) && __has_include(<JPEGDEC.h>) && __has_include(<JPEGENC.h>)
#include <JPEGDEC.h>
#include <JPEGENC.h>

namespace {

// JPEGDEC hands the image over in blocks of MCUs, left to right, then top to bottom. Blocks are gathered
// into one full-width strip; each finished strip resamples every output row whose two source rows are
// available (the last row of the previous strip is kept for rows that straddle the boundary).
struct Resampler {
  int w = 0, h = 0;          // decoded (scaled) size
  int crop_x = 0, crop_y = 0, side = 0;
  uint16_t *strip = nullptr;  // w × strip_h, RGB565
  int strip_h = 0;
  uint16_t *prev = nullptr;   // last row of the previous strip
  int prev_y = -1;
  uint16_t *out = nullptr;    // 128 × 128, RGB565
  int next_row = 0;
  int16_t xa[kArtSmallSide], xb[kArtSmallSide];
  uint8_t wx[kArtSmallSide];
};

inline void rgb(uint16_t p, int &r, int &g, int &b) {
  r = (p >> 11) & 31;
  r = (r << 3) | (r >> 2);
  g = (p >> 5) & 63;
  g = (g << 2) | (g >> 4);
  b = p & 31;
  b = (b << 3) | (b >> 2);
}

/** Source row index, second row and weight (0..256) of output coordinate i along an axis. */
inline void sample(int i, int origin, int side, int limit, int &a, int &b, int &wgt) {
  const int fx = origin * 256 + ((2 * i + 1) * side * 256) / (2 * kArtSmallSide) - 128;  // 8.8 fixed point
  const int f = fx < 0 ? 0 : fx;
  a = f >> 8;
  wgt = f & 255;
  if (a > limit - 1) {
    a = limit - 1;
    wgt = 0;
  }
  b = a + 1 > limit - 1 ? limit - 1 : a + 1;
}

void emit_rows(Resampler &R, int y0, int y1) {
  while (R.next_row < kArtSmallSide) {
    int ya, yb, wy;
    sample(R.next_row, R.crop_y, R.side, R.h, ya, yb, wy);
    if (yb >= y1) {
      return;  // wait for the next strip
    }
    const uint16_t *ra = ya >= y0 ? R.strip + (size_t)(ya - y0) * R.w : R.prev;
    const uint16_t *rb = R.strip + (size_t)(yb - y0) * R.w;
    if (ya < y0 && (ya != R.prev_y || !R.prev)) {
      ra = rb;  // cannot happen with in-order strips; stay safe
    }
    uint16_t *o = R.out + (size_t)R.next_row * kArtSmallSide;
    for (int x = 0; x < kArtSmallSide; x++) {
      int c[4][3];
      rgb(ra[R.xa[x]], c[0][0], c[0][1], c[0][2]);
      rgb(ra[R.xb[x]], c[1][0], c[1][1], c[1][2]);
      rgb(rb[R.xa[x]], c[2][0], c[2][1], c[2][2]);
      rgb(rb[R.xb[x]], c[3][0], c[3][1], c[3][2]);
      int v[3];
      for (int k = 0; k < 3; k++) {
        const int top = c[0][k] * (256 - R.wx[x]) + c[1][k] * R.wx[x];
        const int bot = c[2][k] * (256 - R.wx[x]) + c[3][k] * R.wx[x];
        v[k] = (top * (256 - wy) + bot * wy) >> 16;
      }
      o[x] = (uint16_t)(((v[0] >> 3) << 11) | ((v[1] >> 2) << 5) | (v[2] >> 3));
    }
    R.next_row++;
  }
}

int on_draw(JPEGDRAW *d) {
  Resampler &R = *(Resampler *)d->pUser;
  if (d->y >= R.h) {
    return 1;
  }
  const int rows = d->iHeight;
  if (rows > R.strip_h) {
    return 0;  // unexpected block height: stop
  }
  const int used = d->x + d->iWidthUsed > R.w ? R.w - d->x : d->iWidthUsed;
  for (int r = 0; r < rows; r++) {
    memcpy(R.strip + (size_t)r * R.w + d->x, d->pPixels + (size_t)r * d->iWidth, (size_t)used * 2);
  }
  if (d->x + used < R.w) {
    return 1;  // more blocks of this strip to come
  }
  const int y0 = d->y, y1 = d->y + rows > R.h ? R.h : d->y + rows;
  emit_rows(R, y0, y1);
  memcpy(R.prev, R.strip + (size_t)(y1 - y0 - 1) * R.w, (size_t)R.w * 2);
  R.prev_y = y1 - 1;
  return 1;
}

bool make_gray(const uint16_t *img, uint8_t gray[kArtGrayBytes]) {
  const int n = kArtGraySide * kArtGraySide;
  uint16_t *sum = (uint16_t *)calloc(n, sizeof(uint16_t));
  uint8_t *cnt = (uint8_t *)calloc(n, 1);
  if (!sum || !cnt) {
    free(sum);
    free(cnt);
    return false;
  }
  for (int y = 0; y < kArtSmallSide; y++) {
    const int gy = y * kArtGraySide / kArtSmallSide;
    for (int x = 0; x < kArtSmallSide; x++) {
      int r, g, b;
      rgb(img[y * kArtSmallSide + x], r, g, b);
      const int i = gy * kArtGraySide + x * kArtGraySide / kArtSmallSide;
      sum[i] += (uint16_t)((77 * r + 150 * g + 29 * b) >> 8);
      cnt[i]++;
    }
  }
  memset(gray, 0, kArtGrayBytes);
  for (int i = 0; i < kArtGraySide * kArtGraySide; i++) {
    const int avg = cnt[i] ? sum[i] / cnt[i] : 0;
    const uint8_t level = (uint8_t)((avg * 15 + 127) / 255);
    gray[i / 2] |= (i & 1) ? level : (uint8_t)(level << 4);
  }
  free(sum);
  free(cnt);
  return true;
}

}  // namespace

uint8_t art_make_variants(const uint8_t *jpg, size_t len, uint8_t *out_jpeg, size_t out_cap, size_t *out_len,
                          uint8_t gray48[kArtGrayBytes]) {
  if (out_len) *out_len = 0;
  if (!jpg || len < 4) {
    return 0;
  }
  JPEGDEC *dec = new JPEGDEC();
  Resampler R;
  uint8_t made = 0;
  R.out = (uint16_t *)malloc((size_t)kArtSmallSide * kArtSmallSide * 2);
  if (dec && R.out && dec->openRAM((uint8_t *)jpg, (int)len, on_draw)) {
    // Largest decoder scale that still leaves at least 128 px on the short side.
    const int sw = dec->getWidth(), sh = dec->getHeight();
    int scale = 1;
    while (scale < 8 && (sw < sh ? sw : sh) / (scale * 2) >= kArtSmallSide) {
      scale *= 2;
    }
    R.w = sw / scale;
    R.h = sh / scale;
    R.side = R.w < R.h ? R.w : R.h;
    R.crop_x = (R.w - R.side) / 2;
    R.crop_y = (R.h - R.side) / 2;
    R.strip_h = 16;  // tallest MCU (4:2:0)
    R.strip = (uint16_t *)malloc((size_t)(R.w + 16) * R.strip_h * 2);
    R.prev = (uint16_t *)malloc((size_t)(R.w + 16) * 2);
    for (int x = 0; x < kArtSmallSide; x++) {
      int a, b, w;
      sample(x, R.crop_x, R.side, R.w, a, b, w);
      R.xa[x] = (int16_t)a;
      R.xb[x] = (int16_t)b;
      R.wx[x] = (uint8_t)(w > 255 ? 255 : w);
    }
    if (R.strip && R.prev && R.w > 0 && R.h > 0) {
      dec->setPixelType(RGB565_LITTLE_ENDIAN);
      dec->setUserPointer(&R);
      const int opt = scale == 8 ? JPEG_SCALE_EIGHTH : scale == 4 ? JPEG_SCALE_QUARTER : scale == 2 ? JPEG_SCALE_HALF : 0;
      if (dec->decode(0, 0, opt) && R.next_row == kArtSmallSide) {
        if (gray48 && make_gray(R.out, gray48)) {
          made |= kArtHaveGray48;
        }
        JPEGENC *enc = new JPEGENC();
        JPEGENCODE je;
        if (enc && out_jpeg && out_cap >= 1024 && enc->open(out_jpeg, (int)out_cap) == JPEGE_SUCCESS &&
            enc->encodeBegin(&je, kArtSmallSide, kArtSmallSide, JPEGE_PIXEL_RGB565, JPEGE_SUBSAMPLE_420,
                             JPEGE_Q_HIGH) == JPEGE_SUCCESS &&
            enc->addFrame(&je, (uint8_t *)R.out, kArtSmallSide * 2) == JPEGE_SUCCESS) {
          const int n = enc->close();
          if (n > 0 && (size_t)n <= out_cap) {
            if (out_len) *out_len = (size_t)n;
            made |= kArtHaveJpeg128;
          }
        }
        delete enc;
      }
    }
    dec->close();
  }
  free(R.strip);
  free(R.prev);
  free(R.out);
  delete dec;
  return made;
}

#else

uint8_t art_make_variants(const uint8_t *, size_t, uint8_t *, size_t, size_t *out_len, uint8_t *) {
  if (out_len) *out_len = 0;
  return 0;
}

#endif
