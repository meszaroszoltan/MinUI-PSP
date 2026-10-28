#!/bin/bash
# verifies the PSP.pak 64-bit stack against the stock rg35xxplus rootfs, fails the build on any error
# runs inside the minui-psp64 docker image (make check)

cd "$(dirname "$0")"
OUT=output
SYSROOT=$(pwd)/.sysroot
CFG=${PSP_CFG:-/root/skeleton/EXTRAS/Emus/rg35xxplus/PSP.pak/default.cfg}
BINS="$OUT/minarch64.elf $OUT/ppsspp_libretro.so $OUT/lib/libSDL2-2.0.so.0 $OUT/lib/libmsettings.so"
# same order as PSP.pak/launch.sh: pak libs, stock mali, then multiarch
SEARCH="$OUT/lib $SYSROOT/usr/lib $SYSROOT/usr/lib/aarch64-linux-gnu"

errors=0
fail() { echo "FAIL: $*"; errors=$((errors+1)); }
pass() { echo "ok: $*"; }

# follows symlinks inside the sysroot (absolute targets are re-rooted, /lib is /usr/lib on stock)
resolve() {
	local path=$1
	for i in $(seq 1 16); do
		[ -L "$path" ] || break
		local target=$(readlink "$path")
		case "$target" in
			/lib/*) path="$SYSROOT/usr$target" ;;
			/*) path="$SYSROOT$target" ;;
			*) path="$(dirname "$path")/$target" ;;
		esac
	done
	[ -f "$path" ] && echo "$path"
}
find_lib() {
	for dir in $SEARCH; do
		local found=$(resolve "$dir/$1")
		[ -n "$found" ] && file -L "$found" | grep -q "ARM aarch64" && { echo "$found"; return; }
	done
}
needed() { readelf -d "$1" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'; }

echo "== 1. architecture"
for f in $BINS; do
	if [ ! -f "$f" ]; then fail "missing $f"; continue; fi
	file -L "$f" | grep -q "ELF 64-bit LSB .*ARM aarch64" && pass "$f is aarch64" || fail "$f is not aarch64: $(file -L "$f")"
done

echo "== 2. dependencies exist on stock (transitively)"
declare -A seen
queue=""
for f in $BINS; do queue="$queue $(needed "$f")"; done
while [ -n "${queue// /}" ]; do
	set -- $queue
	lib=$1; shift; queue="$*"
	[ -n "${seen[$lib]}" ] && continue
	found=$(find_lib "$lib")
	if [ -z "$found" ]; then
		seen[$lib]=missing
		fail "$lib not found in PSP.pak/lib or stock aarch64 libs"
		continue
	fi
	seen[$lib]=$found
	queue="$queue $(needed "$found")"
done
for lib in "${!seen[@]}"; do [ "${seen[$lib]}" != missing ] && echo "   $lib -> ${seen[$lib]#$(pwd)/}"; done | sort

# dlopen()ed by SDL, not in DT_NEEDED
for lib in libasound.so.2 libEGL.so.1 libGLESv2.so.2; do
	[ -n "${seen[$lib]}" ] && continue
	found=$(find_lib "$lib")
	[ -n "$found" ] && seen[$lib]=$found || fail "$lib (dlopen) not found on stock"
done
for lib in libEGL.so.1 libGLESv2.so.2; do
	echo "${seen[$lib]}" | grep -q "^$SYSROOT/usr/lib/lib" && pass "$lib resolves to stock mali" || fail "$lib resolves to ${seen[$lib]}, expected $SYSROOT/usr/lib (mali)"
done
mali=$(resolve "$SYSROOT/usr/lib/libmali.so.0")
for sym in egl_create_pixmap_ID_mapping eglCreatePbufferSurface glReadPixels glBindFramebuffer; do
	nm -D --defined-only "$mali" | grep -qw "$sym" && pass "libmali exports $sym" || fail "libmali missing $sym"
done

echo "== 3. undefined symbols against stock (ld.so trace, same as ldd -r)"
LDSO="$SYSROOT/usr/lib/ld-linux-aarch64.so.1"
LP=$(echo $SEARCH | tr ' ' ':')
for f in $BINS; do
	trace=$(LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_BIND_NOW=yes "$LDSO" --inhibit-cache --library-path "$LP" "$f" 2>&1)
	if echo "$trace" | grep -q "undefined symbol\|not found\|error"; then
		fail "$f:"; echo "$trace" | grep "undefined symbol\|not found\|error" | head -20
	else
		pass "$f links cleanly"
	fi
	# nothing may come from outside the sysroot or the pak
	echo "$trace" | grep "=>" | grep -v "$SYSROOT\|$OUT/lib\|$(pwd)/$OUT/lib" | grep -q . && { fail "$f resolved libs outside sysroot:"; echo "$trace" | grep "=>" | grep -v "$SYSROOT\|$OUT/lib"; }
done

echo "== 4. symbol versions"
for f in $BINS; do
	max=$(objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -1)
	[ -z "$max" ] || [ "$(printf '%s\n2.35\n' "$max" | sort -V | tail -1)" = "2.35" ] && pass "$f needs GLIBC_${max:-none} (stock 2.35)" || fail "$f needs GLIBC_$max > 2.35"
done
objdump -T "$OUT/ppsspp_libretro.so" | grep -q "GLIBCXX_\|CXXABI_" && fail "ppsspp_libretro.so depends on libstdc++ symbols" || pass "ppsspp_libretro.so has no GLIBCXX_/CXXABI_ requirements"
for lib in libstdc++.so.6 libgcc_s.so.1; do
	needed "$OUT/ppsspp_libretro.so" | grep -qx "$lib" && fail "ppsspp_libretro.so needs $lib" || pass "ppsspp_libretro.so doesn't need $lib"
done

echo "== 5. exports"
for sym in retro_init retro_load_game retro_run retro_serialize retro_unserialize retro_set_environment; do
	nm -D --defined-only "$OUT/ppsspp_libretro.so" | grep -qw "$sym" && pass "core exports $sym" || fail "core missing $sym"
done
nm -D "$OUT/minarch64.elf" | grep -qw SDL_GL_CreateContext && pass "minarch64 built with HAS_GLES" || fail "minarch64 lacks SDL_GL_CreateContext (HAS_GLES not set?)"

echo "== 6. assets"
for f in compat.ini ppge_atlas.zim flash0/font vfpu .version; do
	[ -e "$OUT/assets/$f" ] && pass "assets/$f" || fail "missing assets/$f"
done

echo "== 7. smoke test (sw + gl)"
if [ ! -f "$CFG" ]; then
	fail "missing $CFG (mount the skeleton, see the root makefile psp64 target)"
else
	DEMOS=src/ppsspp/pspautotests/demos
	rm -rf build/smoke-run && mkdir -p build/smoke-run/system build/smoke-run/saves
	cp -R $OUT/assets build/smoke-run/system/PPSSPP
	# cube: 3d + save states, controller: debug text that must end up top left (orientation)
	for test in cube:picture controller:toptext; do
		demo=${test%%:*}
		for mode in sw gl; do
			name=$demo-$mode
			LIBGL_ALWAYS_SOFTWARE=1 timeout 300 build/smoke $OUT/ppsspp_libretro.so $DEMOS/$demo.pbp build/smoke-run/system build/smoke-run/saves $mode 600 "$CFG" build/smoke-run/options.txt build/smoke-run/$name.bmp ${test#*:} > build/smoke-run/$name.log 2>&1
			status=$?
			grep "^smoke:\|^FAIL\|^PASS" build/smoke-run/$name.log | sed 's/^/   /'
			[ $status = 0 ] && pass "smoke $name" || { fail "smoke $name (exit $status, see build/smoke-run/$name.log)"; tail -20 build/smoke-run/$name.log; }
		done
	done

	echo "== 8. PSP.pak default.cfg keys and values exist in the core"
	grep -E '^-?ppsspp_' "$CFG" | sed 's/^-//' | while IFS= read -r line; do
		key=${line%% = *}
		value=${line#* = }
		values=$(awk -F'\t' -v k="$key" '$1==k {print $2}' build/smoke-run/options.txt)
		if [ -z "$values" ]; then echo "FAIL: unknown option $key"; continue; fi
		echo "|$values|" | grep -qF "|$value|" && echo "ok: $key = $value" || echo "FAIL: $key = $value not in $values"
	done > build/smoke-run/cfg.log
	cat build/smoke-run/cfg.log
	errors=$((errors + $(grep -c '^FAIL' build/smoke-run/cfg.log)))
fi

echo
if [ $errors != 0 ]; then
	echo "check: $errors error(s)"
	exit 1
fi
echo "check: all passed"
