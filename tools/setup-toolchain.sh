#!/usr/bin/env bash
#
# Rootless, devkitPro-compatible Switch toolchain.
#
# Installs into $DEVKITPRO_LOCAL (default: ~/devkitpro-local):
#   tools/                     ARM GNU aarch64-none-elf GCC 13.2 + patched newlib
#   devkitA64/{bin,base_rules} shim expected by libnx/switch_rules
#   libnx/{include,lib,...}    built from switchbrew/libnx
#   portlibs/switch/           mbedTLS 2.28 + ffmpeg 7.1 for Horizon OS
#   tools/bin/{elf2nro,nacptool,bin2o.sh}
#
# Usage:
#   ./tools/setup-toolchain.sh
#   export DEVKITPRO=$HOME/devkitpro-local
#   export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH
#   make
#
set -euo pipefail

DEST="${DEVKITPRO_LOCAL:-$HOME/devkitpro-local}"
TC_VER="13.2.1-1.1"
TC_NAME="xpack-aarch64-none-elf-gcc-${TC_VER}"
TC_URL="https://github.com/xpack-dev-tools/aarch64-none-elf-gcc-xpack/releases/download/v${TC_VER}/${TC_NAME}-linux-x64.tar.gz"

NEWLIB_PATCH_URL="https://raw.githubusercontent.com/devkitPro/buildscripts/master/patches/newlib-4.6.0.20260123-5.patch"
NEWLIB_TARBALL="https://codeload.github.com/devkitPro/newlib/tar.gz/refs/heads/devkitPro"

MBEDTLS_VER="2.28.10"
MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VER}/mbedtls-${MBEDTLS_VER}.tar.bz2"
MBEDTLS_PATCH_URL="https://raw.githubusercontent.com/devkitPro/pacman-packages/master/switch/mbedtls/mbedtls-${MBEDTLS_VER}.patch"

FFMPEG_VER="7.1"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VER}.tar.xz"
FFMPEG_PATCH_URL="https://raw.githubusercontent.com/devkitPro/pacman-packages/master/switch/ffmpeg/ffmpeg-${FFMPEG_VER}.patch"
FFMPEG_TLS_URL="https://raw.githubusercontent.com/devkitPro/pacman-packages/master/switch/ffmpeg/tls.patch"

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

fetch() {
    local url="$1" out="$2"
    [ -f "$out" ] || curl -L --retry 20 --retry-all-errors --retry-delay 3 -o "$out" "$url"
    # resume if a previous attempt was interrupted
    curl -L -C - --retry 20 --retry-all-errors --retry-delay 3 -sS -o "$out" "$url" || true
    [ -s "$out" ] || { echo "download failed: $url" >&2; exit 1; }
}

say "Каталог: $DEST"
mkdir -p "$DEST"
cd "$DEST"

# ---------------------------------------------------------------------------
# 1. ARM GNU toolchain
# ---------------------------------------------------------------------------
if [ ! -x "$DEST/tools/bin/aarch64-none-elf-gcc" ]; then
    say "ARM GNU Toolchain ${TC_VER}"
    fetch "$TC_URL" tc.tar.gz
    tar -xf tc.tar.gz
    rm -rf tools && mv "$TC_NAME" tools
    rm -f tc.tar.gz
else
    say "Toolchain уже установлен"
fi

# ---------------------------------------------------------------------------
# 2. newlib with devkitPro patches (devoptab, dynamic reent, lock wrappers)
# ---------------------------------------------------------------------------
if [ ! -f "$DEST/tools/aarch64-none-elf/include/sys/iosupport.h" ] || \
   [ ! -f "$DEST/.newlib-patched" ]; then
    say "newlib (devkitPro fork + patches)"
    if [ ! -d "$DEST/newlib-devkitPro" ]; then
        fetch "$NEWLIB_TARBALL" newlib-src.tar.gz
        tar -xf newlib-src.tar.gz
        rm -f newlib-src.tar.gz
    fi

    fetch "$NEWLIB_PATCH_URL" newlib-dkp.patch
    (
        cd "$DEST/newlib-devkitPro"
        patch -p1 -N --batch -i "$DEST/newlib-dkp.patch" || true
        # The fork already contains the RELR typedefs; the patch adds a second
        # copy. Keep exactly one.
        python3 - <<'PY'
p = 'newlib/libc/include/elf.h'
lines = open(p).read().split('\n')
out, seen, i = [], False, 0
while i < len(lines):
    if (lines[i].strip() == 'typedef Elf32_Word Elf32_Relr;'
            and i + 1 < len(lines)
            and lines[i + 1].strip() == 'typedef Elf64_Xword Elf64_Relr;'):
        if not seen:
            out.extend([lines[i], lines[i + 1]])
            seen = True
        i += 2
        continue
    out.append(lines[i])
    i += 1
open(p, 'w').write('\n'.join(out))
PY

        # These const objects contain relocatable pointers. If they stay in
        # .rodata, NRO startup crashes in libnx _dynProcessRela (write to RO).
        sed -i 's/^const struct lc_time_T\t_C_time_locale = {/struct lc_time_T\t_C_time_locale = {/' \
            newlib/libc/locale/timelocal.c
        sed -i 's/extern const struct lc_time_T _C_time_locale;/extern struct lc_time_T _C_time_locale;/' \
            newlib/libc/locale/setlocale.h
        sed -i 's/^const devoptab_t dotab_stdnull = {/devoptab_t dotab_stdnull = {/' \
            libgloss/libsysbase/iosupport.c
    )

    export PATH="$DEST/tools/bin:$PATH"
    rm -rf "$DEST/build-newlib" && mkdir -p "$DEST/build-newlib"
    (
        cd "$DEST/build-newlib"
        CFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" \
            ../newlib-devkitPro/configure \
            --target=aarch64-none-elf \
            --prefix="$DEST/tools" \
            --disable-newlib-supplied-syscalls \
            --enable-newlib-mb \
            --disable-newlib-wide-orient \
            --enable-newlib-register-fini
        make -j"$(nproc)" MAKEINFO=true
        make install -j1 MAKEINFO=true
    )
    touch "$DEST/.newlib-patched"
else
    say "newlib уже собран"
fi

# ---------------------------------------------------------------------------
# 3. devkitA64 shim (base_rules + bin2o)
# ---------------------------------------------------------------------------
say "devkitA64 shim"
mkdir -p "$DEST/devkitA64" "$DEST/portlibs" "$DEST/tools/bin"
rm -rf "$DEST/devkitA64/bin"
ln -sfn ../tools/bin "$DEST/devkitA64/bin"

cat > "$DEST/devkitA64/base_rules" <<'EOF'
# Local shim for devkitA64/base_rules (rootless install)
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=$HOME/devkitpro-local")
endif

.SUFFIXES:

TOOLCHAIN	:= $(DEVKITPRO)/devkitA64/bin

export CC	:= $(TOOLCHAIN)/aarch64-none-elf-gcc
export CXX	:= $(TOOLCHAIN)/aarch64-none-elf-g++
export AS	:= $(TOOLCHAIN)/aarch64-none-elf-as
export AR	:= $(TOOLCHAIN)/aarch64-none-elf-ar
export LD	:= $(TOOLCHAIN)/aarch64-none-elf-g++
export OBJCOPY	:= $(TOOLCHAIN)/aarch64-none-elf-objcopy
export STRIP	:= $(TOOLCHAIN)/aarch64-none-elf-strip
export NM	:= $(TOOLCHAIN)/aarch64-none-elf-nm
export RANLIB	:= $(TOOLCHAIN)/aarch64-none-elf-ranlib

export PORTLIBS_PATH ?= $(DEVKITPRO)/portlibs
export ADD_COMPILE_COMMAND := @:

DEPSDIR ?= $(CURDIR)

define bin2o
	$(DEVKITPRO)/tools/bin/bin2o.sh "$<" "$@"
endef

%.o: %.c
	@echo $(notdir $<)
	@$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) -c $< -o $@

%.o: %.cpp
	@echo $(notdir $<)
	@$(CXX) -MMD -MP -MF $(DEPSDIR)/$*.d $(CXXFLAGS) -c $< -o $@

%.o: %.s
	@echo $(notdir $<)
	@$(CC) $(ASFLAGS) -c $< -o $@

%.o: %.S
	@echo $(notdir $<)
	@$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) -c $< -o $@

%.a:
	@echo $(notdir $@)
	@rm -f $@
	@$(AR) -rc $@ $^

%_bin.h %.bin.o : %.bin
	@echo $(notdir $<)
	@$(bin2o)
EOF

cat > "$DEST/tools/bin/bin2o.sh" <<'EOF'
#!/usr/bin/env bash
# binary -> aarch64 object + header, used by the bin2o macro in base_rules
set -euo pipefail
in="$1"; out="$2"
name="$(basename "$in" | tr '.' '_')"
asm="$(mktemp "${TMPDIR:-/tmp}/bin2o.XXXXXX.s")"
trap 'rm -f "$asm"' EXIT
{
    printf '.section .rodata\n.balign 4\n'
    printf '.global %s\n.type %s, %%object\n%s:\n' "$name" "$name" "$name"
    printf '.incbin "%s"\n' "$(realpath "$in")"
    printf '.global %s_size\n%s_size:\n.int %s\n' "$name" "$name" "$(stat -c%s "$in")"
} > "$asm"
"${AS:-aarch64-none-elf-as}" "$asm" -o "$out"
{
    printf '#pragma once\n#include <switch.h>\n'
    printf 'extern const u8 %s[];\n' "$name"
    printf 'extern const u32 %s_size;\n' "$name"
} > "${name}.h"
EOF
chmod +x "$DEST/tools/bin/bin2o.sh"

# ---------------------------------------------------------------------------
# 4. libnx
# ---------------------------------------------------------------------------
export DEVKITPRO="$DEST"
export PATH="$DEST/tools/bin:$DEST/devkitA64/bin:$PATH"

if [ ! -f "$DEST/libnx/lib/libnx.a" ]; then
    say "libnx"
    if [ ! -d "$DEST/libnx/.git" ]; then
        rm -rf "$DEST/libnx"
        git clone --depth 1 https://github.com/switchbrew/libnx.git "$DEST/libnx"
    fi
    # xPack GCC has no -mtp=soft; default AArch64 TLS model is fine on Horizon.
    sed -i 's/ -mtp=soft//' "$DEST/libnx/nx/Makefile"
    make -C "$DEST/libnx" -j"$(nproc)"
    make -C "$DEST/libnx" install

    # Stock binutils has no Switch-specific linker options.
    sed -i 's/ --spare-dynamic-tags=0//; s/ -z nodynamic-undefined-weak//; s/ -z pack-relative-relocs//; s/ --nx-module-name//' \
        "$DEST/libnx/switch.specs"
else
    say "libnx уже собран"
fi

# ---------------------------------------------------------------------------
# 5. switch-tools: elf2nro + nacptool
# ---------------------------------------------------------------------------
if [ ! -x "$DEST/tools/bin/elf2nro" ]; then
    say "switch-tools"
    ST="$DEST/switch-tools"
    if [ ! -d "$ST/.git" ]; then
        rm -rf "$ST"
        git clone --depth 1 https://github.com/switchbrew/switch-tools.git "$ST"
    fi

    INC=""; LIB=""
    if [ ! -f /usr/include/lz4.h ]; then
        TMP="$DEST/.hostdeps"
        mkdir -p "$TMP"
        ( cd "$TMP"
          apt-get download liblz4-dev 2>/dev/null || true
          for f in *.deb; do dpkg-deb -x "$f" root; done )
        INC="-I$TMP/root/usr/include"
        LIB="-L$TMP/root/usr/lib/x86_64-linux-gnu"
    fi

    cc -O2 -w $INC -o "$DEST/tools/bin/elf2nro" \
       "$ST/src/elf2nro.c" "$ST/src/romfs.c" "$ST/src/sha256.c" \
       "$ST/src/filepath.c" $LIB -llz4
    cc -O2 -w -o "$DEST/tools/bin/nacptool" "$ST/src/nacptool.c"
else
    say "switch-tools уже собраны"
fi

# ---------------------------------------------------------------------------
# 6. mbedTLS portlib
# ---------------------------------------------------------------------------
if [ ! -f "$DEST/portlibs/switch/lib/libmbedtls.a" ]; then
    say "mbedTLS ${MBEDTLS_VER}"
    if [ ! -d "$DEST/mbedtls-${MBEDTLS_VER}" ]; then
        fetch "$MBEDTLS_URL" "mbedtls-${MBEDTLS_VER}.tar.bz2"
        tar -xf "mbedtls-${MBEDTLS_VER}.tar.bz2"
    fi
    fetch "$MBEDTLS_PATCH_URL" "mbedtls-${MBEDTLS_VER}.patch"
    (
        cd "$DEST/mbedtls-${MBEDTLS_VER}"
        patch -Np1 -i "$DEST/mbedtls-${MBEDTLS_VER}.patch"
        ./scripts/config.pl set MBEDTLS_ENTROPY_HARDWARE_ALT
        ./scripts/config.pl set MBEDTLS_NO_PLATFORM_ENTROPY
        ./scripts/config.pl unset MBEDTLS_SELF_TEST
        make -j"$(nproc)" lib \
            CC=aarch64-none-elf-gcc AR=aarch64-none-elf-ar \
            RANLIB=aarch64-none-elf-ranlib \
            CFLAGS="-O2 -fPIC -D__SWITCH__ -I$DEST/libnx/include"
        P="$DEST/portlibs/switch"
        mkdir -p "$P/include" "$P/lib"
        cp -r include/mbedtls include/psa "$P/include/"
        cp library/libmbedtls.a library/libmbedx509.a library/libmbedcrypto.a "$P/lib/"
    )
else
    say "mbedTLS уже собран"
fi

# ---------------------------------------------------------------------------
# 7. ffmpeg portlib (software H.264/AAC playback)
# ---------------------------------------------------------------------------
if [ ! -f "$DEST/portlibs/switch/lib/libavcodec.a" ]; then
    say "ffmpeg ${FFMPEG_VER}"
    if [ ! -d "$DEST/ffmpeg-src/ffmpeg-${FFMPEG_VER}" ]; then
        mkdir -p "$DEST/ffmpeg-src"
        fetch "$FFMPEG_URL" "$DEST/ffmpeg-src/ffmpeg-${FFMPEG_VER}.tar.xz"
        tar -xf "$DEST/ffmpeg-src/ffmpeg-${FFMPEG_VER}.tar.xz" -C "$DEST/ffmpeg-src"
        fetch "$FFMPEG_PATCH_URL" "$DEST/ffmpeg-src/ffmpeg-${FFMPEG_VER}.patch"
        fetch "$FFMPEG_TLS_URL" "$DEST/ffmpeg-src/tls.patch"
    fi
    (
        cd "$DEST/ffmpeg-src/ffmpeg-${FFMPEG_VER}"
        patch -Np1 -i ../ffmpeg-${FFMPEG_VER}.patch
        patch -Np1 -i ../tls.patch
        ./configure --prefix="$DEST/portlibs/switch" \
            --enable-cross-compile --target-os=horizon --arch=aarch64 \
            --cpu=cortex-a57 --cross-prefix=aarch64-none-elf- \
            --disable-shared --enable-static --enable-pic \
            --disable-programs --disable-doc --disable-debug --disable-autodetect \
            --disable-avdevice --disable-encoders --disable-muxers --disable-filters \
            --disable-everything --disable-network \
            --enable-decoder=h264,aac,mp3 --enable-parser=h264,aac \
            --enable-demuxer=mov --enable-protocol=file \
            --enable-swscale --enable-swresample \
            --enable-avformat --enable-avcodec --enable-avutil \
            --extra-cflags="-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -fPIC -ftls-model=local-exec -I$DEST/libnx/include" \
            --extra-ldflags="-specs=sysbase.specs -L$DEST/libnx/lib"
        make -j"$(nproc)"
        make install
    )
else
    say "ffmpeg уже собран"
fi


say "Готово"
cat <<EOF

  export DEVKITPRO=$DEST
  export PATH=\$DEVKITPRO/tools/bin:\$DEVKITPRO/devkitA64/bin:\$PATH

  make -j\$(nproc)
EOF
