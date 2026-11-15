#!/bin/bash
# Pack the run-in-place tree /home/user/linuxcnc-2.9.10 into split .tar.xz parts (< 95 MB each,
# GitHub's file limit is 100 MB) and SHA256SUMS, in the current directory. Leaves out object
# files, dependency files, the docs sources (keeps docs/man) and test results.
set -euo pipefail
LCNC=/home/user/linuxcnc-2.9.10
rm -f linuxcnc-2.9.10-uspace-x86_64.tar.xz.* SHA256SUMS
tar -C /home/user --owner=0 --group=0 --sort=name \
    --exclude='linuxcnc-2.9.10/src/objects' --exclude='*.o' --exclude='*.d' \
    --exclude='linuxcnc-2.9.10/src/depends' --exclude='linuxcnc-2.9.10/docs/src' \
    --exclude='linuxcnc-2.9.10/docs/html' --exclude='linuxcnc-2.9.10/pybin' \
    --exclude='linuxcnc-2.9.10/BIN_BRANCH' \
    --exclude='linuxcnc-2.9.10/tests/*/result' --exclude='linuxcnc-2.9.10/tests/*/stderr' \
    --exclude='linuxcnc-2.9.10/tests/*/*/result' --exclude='linuxcnc-2.9.10/tests/*/*/stderr' \
    -cf - linuxcnc-2.9.10 | xz -T0 -9 | split -b 95M -d - linuxcnc-2.9.10-uspace-x86_64.tar.xz.
sha256sum linuxcnc-2.9.10-uspace-x86_64.tar.xz.* > SHA256SUMS
cat SHA256SUMS; du -ch linuxcnc-2.9.10-uspace-x86_64.tar.xz.* | tail -1
