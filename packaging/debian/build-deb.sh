#!/bin/sh
# Builds a Debian package (.deb) of mac68k-disk with dpkg-deb - no debhelper needed.
#   packaging/debian/build-deb.sh [version]      -> mac68k-disk_<version>_<arch>.deb
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
VERSION=${1:-$(sed -n 's/^#define MAC68K_DISK_VERSION "\([0-9.]*\)".*/\1/p' "$ROOT/src/main.c")}
ARCH=$(dpkg --print-architecture)
PKG=$(mktemp -d)
trap 'rm -rf "$PKG"' EXIT
make -C "$ROOT" clean all
make -C "$ROOT" install PREFIX=/usr DESTDIR="$PKG"
mkdir -p "$PKG/usr/share/doc/mac68k-disk" "$PKG/DEBIAN"
cp "$ROOT/README.md" "$PKG/usr/share/doc/mac68k-disk/"
cp "$ROOT/LICENSE" "$PKG/usr/share/doc/mac68k-disk/copyright"
cat > "$PKG/DEBIAN/control" <<CTL
Package: mac68k-disk
Version: $VERSION
Section: otherutils
Priority: optional
Architecture: $ARCH
Maintainer: Tobias Bircher <mac68k@bircher.ai>
Depends: libc6
Homepage: https://github.com/bircher988/mac68k-disk
Description: disk images for the classic 68k Macintosh (MFS and HFS)
 Creates and edits floppy disk images for the first Macintosh models:
 MFS (400K, Macintosh 128K/512K with the 64K ROM) and HFS (400K, 800K,
 1440K). Adds MacBinary or raw files, lists, extracts and deletes files,
 creates folders. The images work in Mini vMac, Basilisk II and on real
 hardware.
CTL
find "$PKG" -type d -exec chmod 755 {} +
chmod 644 "$PKG/usr/share/doc/mac68k-disk/"* "$PKG/DEBIAN/control"
dpkg-deb --build --root-owner-group "$PKG" "$ROOT/mac68k-disk_${VERSION}_${ARCH}.deb"
echo "built $ROOT/mac68k-disk_${VERSION}_${ARCH}.deb"
