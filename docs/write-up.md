# Reverse-engineering the WinTV-UnoHD

How a DVB-T2 stick with no Linux support turned out to be a Common Interface
module in a plastic case, and what it took to make it an ordinary DVB adapter.

Throughout, claims are marked:

- **Measured** — observed directly against hardware, reproducibly.
- **Inferred** — a reading of the evidence that fits everything seen, but was
  not proved on its own.
- **Unknown** — genuinely not established. Said plainly rather than glossed.

---

## 1. It is not a soloHD, and that is the whole problem

Hauppauge sells a WinTV-**soloHD** and a WinTV-**UnoHD**. The names suggest
variants of one product. They are not. The soloHD is a conventional DVB-T2
stick with a Linux driver and has worked for years.

The UnoHD (also sold as WinTV-NexusHD, and as the German *Freenet TV* stick) is
a different device with a different USB ID — **`29df:0280`** — and no Linux
driver at all. Buying one expecting soloHD behaviour is a common way to end up
here.

The reason it has no driver becomes clear once you look inside it, and it is
not that nobody tried.

## 2. What is actually in it

**Measured**, from descriptors and the vendor's own software:

| Part | Role |
|---|---|
| SMiT SM1670 | Common Interface controller — runs an EN 50221 stack in firmware |
| Availink AVL6762 | DVB-T/T2 demodulator |
| MaxLinear MxL608 | RF tuner |

The USB device is composite, with two interfaces, each with exactly one
altsetting:

| Interface | Name | Endpoints |
|---|---|---|
| 0 | CI Command Interface | `0x01` OUT, `0x82` IN — **interrupt**, 512 bytes |
| 1 | CI Media Interface | `0x03` OUT, `0x84` IN — **bulk** |

The shape is the giveaway. There is no vendor register interface, no firmware
upload endpoint, no I²C-over-control-transfer bridge — none of the furniture a
normal tuner driver expects. The demodulator and tuner are not on the USB bus
at all. They sit **behind** the SM1670, and the only way to reach them is to
talk to the CI controller.

So the driver is not a tuner driver. It is a **CI host**.

## 3. The Windows software does not use a tuner driver either

**Measured.** Hauppauge's Windows stack does not present the stick through
BDA, the standard Windows broadcast driver model. It drives the device from
**userspace**, through a libusb-style USB layer, inside a DirectShow source
filter.

That is an unusual choice, and it was the single most useful clue in the
project: it meant the entire protocol lived in a userspace binary that could be
studied statically, rather than in a signed kernel driver. It also explains the
absence of Linux support — there was never a conventional driver to port.

The protocol described here was recovered by analysing that filter's behaviour
and then confirming every claim against real hardware. **No vendor code,
binary, decompiled source or firmware is reproduced in this repository**; what
is published is a description of the wire protocol and an original
implementation of it.

## 4. The device talks first, exactly once

**Measured.** After a USB port reset, with no host traffic whatsoever, the
module starts pushing EN 50221 messages at the host on the interrupt IN
endpoint. It announces its resources unprompted.

It does this **once**. A host that attaches later, or misses the announcement,
gets silence — and there is no command to ask for a repeat. This is why the
driver issues one `usb_reset_device()` on probe: it is how you get the module
to start its announcement sequence from the top for a host that is now ready to
listen.

The wire model is the other surprise. EN 50221 defines a link layer and a
transport layer beneath the session layer. Here there are **neither**. Each USB
transfer carries one bare **SPDU**:

```
host   -> device : interrupt OUT on 0x01, one SPDU  (ZLP when length % 512 == 0)
device -> host   : interrupt IN  on 0x82, one SPDU
```

**Inferred**, and consistent with everything observed: the USB endpoints
already deliver the framing, ordering and reliability that the link and
transport layers exist to provide, so the vendor discarded them. Anyone
expecting `T_DATA_LAST`/`T_RCV` TPDUs will find nothing that parses.

## 5. An ordinary CI bring-up, plus a private one

**Measured.** Once talking, it is textbook EN 50221. The module opens sessions
and the host answers:

| Resource | ID |
|---|---|
| Resource Manager | `0x00010041` |
| Application Information | `0x00020041` |
| Conditional Access Support | `0x00030041` |
| Date/Time | `0x00240041` |
| **SMiT private** | `0x00961001` |

The module identifies itself as `app_type=1 manufacturer=cafe code=babe`, menu
string `"Irdeto Access"`.

On the private resource the host completes a **CI+ SAS** handshake —
`9F9A00 sas_connect_rqst` — naming application id **`SMiTZBJL`**, and the
module replies `sas_connect_cnf` with status 0. Tuner commands then travel as
SMiT private TLVs inside `sas_async_msg`.

At this point everything looks finished. It is not.

## 6. The wall: every command accepted, every command ignored

For a long stretch the project was stuck here, and it was the least
informative kind of stuck. The module accepted every session. It confirmed the
SAS handshake with status 0. And then it **silently discarded every tuner
command** — tune, get-status, get-lock, get-hardware-info, get-type. No error.
No timeout. No refusal. Just nothing, for ever.

Identical on both sticks, so **measured** as deterministic firmware behaviour
rather than RF trouble or a faulty unit — which was itself the useful
observation, because it ruled out the entire class of explanations everyone
reaches for first.

The cause: alongside the resources above, the module opens a session on its own
private **content-protection** resource, **`0xFCC00011`**, and pushes an APDU
at the host expecting a CI+ CP exchange. While that is outstanding, its
application services no commands.

**The fix is to refuse it.** Answer the `open_session_request` for
`0xFCC00011` with status `0xF0` (*resource not found*) and omit it from
`profile_reply`. The module falls back and answers everything immediately.

This is the single most important fact in the project, and it deserves to be
stated precisely:

> `0xFCC00011` is CI+ content protection. It requires a real device certificate
> and authentication material, which this project does not have and never
> attempted to obtain, forge or extract. Declining the resource makes the
> driver **not** a content-protection-capable host. The consequence is that it
> receives **free-to-air broadcasts only**.
>
> Nothing here descrambles anything, defeats or weakens any protection
> mechanism, or makes any pay-TV service receivable. The driver declines to
> participate in CI+ — which is the correct and honest posture for a
> free-to-air driver, and happens also to be what makes the tuner answer.

## 7. The tuner protocol, and the first lock

**Measured.** With `0xFCC00011` refused, the SMiT private command set responds.
Commands are short TLVs with a 16-bit tag:

| Tag | Meaning |
|---|---|
| `0003` / `0004` | tune / tune response |
| `0005` / `0006` | get tuner status / response |
| `0010` | get CI info |
| `0021` | get type |
| `0031` | get CI status |
| `0041` | get list |

The tune payload carries frequency, bandwidth, a delivery-system byte, and a
PLP selector.

Two tags — `0x1000` and `0x1004` — belong to a **firmware upgrade** path. They
are named here for completeness and were deliberately never sent. They are
absent from the driver, and there is no code path that can reach them.

The first real DVB-T2 lock on the aerial-connected stick was the moment the
device stopped being a puzzle and became hardware.

## 8. Locked, and still no transport stream

A lock is not a stream, and the gap between them cost a great deal of time.

The instinct is to hunt for a start-of-stream command. **There is none** — the
vendor's framer has no such tag. Two host-side details were the entire problem:

1. **Interface 1 must be claimed *before* the session layer is brought up.**
   Claim it afterwards and endpoint `0x84` delivers **zero bytes for ever**,
   silently. Claim first, `clear_halt(0x84)`, then bring the module up.
   (**Measured**; the mechanism inside the firmware is **unknown**.)

2. **DVB-T2 needs a real PLP id** — see below.

With both right, bulk endpoint `0x84` carries **the entire multiplex**, all
PIDs, at full rate. There is no PID filtering on the device: the host gets
everything and filters in software, which is exactly what `dvb_core`'s software
demux expects.

## 9. `FFFF` is not "any PLP"

**Measured**, and worth stating loudly because the failure mode is so
misleading.

For DVB-T2, setting the PLP selector to `FFFF` produces a **perfect lock** —
full strength, quality 100, `nplp=1`, `plp[0]=0` — and **no transport stream at
all**. It does not mean "any PLP"; it means no PLP is routed to the output.

Send the actual PLP id and the same mux streams at full rate. On 586 MHz over
six seconds:

| PLP field | transport stream |
|---|---|
| `0000` | 26,021,456 bytes |
| `FFFF` | **0 bytes** |

DVB-T muxes are unaffected — there is no PLP layer to route.

**Unknown:** the *endianness* of the PLP field. Every mux reachable from the
development site reports `nplp=1, plp[0]=0`, a value where both byte orders
coincide. This is unresolved and is listed as a limitation rather than quietly
assumed correct. A report from a real multi-PLP transmission would settle it.

## 10. The delivery-system byte is advisory

**Measured**, by direct experiment. Byte 13 of the tune payload — the
DVB-T/DVB-T2 selector — makes no difference to anything. The demodulator
auto-detects the standard from the signal.

At 578 MHz (a DVB-T mux), six-second captures:

| byte 13 | lock | strength | quality | bytes |
|---|---|---|---|---|
| 0 (DVB-T) | yes | 50 | 24 | 15,684,464 |
| 2 (undefined) | yes | 50 | 24 | 15,684,464 |
| 3 (DVB-T2) | yes | 50 | 24 | 15,749,888 |

Byte-identical behaviour — including from a value the vendor never uses. The
driver still sets the documented value, to match the vendor rather than because
the hardware cares.

## 11. Real television

**Measured.** Captures from UK Freeview parsed as ordinary MPEG-TS with correct
SDT service names and zero sync errors: DVB-T at 578 MHz (~24 Mbit/s) and
DVB-T2 at 586 MHz (~37 Mbit/s).

At that point the stick worked — from a userspace libusb program. That was the
proof, not the product.

## 12. Why a kernel driver after all

The userspace host proved the protocol, but nothing else could use it. TVHeadend,
VLC, `dvbv5-*` and every other tool speak the Linux DVB API. A userspace
implementation would have meant a bespoke integration for each.

Writing a real driver meant the stick becomes `/dev/dvb/adapterN` and
**everything** works with no special support. TVHeadend needed no patch, no
plugin and no configuration — it logs `linuxdvb: adapter added` and treats it
as any other DVB-T2 adapter.

## 13. What the driver actually does

`unohd_dvb` binds both interfaces of `29df:0280` and runs the CI host in the
kernel:

- one kthread pumping SPDUs from the interrupt IN endpoint and dispatching the
  EN 50221 state machine;
- one kthread draining bulk endpoint `0x84` into the DVB software demux;
- `set_frontend` / `read_status` translated into SMiT TLVs over SAS;
- `0xFCC00011` refused, per §6;
- interface 1 claimed before bring-up, per §8;
- one `usb_reset_device()` per device on probe, per §4.

Both kthreads are named per USB port path (`unohd-ts/5`, `unohd-spdu/5`), which
matters once there is more than one stick and you need to tell them apart.

## 14. The lifecycle bugs, which are the honest part of this story

The driver tuned and streamed long before it was *correct*. Everything below
was found by testing unplug, unbind and unload against real hardware, and it is
recorded because it is the most useful engineering content here — not something
to tidy away.

**Bug 1 — double `kthread_stop()`.** Unplugging while streaming oopsed the
driver. `disconnect` stopped the TS thread without holding the feed lock and
without clearing the pointer; `dvb_dmxdev_release()` then ran `stop_feed` on
behalf of the still-open application and stopped the same task again. That
underflows the task refcount and dereferences NULL inside `kthread_stop()`.
Fixed by routing every stop through one locked helper with a `disconnected`
flag.

**Bug 2 — an adapter leak on the retry path.** The SPDU pump retried DVB
registration on *every received packet* after the handshake came up, each
attempt allocating and unwinding an adapter number. Fixed by giving up after
the first failure.

**Bug 3 — a use-after-free, and a wrong assumption of mine.** Auditing the
teardown, I concluded that `dvb_frontend_detach()` waits for the application to
close `frontend0`, and wrote that into a code comment. **It was simply wrong**,
and the comment made the code look considered while leaving the bug in place.

The real contract, from `dvb_core`'s own source: `dvb_register_frontend()`
leaves the reference count at **two** ("one for `dvb_unregister_frontend()`,
and another one for `dvb_frontend_detach()`"), and every `open()` adds another.
With one descriptor open, disconnect's unregister + detach takes 3 → 1, not to
zero. The eventual `close()` then walks freed memory in
`dvb_frontend_release()`.

It presented exactly as that description predicts:

```
Oops: general protection fault, non-canonical address 0x10000004001f0
RIP: __raw_spin_lock_irqsave <- __wake_up
  <- dvb_frontend_release [dvb_core] <- __fput <- __x64_sys_close
```

The sanctioned fix is `fe->ops.release` — and it is **unusable** in a
self-contained module. With `CONFIG_MEDIA_ATTACH=y`,
`dvb_frontend_invoke_release()` follows that hook with `dvb_detach()` →
`symbol_put_addr()`, dropping a module reference that `dvb_attach()` never
took, because the hook is designed for separately built demodulator modules.
Balancing it with `__module_get(THIS_MODULE)` deadlocks `rmmod` — which is what
triggers `disconnect` in the first place.

So the driver frees its state only once the frontend's reference count has
genuinely reached zero, and otherwise parks it on a list drained at module
unload. That is safe because an open DVB device node pins the module: `dvbdev`
sets the device node's `fops->owner` to the adapter's module, so unload cannot
run while any descriptor is open. `probe` also takes `usb_get_dev()`, because a
parked structure outlives the USB device while `dvb_core` can still reach
`fe->dvb->device`.

The lesson worth carrying: the second attempt failed because it was reasoned
from what the API *ought* to do. The third worked because it was reasoned from
`dvb_core`'s actual source. **A fault in a release path also costs a reboot** —
the oops killed the process inside `__fput`, so the module reference taken by
`open()` was never dropped, leaving the module permanently unremovable.

**Bug 4 — a two-device livelock, found by inspection.** "Have I just reset this
one?" was kept in a single global slot. With two sticks it ping-pongs:
probe(A) records A and resets it; probe(B) overwrites the slot and resets B; A
re-enumerates, the slot still says B, so A is reset again — for ever. Now one
record per device, keyed by bus number and port path, which are what survive a
reset. Found and fixed before the second stick was even connected, and
confirmed correct on hardware afterwards.

## 15. The bug that only appeared when nobody was watching

Every test up to this point had been busy. Tune, capture, check, unbind,
reload — the stick never sat still for long. Then came a comparison run
against three other tuners on the same aerial, and the two UnoHDs spent about
ten minutes idle waiting their turn. When their turn came they failed to lock
on all seven multiplexes. Not one. Signal `0x00`, nothing.

The first instinct — that the comparison harness was at fault, or that
something had disturbed the aerial — was wrong on both counts. The same stick
with the same configuration file locked instantly at 82 % signal the moment
the module was reloaded, and the harness's configuration turned out to be
byte-for-byte the one that had worked all week.

Turning on tracing against a stick already in that state settled it:

```
usb 1-5: tune 586000 kHz bw 8 DVB-T2 plp 0
usb 1-5: -> SMiT cmd 0003 len=16          tuner_lock sent
                                          (nothing back; the 5 s timeout expires)
usb 1-5: -> SMiT cmd 0005 len=4           status read sent
                                          (nothing back either)
```

Six seconds of listening produced no incoming messages at all. The module had
not failed to tune; it had stopped talking. Bisecting the idle time put the
boundary between two and four minutes: 120 seconds of silence was safe, 240
seconds was not.

There was an elegant explanation available, and it was wrong. EN 50221 lets a
module ask the host to repeat the current date and time unprompted, every
`response_interval` seconds, and this driver had only ever answered that
enquiry when it was asked. A host that stops sending a required heartbeat,
and a module that stops responding, is a very satisfying pair of facts. So the
field was instrumented — and the module turned out to ask for an interval of
zero. It wants no periodic time updates whatsoever. The theory was dead, and it
died to a measurement rather than to an argument, which is the only way these
things should die.

The mechanism behind the watchdog is still unknown. What is known is that
ordinary traffic prevents it, so the driver now sends the same status read a
tuned adapter already sends, once a minute, whenever nothing else has gone
out. A streaming adapter adds no traffic at all, because it is already well
inside that window.

It is worth being clear about why this mattered enough to delay a release. A
driver that works perfectly while you are testing it and dies after four
minutes of inactivity is not a driver with a minor defect — it is a driver
that fails for every real user on their first evening, because a PVR back end
leaves adapters idle almost all of the time. It was found by accident, during
a test aimed at something else entirely.

## 16. Two sticks at once

**Measured**, with two units on one host, sharing an aerial through a splitter:

- Both register as independent adapters, with per-port kthreads and no reset
  ping-pong: **exactly one reset and one registration per device per cold
  enumeration**, across load/unload cycles, both bind orders, and
  deauthorize/reauthorize cycles.
- Both tune and stream **simultaneously on different muxes**. A 150-second
  concurrent capture produced 754,142,448 bytes (DVB-T2, 586 MHz) and
  509,064,144 bytes (DVB-T, 554 MHz) — 6.7 million packets in total with
  **zero sync errors, zero TEI-flagged packets and zero continuity
  discontinuities** on both.
- Unbinding, rebinding and deferred teardown of one stick while the other
  streams leaves the survivor untouched: no lock loss, no renumbering, no
  thread or state corruption.
- Across the whole test session: **zero** BUG, Oops, WARNING, refcount or
  use-after-free reports in the kernel log.

One measurement was initially misread, and the correction is instructive. An
early dual-device capture showed 14 continuity discontinuities and what looked
like phantom PIDs. Inspecting the packet headers showed every packet in the
burst had **TEI set** — the transport error indicator, which the *demodulator*
raises for uncorrectable errors. It was on-air reception damage, faithfully
passed through, during a moment when the aerial was physically being connected.
The analysis tool was wrong to count those packets, not the driver. Continuity
checks on DVB must exclude TEI-flagged packets, or they measure the atmosphere
instead of the software.

## 17. What is left

**Unknown / untested**, stated plainly:

- **Multi-PLP DVB-T2.** No multi-PLP transmission was reachable, so PLP field
  endianness is unconfirmed. Single-PLP works.
- **Suspend/resume** across host sleep.
- **Breadth.** Two sticks, one host, one transmitter, one country. Everything
  here could be true and still miss something that only appears elsewhere.

**Not implemented, deliberately:**

- CI/CAM support and anything content-protection related. Free-to-air only.
- Firmware update. The upgrade tags are known and are permanently absent from
  the driver.

**For upstreaming to `linux-media`**, still needed: a `checkpatch --strict`
pass as a patch series rather than a file, review of the deferred-teardown
approach by people who maintain `dvb_core` (there may be a sanctioned pattern
this missed), a `Documentation/` entry, and — most of all — reports from
hardware other than these two sticks.

If you own one of these, a report is worth more than anything else on this
list.
