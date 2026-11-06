#!/bin/sh
# Fetch rustc_codegen_gcc and the Rust toolchain it pins, into work/.
#
#     work/rustc_codegen_gcc   at REVISION below + patches/rustc_codegen_gcc-*
#     work/rustup, work/cargo  rustup with the nightly in its rust-toolchain.toml
#
# Nothing goes to ~/.rustup or ~/.cargo.  The libgccjit it drives is
# build-gccjit.sh's; the GCC fork commit it expects (libgccjit.version) is
# what gcc/patches/jit/0102 was taken from, so a bump here needs that patch
# remade too.
set -e

REVISION=85dcbb2972ffb730bcc35ee33fa3d9dc26b5e730
URL=https://github.com/rust-lang/rustc_codegen_gcc.git

HERE=$(cd "$(dirname "$0")" && pwd)

# Check out REV in the current repository with PATCHES applied, unless that
# is already what is there: a rewrite would make the next build start over.
checkout() {
	rev=$1; shift
	if [ "$(git rev-parse HEAD 2>/dev/null)" = "${rev}" ]; then
		current=yes
		for p in "$@"; do
			git apply --reverse --check "${p}" 2>/dev/null || current=no
		done
		[ "${current}" = no ] || return 0
	fi
	git fetch -q --depth 1 origin "${rev}"
	git checkout -q --force FETCH_HEAD
	git clean -qfd -e bootstrap.toml -e build -e target
	for p in "$@"; do
		git apply "${p}"
	done
}

WORK="${HERE}/work"
CG="${WORK}/rustc_codegen_gcc"
mkdir -p "${WORK}"

if [ ! -d "${CG}/.git" ]; then
	git clone -q "${URL}" "${CG}"
fi
cd "${CG}"
checkout "${REVISION}" "${HERE}"/patches/rustc_codegen_gcc-*.patch

export RUSTUP_HOME="${WORK}/rustup" CARGO_HOME="${WORK}/cargo"
TOOLCHAIN=$(sed -n 's/^channel = "\(.*\)"/\1/p' rust-toolchain.toml)
if [ ! -x "${CARGO_HOME}/bin/rustup" ]; then
	init="${WORK}/rustup-init"
	case "$(uname -s)-$(uname -m)" in
	Darwin-arm64)  triple=aarch64-apple-darwin ;;
	Darwin-x86_64) triple=x86_64-apple-darwin ;;
	Linux-aarch64) triple=aarch64-unknown-linux-gnu ;;
	Linux-x86_64)  triple=x86_64-unknown-linux-gnu ;;
	*) echo "no rustup-init for $(uname -sm)" >&2; exit 1 ;;
	esac
	curl -sSfL -o "${init}" \
		"https://static.rust-lang.org/rustup/dist/${triple}/rustup-init"
	chmod +x "${init}"
	"${init}" -y --no-modify-path --profile minimal \
		--default-toolchain "${TOOLCHAIN}"
fi
"${CARGO_HOME}/bin/rustup" toolchain install "${TOOLCHAIN}" --profile minimal \
	-c rust-src,rustc-dev,llvm-tools-preview
echo "==> ${CG} at ${REVISION}, toolchain ${TOOLCHAIN}"

# rustc itself, at the commit that nightly was built from, with the C33
# architecture added (build-rustc.sh builds it).
RUST_COMMIT=$("${CARGO_HOME}/bin/rustc" "+${TOOLCHAIN}" -vV | sed -n 's/^commit-hash: //p')
RUST="${WORK}/rust"
if [ ! -d "${RUST}/.git" ]; then
	git init -q "${RUST}"
	git -C "${RUST}" remote add origin https://github.com/rust-lang/rust.git
fi
cd "${RUST}"
checkout "${RUST_COMMIT}" "${HERE}"/patches/rust-*.patch
cmp -s "${HERE}/bootstrap.toml" bootstrap.toml ||
	cp "${HERE}/bootstrap.toml" bootstrap.toml
echo "==> ${RUST} at ${RUST_COMMIT}"

# libc, the version std's Cargo.lock names, checked against its checksum,
# with the C33 added; library/Cargo.toml (patches/rust-0002) patches it in.
LIBC_VERSION=$(sed -n '/^name = "libc"$/{n;s/^version = "\(.*\)"/\1/p;}' library/Cargo.lock)
LIBC_SUM=$(sed -n '/^name = "libc"$/{n;n;n;s/^checksum = "\(.*\)"/\1/p;}' library/Cargo.lock)
CRATE="${WORK}/libc-${LIBC_VERSION}.crate"
"${HERE}/../host-tools/fetch-verified.sh" \
	"https://static.crates.io/crates/libc/libc-${LIBC_VERSION}.crate" "${CRATE}" "${LIBC_SUM}"
LIBC="${WORK}/libc"
current=no
if [ -d "${LIBC}/.git" ] && [ "$(git -C "${LIBC}" log -1 --format=%s)" = "libc ${LIBC_VERSION}" ]; then
	current=yes
	for p in "${HERE}"/patches/libc-*.patch; do
		git -C "${LIBC}" apply --reverse --check "${p}" 2>/dev/null || current=no
	done
fi
if [ "${current}" = no ]; then
	rm -rf "${LIBC}"
	mkdir -p "${LIBC}"
	tar -C "${LIBC}" --strip-components=1 -xzf "${CRATE}"
	git -C "${LIBC}" init -q
	git -C "${LIBC}" add -A
	git -C "${LIBC}" -c user.name=fetch -c user.email=fetch@localhost commit -qm "libc ${LIBC_VERSION}"
	for p in "${HERE}"/patches/libc-*.patch; do
		git -C "${LIBC}" apply "${p}"
	done
fi
echo "==> ${LIBC} at ${LIBC_VERSION}"

# Other crates that need the C33 (crates.txt), each with its
# patches/NAME-VERSION-*.patch; crates patch them in.
grep -v '^#' "${HERE}/crates.txt" | while read -r name version sum; do
	[ -n "${name}" ] || continue
	dir="${WORK}/crates/${name}-${version}"
	mkdir -p "${WORK}/crates"
	"${HERE}/../host-tools/fetch-verified.sh" \
		"https://static.crates.io/crates/${name}/${name}-${version}.crate" \
		"${dir}.crate" "${sum}"
	current=no
	if [ -d "${dir}/.git" ]; then
		current=yes
		for p in "${HERE}/patches/${name}-${version}"-*.patch; do
			git -C "${dir}" apply --reverse --check "${p}" 2>/dev/null || current=no
		done
	fi
	if [ "${current}" = no ]; then
		rm -rf "${dir}"
		mkdir -p "${dir}"
		tar -C "${dir}" --strip-components=1 -xzf "${dir}.crate"
		git -C "${dir}" init -q
		git -C "${dir}" add -A
		git -C "${dir}" -c user.name=fetch -c user.email=fetch@localhost \
			commit -qm "${name} ${version}"
		for p in "${HERE}/patches/${name}-${version}"-*.patch; do
			git -C "${dir}" apply "${p}"
		done
	fi
	echo "==> ${dir}"
done
