#!/bin/sh

# The first argument is the authoritative version, and a packaging recipe passes
# the source revision it pinned so the binary reports exactly the sources that
# were compiled.  An ambient git query answers a developer build only: a tree
# copied out of a checkout carries no .git of its own, so git walks upward and
# reports the enclosing repository instead.

ver="${1:-}"

if [ -z "$ver" ]; then
  ver=unknown
  if command -v git >/dev/null 2>&1 && git rev-parse HEAD >/dev/null 2>&1; then
    ver=$(git describe 2>/dev/null) || ver=unknown
  fi
fi

out=include/version.h
tmp="$out.tmp"

cat > "$tmp" << EOF
#ifndef VER_H
#define VER_H

#define VERSION "$ver"

#endif
EOF

# Replace the header on a changed value only.  Every object includes it, so an
# unconditional write moves the mtime and rebuilds the whole tree each time the
# version target runs.
if cmp -s "$tmp" "$out"; then
  rm -f "$tmp"
else
  mv -f "$tmp" "$out"
fi
