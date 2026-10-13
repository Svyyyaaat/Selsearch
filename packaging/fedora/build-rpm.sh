#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds the Selsearch RPM inside a Fedora container (CI and local use):
#   podman run --rm -v "$PWD:/src" fedora:43 /src/packaging/fedora/build-rpm.sh 0.1.0
# The RPMs land in ./dist.
set -eu
version=${1:?version}
cd "$(dirname "$0")/../.."
dnf -y -q install rpm-build git-core 'dnf-command(builddep)'
dnf -y -q builddep packaging/fedora/selsearch.spec
top=$(mktemp -d)
mkdir -p "$top/SOURCES"
git config --global --add safe.directory "$PWD"
git archive --format=tar.gz --prefix="selsearch-$version/" -o "$top/SOURCES/selsearch-$version.tar.gz" HEAD
rpmbuild -bb --define "_topdir $top" --define "version_override $version" packaging/fedora/selsearch.spec
mkdir -p dist
find "$top/RPMS" -name '*.rpm' ! -name '*debug*' -exec cp {} dist/ \;
ls -l dist
