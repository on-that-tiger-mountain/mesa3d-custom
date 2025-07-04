#!/usr/bin/env bash
# shellcheck disable=SC2086 # we want word splitting

# When changing this file, you need to bump the following
# .gitlab-ci/image-tags.yml tags:
# DEBIAN_BASE_TAG
# DEBIAN_TEST_GL_TAG
# DEBIAN_TEST_VK_TAG
# KERNEL_ROOTFS_TAG

set -uex

uncollapsed_section_start crosvm "Building crosvm"

git config --global user.email "mesa@example.com"
git config --global user.name "Mesa CI"

CROSVM_VERSION=e27efaf8f4bdc4a47d1e99cc44d2b6908b6f36bd
git clone --single-branch -b main --no-checkout https://chromium.googlesource.com/crosvm/crosvm /platform/crosvm
pushd /platform/crosvm
git checkout "$CROSVM_VERSION"
git submodule update --init

VIRGLRENDERER_VERSION=7570167549358ce77b8d4774041b4a77c72a021c
rm -rf third_party/virglrenderer
git clone --single-branch -b main --no-checkout https://gitlab.freedesktop.org/virgl/virglrenderer.git third_party/virglrenderer
pushd third_party/virglrenderer
git checkout "$VIRGLRENDERER_VERSION"
meson setup build/ -D libdir=lib -D render-server-worker=process -D venus=true ${EXTRA_MESON_ARGS:-}
meson install -C build
popd

rm rust-toolchain

RUSTFLAGS='-L native=/usr/local/lib' cargo install \
  bindgen-cli \
  --locked \
  -j ${FDO_CI_CONCURRENT:-4} \
  --root /usr/local \
  --version 0.71.1 \
  ${EXTRA_CARGO_ARGS:-}

CROSVM_USE_SYSTEM_MINIGBM=1 CROSVM_USE_SYSTEM_VIRGLRENDERER=1 RUSTFLAGS='-L native=/usr/local/lib' cargo install \
  -j ${FDO_CI_CONCURRENT:-4} \
  --locked \
  --features 'default-no-sandbox gpu x virgl_renderer' \
  --path . \
  --root /usr/local \
  ${EXTRA_CARGO_ARGS:-}

popd

rm -rf /platform/crosvm

section_end crosvm
