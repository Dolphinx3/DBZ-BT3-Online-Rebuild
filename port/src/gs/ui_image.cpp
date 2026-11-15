// Loads a picture file (PNG, JPEG, BMP, GIF's first frame, TGA) for the F1 window's wallpaper: stb_image (port/third_party/stb,
// public domain). Big pictures are shrunk (an averaged box, by whole steps) to at most 2048 pixels on the long side: a 4K
// wallpaper would cost 33 MB of video memory for a picture that is seen the size of a window.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#include "../../third_party/stb/stb_image.h"

// RGBA8 pixels (malloc'd: the caller frees them, or hands them to make_strip which does), or NULL.
uint8_t *UiImage_Load(const char *path, int *w, int *h) {
    int iw = 0, ih = 0, n = 0;
    unsigned char *src = stbi_load(path, &iw, &ih, &n, 4);

    if (src == NULL || iw <= 0 || ih <= 0) {
        if (src != NULL) {
            stbi_image_free(src);
        }
        return NULL;
    }
    int step = 1;
    while ((iw / step > 2048) || (ih / step > 2048)) {
        step++;
    }
    int ow = iw / step, oh = ih / step;
    uint8_t *out = (uint8_t *)malloc((size_t)ow * (size_t)oh * 4);
    if (out == NULL) {
        stbi_image_free(src);
        return NULL;
    }
    if (step == 1) {
        memcpy(out, src, (size_t)ow * (size_t)oh * 4);
    } else {
        for (int y = 0; y < oh; y++) {
            for (int x = 0; x < ow; x++) {
                unsigned sum[4] = {0, 0, 0, 0};
                for (int dy = 0; dy < step; dy++) {
                    for (int dx = 0; dx < step; dx++) {
                        const unsigned char *p = src + ((size_t)(y * step + dy) * (size_t)iw + (size_t)(x * step + dx)) * 4;
                        for (int c = 0; c < 4; c++) {
                            sum[c] += p[c];
                        }
                    }
                }
                for (int c = 0; c < 4; c++) {
                    out[((size_t)y * (size_t)ow + (size_t)x) * 4 + (size_t)c] = (uint8_t)(sum[c] / (unsigned)(step * step));
                }
            }
        }
    }
    stbi_image_free(src);
    *w = ow;
    *h = oh;
    return out;
}
