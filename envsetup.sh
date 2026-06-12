#!/usr/bin/env sh

if [ -n "${BASH_SOURCE:-}" ]; then
	script_path="${BASH_SOURCE[0]}"
elif [ -n "${ZSH_VERSION:-}" ]; then
	script_path="${(%):-%N}"
else
	script_path="$0"
fi

script_dir="$(cd "$(dirname "$script_path")" && pwd -P)"
echo "script_dir: ${script_dir}"

milkv_debug=0

milkv_chip=
milkv_arch=

host_tools_git="https://github.com/milkv-duo/host-tools.git"
host_tools="${script_dir}/host-tools"

print_info()    { printf "\033[1;92m%s\033[0m\n" "$1"; }
print_warning() { printf "\033[1;93m%s\033[0m\n" "$1"; }
print_note()    { printf "\033[1;94m%s\033[0m\n" "$1"; }
print_err()     { printf "\033[1;31mError: %s\033[0m\n" "$1"; }

choose_target() {
	echo "Select Product:"
	print_info "1. Duo (CV1800B)"
	print_info "2. Duo256M (SG2002) or DuoS (SG2000)"
	printf "Which would you like: "
	IFS= read -r chip_index

	if [ "${chip_index}" = 1 ]; then
		milkv_chip="CV180X"
		milkv_arch="riscv64"
	elif [ "${chip_index}" = 2 ]; then
		milkv_chip="CV181X"

		echo "Select Arch:"
		print_info "1. ARM64"
		print_info "2. RISCV64"
		printf "Which would you like: "
		IFS= read -r arch_index

		if [ "${arch_index}" = 1 ]; then
			milkv_arch="arm64"
		elif [ "${arch_index}" = 2 ]; then
			milkv_arch="riscv64"
		else
			print_err "Nothing selected for Arch."
			return 1
		fi
	else
		print_err "Nothing selected for Chip."
		return 1
	fi

	print_note "CHIP: ${milkv_chip}"
	print_note "ARCH: ${milkv_arch}"
}

check_host_tools() {
	if [ ! -d "${host_tools}" ]; then
		print_warning "host-tools does not exist, download it now..."
		git clone "${host_tools_git}"
		if [ $? -ne 0 ]; then
			print_err "Get the host-tools failed!"
			return 1
		fi
	else
		if [ ! -d "${host_tools}/.git" ]; then
			print_warning "host-tools is not the latest one!"
			print_warning "Please manually delete it first. [ ${host_tools} ]"
			print_warning "Then source the script again."
			return 2
		fi
	fi
}

choose_target
if [ $? -ne 0 ]; then
	return 1 2>/dev/null || exit 1
fi

if ! (cd "${script_dir}" && check_host_tools); then
	return 1 2>/dev/null || exit 1
fi

sys_inc="${script_dir}/include/system"
tdl_inc="${script_dir}/include/tdl"

if [ "${milkv_arch}" = "riscv64" ]; then

	arch_cflags="-mcpu=c906fdv -march=rv64imafdcv0p7xthead -mcmodel=medany -mabi=lp64d"
	arch_ldflags="-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64"

	toolchain_dir="${host_tools}/gcc/riscv64-linux-musl-x86_64"
	export TOOLCHAIN_PREFIX=${toolchain_dir}/bin/riscv64-unknown-linux-musl-
	toolchain_sysroot="${toolchain_dir}/sysroot"

elif [ "${milkv_arch}" = "arm64" ]; then

	arch_cflags="-march=armv8-a"
	arch_ldflags=""

	toolchain_dir="${host_tools}/gcc/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu"
	export TOOLCHAIN_PREFIX=${toolchain_dir}/bin/aarch64-linux-gnu-
	toolchain_sysroot="${toolchain_dir}/aarch64-linux-gnu/libc"
else
	print_err "Invalid ARCH parameter!"
	return 1 2>/dev/null || exit 1
fi

if [ "${milkv_chip}" = "CV180X" ]; then
	sys_lib="${script_dir}/libs/system/musl_riscv64"
	tdl_lib="${script_dir}/libs/tdl/cv180x_riscv64"
elif [ "${milkv_chip}" = "CV181X" ]; then
	if [ "${milkv_arch}" = "riscv64" ]; then
		sys_lib="${script_dir}/libs/system/musl_riscv64"
		tdl_lib="${script_dir}/libs/tdl/cv181x_riscv64"
	elif [ "${milkv_arch}" = "arm64" ]; then
		sys_lib="${script_dir}/libs/system/glibc_arm64"
		tdl_lib="${script_dir}/libs/tdl/cv181x_arm64"
	fi
else
	print_err "Invalid CHIP parameter!"
	return 1 2>/dev/null || exit 1
fi

if [ "${milkv_debug}" -eq 1 ]; then
	debug_cflags="-g -O0"
else
	debug_cflags="-O3 -DNDEBUG"
fi

export CC="${TOOLCHAIN_PREFIX}gcc"
if [ -d "${toolchain_sysroot}" ]; then
	sysroot_cflags="--sysroot=${toolchain_sysroot}"
else
	sysroot_cflags=""
fi
export CFLAGS="${arch_cflags} ${debug_cflags} ${sysroot_cflags} -I${sys_inc} -I${tdl_inc}"
export LDFLAGS="${arch_ldflags} -L${sys_lib} -L${tdl_lib}"

export CHIP="${milkv_chip}"
export COMMON_DIR="${script_dir}/common"

print_info "Environment is ready."
