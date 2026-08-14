# Maintainer: Terascale Functionalists
#
# Packages the RS480/RS482 (RS4xx) read-path-enhanced radeontop that ships in
# this fork.  On the pre-R600 R300-class IGP the radeon DRM read-reg ioctl
# rejects every register, so this fork reads RBBM_STATUS (0x0E40) through the
# BAR2 PCI sysfs resourceN node; `radeontop -m` forces that path.  Upstream
# radeontop ships no PKGBUILD, so this builds the fork's tree directly.
#
# Run from this directory: makepkg -si
#
# For a byte-identical artifact, pin the build timestamp to the source it
# builds.  makepkg otherwise stamps the current time into builddate and into
# every archive member mtime, and two clean builds of the same commit then
# differ in the package file while every packaged file stays identical:
#
#   SOURCE_DATE_EPOCH=$(git show -s --format=%ct "$_commit") makepkg -f

pkgname=radeontop-gororoba

# The base version upstream released and the commit its tag names.  pkgver()
# counts from that object, so the count holds in a clone whose tags differ.
_basever=1.4
_basecommit='15521706464b78dc9af60495b648b9d536b4d085'

# The exact source this package builds.  A full object id pins one immutable
# tree: a branch follows later commits, a tag moves, and copying the working
# directory ships tracked modifications and untracked files alike.  Advance this
# to a commit already merged to master, because a squash or rebase merge
# rewrites the object ids a pull request head carried and can leave a pinned
# pre-merge object unreachable.
_commit='1b89873e9ba4167bf71eb1ad516fe581e59d69d5'

# pkgver identifies the source and pkgrel identifies the packaging.  Changing
# _commit changes pkgver and resets pkgrel to 1; changing the recipe, the
# dependencies, the flags, or the installed artifact against an unchanged
# _commit increments pkgrel.  The literal below equals what pkgver() derives for
# _commit, and `makepkg --nobuild && git diff --exit-code PKGBUILD` proves it,
# because makepkg rewrites this line when the two disagree.
pkgver=1.4.r117.g1b89873e9ba4
pkgrel=1

pkgdesc="GPU utilization monitor with RS480/RS482 (RS4xx) BAR2 read-path"
url="https://github.com/Oichkatzelesfrettschen/radeontop-gororoba"
arch=('x86_64')
# The source grants GPL version 3 with no "or later" clause.
license=('GPL-3.0-only')
depends=('libpciaccess' 'libdrm' 'ncurses' 'libxcb')
# base-devel supplies pkgconf and gettext.  git fetches the pinned source.
makedepends=('git')
checkdepends=('python')
provides=("radeontop=$pkgver")
conflicts=('radeontop')

source=("$pkgname::git+$url.git#commit=$_commit")
# The pinned object id is the integrity check: git verifies the checkout against
# it, so a content hash over the export restates the same guarantee.
b2sums=('SKIP')

pkgver() {
  cd "$srcdir/$pkgname"

  printf '%s.r%s.g%s' \
    "$_basever" \
    "$(git rev-list --count "$_basecommit..HEAD")" \
    "$(git rev-parse --short=12 HEAD)"
}

build() {
  cd "$srcdir/$pkgname"

  # VERSION stamps include/version.h, so the binary reports the pinned revision
  # rather than whichever repository surrounds the build directory.  plain=1
  # withholds the Makefile's own -s, because makepkg owns stripping and the
  # debug-symbol split.
  make PREFIX=/usr plain=1 VERSION="$pkgver"
}

check() {
  cd "$srcdir/$pkgname"

  # The binary names the source that produced it.  A mismatch means the stamp
  # did not reach the build and the package identity is unverifiable.
  test "$(./radeontop --version)" = "RadeonTop $pkgver"

  ./familycheck.sh

  # The collector unit tests run against the pinned source, so the package
  # attests to the instrument's behavior rather than only to its version string
  # and its PCI tables.  They need no GPU: the backend and the clock are
  # injected.
  make check
}

package() {
  cd "$srcdir/$pkgname"

  make PREFIX=/usr DESTDIR="$pkgdir" plain=1 VERSION="$pkgver" install
  install -Dm644 COPYING "$pkgdir/usr/share/licenses/$pkgname/COPYING"
}
