#!/usr/bin/env bash
# Install a host compiler that nvcc will accept, and print the path to it.
#
# nvcc refuses a host compiler newer than the one its release was built against
# - the check lives in crt/host_config.h - so taking an older CUDA for the GPU
# coverage it still has (see install_cuda_redist.sh) usually leaves the
# release's own compiler too new to drive it. The version gate is not the whole
# problem either: newer libstdc++ headers implement <type_traits> with compiler
# built-ins that nvcc's frontend does not know, so `-allow-unsupported-compiler`
# fails just as hard, a few thousand errors further in. An older GCC, with its
# own headers, is the only thing that works.
#
# Only src/platform/linux/cuda.cu goes through nvcc, at C++20 against a handful
# of standard headers, so pointing CUDAHOSTCXX here leaves every other
# translation unit on the compiler the release ships.
#
# Usage: install_cuda_host_gcc.sh <gcc-major-version> [arch-ci-work-dir]
#   Progress is written to stderr; the path to g++ is printed on stdout.

set -euo pipefail

gcc_major="${1:?usage: install_cuda_host_gcc.sh <gcc-major-version>}"

# shellcheck disable=SC1091  # trusted system file, unavailable to static analysis
. /etc/os-release

log() {
    echo "$*" >&2
}

# Verify the compiler before asking nvcc to use it. This catches compilers
# that are present but not
# usable - most of all the unpacked Arch one, whose cc1plus resolves libisl and
# libmpc against whatever the running system installed.
verify() {
    local cxx="$1"
    local probe
    probe="$(mktemp -d)"

    printf '#include <memory>\n#include <string>\n#include <vector>\nint main() { std::vector<std::string> v{"x"}; return static_cast<int>(v.size()) - 1; }\n' \
        > "${probe}/probe.cpp"

    if ! "${cxx}" -std=c++20 -o "${probe}/probe" "${probe}/probe.cpp" >&2; then
        rm -rf "${probe}"
        log "${cxx} cannot compile a trivial C++20 program; see the errors above."
        exit 1
    fi

    rm -rf "${probe}"
    log "${cxx} verified"
}

case "${ID}" in
    ubuntu | debian)
        log "Installing g++-${gcc_major} from the distribution"
        apt-get install -y "g++-${gcc_major}" >&2
        host_cxx="/usr/bin/g++-${gcc_major}"
        ;;

    fedora)
        if dnf repoquery --quiet --available "gcc${gcc_major}-c++" 2>/dev/null | grep -q .; then
            log "Installing gcc${gcc_major} from Fedora ${VERSION_ID}"
            dnf install -y "gcc${gcc_major}" "gcc${gcc_major}-c++" >&2
        else
            # Fedora retires a compat compiler a release or two after the one
            # that needed it, and CUDA outlives that. The packages from the last
            # release that carried it install here unchanged; only their
            # dependencies are resolved against this release.
            log "gcc${gcc_major} is not in Fedora ${VERSION_ID}; taking it from an earlier release"
            dnf install -y dnf-plugins-core >&2

            download_dir="$(mktemp -d)"
            trap 'rm -rf "${download_dir}"' EXIT

            found=""
            for releasever in $((VERSION_ID - 1)) $((VERSION_ID - 2)) $((VERSION_ID - 3)); do
                log "Looking in Fedora ${releasever}"
                if dnf download --releasever="${releasever}" --arch=x86_64 \
                        --destdir="${download_dir}" \
                        "gcc${gcc_major}" "gcc${gcc_major}-c++" >&2; then
                    found="${releasever}"
                    break
                fi
            done

            if [ -z "${found}" ]; then
                log "No Fedora release within three of ${VERSION_ID} packages gcc${gcc_major}."
                exit 1
            fi

            log "Installing gcc${gcc_major} from Fedora ${found}"
            dnf install -y "${download_dir}"/*.rpm >&2
        fi
        host_cxx="/usr/bin/g++-${gcc_major}"
        ;;

    arch)
        if [[ ${gcc_major} != 14 || $(uname -m) != x86_64 ]]; then
            log "The pinned AUR CUDA host compiler is GCC 14 for x86_64."
            exit 1
        fi
        if (( EUID == 0 )); then
            log "Build and unpack the AUR CUDA host compiler as an unprivileged user."
            exit 1
        fi
        work_dir="$(realpath -m -- "${2:?pass the Arch CI workspace directory}")"
        revision=87e62884e433543b59aaa2a28dcceb2e6f314054
        version=14.4.1+r175+gafa9b4ecc082-1
        packages="${work_dir}/packages"
        gcc_archive="${packages}/gcc14-${version}-x86_64.pkg.tar.zst"
        runtime_archive="${packages}/gcc14-libs-${version}-x86_64.pkg.tar.zst"
        prefix="${work_dir}/prefix"
        mkdir -p "${packages}" "${prefix}"

        if [[ ! -f ${gcc_archive} || ! -f ${runtime_archive} ]]; then
            recipe="$(mktemp -d "${work_dir}/aur-recipe.XXXXXX")"
            trap 'rm -rf -- "${recipe}"' EXIT
            git -C "${recipe}" init -q
            git -C "${recipe}" fetch --depth 1 https://aur.archlinux.org/gcc14.git "${revision}" >&2
            git -C "${recipe}" checkout -q --detach "${revision}"
            log "Building AUR gcc14 ${version} at ${revision}"
            # makepkg verifies the pinned GCC source and launcher checksums.
            # The runner provisions official dependencies before entering this step.
            (
                cd "${recipe}"
                # The compiler only drives nvcc. Skip split debug output to
                # keep the bootstrap within the hosted runner disk budget.
                printf 'source /etc/makepkg.conf\nOPTIONS+=(!debug)\n' > ci-makepkg.conf
                PKGDEST="${recipe}" makepkg --config ci-makepkg.conf --nocheck --noconfirm
            ) >&2
            install -m 0644 "${recipe}/${gcc_archive##*/}" "${gcc_archive}"
            install -m 0644 "${recipe}/${runtime_archive##*/}" "${runtime_archive}"
        else
            log "Using cached Arch gcc14 ${version} packages"
        fi

        # Use only the versioned compiler and runtime files. Keeping these
        # out of /usr leaves the runner's normal gcc and g++ available to mcpp.
        for archive in "${gcc_archive}" "${runtime_archive}"; do
            tar --zstd -xf "${archive}" -C "${prefix}" usr
        done
        host_cxx="${prefix}/usr/bin/g++-14"
        actual_version="$("${host_cxx}" -dumpfullversion)"
        if [[ ${actual_version} != "${version%%+*}" ]]; then
            log "Expected GCC ${version%%+*}, got ${actual_version} from ${host_cxx}."
            exit 1
        fi
        ;;

    *)
        log "No host-compiler recipe for '${ID}'."
        exit 1
        ;;
esac

verify "${host_cxx}"
echo "${host_cxx}"
