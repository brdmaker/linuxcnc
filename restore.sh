#!/bin/bash
# Restore a prebuilt LinuxCNC 2.9.10 (uspace, run-in-place, x86_64) from an orphan branch
# linuxcnc-bin-2.9.10-<date> of brdmaker/linuxcnc, instead of building it (~15-25 min).
# The tree is built from branch 2.9.10 of brdmaker/linuxcnc (upstream v2.9.10 + the 2.9
# fixes + hm2_soc_ol); the branch's README says which commit and how it was built.
#
#   restore.sh [BRANCH]      default: the newest linuxcnc-bin-2.9.10-* branch
#   LCNC_QT=1 restore.sh     also PyQt5 & co for QtVCP screens (+ Xvfb), ~1 min more
#
# -> /home/user/linuxcnc-2.9.10 (the build's paths are absolute: it must live there),
#    /home/user/linuxcnc-2.9 -> linuxcnc-2.9.10 unless something else is there (the path the
#    Makefiles and tests of brdmot and stmbl look for), the runtime packages (Ubuntu 24.04
#    names), python3.12 first on PATH via $LCNC/pybin, and a user "lcnc" (rtapi_app refuses
#    root). Idempotent; ~1-2 min.
#
# Then, as a non-root user:
#   su lcnc -c 'export PATH=/home/user/linuxcnc-2.9.10/pybin:$PATH; \
#               source /home/user/linuxcnc-2.9.10/scripts/rip-environment; linuxcnc ...'
set -euo pipefail
REPO_URL=${LINUXCNC_BIN_REPO:-https://github.com/brdmaker/linuxcnc}
LCNC=/home/user/linuxcnc-2.9.10
BRANCH=${1:-}

if [ -z "$BRANCH" ]; then
  BRANCH=$(git ls-remote --heads "$REPO_URL" 'linuxcnc-bin-2.9.10-*' | sed 's#.*refs/heads/##' | sort | tail -1)
  [ -n "$BRANCH" ] || { echo "no linuxcnc-bin-2.9.10-* branch in $REPO_URL" >&2; exit 1; }
fi

if [ "$(cat "$LCNC/BIN_BRANCH" 2>/dev/null)" != "$BRANCH" ] || [ ! -x "$LCNC/bin/rtapi_app" ]; then
  tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
  git -C "$tmp" init -q
  git -C "$tmp" fetch -q --depth=1 "$REPO_URL" "refs/heads/$BRANCH"
  git -C "$tmp" show FETCH_HEAD:SHA256SUMS > "$tmp/SHA256SUMS"
  for f in $(awk '{print $2}' "$tmp/SHA256SUMS"); do git -C "$tmp" show "FETCH_HEAD:$f" > "$tmp/$f"; done
  (cd "$tmp" && sha256sum -c --quiet SHA256SUMS)
  rm -rf "$LCNC"
  cat "$tmp"/linuxcnc-2.9.10-uspace-x86_64.tar.xz.* | tar -C /home/user -xJf -
  echo "$BRANCH" > "$LCNC/BIN_BRANCH"
fi

# runtime packages: the build's ldd closure, Tcl/Tk, Python 3.12 and the modules AXIS and
# the GTK screens import (QtVCP screens would also need python3-pyqt5 & co: not included)
PKGS="python3.12 libpython3.12t64 python3-tk tcl8.6 tk8.6 tclx8.4 bwidget libboost-python1.83.0
  libmodbus5 libusb-1.0-0 libtirpc3t64 libgpiod2t64 libedit2 libgtk-3-0t64 libgtk2.0-0t64 libxmu6
  libxinerama1 libxss1 libxft2 libepoxy0 libgl1 libglu1-mesa python3-gi gir1.2-gtk-3.0 python3-cairo
  python3-gi-cairo python3-opengl python3-xlib python3-configobj python3-numpy procps psmisc"
missing=$(for p in $PKGS; do dpkg -s "$p" >/dev/null 2>&1 || echo "$p"; done)
if [ -n "$missing" ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get install -y -q --no-install-recommends $missing >/dev/null 2>&1 || {
    apt-get update -q >/dev/null; apt-get install -y -q --no-install-recommends $missing >/dev/null; }
fi

# QtVCP: PyQt5 for python3.12 (Ubuntu's python3-pyqt5 doesn't match) and the xcb plugin's libs
if [ "${LCNC_QT:-0}" = 1 ] && ! python3.12 -c 'import PyQt5.QtWidgets' 2>/dev/null; then
  DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends python3-pip xvfb \
    libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 \
    libxcb-xinerama0 libxcb-xkb1 libxkbcommon-x11-0 >/dev/null
  python3.12 -m pip install -q --break-system-packages PyQt5 QScintilla PyOpenGL
fi

# the old tree's path, which brdmot's and stmbl's Makefiles and tests default to
[ -e /home/user/linuxcnc-2.9 ] || ln -s linuxcnc-2.9.10 /home/user/linuxcnc-2.9

# LinuxCNC's Python modules are built for 3.12; the image's python3 may be another one
mkdir -p "$LCNC/pybin"
ln -sf /usr/bin/python3.12 "$LCNC/pybin/python3"
ln -sf /usr/bin/python3.12 "$LCNC/pybin/python"

# rtapi_app refuses to run as root; the tree must be writable for it (configs, tests, comps)
if [ "$(id -u)" = 0 ]; then
  id lcnc >/dev/null 2>&1 || useradd -m -s /bin/bash lcnc
  chown -R lcnc:lcnc "$LCNC"
  chmod o+x /home/user
fi
echo "LinuxCNC $(cat "$LCNC/VERSION") ($(cat "$LCNC/BRDMAKER_COMMIT" | cut -c1-8), $BRANCH) ready at $LCNC"
