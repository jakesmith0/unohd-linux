# Changelog

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
