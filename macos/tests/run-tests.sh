#!/bin/bash
# Rufus for Mac - engine tests
#
# Builds synthetic Windows/Linux ISOs, writes them to disk image files with
# rufus-cli (no USB drive needed) and verifies the results with macOS' own
# tools (hdiutil, fdisk, fsck_msdos, fsck_exfat) and, if available, ntfs-3g.
#
#   cd macos && make cli && tests/run-tests.sh

set -u
cd "$(dirname "$0")/.."
CLI=build/rufus-cli
T=$(mktemp -d /tmp/rufus-tests.XXXXXX)
NTFSCAT=$(command -v ntfscat || echo /opt/local/bin/ntfscat)
NTFSFIX=$(command -v ntfsfix || echo /opt/local/bin/ntfsfix)
pass=0; fail=0

ok()   { echo "  PASS  $1"; pass=$((pass + 1)); }
bad()  { echo "  FAIL  $1"; fail=$((fail + 1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }
attach(){ hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$1" | head -1 | awk '{print $1}'; }

[ -x $CLI ] || { echo "Build the CLI first: make cli"; exit 1; }

echo "== Creating test ISOs in $T"
mkdir -p $T/win/{efi/boot,sources} $T/linux/{isolinux,boot/grub,EFI/BOOT,casper}
head -c 400000 /dev/urandom > $T/win/bootmgr
head -c 1200000 /dev/urandom > $T/win/bootmgr.efi
head -c 2000000 /dev/urandom > $T/win/efi/boot/bootx64.efi
head -c 30000000 /dev/urandom > $T/win/sources/boot.wim
head -c 50000000 /dev/urandom > $T/win/sources/install.wim
{ head -c 20000 /dev/urandom; printf 'ISOLINUX 6.04 6.04-pre1'; head -c 20000 /dev/urandom; } > $T/linux/isolinux/isolinux.bin
printf 'label live\n  append initrd=/casper/initrd boot=casper quiet\n' > $T/linux/isolinux/isolinux.cfg
printf 'menuentry "Try" {\n  linux /casper/vmlinuz boot=casper quiet\n}\n' > $T/linux/boot/grub/grub.cfg
head -c 900000 /dev/urandom > $T/linux/EFI/BOOT/BOOTx64.EFI
head -c 8000000 /dev/urandom > $T/linux/casper/vmlinuz
mkdir -p $T/ub/boot/grub $T/ub/EFI/BOOT
printf 'menuentry "U" {\n  linux /casper/vmlinuz root=live:CDLABEL=UBUNTU_24_04_3_LTS_AMD64 quiet\n}\n' > $T/ub/boot/grub/grub.cfg
head -c 1000 /dev/urandom > $T/ub/EFI/BOOT/BOOTX64.EFI
hdiutil makehybrid -quiet -o $T/win.iso $T/win -iso -joliet -udf -default-volume-name CCCOMA_X64FRE_EN-US_DV9
hdiutil makehybrid -quiet -o $T/linux.iso $T/linux -iso -joliet -default-volume-name MYLINUX
hdiutil makehybrid -quiet -o $T/ub.iso $T/ub -iso -joliet -default-volume-name UBUNTU_24_04_3_LTS_AMD64

echo "== Image scanning"
check "Windows ISO: bootmgr + EFI + install.wim" "$CLI --scan $T/win.iso 2>&1 | grep -q 'install.wim'"
check "Linux ISO: Syslinux 6.04 detected" "$CLI --scan $T/linux.iso 2>&1 | grep -q 'Syslinux/Isolinux v6.04'"

echo "== Windows ISO -> GPT/UEFI/NTFS (+ UEFI:NTFS)"
$CLI --create 4G --image $T/win.iso --gpt --uefi --fs ntfs $T/w1.img > $T/w1.log 2>&1
check "job succeeded" "grep -q 'Operation completed' $T/w1.log"
D=$(attach $T/w1.img)
check "GPT with 2 partitions" "diskutil list $D | grep -q 'disk.s2'"
if [ -x "$NTFSCAT" ]; then
	check "install.wim intact on NTFS" "[ \"\$($NTFSCAT ${D}s1 /sources/install.wim | md5)\" = \"\$(md5 -q $T/win/sources/install.wim)\" ]"
	check "NTFS volume consistent (ntfsfix)" "$NTFSFIX -n ${D}s1 2>&1 | grep -q 'alternate boot sector... OK'"
else
	echo "  SKIP  NTFS content checks (install ntfs-3g to enable)"
fi
hdiutil detach $D >/dev/null

echo "== Windows ISO -> MBR/BIOS/NTFS"
$CLI --create 4G --image $T/win.iso --mbr --bios --fs ntfs $T/w2.img > $T/w2.log 2>&1
check "Windows 7 MBR + NTFS boot record" "grep -q 'Using Windows 7 MBR' $T/w2.log && grep -q 'NTFS partition boot record' $T/w2.log"
if [ -x "$NTFSFIX" ]; then
	D=$(attach $T/w2.img)
	check "NTFS volume consistent after BIOS boot record (ntfsfix)" "$NTFSFIX -n ${D}s1 2>&1 | grep -q 'alternate boot sector... OK'"
	hdiutil detach $D >/dev/null
fi

echo "== Windows ISO -> GPT/UEFI/FAT32 with Windows customization"
$CLI --create 4G --image $T/win.iso --gpt --uefi --fs fat32 --wue 0x10DD --user tester $T/w3.img > $T/w3.log 2>&1
D=$(attach $T/w3.img)
check "fsck_msdos clean" "fsck_msdos -n ${D}s1 >/dev/null 2>&1"
diskutil mount ${D}s1 >/dev/null 2>&1
MP=$(diskutil info ${D}s1 | awk -F': *' '/Mount Point/{print $2}')
check "autounattend.xml is valid XML" "xmllint --noout '$MP/autounattend.xml'"
check "TPM bypass present" "grep -q BypassTPMCheck '$MP/autounattend.xml'"
hdiutil detach -force $D >/dev/null

echo "== Linux ISO -> MBR/BIOS/FAT32 + Syslinux + persistence"
$CLI --create 2G --image $T/linux.iso --mbr --bios --fs fat32 --persistence 512M $T/l1.img > $T/l1.log 2>&1
check "Syslinux boot record" "grep -q 'Successfully wrote Syslinux boot record' $T/l1.log"
check "persistence partition" "grep -q 'Linux Persistence Partition' $T/l1.log"
check "persistent kernel option" "grep -q \"Added 'persistent' kernel option\" $T/l1.log"
D=$(attach $T/l1.img)
check "fsck_msdos clean" "fsck_msdos -n ${D}s1 >/dev/null 2>&1"
check "active MBR partition" "fdisk $D 2>/dev/null | grep -q '^\*1:'"
hdiutil detach -force $D >/dev/null

echo "== Label patching (long ISO label on FAT)"
$CLI --create 1G --image $T/ub.iso --gpt --uefi --fs fat32 $T/u1.img > $T/u1.log 2>&1
check "CDLABEL patched" "grep -q \"Patched /boot/grub/grub.cfg: 'UBUNTU_24_04_3_LTS_AMD64' ➔ 'UBUNTU_24_0'\" $T/u1.log"

echo "== FreeDOS, exFAT, ext2/ext3"
$CLI --create 1G --freedos --mbr --bios --fs fat32 $T/f.img > $T/f.log 2>&1
check "FreeDOS boot record" "grep -q 'Using FreeDOS FAT32 partition boot record' $T/f.log"
$CLI --create 1G --gpt --uefi --fs exfat --label DATA $T/x.img > $T/x.log 2>&1
D=$(attach $T/x.img); check "fsck_exfat clean" "fsck_exfat -n ${D}s1 >/dev/null 2>&1"; hdiutil detach $D >/dev/null
for fs in ext2 ext3; do
	$CLI --create 1G --mbr --fs $fs --label persist $T/$fs.img > $T/$fs.log 2>&1
	dd if=$T/$fs.img bs=1m skip=1 count=16 2>/dev/null > $T/$fs.part
	check "$fs recognised" "file -s $T/$fs.part | grep -q '$fs filesystem'"
done

echo "== DD mode"
head -c 67108864 $T/l1.img > $T/dd.img   # has a real MBR
gzip -kf $T/dd.img
for src in dd.img dd.img.gz; do
	$CLI --create 128M --image $T/$src --dd $T/out.img > $T/dd.log 2>&1
	check "DD $src byte identical" "[ \"\$(head -c 67108864 $T/out.img | md5)\" = \"\$(md5 -q $T/dd.img)\" ]"
done

echo
echo "Results: $pass passed, $fail failed (work dir: $T)"
[ $fail -eq 0 ] && rm -rf $T
exit $fail
