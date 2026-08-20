#!/usr/bin/env bash
# Build the PORTABLE Linux release binary in a rootless Ubuntu 22.04 sandbox
# (bubblewrap). Result: build-linux-release/{dude,base.so,d3xp.so} with
#   - glibc floor 2.34/2.35 (works on Ubuntu 22.04+/Debian 12+/all gaming distros)
#   - libstdc++/libgcc/shaderc linked statically
#   - RUNPATH $ORIGIN/lib for the bundled Vulkan-loader fallback
#
# First run self-provisions deps-linux-release/ (~30 MB rootfs download + a
# one-time shaderc/glslang build inside the sandbox). Needs: bwrap, curl, network.
#
# Usage: ./build-linux-release.sh [-c]   (-c wipes build-linux-release/)

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS="$ROOT/deps-linux-release"
ROOTFS="$DEPS/rootfs"
UBASE=ubuntu-base-22.04.5-base-amd64.tar.gz
SHADERC_TAG=v2025.4

[ "${1:-}" = "-c" ] && rm -rf "$ROOT/build-linux-release"

enter() { # run a command as mapped-root inside the sandbox
	bwrap \
		--bind "$ROOTFS" / \
		--proc /proc --dev /dev --tmpfs /tmp \
		--bind "$ROOT" /work \
		--ro-bind /etc/resolv.conf /etc/resolv.conf \
		--unshare-user --uid 0 --gid 0 \
		--setenv HOME /root \
		--setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		--setenv DEBIAN_FRONTEND noninteractive \
		"$@"
}

# ---- one-time sandbox provisioning ----------------------------------------
if [ ! -x "$ROOTFS/usr/bin/gcc" ]; then
	echo ">> provisioning Ubuntu 22.04 rootfs in $ROOTFS"
	mkdir -p "$DEPS"
	[ -f "$DEPS/$UBASE" ] || curl -fLo "$DEPS/$UBASE" \
		"https://cdimage.ubuntu.com/ubuntu-base/releases/22.04/release/$UBASE"
	rm -rf "$ROOTFS"; mkdir -p "$ROOTFS"
	tar xzf "$DEPS/$UBASE" -C "$ROOTFS"
	echo 'APT::Sandbox::User "root";' > "$ROOTFS/etc/apt/apt.conf.d/99-bwrap"
	sed -i 's/jammy\(-updates\)\? main restricted$/& universe multiverse/' \
		"$ROOTFS/etc/apt/sources.list"
	enter apt-get update -qq
	enter apt-get install -y -qq --no-install-recommends \
		build-essential cmake git python3 ca-certificates \
		libsdl2-dev libopenal-dev libcurl4-openssl-dev libvulkan-dev
fi

if [ ! -f "$ROOTFS/usr/local/lib/libshaderc_combined.a" ]; then
	echo ">> building static shaderc $SHADERC_TAG + glslang in the sandbox (one-time)"
	enter bash -ec "
		rm -rf /opt/shaderc
		git clone -q --depth 1 --branch $SHADERC_TAG https://github.com/google/shaderc /opt/shaderc
		cd /opt/shaderc && ./utils/git-sync-deps >/dev/null 2>&1
		cmake -B build -DCMAKE_BUILD_TYPE=Release -DSHADERC_SKIP_TESTS=ON \
			-DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON >/dev/null
		cmake --build build -j\$(nproc) --target shaderc_combined glslang-standalone >/dev/null
		mkdir -p /usr/local/lib /usr/local/include /usr/local/bin
		cp build/libshaderc/libshaderc_combined.a /usr/local/lib/
		cp -r libshaderc/include/shaderc /usr/local/include/
		cp \$(find build -type f -executable -name glslang | head -1) /usr/local/bin/glslangValidator
		git clone -q --depth 1 https://github.com/KhronosGroup/Vulkan-Headers /opt/vulkan-headers"
fi

# ---- configure + build ------------------------------------------------------
LINKFLAGS='-static-libstdc++ -static-libgcc -Wl,-rpath,$ORIGIN/lib,--enable-new-dtags'
if [ ! -f "$ROOT/build-linux-release/CMakeCache.txt" ]; then
	echo ">> configuring (Ubuntu 22.04 sandbox, Vulkan ON, static runtimes)"
	enter cmake -S /work/neo -B /work/build-linux-release \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DDHEWM3_VULKAN=ON \
		-DVulkan_INCLUDE_DIR=/opt/vulkan-headers/include \
		-DVulkan_LIBRARY=/usr/lib/x86_64-linux-gnu/libvulkan.so \
		-DVulkan_GLSLANG_VALIDATOR_EXECUTABLE=/usr/local/bin/glslangValidator \
		-DSHADERC_INCLUDE_DIR=/usr/local/include \
		-DSHADERC_LIBRARY=/usr/local/lib/libshaderc_combined.a \
		-DCMAKE_EXE_LINKER_FLAGS="$LINKFLAGS" \
		-DCMAKE_SHARED_LINKER_FLAGS="$LINKFLAGS"
fi

echo ">> building"
enter cmake --build /work/build-linux-release -j"$(nproc)"

echo ">> done: $ROOT/build-linux-release/dude"
echo "   bundle-loader fallback: copy the sandbox's libvulkan.so.1 into <staging>/lib/"