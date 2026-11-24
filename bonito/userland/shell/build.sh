#!/bin/sh
# Build dropbear (ssh server), dash (/bin/sh) and a small toybox for iOS 16 arm64, on a macOS host with the iPhone SDK.
#   sh bonito/userland/shell/build.sh OUTDIR      (OUTDIR/dropbearmulti dash toybox, ad hoc signed)
# Source versions are pinned below. The iOS SDK on the runner is newer than iOS 16: a function that exists in the new SDK but not in iOS 16's
# libSystem becomes a weak import and crashes with PC 0 when called (the Pixel 4 port hit this with strchrnul), so the end of this script lists
# every weak import for review, and dash is configured with ac_cv_func_strchrnul=no.
set -eu
OUT=$(cd "$(dirname "${1:?usage: build.sh OUTDIR}")" && pwd)/$(basename "$1")
mkdir -p "$OUT"
W=$(mktemp -d)
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot $SDK -O2"
export CC
DROPBEAR=2025.88; DASH=0.5.12; TOYBOX=0.8.12
cd "$W"
curl -fsSL -o db.tar.bz2 "https://matt.ucc.asn.au/dropbear/releases/dropbear-$DROPBEAR.tar.bz2"
curl -fsSL -o dash.tar.gz "https://git.kernel.org/pub/scm/utils/dash/dash.git/snapshot/dash-$DASH.tar.gz"
curl -fsSL -o toybox.tar.gz "https://landley.net/toybox/downloads/toybox-$TOYBOX.tar.gz"
tar xjf db.tar.bz2; tar xzf dash.tar.gz; tar xzf toybox.tar.gz

echo "== dropbear"
cd "$W/dropbear-$DROPBEAR"
cat > localoptions.h <<'OPT'
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_ZLIB 0
#define DROPBEAR_SMALL_CODE 1
#define DEFAULT_PATH "/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
#define DEFAULT_ROOT_PATH "/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
OPT
./configure --host=aarch64-apple-darwin --disable-zlib --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
  --disable-loginfunc --disable-pututline --disable-pututxline --enable-bundled-libtom CC="$CC" >/dev/null
make -j4 PROGRAMS="dropbear dropbearkey scp" MULTI=1 >/dev/null
cp dropbearmulti "$OUT/dropbearmulti"

echo "== dash"
cd "$W/dash-$DASH"
[ -x configure ] || ./autogen.sh
# dash uses K&R function definitions: the runner's clang rejects them in its default (C23) mode
ac_cv_func_strchrnul=no ./configure --host=aarch64-apple-darwin CC="$CC -std=gnu99" >/dev/null
# autoconf 2.72 folds -std=gnu23 into CC: override CC on the make command line instead
make -j4 CC="$CC -std=gnu99" >/dev/null
cp src/dash "$OUT/dash"

echo "== toybox"
# toybox's scripts use $CC as one command name (no spaces): wrap the full iOS compiler command in a script
cat > "$W/ioscc" <<WRAP
#!/bin/sh
exec xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot $SDK -I$W/stub -O2 "\$@"
WRAP
chmod +x "$W/ioscc"
# the iOS SDK has no <sys/disk.h> (toybox's fdlength() wants two ioctl numbers from it)
mkdir -p "$W/stub/sys"
printf '#define DKIOCGETBLOCKSIZE 0x40046418\n#define DKIOCGETBLOCKCOUNT 0x40086419\n' > "$W/stub/sys/disk.h"
export CC="$W/ioscc"
# toybox's scripts need GNU sed (macOS ships BSD sed: "invalid command code")
PATH="$(brew --prefix gnu-sed)/libexec/gnubin:$PATH"; export PATH
cd "$W/toybox-$TOYBOX"
make allnoconfig >/dev/null
for a in LS CAT CP MV RM MKDIR RMDIR LN CHMOD CHOWN KILL GREP SED HEAD TAIL WC FIND XARGS SORT UNIQ CUT TR DU UNAME DATE ENV ID WHOAMI HEXDUMP OD DD \
  SLEEP TOUCH STAT WHICH TEE PRINTF ECHO TRUE FALSE TEST BASENAME DIRNAME READLINK REALPATH UPTIME HOSTNAME NETCAT NETSTAT SYNC MD5SUM SHA1SUM SHA256SUM \
  TAR GZIP ZCAT LSOF IFCONFIG CHGRP MKNOD TRUNCATE FILE STRINGS XXD SEQ YES CMP DIFF; do
  sed -i.bak "s/^# CONFIG_${a} is not set/CONFIG_${a}=y/" .config
done
yes "" | make oldconfig >/dev/null 2>&1 || true
make CC="$CC" HOSTCC=cc toybox 2>&1 | tail -n 40
cp toybox "$OUT/toybox"

echo "== sign and check"
for b in dropbearmulti dash toybox; do
  codesign -s - -f "$OUT/$b"
  file "$OUT/$b" | cut -c1-110
  echo "-- weak imports of $b:"; nm -m "$OUT/$b" 2>/dev/null | grep "weak external" || echo "(none)"
done
ls -la "$OUT"
