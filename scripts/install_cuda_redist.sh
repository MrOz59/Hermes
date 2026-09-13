#!/usr/bin/env bash
# Install a CUDA toolkit from NVIDIA's redistributable archives.
#
# The per-release repositories under developer.download.nvidia.com/compute/cuda/repos
# only carry the toolkits NVIDIA chose to build for that release, and for the
# newer ones that is CUDA 13 only. CUDA 13 dropped code generation for compute
# capabilities below 7.5, so a package built against it leaves Maxwell through
# Volta - GTX 900 and 10-series, Titan V - with no kernel image, and NVENC dies
# at runtime with cudaErrorNoKernelImageForDevice. The redistributable archives
# are plain tarballs that carry no distro packaging, so a build can take the
# toolkit its GPU coverage needs rather than the one its release happens to ship.
#
# Only the components the build actually uses are fetched: nvcc, the runtime
# (CMake links libcudart_static.a, so nothing follows into the package) and the
# CCCL headers the runtime headers include. That is roughly 84 MB against the
# 4 GB of the monolithic runfile installer.
#
# Usage: install_cuda_redist.sh <cuda-version> [prefix]

set -euo pipefail

cuda_version="${1:?usage: install_cuda_redist.sh <cuda-version> [prefix]}"
prefix="${2:-/usr/local/cuda-${cuda_version%.*}}"

# Component versions do not track the toolkit version and have to be read off
# the release manifest, so they are pinned here rather than guessed. Refresh
# from https://developer.download.nvidia.com/compute/cuda/redist/redistrib_<version>.json
# when bumping cuda_version, and check crt/host_config.h for the new toolkit's
# host-compiler cap - it is what decides which distros can use it at all.
case "${cuda_version}" in
    12.9.1)
        components=(
            "cuda_nvcc:12.9.86"
            "cuda_cudart:12.9.79"
            "cuda_cccl:12.9.27"
        )
        ;;
    *)
        echo "No pinned component list for CUDA ${cuda_version}." >&2
        exit 1
        ;;
esac

base="https://developer.download.nvidia.com/compute/cuda/redist"
arch="linux-x86_64"

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

mkdir -p "${prefix}"

for component in "${components[@]}"; do
    name="${component%%:*}"
    version="${component##*:}"
    archive="${name}-${arch}-${version}-archive.tar.xz"

    echo "Fetching ${archive}"
    wget -q --tries=3 --retry-connrefused \
        -O "${tmp}/${archive}" \
        "${base}/${name}/${arch}/${archive}"
    tar -xJf "${tmp}/${archive}" --directory="${prefix}" --strip-components=1
done

# glibc 2.41 added the C23 rsqrt/sinpi/cospi family and declared it noexcept.
# CUDA 12's crt/math_functions.h declares the same six names without an
# exception specification, and nvcc's frontend rejects the pair - which kills
# every translation unit that reaches <cmath>, so nothing compiles at all, not
# even CMake's compiler-identification probe. Upstream Sunshine carries the same
# fix as a line-numbered patch file; matching on the declarations survives a
# toolkit bump, where a patch file would not.
math_functions="${prefix}/include/crt/math_functions.h"
if [ -f "${math_functions}" ]; then
    names="rsqrt|rsqrtf|sinpi|sinpif|cospi|cospif"
    before="$(grep -cE "noexcept \(true\)" "${math_functions}" || true)"
    sed -i -E \
        -e "s/^(extern __DEVICE_FUNCTIONS_DECL__ __device_builtin__[[:space:]]+(double|float)[[:space:]]+(${names})\([^()]*\));/\1 noexcept (true);/" \
        -e "s/^(__func__\((double|float) (${names})\([^()]*\))\);/\1 noexcept (true));/" \
        "${math_functions}"
    after="$(grep -cE "noexcept \(true\)" "${math_functions}" || true)"
    echo "Patched $((after - before)) math_functions.h declarations for glibc 2.41+"
fi

# The archives put their libraries in lib/, while everything that consumes a
# CUDA install - CMake's FindCUDAToolkit included - looks for the lib64/ and
# targets/<arch>/ layout the regular installer creates.
ln -sfn lib "${prefix}/lib64"
mkdir -p "${prefix}/targets/x86_64-linux"
ln -sfn ../../include "${prefix}/targets/x86_64-linux/include"
ln -sfn ../../lib "${prefix}/targets/x86_64-linux/lib"

# The build looks for nvcc under the unversioned path, the way a distro toolkit
# or the runfile installer leaves it.
if [ "${prefix}" != "/usr/local/cuda" ]; then
    ln -sfn "${prefix}" /usr/local/cuda
fi

"${prefix}/bin/nvcc" --version
