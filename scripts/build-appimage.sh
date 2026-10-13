BUILD_CONFIG="release"
BUILD_JOBS=${BUILD_JOBS:-$(python3 scripts/linux-build-budget.py --jobs)}
[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid BUILD_JOBS' >&2; exit 1; }
export MAKEFLAGS="-j$BUILD_JOBS"

fail()
{
	echo "$1" 1>&2
	exit 1
}

BUILD_ROOT=$PWD/build
SOURCE_ROOT=$PWD
BUILD_FOLDER=$BUILD_ROOT/build-$BUILD_CONFIG
DEPLOY_FOLDER=$BUILD_ROOT/deploy-$BUILD_CONFIG
INSTALLER_FOLDER=$BUILD_ROOT/installer-$BUILD_CONFIG

LINUXDEPLOY=linuxdeploy-$(uname -m).AppImage

if [ -n "$CI_VERSION" ]; then
  VERSION=$CI_VERSION
else
  VERSION=`cat $SOURCE_ROOT/app/version.txt`
fi

command -v qmake6 >/dev/null 2>&1 || fail "Unable to find 'qmake6' in your PATH!"
command -v $LINUXDEPLOY >/dev/null 2>&1 || fail "Unable to find '$LINUXDEPLOY' in your PATH!"

echo Cleaning output directories
rm -rf $BUILD_FOLDER
rm -rf $DEPLOY_FOLDER
rm -rf $INSTALLER_FOLDER
mkdir $BUILD_ROOT
mkdir $BUILD_FOLDER
mkdir $DEPLOY_FOLDER
mkdir $INSTALLER_FOLDER

# Enable LTO for official builds
# Each make worker may link a separate target; keep LTO from spawning another
# full CPU-sized pool inside every link. LTO remains enabled.
export CFLAGS="-flto=1"
export CXXFLAGS="-flto=1"
export LDFLAGS="-flto=1"

echo Configuring the project
pushd $BUILD_FOLDER
# Native Wayland feedback requires Moonlight, SDL and matching Qt Wayland plugins.
# Use host libwayland with host Mesa/EGL: an older bundled libwayland can hide
# symbols required by the host Mesa implementation, breaking EGL even on X11.
# We disable DRM support because linuxdeploy doesn't bundle the appropriate libraries for Qt EGLFS.
compiler_args=()
if [ -n "${CC:-}" ]; then compiler_args+=("QMAKE_CC=$CC"); fi
if [ -n "${CXX:-}" ]; then compiler_args+=("QMAKE_CXX=$CXX"); fi
qmake6 $SOURCE_ROOT/moonlight-qt.pro CONFIG+=disable-libdrm PREFIX=$DEPLOY_FOLDER/usr DEFINES+=APP_IMAGE "${compiler_args[@]}" || fail "Qmake failed!"
popd

echo Compiling Moonlight in $BUILD_CONFIG configuration
pushd $BUILD_FOLDER
make -j"$BUILD_JOBS" $(echo "$BUILD_CONFIG" | tr '[:upper:]' '[:lower:]') || fail "Make failed!"
grep -q -- '-DHAS_WAYLAND' app/Makefile.Release || fail "Native Wayland feedback was not compiled"
popd

echo Deploying to staging directory
pushd $BUILD_FOLDER
make install || fail "Make install failed!"
popd

export QML_SOURCES_PATHS=$SOURCE_ROOT/app/gui
export QMAKE=qmake6
export EXTRA_PLATFORM_PLUGINS='libqwayland-egl.so;libqwayland-generic.so'
export EXTRA_QT_MODULES='waylandclient;waylandcompositor'

# Stage the build-environment libva in opt/libva-fallback (outside usr/, so linuxdeploy
# does not scan it, and outside every loader search path), together with a small probe
# binary that carries the same libva ELF requirements as Moonlight. The AppRun below
# uses the probe to decide, via the dynamic loader itself, whether the host libva can
# satisfy Moonlight; when it cannot, the fallback copy is made visible. opt/ is a
# standard linuxdeploy location for application data that must not be processed.
LIBVA_FALLBACK_DIR=$DEPLOY_FOLDER/opt/libva-fallback
SYSTEM_LIBVA=$(ldconfig -p 2>/dev/null | awk '/libva\.so\.2/{print $NF; exit}')
mkdir -p $LIBVA_FALLBACK_DIR
if [ -n "$SYSTEM_LIBVA" ]; then
  cp -a "$(dirname "$SYSTEM_LIBVA")"/libva*.so* $LIBVA_FALLBACK_DIR/ || fail "Unable to stage libva fallback copy!"

  echo Compiling libva-probe
  cc -O2 -L"$(dirname "$SYSTEM_LIBVA")" -Wl,--no-as-needed -o $LIBVA_FALLBACK_DIR/libva-probe \
    $SOURCE_ROOT/app/deploy/linux/libva-probe.c -lva -lva-x11 || fail "Unable to compile libva-probe!"

  # Keep the probe honest: it must reference every VA_API_* version node that the
  # binaries shipped in the AppImage require, otherwise a host libva could pass the
  # probe and still fail to load Moonlight or the bundled FFmpeg.
  va_nodes() { LC_ALL=C readelf -V "$1" 2>/dev/null | grep -oE 'VA_API_[0-9]+\.[0-9]+\.[0-9]+' | sort -u; }
  NEEDED_NODES=$(for b in $DEPLOY_FOLDER/usr/bin/moonlight \
                          /usr/local/lib*/libav*.so* /usr/local/lib*/libsw*.so* \
                          /usr/lib/x86_64-linux-gnu/libav*.so* /usr/lib/x86_64-linux-gnu/libsw*.so*; do
                   [ -f "$b" ] && va_nodes "$b"; done | sort -u)
  PROBE_NODES=$(va_nodes $LIBVA_FALLBACK_DIR/libva-probe)
  [ -z "$(comm -13 <(echo "$PROBE_NODES") <(echo "$NEEDED_NODES"))" ] || \
    fail "libva-probe is missing version node(s): $(comm -13 <(echo "$PROBE_NODES") <(echo "$NEEDED_NODES")) - update app/deploy/linux/libva-probe.c!"
fi

APP_RUN=$BUILD_ROOT/AppRun-libva
cat > $APP_RUN <<'APPRUN_EOF'
#!/bin/bash
# AppRun: prefer the host libva; use the staged copy only if the host cannot run us.
#
# VA-API driver modules on the host are loaded by libva and export an entrypoint
# named after the libva version they were built against (__vaDriverInit_1_XX).
# A libva can only load drivers that are not newer than itself, so bundling our
# own (older) libva silently breaks hardware decoding on up-to-date distros.
# Distros keep host libva and host drivers in step, so whenever the host libva
# can satisfy Moonlight's own ELF version requirements it is the right choice.
#
# "Can satisfy" is answered here by libva-probe, a tiny program linked against
# the same versioned libva symbols as Moonlight and the bundled FFmpeg: if the
# dynamic loader can start it, the host libva works; if not (no libva installed,
# or one too old to link), we make the staged build-environment copy visible
# via LD_LIBRARY_PATH, matching the pre-existing bundled behavior.
APPDIR="${APPDIR:-$(dirname "$(readlink -f "$0")")}"
LIBVA_FALLBACK="$APPDIR/opt/libva-fallback"

if [ -d "$LIBVA_FALLBACK" ] && ! "$LIBVA_FALLBACK/libva-probe" 2>/dev/null; then
    export LD_LIBRARY_PATH="$LIBVA_FALLBACK${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export VAAPI_USE_FALLBACK_PATHS=1
fi

exec "$APPDIR/usr/bin/moonlight" "$@"
APPRUN_EOF
chmod +x $APP_RUN

echo Creating AppImage
pushd $INSTALLER_FOLDER
# Don't bundle libva: the bundled build-environment version (jammy: VA-API 1.20/1.22)
# cannot dlopen GPU drivers compiled against newer libva on the host (which export
# __vaDriverInit_1_23+), so va_openDriver() always fails and VAAPI falls back to
# software decoding. The host always provides libva on systems where VA-API is
# usable, so link against it at runtime instead (the AppRun shim above keeps a
# bundled last-resort copy for hosts without libva).
host_libraries=(--exclude-library='libwayland-*.so*' --exclude-library='libEGL*.so*' --exclude-library='libGLX_mesa.so*'
  --exclude-library='libva.so*' --exclude-library='libva-drm.so*' --exclude-library='libva-wayland.so*' --exclude-library='libva-x11.so*')
# Deploy Qt before creating the image: the Qt plugin may copy dependencies even
# when the main deployer excludes them. Remove only host ABI libraries in this
# known staging directory, then maintain exclusions during final AppDir scanning.
$LINUXDEPLOY --appdir $DEPLOY_FOLDER \
  --library=/usr/local/lib/libSDL3.so.0 \
  "${host_libraries[@]}" --plugin qt || fail "linuxdeploy Qt staging failed!"
find "$DEPLOY_FOLDER/usr/lib" -maxdepth 1 \
  \( -name 'libwayland-*.so*' -o -name 'libEGL*.so*' -o -name 'libGLX_mesa.so*' -o -name 'libva*.so*' \) -delete
VERSION=$VERSION $LINUXDEPLOY --appdir $DEPLOY_FOLDER \
  "${host_libraries[@]}" --custom-apprun $APP_RUN --output appimage || fail "linuxdeploy AppImage creation failed!"
popd

echo Build successful
