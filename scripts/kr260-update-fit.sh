#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Rebuild the KR260 boot FIT using the current kernel output and known-good
# device trees extracted from the currently installed FIT.

set -euo pipefail

if [ "${EUID}" -ne 0 ]; then
	echo "run this script with sudo" >&2
	exit 1
fi

src="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${OUT:-$src/out/arm64/kr260-przero}"
ver="$(<"$out/include/config/kernel.release")"
firmware=/boot/firmware
fit="$firmware/image.fit"
work="$(mktemp -d "/tmp/kr260-fit.${ver}.XXXXXX")"

cleanup()
{
	rm -rf "$work"
}
trap cleanup EXIT

for file in "$out/arch/arm64/boot/Image.gz" \
		    "/boot/initrd.img-$ver" "$fit"; do
	if [ ! -f "$file" ]; then
		echo "missing required file: $file" >&2
		exit 1
	fi
done

mkdir -p "$work/dtbs"
install -m 644 "$out/arch/arm64/boot/Image.gz" "$work/Image.gz"
install -m 644 "/boot/initrd.img-$ver" "$work/initrd.img"

# The U-Boot script selects these configuration names from the carrier-card
# identity. Reuse their DTBs rather than changing the board description.
dumpimage -T flat_dt -p 2 -o "$work/dtbs/zynqmp-smk-k26-revA.dtb" "$fit"
dumpimage -T flat_dt -p 3 -o "$work/dtbs/smk-k26-revA-sck-kr-g-revA.dtb" "$fit"
dumpimage -T flat_dt -p 4 -o "$work/dtbs/smk-k26-revA-sck-kv-g-revA.dtb" "$fit"
dumpimage -T flat_dt -p 5 -o "$work/dtbs/smk-k26-revA-sck-kr-g-revB.dtb" "$fit"
dumpimage -T flat_dt -p 6 -o "$work/dtbs/smk-k26-revA-sck-kv-g-revB.dtb" "$fit"
dumpimage -T flat_dt -p 7 -o "$work/dtbs/smk-k24-revA-sck-kd-g-revA.dtb" "$fit"

cat > "$work/image.its" <<'EOF'
/dts-v1/;

/ {
	description = "KR260 Ubuntu Linux PRZero";
	#address-cells = <1>;

	images {
		kernel-1 {
			description = "Linux PRZero";
			data = /incbin/("Image.gz");
			type = "kernel"; arch = "arm64"; os = "linux";
			compression = "gzip";
			load = <0x00200000>; entry = <0x00200000>;
			hash-1 { algo = "sha1"; };
		};
		ramdisk-1 {
			description = "Ubuntu initramfs";
			data = /incbin/("initrd.img");
			type = "ramdisk"; arch = "arm64"; os = "linux";
			compression = "none";
			hash-1 { algo = "sha1"; };
		};
		fdt-zynqmp-smk-k26-revA.dtb {
			data = /incbin/("dtbs/zynqmp-smk-k26-revA.dtb");
			type = "flat_dt"; arch = "arm64"; compression = "none";
			load = <0x44000000>; hash-1 { algo = "sha1"; };
		};
		fdt-smk-k26-revA-sck-kr-g-revA.dtb {
			data = /incbin/("dtbs/smk-k26-revA-sck-kr-g-revA.dtb");
			type = "flat_dt"; arch = "arm64"; compression = "none";
			load = <0x44000000>; hash-1 { algo = "sha1"; };
		};
		fdt-smk-k26-revA-sck-kv-g-revA.dtb {
			data = /incbin/("dtbs/smk-k26-revA-sck-kv-g-revA.dtb");
			type = "flat_dt"; arch = "arm64"; compression = "none";
			load = <0x44000000>; hash-1 { algo = "sha1"; };
		};
		fdt-smk-k26-revA-sck-kr-g-revB.dtb {
			data = /incbin/("dtbs/smk-k26-revA-sck-kr-g-revB.dtb");
			type = "flat_dt"; arch = "arm64"; compression = "none";
			load = <0x44000000>; hash-1 { algo = "sha1"; };
		};
		fdt-smk-k26-revA-sck-kv-g-revB.dtb {
			data = /incbin/("dtbs/smk-k26-revA-sck-kv-g-revB.dtb");
			type = "flat_dt"; arch = "arm64"; compression = "none";
			load = <0x44000000>; hash-1 { algo = "sha1"; };
		};
		fdt-smk-k24-revA-sck-kd-g-revA.dtb {
			data = /incbin/("dtbs/smk-k24-revA-sck-kd-g-revA.dtb");
			type = "flat_dt"; arch = "arm64"; compression = "none";
			load = <0x44000000>; hash-1 { algo = "sha1"; };
		};
	};

	configurations {
		default = "conf-zynqmp-smk-k26-revA";
		conf-zynqmp-smk-k26-revA {
			kernel = "kernel-1"; ramdisk = "ramdisk-1";
			fdt = "fdt-zynqmp-smk-k26-revA.dtb";
		};
		conf-smk-k26-revA-sck-kr-g-revA {
			kernel = "kernel-1"; ramdisk = "ramdisk-1";
			fdt = "fdt-smk-k26-revA-sck-kr-g-revA.dtb";
		};
		conf-smk-k26-revA-sck-kv-g-revA {
			kernel = "kernel-1"; ramdisk = "ramdisk-1";
			fdt = "fdt-smk-k26-revA-sck-kv-g-revA.dtb";
		};
		conf-smk-k26-revA-sck-kr-g-revB {
			kernel = "kernel-1"; ramdisk = "ramdisk-1";
			fdt = "fdt-smk-k26-revA-sck-kr-g-revB.dtb";
		};
		conf-smk-k26-revA-sck-kv-g-revB {
			kernel = "kernel-1"; ramdisk = "ramdisk-1";
			fdt = "fdt-smk-k26-revA-sck-kv-g-revB.dtb";
		};
		conf-zynqmp-smk-kd-g-revA {
			kernel = "kernel-1"; ramdisk = "ramdisk-1";
			fdt = "fdt-smk-k24-revA-sck-kd-g-revA.dtb";
		};
	};
};
EOF

(cd "$work" && mkimage -f image.its image.fit)
dumpimage -l "$work/image.fit" >/dev/null

backup="$firmware/image.fit.before-${ver}-$(date +%Y%m%d-%H%M%S)"
cp -a "$fit" "$backup"
install -m 755 "$work/image.fit" "$firmware/image.fit.new"
mv "$firmware/image.fit.new" "$fit"
sync

echo "Installed $fit"
echo "Backup: $backup"
dumpimage -l "$fit" | sed -n '1,40p'
