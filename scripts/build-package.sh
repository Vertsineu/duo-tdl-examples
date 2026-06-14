#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd "${script_dir}/.." && pwd -P)"

package_config="${repo_root}/packaging/duo-tdl-dev/package.conf"
debian_config_dir="${repo_root}/packaging/duo-tdl-dev/DEBIAN"
dist_dir="${DIST_DIR:-${repo_root}/dist}"
work_dir="${WORK_DIR:-${dist_dir}/deb-work}"

if [ ! -f "${package_config}" ]; then
	echo "Missing package config: ${package_config}" >&2
	exit 1
fi

# shellcheck source=/dev/null
. "${package_config}"

: "${VERSION_PREFIX:?VERSION_PREFIX is required}"
: "${SYSTEM_LIB_RISCV64:?SYSTEM_LIB_RISCV64 is required}"
: "${SYSTEM_LIB_ARM64:?SYSTEM_LIB_ARM64 is required}"

target_product="${MILKV_PRODUCT:-}"
target_chip="${MILKV_CHIP:-${CHIP:-}}"
target_arch="${MILKV_ARCH:-}"

if [ -z "${target_arch}" ]; then
	case "${TOOLCHAIN_PREFIX:-} ${CC:-} ${CFLAGS:-}" in
		*riscv64*|*rv64*) target_arch="riscv64" ;;
		*aarch64*|*armv8-a*) target_arch="arm64" ;;
	esac
fi

if [ -z "${target_product}" ]; then
	case "${target_chip}" in
		CV180X) target_product="duo" ;;
		CV181X) target_product="duo256" ;;
	esac
fi

case "${target_product}" in
	duo|duo256|duos)
		package_name="duo-tdl-dev-${target_product}"
		;;
	"")
		echo "Missing target product. Run 'source envsetup.sh' before building the package." >&2
		exit 1
		;;
	*)
		echo "Unsupported target product: ${target_product}" >&2
		exit 1
		;;
esac

case "${target_chip}" in
	CV180X)
		if [ "${target_product}" != "duo" ]; then
			echo "Product ${target_product} does not match chip ${target_chip}" >&2
			exit 1
		fi
		;;
	CV181X)
		if [ "${target_product}" != "duo256" ] && [ "${target_product}" != "duos" ]; then
			echo "Product ${target_product} does not match chip ${target_chip}" >&2
			exit 1
		fi
		;;
	"")
		echo "Missing target chip. Run 'source envsetup.sh' before building the package." >&2
		exit 1
		;;
	*)
		echo "Unsupported target chip: ${target_chip}" >&2
		exit 1
		;;
esac

case "${target_arch}" in
	riscv64)
		architecture="riscv64"
		multiarch_triplet="riscv64-linux-gnu"
		source_dir="${repo_root}/${SYSTEM_LIB_RISCV64}"
		;;
	arm64)
		architecture="arm64"
		multiarch_triplet="aarch64-linux-gnu"
		source_dir="${repo_root}/${SYSTEM_LIB_ARM64}"
		;;
	"")
		echo "Missing target architecture. Run 'source envsetup.sh' before building the package." >&2
		exit 1
		;;
	*)
		echo "Unsupported target architecture: ${target_arch}" >&2
		exit 1
		;;
esac

if [ "${target_chip}" = "CV180X" ] && [ "${target_arch}" != "riscv64" ]; then
	echo "Product ${target_product} (${target_chip}) only supports riscv64 packaging" >&2
	exit 1
fi

git_rev="$(git -C "${repo_root}" rev-parse --short HEAD 2>/dev/null || true)"
if [ -n "${VERSION:-}" ]; then
	package_version="${VERSION}"
elif [ -n "${git_rev}" ]; then
	package_version="${VERSION_PREFIX}+git${git_rev}"
else
	package_version="${VERSION_PREFIX}"
fi

package_root="${work_dir}/${package_name}-${architecture}/root"
debian_root="${package_root}/DEBIAN"
system_install_root="${package_root}/mnt/system/lib"
usr_install_root="${package_root}/usr/lib/${multiarch_triplet}"
deb_file="${dist_dir}/${package_name}_${package_version}_${architecture}.deb"

if [ ! -d "${source_dir}" ]; then
	echo "Missing source directory: ${source_dir}" >&2
	exit 1
fi

if [ ! -f "${debian_config_dir}/control.in" ]; then
	echo "Missing Debian control template: ${debian_config_dir}/control.in" >&2
	exit 1
fi

rm -rf "${package_root}"
install -d "${debian_root}" "${system_install_root}" "${usr_install_root}" "${dist_dir}"

lib_count=0
while IFS= read -r -d '' lib; do
	lib_name="$(basename "${lib}")"
	case "${lib_name}" in
		libini.so|libwiringx.so)
			install -m 0644 "${lib}" "${usr_install_root}/"
			;;
		*)
			install -m 0644 "${lib}" "${system_install_root}/"
			;;
	esac
	lib_count=$((lib_count + 1))
done < <(find "${source_dir}" -maxdepth 1 -type f -name '*.so*' -print0 | sort -z)

if [ "${lib_count}" -eq 0 ]; then
	echo "No shared libraries found in: ${source_dir}" >&2
	exit 1
fi

for maint_script in postinst postrm preinst prerm; do
	if [ -f "${debian_config_dir}/${maint_script}" ]; then
		install -m 0755 "${debian_config_dir}/${maint_script}" "${debian_root}/${maint_script}"
	fi
done

installed_size="$(du -sk "${package_root}" | awk '{print $1}')"
sed \
	-e "s/@PACKAGE@/${package_name}/g" \
	-e "s/@VERSION@/${package_version}/g" \
	-e "s/@ARCHITECTURE@/${architecture}/g" \
	-e "s/@INSTALLED_SIZE@/${installed_size}/g" \
	"${debian_config_dir}/control.in" > "${debian_root}/control"

(
	cd "${package_root}"
	find . -path './DEBIAN' -prune -o -type f -print0 \
		| sort -z \
		| while IFS= read -r -d '' file; do
			md5sum "${file#./}"
		done
) > "${debian_root}/md5sums"

dpkg-deb --root-owner-group --build "${package_root}" "${deb_file}"

echo "Built ${deb_file}"
echo "Target ${target_product}/${target_chip}/${target_arch}"
echo "Packaged ${lib_count} libraries from ${source_dir}"
