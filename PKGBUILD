# Maintainer: Terascale Functionalists
#
# Packages the RS480/RS482 (RS4xx) read-path-enhanced radeontop that ships in
# this fork.  On the pre-R600 R300-class IGP the radeon DRM read-reg ioctl
# rejects every register, so this fork reads RBBM_STATUS (0x0E40) through the
# BAR2 PCI sysfs resourceN node; `radeontop -m` forces that path.  Upstream
# radeontop ships no PKGBUILD, so this builds the fork's tree directly.
#
# Run from this directory: makepkg -si

pkgname=radeontop-gororoba
pkgver=1.4
# Bump pkgrel on every fork source change (new gauge, read-path fix, etc.) so a
# rebuilt package is a distinguishable revision and `pacman -Syu`/`-U` treats it
# as an upgrade.  -3: the collector's defined-value seed for the UVD/VCE words.
pkgrel=3
pkgdesc="GPU utilization monitor with RS480/RS482 (RS4xx) BAR2 read-path"
url="https://github.com/Oichkatzelesfrettschen/radeontop-gororoba"
arch=('x86_64')
license=('GPL3')
depends=('libpciaccess' 'libdrm' 'ncurses' 'libxcb')
makedepends=('pkgconf')
provides=('radeontop')
conflicts=('radeontop')
# The source is this repository.  Copy the tree into the build dir (excluding
# makepkg's own src/pkg and the VCS dir) so the build stays out of the checkout.
source=()
sha512sums=()

prepare() {
  rm -rf "$srcdir/build"
  mkdir -p "$srcdir/build"
  tar -C "$startdir" \
      --exclude=./src --exclude=./pkg --exclude=./.git \
      --exclude='./*.pkg.tar*' -cf - . | tar -C "$srcdir/build" -xf -
  cd "$srcdir/build"
  make clean >/dev/null 2>&1 || true
}

build() {
  cd "$srcdir/build"
  make PREFIX=/usr
}

package() {
  cd "$srcdir/build"
  make install PREFIX=/usr DESTDIR="$pkgdir"
  install -Dm644 COPYING "$pkgdir/usr/share/licenses/$pkgname/COPYING"
}
