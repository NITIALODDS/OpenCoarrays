#!/usr/bin/env bash

set -o errexit
set -o verbose
set -o pipefail
set -o nounset
set -o errtrace

__file=developer-scripts/travis/install.linux.sh

# Error tracing
# requires `set -o errtrace`
__caf_err_report() {
    error_code=${?}
    echo "Error (code=${error_code}) in ${__file} in function ${1} on line ${2}." >&2
    false
    return ${error_code}
}
# Always provide an error backtrace
trap '__caf_err_report "${FUNCNAME:-.}" ${LINENO}' ERR

echo "Performing Travis-CI installation phase on Linux..."

if [[ "${BUILD_TYPE:-}" != InstallScript ]]; then # Ubuntu on Travis-CI, NOT testing install.sh
    if ! [[ -x "${HOME}/.local/bin/mpif90" && -x "${HOME}/.local/bin/mpicc" ]]; then
        # mpich install not cached
        # could use prerequisites/build instead...
        echo "Downloading MPICH from ${MPICH_URL_HEAD}/${MPICH_URL_TAIL} ..."
        wget "${MPICH_URL_HEAD}/${MPICH_URL_TAIL}" > wget_mpich.log 2>&1 || cat wget_mpich.log
        echo "Extracting MPICH ..."
        tar -xzvf "${MPICH_URL_TAIL}" > tar_mpich.log 2>&1 || cat tar_mpich.log
        export CC=gcc-${GCC}
        export FC=gfortran-${GCC}
        (
            cd "${MPICH_URL_TAIL%.tar.gz}"
            echo "Configuring MPICH ..."
            ${TRAVIS:+travis_wait} ./configure --prefix="${CACHE}" > configure_mpich.log 2>&1 || cat configure_mpich.log
            echo "Building MPICH ..."
            ${TRAVIS:+travis_wait 30} make -j 4 > make_mpich.log 2>&1 || cat make_mpich.log
            echo "Installing MPICH ..."
            ${TRAVIS:+travis_wait} make install > install_mpich.log 2>&1 || cat install_mpich.log
        )
    fi
fi

{
    mpif90 --version && mpif90 -show
} || echo "No mpif90"
{
    mpicc --version && mpicc -show
} || echo "No mpicc"

type -a cmake || echo "CMake not installed"
cmake --version || true

echo "Done."