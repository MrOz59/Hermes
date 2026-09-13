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
# Only src/platform/linux/cuda.cu goes through nvcc, at C++17 against a handful
# of standard headers, so pointing CUDAHOSTCXX here leaves every other
# translation unit on the compiler the release ships.
#
# Usage: install_cuda_host_gcc.sh <gcc-major-version>
#   Progress is written to stderr; the path to g++ is printed on stdout.

set -euo pipefail

gcc_major="${1:?usage: install_cuda_host_gcc.sh <gcc-major-version>}"

# shellcheck disable=SC1091  # trusted system file, unavailable to static analysis
. /etc/os-release

log() {
    echo "$*" >&2
}

# Fail here, with something readable, rather than several layers into CMake's
# compiler detection. This catches the ways a compiler can be present but not
# usable - most of all the unpacked Arch one, whose cc1plus resolves libisl and
# libmpc against whatever the running system installed.
verify() {
    local cxx="$1"
    local probe
    probe="$(mktemp -d)"

    printf '#include <memory>\n#include <string>\n#include <vector>\nint main() { std::vector<std::string> v{"x"}; return static_cast<int>(v.size()) - 1; }\n' \
        > "${probe}/probe.cpp"

    if ! "${cxx}" -std=c++17 -o "${probe}/probe" "${probe}/probe.cpp" >&2; then
        rm -rf "${probe}"
        log "${cxx} cannot compile a trivial C++17 program; see the errors above."
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
        # Arch keeps no compat compilers, but every package it has ever shipped
        # stays in the archive. GCC locates its own headers and libraries
        # relative to the driver binary, so the package works unpacked under a
        # prefix without being installed - which is what is wanted here, since
        # installing it would displace the compiler the rest of the build uses.
        #
        # Refresh from https://archive.archlinux.org/packages/g/gcc/ if a newer
        # point release of the same major is ever needed; the archive is
        # permanent, so a pinned URL keeps working.
        case "${gcc_major}" in
            14) package="gcc-14.2.1%2Br753%2Bg1cd744a6828f-1-x86_64.pkg.tar.zst" ;;
            *)
                log "No pinned Arch archive package for GCC ${gcc_major}."
                exit 1
                ;;
        esac

        prefix="/opt/gcc${gcc_major}"
        log "Unpacking ${package} under ${prefix}"

        archive="$(mktemp)"
        trap 'rm -f "${archive}"' EXIT

        wget -q --tries=3 --retry-connrefused -O "${archive}" \
            "https://archive.archlinux.org/packages/g/gcc/${package}"

        mkdir -p "${prefix}"
        # The package carries .PKGINFO and friends beside the tree; only /usr
        # is wanted.
        tar --use-compress-program=unzstd -xf "${archive}" -C "${prefix}" usr

        # libgcc_s lives in gcc-libs, which is deliberately not unpacked: the
        # host compiler must not bring a second runtime alongside the one the
        # rest of the build links against. The installed libgcc_s is newer and
        # a strict superset, so the link name is pointed at it.
        ln -sfn /usr/lib/libgcc_s.so.1 "${prefix}/usr/lib/libgcc_s.so"

        host_cxx="${prefix}/usr/bin/g++"
        ;;

    *)
        log "No host-compiler recipe for '${ID}'."
        exit 1
        ;;
esac

verify "${host_cxx}"
echo "${host_cxx}"
