# Changelog

## Unreleased

From a maintainer-style review of v0.1.0 ahead of submission to `linux-media`.

### Added

- Suspend and resume. Without these callbacks the USB core unbinds the driver
  across a system sleep, and with a `demux0` or `dvr0` handle open that unbind
  waits for a frozen application and the suspend never finishes. The module
  keeps its session across a USB suspend, so resume only restarts the
  transfers. Tested with `pm_test=devices`, which suspends and resumes every
  device without putting the machine to sleep. Both sticks kept their USB and
  adapter numbers, and streams that were open carried on within a second of
  resume at the same rate, with one continuity-counter jump per PID. A real
  sleep has not been tried.
- `adapter_nr` module parameter, as in other DVB drivers.

### Changed

- Protocol tracing uses the kernel's dynamic debug (`dyndbg=+p`) instead of
  the `debug` parameter, which is gone. The per-tune log line is now part of
  that tracing.
- An unplugged stick whose `frontend0` is still open is freed when the last
  handle closes, through the frontend's release hook. v0.1.0 kept it until the
  module was unloaded.

### Fixed

- A transient error on the command endpoint ended the session thread for
  good, leaving the stick unusable until it was replugged. It now retries and
  warns once.
- A transport-stream read that timed out part-way dropped the data it had
  received.
- A DVB-T2 stream id above 255 was silently truncated to its low byte. It is
  now refused with `-EINVAL`; unset still means PLP 0.
- After an unplug, `FE_READ_STATUS` could go on reporting the last lock.
- The legacy `FE_READ_SIGNAL_STRENGTH` and `FE_READ_SNR` values were not
  clamped and could wrap.
- `FE_READ_BER` and `FE_READ_UNCORRECTED_BLOCKS` returned a made-up zero. The
  module reports neither, so they now return `-EOPNOTSUPP`.

## v0.1.0 — experimental (unreleased)

First public release. The stick works as an ordinary Linux DVB adapter.

### Added

- `unohd_dvb`, a USB DVB driver for the SMiT SM1670 based Hauppauge
  WinTV-UnoHD (`29df:0280`), registering a standard DVB adapter with
  `frontend0`, `demux0` and `dvr0`.
- DVB-T and DVB-T2 tuning across 174–862 MHz.
- Full-multiplex transport stream delivery on the bulk media endpoint.
- Signal status, strength and C/N through the DVB v5 property API.
- Multi-device support: several sticks on one host tune and stream
  independently.
- DKMS packaging, a udev rule for stable per-serial device names, and
  a build/`checkpatch` CI workflow.
- A once-a-minute status read that keeps the CI module's session alive across
  idle periods. Without it the module stops answering after a few minutes of
  host silence and needs a device reset; measured, 120 s idle was safe and
  240 s was not. Skipped whenever a real command went out recently, so a
  streaming adapter adds no traffic.
- `date_time_enq`'s `response_interval` is honoured if a module ever asks for
  a non-zero one, as EN 50221 requires. This particular module asks for zero.

### Known limitations

- **Multi-PLP DVB-T2 is untested.** No multi-PLP transmission was reachable
  from the development site, so the PLP byte order is unconfirmed. Single-PLP
  muxes work.
- **Free-to-air only.** The driver declines the module's CI+ content-protection
  resource, which is what makes the tuner respond at all. It does not implement
  or circumvent content protection and does not descramble anything.
- DVB adapter numbers follow probe order and are not stable across reloads.
  Use the supplied udev rule for stable names.
- Suspend/resume across host sleep is untested.
- Out-of-tree only; not yet submitted to `linux-media`.
- Compiles against 5.15, 6.1, 6.8 and 7.0 kernel headers, but has only been
  run against hardware on 7.0.
