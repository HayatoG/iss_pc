// SPDX-License-Identifier: GPL-2.0-or-later
// issn-axy: PNG encoding for screenshots (stb_image_write, public domain, third_party/stb).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#include "issn_png.h"

namespace IssnPng {
std::vector<unsigned char> EncodeRgba(const unsigned char* rgba, int width, int height) {
    std::vector<unsigned char> out;
    if (!rgba || width <= 0 || height <= 0) {
        return out;
    }
    const auto sink = [](void* context, void* data, int size) {
        auto* bytes = static_cast<std::vector<unsigned char>*>(context);
        const auto* p = static_cast<const unsigned char*>(data);
        bytes->insert(bytes->end(), p, p + size);
    };
    if (!stbi_write_png_to_func(sink, &out, width, height, 4, rgba, width * 4)) {
        out.clear();
    }
    return out;
}
} // namespace IssnPng
