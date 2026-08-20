#!/bin/sh
set -e
cd "$(dirname "$0")"
if [ ! -d .kp/.git ]; then
  git clone --depth 1 https://github.com/bmax121/KernelPatch.git .kp
fi
ln -sfn .kp/kernel kernel
echo "KernelPatch ready: kernel -> .kp/kernel"
