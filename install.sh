#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# unohd-dvb installer -- Debian/Ubuntu.
#
# Fetches a tagged release of https://github.com/jakesmith0/unohd-linux,
# installs it through DKMS so kernel updates rebuild it, and checks that the
# module actually loaded.  Everything it does is visible in this file.
#
#   curl -fsSL https://raw.githubusercontent.com/jakesmith0/unohd-linux/main/install.sh | sudo sh
#
# Nothing here is interactive, so it is safe to pipe.  --dry-run prints the
# plan and changes nothing.

set -eu

REPO=jakesmith0/unohd-linux
HOME_URL=https://github.com/$REPO
SELF_URL=https://raw.githubusercontent.com/$REPO/main/install.sh
API=https://api.github.com/repos/$REPO
# Used when the GitHub API cannot be reached.  Bumped with each release.
FALLBACK_TAG=v0.1.0

USB_ID=29df:0280
PKG=unohd-dvb
MOD=unohd_dvb

WANT_TAG=
DRY=0
FORCE=0
DO_UNINSTALL=0

# ------------------------------------------------------------------ output

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
	B=$(printf '\033[1m'); R=$(printf '\033[0m')
	RED=$(printf '\033[31m'); YEL=$(printf '\033[33m'); GRN=$(printf '\033[32m')
else
	B=; R=; RED=; YEL=; GRN=
fi

say()  { printf '%s\n' "$*"; }
step() { printf '%s==>%s %s\n' "$B" "$R" "$*"; }
warn() { printf '%s warning:%s %s\n' "$YEL" "$R" "$*" >&2; }
ok()   { printf '%s  ok:%s %s\n' "$GRN" "$R" "$*"; }
die()  { printf '%s error:%s %s\n' "$RED" "$R" "$*" >&2; exit 1; }

run() {
	if [ "$DRY" = 1 ]; then
		printf '     would run: %s\n' "$*"
	else
		"$@"
	fi
}

usage() {
	cat <<EOF
unohd-dvb installer

  sudo sh install.sh [options]

  --version=vX.Y.Z   install this release tag (default: latest release)
  --dry-run          show what would happen; change nothing
  --force            reinstall even if this version is already present
  --uninstall        remove the DKMS package and unload the module
  --help             this text

Installs the driver for the SMiT SM1670 USB DVB-T/T2 stick ($USB_ID),
sold as the Hauppauge WinTV-UnoHD / NexusHD and the Freenet TV stick.

Debian and Ubuntu only.  Other distributions: build from source, see
$HOME_URL#building-and-installing
EOF
}

for arg in "$@"; do
	case "$arg" in
	--version=*) WANT_TAG=${arg#--version=} ;;
	--dry-run)   DRY=1 ;;
	--force)     FORCE=1 ;;
	--uninstall) DO_UNINSTALL=1 ;;
	--help|-h)   usage; exit 0 ;;
	*)           usage >&2; die "unknown option: $arg" ;;
	esac
done

# ----------------------------------------------------------------- preflight

[ "$(uname -s)" = Linux ] || die "this driver is for Linux; found $(uname -s)"

if [ "$(id -u)" != 0 ] && [ "$DRY" != 1 ]; then
	die "needs root -- rerun with sudo, or add --dry-run to just see the plan"
fi

have() { command -v "$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------- uninstall

if [ "$DO_UNINSTALL" = 1 ]; then
	step "Removing $PKG"
	if have dkms; then
		# Only ever touches our own package.
		dkms status 2>/dev/null | sed -n "s|^${PKG}[/,] *\([^,:]*\).*|\1|p" |
		sort -u | while read -r v; do
			[ -n "$v" ] || continue
			say "  dkms remove $PKG/$v"
			run dkms remove -m "$PKG" -v "$v" --all || true
			run rm -rf "/usr/src/$PKG-$v"
		done
	fi
	if lsmod 2>/dev/null | grep -q "^$MOD "; then
		run rmmod "$MOD" ||
			warn "the module is in use and was not unloaded; it is gone after a reboot"
	fi
	run rm -f /etc/udev/rules.d/99-unohd-dvb.rules
	run depmod -a || true
	ok "removed"
	exit 0
fi

KVER=$(uname -r)
step "Host"
say "  kernel        $KVER"
say "  architecture  $(uname -m)"

case "$KVER" in
[0-4].*) die "kernel $KVER is too old; 5.15 or newer is needed" ;;
5.[0-9].*|5.1[0-4].*) die "kernel $KVER is too old; 5.15 or newer is needed" ;;
esac

if ! have apt-get; then
	die "this installer supports Debian and Ubuntu only.
       Build from source instead: $HOME_URL#building-and-installing"
fi
if [ -r /etc/os-release ]; then
	# In a subshell: os-release defines VERSION, which we use for our own.
	# shellcheck source=/dev/null
	say "  distribution  $(. /etc/os-release; echo "${PRETTY_NAME:-unknown}")"
fi

# Is the stick actually here?  Not fatal -- people install before plugging in.
DEV_PRESENT=0
vid=${USB_ID%%:*}; pid=${USB_ID##*:}
for d in /sys/bus/usb/devices/*; do
	[ -r "$d/idVendor" ] && [ -r "$d/idProduct" ] || continue
	if [ "$(cat "$d/idVendor")" = "$vid" ] && [ "$(cat "$d/idProduct")" = "$pid" ]; then
		DEV_PRESENT=$((DEV_PRESENT + 1))
	fi
done
if [ "$DEV_PRESENT" -gt 0 ]; then
	say "  device        $DEV_PRESENT x $USB_ID present"
else
	say "  device        $USB_ID not plugged in (installing anyway)"
fi

# Secure Boot rejects unsigned modules.  DKMS can sign, but enrolling the key
# needs a reboot and a password at the firmware prompt, which cannot happen
# inside a piped script -- so say so plainly rather than pretending.
SECUREBOOT=unknown
if have mokutil; then
	case "$(mokutil --sb-state 2>/dev/null)" in
	*enabled*)  SECUREBOOT=enabled ;;
	*disabled*) SECUREBOOT=disabled ;;
	esac
elif [ -d /sys/firmware/efi ]; then
	f=$(find /sys/firmware/efi/efivars -maxdepth 1 -name 'SecureBoot-*' 2>/dev/null | head -1) || true
	if [ -n "${f:-}" ]; then
		case "$(od -An -t u1 "$f" 2>/dev/null | tr -d ' \n')" in
		*1) SECUREBOOT=enabled ;;
		*0) SECUREBOOT=disabled ;;
		esac
	fi
else
	SECUREBOOT=disabled
fi
say "  secure boot   $SECUREBOOT"

# ------------------------------------------------------------- dependencies

step "Dependencies"
NEED=
have dkms  || NEED="$NEED dkms"
have make  || NEED="$NEED build-essential"
have gcc   || NEED="$NEED build-essential"
have curl  || NEED="$NEED curl"

HDR=/lib/modules/$KVER/build
if [ ! -e "$HDR" ]; then
	NEED="$NEED linux-headers-$KVER"
fi

# De-duplicate; build-essential can be added twice above.
# shellcheck disable=SC2086  # deliberate word splitting
NEED=$(printf '%s\n' $NEED | sort -u | tr '\n' ' ')

if [ -n "$(printf %s "$NEED" | tr -d ' ')" ]; then
	say "  installing:  $NEED"
	run env DEBIAN_FRONTEND=noninteractive apt-get update -qq
	# shellcheck disable=SC2086
	if ! run env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends $NEED; then
		case "$NEED" in
		*linux-headers-*) die "apt could not install: $NEED
       If it is linux-headers-$KVER that is missing, this kernel has no
       headers package.  On a custom or cloud kernel, install its matching
       headers another way and rerun." ;;
		esac
		die "apt could not install: $NEED"
	fi
else
	say "  all present"
fi

if [ "$DRY" != 1 ] && [ ! -e "$HDR" ]; then
	die "no kernel headers for $KVER at $HDR.
       linux-headers-$KVER is not available for this kernel.  If you are on a
       custom or cloud kernel, install its matching headers and rerun."
fi

# ------------------------------------------------------------------- version

if [ -z "$WANT_TAG" ]; then
	step "Finding the latest release"
	WANT_TAG=$(curl -fsSL --max-time 20 "$API/releases/latest" 2>/dev/null |
		   sed -n 's/.*"tag_name" *: *"\([^"]*\)".*/\1/p' | head -1) || true
	if [ -z "$WANT_TAG" ]; then
		WANT_TAG=$FALLBACK_TAG
		warn "could not reach the GitHub API; using $WANT_TAG"
	fi
fi
say "  release       $WANT_TAG"

# ------------------------------------------------------------------- fetch

TMP=
# In dash the EXIT trap's last command sets the script's exit status, so this
# must not end on a false test.
cleanup() {
	if [ -n "$TMP" ]; then rm -rf "$TMP"; fi
}
trap cleanup EXIT INT TERM

step "Fetching $WANT_TAG"
TARBALL=$HOME_URL/archive/refs/tags/$WANT_TAG.tar.gz
if [ "$DRY" = 1 ]; then
	say "     would download $TARBALL"
	SRC=/dev/null
	VERSION=${WANT_TAG#v}
else
	TMP=$(mktemp -d)
	curl -fsSL --max-time 120 "$TARBALL" -o "$TMP/src.tar.gz" ||
		die "could not download $TARBALL
       check the tag exists: $HOME_URL/releases"
	tar -xzf "$TMP/src.tar.gz" -C "$TMP" || die "the downloaded archive did not extract"
	SRC=$(find "$TMP" -maxdepth 1 -mindepth 1 -type d | head -1)
	[ -n "$SRC" ] || die "unexpected archive layout"
	[ -f "$SRC/dkms.conf" ] && [ -f "$SRC/src/$MOD.c" ] ||
		die "$WANT_TAG does not look like a unohd-linux release"

	VERSION=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$SRC/dkms.conf")
	[ -n "$VERSION" ] || die "no PACKAGE_VERSION in dkms.conf"
	say "  source        $SRC"
fi
say "  dkms version  $VERSION"

DEST=/usr/src/$PKG-$VERSION

# ------------------------------------------------------------------ install

ALREADY=0
if have dkms && dkms status -m "$PKG" -v "$VERSION" 2>/dev/null |
		grep -q "$KVER.*installed"; then
	ALREADY=1
fi

# What DKMS already has of ours: every registered version, and every kernel
# some version is installed for.  dkms status prints either
#   unohd-dvb/0.1.0, 6.8.0-1-generic, x86_64: installed     (dkms 3)
#   unohd-dvb, 0.1.0, 6.8.0-1-generic, x86_64: installed    (dkms 2)
OLD_VERSIONS=
OLD_KERNELS=
if have dkms; then
	OLD_VERSIONS=$(dkms status -m "$PKG" 2>/dev/null |
		sed -n "s|^${PKG}[/,] *\([^,:]*\).*|\1|p" | sort -u | tr '\n' ' ')
	OLD_KERNELS=$(dkms status -m "$PKG" 2>/dev/null |
		sed -n 's|.*, *\([^,]*\), *[^,]*: installed.*|\1|p' | sort -u | tr '\n' ' ')
fi

if [ "$ALREADY" = 1 ] && [ "$FORCE" != 1 ]; then
	step "Already installed"
	say "  $PKG/$VERSION is built and installed for $KVER"
	say "  rerun with --force to rebuild it"
else
	step "Installing $PKG/$VERSION through DKMS"

	# Clear out every registered version of our own package first: a
	# half-registered tree from an interrupted run makes dkms add fail, and an
	# older release left registered would be rebuilt alongside this one on
	# every kernel update.
	for v in $OLD_VERSIONS; do
		say "  dkms remove $PKG/$v"
		run dkms remove -m "$PKG" -v "$v" --all >/dev/null 2>&1 || true
		[ "$v" = "$VERSION" ] || run rm -rf "/usr/src/$PKG-$v"
	done
	run rm -rf "$DEST"
	run mkdir -p "$DEST"
	if [ "$DRY" = 1 ]; then
		say "     would copy src/ and dkms.conf into $DEST"
	else
		cp -r "$SRC/src" "$SRC/dkms.conf" "$DEST/"
	fi

	run dkms add -m "$PKG" -v "$VERSION" ||
		die "dkms add failed"
	if ! run dkms build -m "$PKG" -v "$VERSION"; then
		die "the module did not build against $KVER.
       Full log: /var/lib/dkms/$PKG/$VERSION/build/make.log
       Please report it with that log: $HOME_URL/issues"
	fi
	run dkms install -m "$PKG" -v "$VERSION" ||
		die "dkms install failed"

	# The removal above covered every kernel, so put the build back on the
	# other kernels that had one, where their headers are still installed.
	for k in $OLD_KERNELS; do
		[ "$k" != "$KVER" ] && [ -e "/lib/modules/$k/build" ] || continue
		say "  also rebuilding for $k"
		run dkms install -m "$PKG" -v "$VERSION" -k "$k" >/dev/null 2>&1 ||
			warn "could not rebuild for $k; to retry: dkms install -m $PKG -v $VERSION -k $k"
	done
fi

# Stable per-stick device names.  Additive; touches nothing else.
if [ "$DRY" = 1 ]; then
	say "     would install /etc/udev/rules.d/99-unohd-dvb.rules"
elif [ -f "$SRC/udev/99-unohd-dvb.rules" ]; then
	install -m 0644 "$SRC/udev/99-unohd-dvb.rules" /etc/udev/rules.d/
	udevadm control --reload >/dev/null 2>&1 || true
fi

run depmod -a

# -------------------------------------------------------------------- load

step "Loading"
if [ "$DRY" = 1 ]; then
	say "     would modprobe $MOD and check for a DVB adapter"
	ok "dry run complete; nothing was changed"
	exit 0
fi

STALE=0
if lsmod | grep -q "^$MOD "; then
	say "  already loaded; reloading to pick up the new build"
	if ! rmmod "$MOD" 2>/dev/null; then
		STALE=1
		warn "the running module is in use and could not be unloaded"
		if have fuser; then
			# fuser exits 1 when it finds nothing; under set -e that
			# would end the run here.
			HOLD=$(fuser /dev/dvb/*/frontend0 /dev/dvb/*/demux0 \
				/dev/dvb/*/dvr0 2>/dev/null) || true
			if [ -n "$HOLD" ]; then
				# shellcheck disable=SC2086  # deliberate word splitting
				HOLD=$(ps -o comm= -p $HOLD 2>/dev/null |
					sort -u | tr '\n' ' ') || true
			fi
			[ -n "$HOLD" ] && say "       in use by: $HOLD"
		fi
		say "       the new build takes effect once they release it, or after a reboot"
	fi
fi

if ! modprobe "$MOD" 2>"$TMP/modprobe.err"; then
	err=$(cat "$TMP/modprobe.err")
	if [ "$SECUREBOOT" = enabled ]; then
		# DKMS signs with Ubuntu's shim-signed key where it exists, and with
		# its own key otherwise.
		MOKCERT=/var/lib/dkms/mok.pub
		if [ -f /var/lib/shim-signed/mok/MOK.der ]; then
			MOKCERT=/var/lib/shim-signed/mok/MOK.der
		fi
		die "modprobe failed and Secure Boot is enabled: $err

       Secure Boot refuses unsigned modules.  Either:
        - enrol the DKMS signing key:  sudo mokutil --import $MOKCERT
          then reboot and confirm at the blue MOK screen (needs a password you
          set during that command, entered at the firmware prompt), or
        - turn Secure Boot off in your firmware settings.
       This installer will not do either for you."
	fi
	die "modprobe $MOD failed: $err"
fi
if [ "$STALE" = 1 ]; then
	say "  the previously loaded build is still the running one"
else
	ok "$MOD loaded"
fi

# ------------------------------------------------------------------- verify

step "Checking"
FAIL=0

modinfo "$MOD" >/dev/null 2>&1 &&
	say "  module        $(modinfo -F version "$MOD" 2>/dev/null || echo present) at $(modinfo -n "$MOD")"

if [ "$DEV_PRESENT" = 0 ]; then
	say "  device        not plugged in -- plug the stick in and it will appear"
else
	# Count the frontends whose USB parent is one of our sticks.  Other DVB
	# hardware in the machine registers frontends too, so waiting for any
	# frontend to appear proves nothing -- wait for ours.
	count_ours() {
		c=0
		for f in /sys/class/dvb/dvb*.frontend0; do
			[ -e "$f" ] || continue
			p=$(readlink -f "$f/device" 2>/dev/null) || continue
			while [ -n "$p" ] && [ "$p" != / ]; do
				if [ -r "$p/idVendor" ] &&
				   [ "$(cat "$p/idVendor")" = "$vid" ] &&
				   [ "$(cat "$p/idProduct" 2>/dev/null)" = "$pid" ]; then
					c=$((c + 1)); break
				fi
				p=$(dirname "$p")
			done
		done
		echo "$c"
	}

	# Each stick resets once and probes twice, so give it time.
	n=0
	OURS=$(count_ours)
	while [ "$OURS" -lt "$DEV_PRESENT" ] && [ $n -lt 20 ]; do
		sleep 1; n=$((n + 1))
		OURS=$(count_ours)
	done

	if [ "$OURS" -ge "$DEV_PRESENT" ]; then
		ok "$OURS DVB adapter(s) registered for $DEV_PRESENT stick(s)"
	elif [ "$OURS" -gt 0 ]; then
		warn "$OURS adapter(s) for $DEV_PRESENT stick(s) -- one did not come up"
		FAIL=1
	else
		warn "the module loaded but no DVB adapter appeared"
		say  "  recent kernel messages:"
		dmesg 2>/dev/null | grep -i -e unohd -e 'SM1670' | tail -10 | sed 's/^/    /'
		FAIL=1
	fi
fi

say ""
if [ "$FAIL" = 0 ]; then
	printf '%sunohd-dvb %s installed.%s\n' "$B" "$VERSION" "$R"
	say ""
	say "  DKMS will rebuild it automatically on kernel updates."
	if [ "$DEV_PRESENT" -gt 0 ]; then
		say "  Your stick(s):"
		# The pipeline's status is sed's, so test the glob instead of
		# relying on ls failing.
		set -- /dev/dvb/by-serial/unohd-*
		[ -e "$1" ] || set -- /dev/dvb/adapter*
		[ -e "$1" ] && printf '    %s\n' "$@"
	fi
	say ""
	say "  Scan for channels:  dvbv5-scan /usr/share/dvb/dvb-t/<your-transmitter>"
	say "  Remove it:          curl -fsSL $SELF_URL | sudo sh -s -- --uninstall"
	say "  It is experimental -- please report anything odd: $HOME_URL/issues"
else
	printf '%sInstalled, but it did not come up cleanly.%s\n' "$YEL" "$R"
	say "  Please open an issue with the output above: $HOME_URL/issues"
	exit 1
fi
