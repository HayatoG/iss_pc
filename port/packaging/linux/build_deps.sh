#!/usr/bin/env bash
# Builds the libraries Ubuntu 24.04 lacks or ships too old for the port, into $PREFIX
# (default /opt/tlgdeps): SDL3, Zydis 5, fmt 11, robin-map, VMA and a minimal FFmpeg (only the H.264, MP3 and
# PNG decoders the runtime uses, instead of the distribution's ~250-library FFmpeg).
# Used by the GitHub Actions Linux build; the resulting .so files ship in the package (out/lib).
set -euo pipefail
PREFIX=${PREFIX:-/opt/tlgdeps}
JOBS=$(nproc)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"

fetch() { # url dir [sha256]
    local archive="$work/$2-download.tar.gz"
    mkdir -p "$2"
    # Download completely before extracting; a reset must not leave a partial tree.
    if ! curl -fsSL --retry 2 --retry-all-errors --connect-timeout 15 --max-time 60 "$1" -o "$archive"; then
        if [[ "$1" == https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.gz ]]; then
            echo 'FFmpeg release server unavailable; using the same official n7.1.2 tag.'
            curl -fsSL --retry 2 --retry-all-errors --connect-timeout 15 --max-time 90 \
                https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n7.1.2.tar.gz -o "$archive"
        else
            return 1
        fi
    fi
    if [[ -n ${3:-} ]]; then
        printf '%s  %s\n' "$3" "$archive" | sha256sum -c -
    fi
    tar -xzf "$archive" -C "$2" --strip-components=1
}

# The DualSense Bluetooth encoder needs the restricted CELT mode in Opus 1.6.
fetch https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz opus \
    6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
cmake -S opus -B opus/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DOPUS_BUILD_SHARED_LIBRARY=ON
ninja -C opus/build install
install -Dm644 opus/COPYING "$PREFIX/share/licenses/opus/COPYING"

fetch https://github.com/libsdl-org/SDL/releases/download/release-3.2.24/SDL3-3.2.24.tar.gz sdl
cmake -S sdl -B sdl/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
ninja -C sdl/build install

# Zydis 5 has no release tag yet: a pinned master commit (ZYDIS_VERSION 5.0.0).
git clone -q https://github.com/zyantific/zydis.git zydis
git -C zydis checkout -q a95bb71
git -C zydis submodule update -q --init --recursive
# Zycore (bundled submodule) is installed on its own, static + PIC, so zydis-config.cmake's
# find_dependency(Zycore) resolves and the Zycore headers that Zydis.h includes are present.
cmake -S zydis/dependencies/zycore -B zycore/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DZYCORE_BUILD_SHARED_LIB=OFF -DZYCORE_BUILD_EXAMPLES=OFF -DZYCORE_BUILD_TESTS=OFF
ninja -C zycore/build install
cmake -S zydis -B zydis/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$PREFIX" -DZYAN_SYSTEM_ZYCORE=ON -DZYDIS_BUILD_SHARED_LIB=ON -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_TESTS=OFF \
    -DZYDIS_BUILD_DOXYGEN=OFF
ninja -C zydis/build install

fetch https://github.com/fmtlib/fmt/archive/refs/tags/11.2.0.tar.gz fmt
cmake -S fmt -B fmt/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_SHARED_LIBS=ON -DFMT_TEST=OFF -DFMT_DOC=OFF
ninja -C fmt/build install

fetch https://github.com/Tessil/robin-map/archive/refs/tags/v1.4.0.tar.gz robin-map
cmake -S robin-map -B robin-map/build -G Ninja -DCMAKE_INSTALL_PREFIX="$PREFIX"
ninja -C robin-map/build install

fetch https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz vma
cmake -S vma -B vma/build -G Ninja -DCMAKE_INSTALL_PREFIX="$PREFIX"
ninja -C vma/build install

fetch https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.gz ffmpeg
(cd ffmpeg && ./configure --prefix="$PREFIX" --enable-shared --disable-static --disable-programs --disable-avdevice --disable-avfilter \
    --disable-doc --disable-network --disable-autodetect --enable-zlib --disable-everything \
    --enable-decoder=h264,mp3,mp3float,png --enable-parser=h264,mpegaudio,png \
    --enable-demuxer=h264,mp3,image2 --enable-protocol=file \
    --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
    && make -j"$JOBS" && make install)
echo "Dependencies in $PREFIX"

