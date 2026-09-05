# unohd-dvb — a Linux DVB-T/T2 driver for the Hauppauge WinTV-UnoHD

`unohd_dvb` makes the SMiT-based WinTV-UnoHD USB stick work as an **ordinary
Linux DVB adapter**. Once loaded it presents `/dev/dvb/adapterN/{frontend0,
demux0,dvr0}`, so stock tools — `dvbv5-scan`, `dvbv5-zap`, `w_scan`, VLC — and
PVR software such as **TVHeadend** use it with no special support.

There was no Linux driver for this stick before. The vendor ships Windows-only
software that drives it from userspace rather than through a normal tuner
driver, which is part of why it never got Linux support.

> **Status: v0.1.0, experimental.** Developed and tested against two sticks on
> one machine, on UK Freeview, on Linux 7.0. It works well there, but that is
> the whole of the sample. Please report what happens on yours — see
> [Reporting problems](#reporting-problems).

**Kernel:** builds cleanly against 5.15, 6.1, 6.8 and 7.0 headers. Only 7.0 has
been run against real hardware; the others are compile-only evidence.

## Supported hardware

| | |
|---|---|
| USB ID | `29df:0280` |
| Sold as | Hauppauge WinTV-UnoHD, WinTV-NexusHD, Freenet TV stick (DE) |
| CI controller | SMiT SM1670 |
| Demodulator | Availink AVL6762 |
| Tuner | MaxLinear MxL608 |
| Delivery systems | DVB-T, DVB-T2 |
| RF range | 174–862 MHz |

**Not** the Hauppauge WinTV-soloHD. That is a different, already-supported
device; this driver will not bind to it and you do not need it.

Check what you have:

```console
$ lsusb | grep 29df
Bus 001 Device 007: ID 29df:0280 SMIT CI Device
```

## What works

- DVB-T and DVB-T2 tuning across the full 174–862 MHz range
- Full-rate transport stream via `dvr0` — the whole multiplex, all PIDs
- Signal status, strength and C/N via the standard DVB v5 properties
- Multiple sticks on one host, tuned independently and simultaneously
- Hotplug: plug, unplug and replug while other sticks keep streaming
- TVHeadend, which detects it as a plain `linuxdvb` adapter

## What does not work, or is untested

- **Multi-PLP DVB-T2 is untested.** Single-PLP muxes work. Every mux reachable
  from the development site is single-PLP, so the PLP byte order could not be
  confirmed against a real multi-PLP transmission. If you can receive one,
  a report would be genuinely useful.
- **No Common Interface / CAM support, and no descrambling.** The stick
  contains a CI module, and this driver refuses its content-protection
  resource in order to reach the tuner at all (see
  [How it works](docs/how-it-works.md)). It receives **free-to-air** services
  only. This project does not implement, circumvent or otherwise defeat
  content protection, and no pay-TV service is made receivable by it.
- **Idle sessions.** The CI module stops answering altogether if the host
  sends it nothing for a few minutes — measured here, 120 s of silence was
  fine and 240 s was not, after which it answered neither a tune nor a status
  read until the device was reset. The driver therefore sends a status read
  once a minute when nothing else has gone out, which costs nothing and is the
  same read a tuned adapter already makes. If you ever see a stick stop
  locking after a long idle, `unbind`/`bind` that one device revives it and a
  bug report would be very welcome.
- **Signal strength is not comparable between sticks.** Two units on the same
  aerial and splitter, tuned to the same muxes seconds apart, reported 58-94 %
  and 10-62 % while delivering identical byte counts with the same near-zero
  error rates. Trust the C/N figure and the error counters; treat the strength
  percentage as a per-unit indication only.
- Suspend/resume across a host sleep is untested.
- Not yet submitted to `linux-media`; this is an out-of-tree module.

## Installing

On Debian or Ubuntu, one command:

```console
$ curl -fsSL https://raw.githubusercontent.com/jakesmith0/unohd-linux/main/install.sh | sudo sh
```

It fetches the latest tagged release and installs it through DKMS, so kernel
updates rebuild it. [`install.sh`](install.sh) is a plain shell script — read it
first if you would rather, or append `-s -- --dry-run` to see exactly what it
would do without changing anything. Other options: `--version=vX.Y.Z`,
`--force`, `--uninstall`, `--help`.

## Building it yourself

Any distribution. You need kernel headers and a compiler:

```console
# Debian/Ubuntu
$ sudo apt install build-essential linux-headers-$(uname -r)
# Fedora
$ sudo dnf install kernel-devel gcc make
```

### Straight build

```console
$ git clone https://github.com/jakesmith0/unohd-linux
$ cd unohd-linux/src
$ make
$ sudo insmod unohd_dvb.ko
```

### DKMS (rebuilds automatically on kernel updates — recommended)

```console
$ sudo mkdir -p /usr/src/unohd-dvb-0.1.0
$ sudo cp -r src dkms.conf /usr/src/unohd-dvb-0.1.0/
$ sudo dkms add    -m unohd-dvb -v 0.1.0
$ sudo dkms build  -m unohd-dvb -v 0.1.0
$ sudo dkms install -m unohd-dvb -v 0.1.0
$ sudo cp udev/99-unohd-dvb.rules /etc/udev/rules.d/   # optional, see below
$ sudo udevadm control --reload
$ sudo modprobe unohd_dvb
```

Plug the stick in, then check:

```console
$ dmesg | grep -i unohd
usb 1-5: WinTV-UnoHD: bringing up EN 50221 session layer
usb 1-5: SAS connected on session 3
usb 1-5: DVB: registering adapter 3 frontend 0 (SMiT SM1670 (WinTV-UnoHD))...
usb 1-5: registered DVB adapter 3
```

### Removing it

```console
# installed with install.sh
$ curl -fsSL https://raw.githubusercontent.com/jakesmith0/unohd-linux/main/install.sh | sudo sh -s -- --uninstall
# installed by hand
$ sudo rmmod unohd_dvb
$ sudo dkms remove -m unohd-dvb -v 0.1.0 --all     # if installed via DKMS
```

### Secure Boot

This is an unsigned out-of-tree module. With Secure Boot enabled the kernel
will refuse to load it (`ERROR: could not insert module ...: Key was rejected
by service`). Either enrol your own Machine Owner Key and sign the module
(DKMS can do this for you on Debian/Ubuntu via `mokutil`), or disable Secure
Boot. Loading an unsigned module taints the kernel; that is expected and
harmless.

## Using it

### Scan and watch with the `dvb-tools`

```console
$ sudo apt install dvb-tools
$ dvbv5-scan /usr/share/dvb/dvb-t/uk-YourTransmitter -o channels.conf
$ dvbv5-zap -c channels.conf "BBC ONE" -o out.ts -t 30 -P
```

`-P` records the whole multiplex rather than one service, which is what
TVHeadend does too.

### TVHeadend

No configuration is needed. TVHeadend picks the adapter up by itself:

```
linuxdvb: adapter added /dev/dvb/adapter3
```

Then add it under *Configuration → DVB Inputs → TV adapters* as you would any
other DVB-T adapter.

### Stable device names

**DVB adapter numbers are assigned in probe order and are not stable** across
reboots, module reloads, or replugging in a different order. This is not
hypothetical: two sticks on the test host swapped between `adapter3` and
`adapter4` from one module load to the next.

Install [`udev/99-unohd-dvb.rules`](udev/99-unohd-dvb.rules) to get names keyed
to each stick's serial number instead:

```console
$ sudo cp udev/99-unohd-dvb.rules /etc/udev/rules.d/
$ sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=dvb
$ ls /dev/dvb/by-serial/
unohd-1234567890  unohd-1234567891
$ ls /dev/dvb/by-serial/unohd-1234567890/
demux0  dvr0  frontend0  net0
```

Point TVHeadend or your recorder at `/dev/dvb/by-serial/unohd-<serial>/` and it
will follow the stick rather than the number.

## Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `debug` | `0` | Set to `1` for protocol tracing in `dmesg`. Verbose; useful in bug reports. |

```console
$ sudo insmod unohd_dvb.ko debug=1
```

## Reporting problems

Bug reports are the most useful thing you can contribute right now, especially
from a transmitter or mux this has never seen. Please open an issue using the
template, and include:

- kernel version (`uname -a`) and distribution
- `lsusb -d 29df:0280 -v` (at least the descriptor summary)
- `dmesg | grep -i unohd`, ideally with `debug=1`
- what you tried and what the DVB tools reported

## How it works

The stick is not a conventional USB tuner — it is a Common Interface module
with a DVB front end behind it, and everything is driven over EN 50221.
[`docs/how-it-works.md`](docs/how-it-works.md) is the short version;
[`docs/write-up.md`](docs/write-up.md) is the full reverse-engineering account,
including the wrong turns.

## Licence

GPL-2.0-or-later. See [`COPYING`](COPYING).
