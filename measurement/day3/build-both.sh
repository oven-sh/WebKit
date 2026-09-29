#!/bin/bash
/workspace/wkbuild/tools/build-new.sh relassert-new
/workspace/wkbuild/tools/build-new.sh release-new
/usr/bin/python3 /workspace/wkbuild/tools/make-noscav.py /workspace/wkbuild/release-new/bin/jsc >> /workspace/wkbuild/build-new.log 2>&1
echo "[$(date +%H:%M:%S)] both builds done" >> /workspace/wkbuild/build-new.log
