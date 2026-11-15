# linuxcnc-bin-2.9.10-2026-11-15: prebuilt LinuxCNC 2.9.10 for cloud sessions

This orphan branch has no shared history with the code. It holds a prebuilt LinuxCNC tree, so that
a fresh cloud container (or a similar Ubuntu 24.04 x86_64 box) can run LinuxCNC, its tests and
the brdmot and stmbl simulations without a ~20 min build.

## Contents

| file | what |
|---|---|
| `linuxcnc-2.9.10-uspace-x86_64.tar.xz.NN` | the tree, split into parts under 95 MB |
| `SHA256SUMS` | checksums of the parts |
| `restore.sh` | unpacks the tree and sets it up: runtime packages, Python 3.12 shim, user `lcnc`, link `/home/user/linuxcnc-2.9` |
| `pack.sh` | how the parts were made |

## What it is

- **Source:** branch `2.9.10` of brdmaker/linuxcnc, commit `b4c570fdcb89370d9a1340c08af90286f267f1a6`. That is upstream v2.9.10, the
  fixes from upstream's `2.9` branch up to 94c87ada, `hm2_soc_ol`, and 14-pin hostmot2 port names.
- **Build:** uspace, run-in-place at `/home/user/linuxcnc-2.9.10`. Its paths are absolute, so
  it works only there.
- **Platform:** Ubuntu 24.04 x86_64, Python 3.12, GTK 2 and 3. The man pages are built, in
  `docs/man`.
- **Left out:** object files, the docs sources and the HTML/PDF docs.
- **Kept:** `src/Makefile.modinc`, `include/` and the sources, so modules build with
  `MODINC=/home/user/linuxcnc-2.9.10/src/Makefile.modinc` or `halcompile --install`.
- **Tests:** `scripts/runtests -p tests` passed as user `lcnc`, POSIX non-realtime, on
  2026-11-15. The result was 264 tests run, 264 successful, 0 failed. One expected failure
  passed (XPASS: `interp/m98m99/12-M99-endless-main-program`).

## Restore

`.claude/skills/linuxcnc-bin/restore.sh` in brdmot, or `hwtest/linuxcnc_restore.sh` in
stmbl, picks the newest `linuxcnc-bin-2.9.10-*` branch and runs this branch's `restore.sh`.

## How it was built

```
git -C linuxcnc archive 2.9.10 | tar -x -C /home/user/linuxcnc-2.9.10
apt-get install libtirpc-dev libudev-dev libmodbus-dev libusb-1.0-0-dev libreadline-dev \
  libgpiod-dev tcl-dev tk-dev bwidget tclx8.4-dev libboost-python-dev libxmu-dev \
  libglu1-mesa-dev libgl-dev yapps2 python3-dev intltool libxaw7-dev libxinerama-dev \
  libedit-dev libboost-dev python3-tk libxft-dev libgtk-3-dev libgtk2.0-dev libepoxy-dev \
  asciidoc-base docbook-xsl docbook-xml xsltproc groff
cd /home/user/linuxcnc-2.9.10/src
export PATH=/usr/bin:/bin:/usr/sbin:/sbin      # a python3.13 in /usr/local breaks configure
./autogen.sh
PYTHON_BIN=python3.12 ./configure --with-realtime=uspace --disable-check-runtime-deps
make -j$(nproc)
./pack.sh
```

The runtime package list in `restore.sh` comes from the ldd closure of the built files:

```
find bin lib rtlib -type f | xargs file | grep ELF | cut -d: -f1 | xargs ldd \
  | awk '/=> \//{print $3}' | sort -u | grep -v /home/user | xargs -n1 readlink -f \
  | xargs dpkg -S | cut -d: -f1 | sort -u
```
