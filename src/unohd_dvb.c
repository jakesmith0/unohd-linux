// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * unohd_dvb — Linux DVB driver for the SMiT SM1670 USB stick
 *             (Hauppauge WinTV-UnoHD / NexusHD, Freenet TV stick), 29df:0280.
 *
 * The stick is not a conventional USB tuner.  It is a Common Interface module
 * bolted to a DVB-T/T2 front end, and everything — tuning, status, the lot —
 * is driven over EN 50221 as bare SPDUs on the interrupt endpoints:
 *
 *   host -> device : interrupt OUT on EP 0x01, a bare SPDU (ZLP if n % 512 == 0)
 *   device -> host : interrupt IN  on EP 0x82, a bare SPDU
 *
 * Once the session layer is up, the module opens its own private resource
 * 0x00961001 and the host completes a CI+ SAS handshake against application id
 * "SMiTZBJL".  Tuner commands are SMiT TLVs carried inside sas_async_msg.
 *
 * Two things are not obvious and are the whole reason this works:
 *
 *   1. The module also opens its private *content protection* resource
 *      0xFCC00011 and pushes 9F9600 at the host.  That is CI+ CP, needing a
 *      real device certificate.  While it is outstanding the module accepts
 *      every tuner command and silently drops it.  We must refuse the
 *      resource — answer open_session_request with 0xF0, resource_not_found.
 *
 *   2. The media interface must be claimed before the session layer is brought
 *      up, or EP 0x84 stays silent forever.
 *
 * The protocol was recovered by observing the device on the wire and studying
 * the behaviour of the vendor's Windows userspace filter, then confirming every
 * claim against real hardware.  No vendor code, binary, decompiled source or
 * firmware is reproduced here.  See docs/write-up.md for the full account and
 * docs/how-it-works.md for the short version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/usb.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>	/* moved to linux/ in 6.12 */
#endif

#include <media/dvb_frontend.h>
#include <media/dvb_demux.h>
#include <media/dmxdev.h>
#include <media/dvb_net.h>
#include <media/dvbdev.h>

#define DRIVER_NAME "unohd_dvb"

#define UNOHD_VID   0x29df
#define UNOHD_PID   0x0280

#define EP_CMD_OUT  0x01
#define EP_CMD_IN   0x82
#define EP_TS_IN    0x84
#define CMD_MAXPKT  512
#define TS_BUFSZ    0xbc00      /* what the vendor filter submits on 0x84 */

/* SPDU tags */
#define S_SESSION_NUMBER  0x90
#define S_OPEN_SS_REQ     0x91
#define S_OPEN_SS_RSP     0x92
#define S_CREATE_SS       0x93
#define S_CREATE_SS_RSP   0x94
#define S_CLOSE_SS_REQ    0x95
#define S_CLOSE_SS_RSP    0x96

/* Resource ids */
#define RID_RM      0x00010041
#define RID_APPINFO 0x00020041
#define RID_CA      0x00030041
#define RID_HOST    0x00200041
#define RID_DATE    0x00240041
#define RID_MMI     0x00400041
#define RID_SMIT    0x00961001
#define RID_SMIT_CP 0xfcc00011  /* CI+ content protection — must be refused */

/* SMiT private commands */
#define SMIT_TUNER_LOCK       0x0003
#define SMIT_TUNER_LOCK_RSP   0x0004
#define SMIT_TUNER_STATUS     0x0005
#define SMIT_TUNER_STATUS_RSP 0x0006

/*
 * The module stops servicing its session altogether if the host goes quiet.
 * Measured on this hardware: idle for 120 s was still fine, idle for 240 s
 * left it answering nothing at all -- neither tuner_lock nor a status read --
 * until a USB reset and a fresh session announcement.  Poking it once a
 * minute with the same status read a tuned adapter already sends keeps it
 * awake.  It is a plain read; nothing is written and nothing persists.
 */
#define UNOHD_KEEPALIVE_MS 60000

#define UNOHD_MAX_SESS 32

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "enable protocol tracing");

#define dbg(d, fmt, ...) \
	do { if (debug) dev_info(&(d)->udev->dev, fmt, ##__VA_ARGS__); } while (0)

struct unohd_sess {
	u16 ssnb;
	u32 rid;
	bool open;
};

struct unohd {
	struct usb_device *udev;
	struct usb_interface *intf_cmd, *intf_ts;

	/* EN 50221 session layer */
	struct unohd_sess sess[UNOHD_MAX_SESS];
	int nsess;
	u16 next_ssnb;
	u16 sas_ssnb;
	bool sas_connected;
	bool sas_requested;
	u8 sas_msgnb;

	/*
	 * EN 50221 date_time.  A non-zero response_interval in date_time_enq
	 * means the module wants the time repeated every that many seconds
	 * without enquiring again; see unohd_pump_thread().
	 */
	u16 dt_ssnb;
	u8 dt_interval;
	unsigned long dt_next;

	/* see unohd_keepalive_work() */
	struct delayed_work keepalive;
	unsigned long last_cmd;

	struct mutex tx_lock;		/* serialises EP 0x01 */
	u8 *txbuf, *rxbuf;

	/* one outstanding SMiT command at a time */
	struct mutex cmd_lock;
	struct completion cmd_done;
	u16 cmd_wait_tag;
	u8 cmd_resp[64];
	int cmd_resp_len;

	/* cached front-end state, updated from tuner_status_rsp */
	bool locked;
	u8 strength, quality;

	struct task_struct *pump_task;
	struct task_struct *ts_task;

	/* DVB */
	struct dvb_adapter adap;
	struct dvb_demux demux;
	struct dmxdev dmxdev;
	struct dvb_net dvbnet;
	struct dvb_frontend fe;
	bool dvb_registered;
	bool dvb_failed;
	bool disconnected;
	int feed_count;
	/* guards feed_count and the TS thread's start/stop */
	struct mutex feed_lock;
	u8 *ts_buf;

	/* see unohd_put() */
	struct list_head zombie;
};

/* ---------------------------------------------------------------- wire */

static int unohd_tx(struct unohd *d, const u8 *b, int n)
{
	int actual, ret;

	mutex_lock(&d->tx_lock);
	memcpy(d->txbuf, b, n);
	ret = usb_interrupt_msg(d->udev, usb_sndintpipe(d->udev, EP_CMD_OUT),
				d->txbuf, n, &actual, 400);
	/* UCAM_Write: a whole number of max packets needs a terminating ZLP */
	if (!ret && n % CMD_MAXPKT == 0)
		usb_interrupt_msg(d->udev, usb_sndintpipe(d->udev, EP_CMD_OUT),
				  d->txbuf, 0, &actual, 400);
	mutex_unlock(&d->tx_lock);

	if (ret)
		dev_warn(&d->udev->dev, "TX failed: %d\n", ret);
	return ret;
}

/* EN 50221 length_field() */
static int unohd_lenfield(u8 *o, int len)
{
	if (len < 0x80) {
		o[0] = len;
		return 1;
	}
	if (len < 0x100) {
		o[0] = 0x81;
		o[1] = len;
		return 2;
	}
	o[0] = 0x82;
	o[1] = len >> 8;
	o[2] = len;
	return 3;
}

static int unohd_parse_lenfield(const u8 *b, int avail, int *len)
{
	int nb, i;

	if (avail < 1)
		return -1;
	if (!(b[0] & 0x80)) {
		*len = b[0];
		return 1;
	}
	nb = b[0] & 0x7f;
	if (nb == 0 || nb > 3 || avail < 1 + nb)
		return -1;
	for (*len = 0, i = 0; i < nb; i++)
		*len = (*len << 8) | b[1 + i];
	return 1 + nb;
}

static int unohd_send_apdu(struct unohd *d, u16 ssnb, u32 tag,
			   const u8 *data, int len)
{
	u8 f[512];
	int p = 0;

	if (len > (int)sizeof(f) - 16)
		return -EINVAL;

	f[p++] = S_SESSION_NUMBER;
	f[p++] = 0x02;
	f[p++] = ssnb >> 8;
	f[p++] = ssnb;
	f[p++] = tag >> 16;
	f[p++] = tag >> 8;
	f[p++] = tag;
	p += unohd_lenfield(f + p, len);
	if (len)
		memcpy(f + p, data, len);
	p += len;

	dbg(d, "-> ssnb=%u APDU %06x len=%d\n", ssnb, tag, len);
	return unohd_tx(d, f, p);
}

static int unohd_send_open_ss_rsp(struct unohd *d, u32 rid, u16 ssnb, u8 status)
{
	u8 f[9] = {
		S_OPEN_SS_RSP, 0x07, status,
		rid >> 24, rid >> 16, rid >> 8, rid,
		ssnb >> 8, ssnb,
	};

	dbg(d, "-> open_session_response %08x ssnb=%u status=%u\n",
	    rid, ssnb, status);
	return unohd_tx(d, f, sizeof(f));
}

static int unohd_send_close_ss_rsp(struct unohd *d, u16 ssnb, u8 status)
{
	u8 f[5] = { S_CLOSE_SS_RSP, 0x03, status, ssnb >> 8, ssnb };

	return unohd_tx(d, f, sizeof(f));
}

/* One SMiT TLV inside a CI+ sas_async_msg (9F9A07). */
static int unohd_sas_send(struct unohd *d, u16 tag, const u8 *data, int len)
{
	u8 m[128];
	int total = 4 + len;			/* message_length covers the TLV */

	if (!d->sas_connected)
		return -ENODEV;
	if (total > (int)sizeof(m) - 3)
		return -EINVAL;

	m[0] = d->sas_msgnb++;
	m[1] = total >> 8;
	m[2] = total;
	m[3] = tag >> 8;
	m[4] = tag;
	m[5] = len >> 8;
	m[6] = len;
	if (len)
		memcpy(m + 7, data, len);

	dbg(d, "-> SMiT cmd %04x len=%d\n", tag, len);
	return unohd_send_apdu(d, d->sas_ssnb, 0x9f9a07, m, 3 + total);
}

/*
 * Send one SMiT command and wait for its reply.  The module answers strictly
 * in order and only ever has one command outstanding, so a single rendezvous
 * slot is enough.
 */
static int unohd_sas_cmd(struct unohd *d, u16 tag, const u8 *data, int len,
			 u16 rsp_tag, u8 *rsp, int rsp_cap, int timeout_ms)
{
	int ret, n;

	mutex_lock(&d->cmd_lock);
	d->last_cmd = jiffies;
	reinit_completion(&d->cmd_done);
	d->cmd_wait_tag = rsp_tag;

	ret = unohd_sas_send(d, tag, data, len);
	if (ret)
		goto out;

	if (!wait_for_completion_timeout(&d->cmd_done,
					 msecs_to_jiffies(timeout_ms))) {
		ret = -ETIMEDOUT;
		goto out;
	}

	n = min(d->cmd_resp_len, rsp_cap);
	if (rsp && n > 0)
		memcpy(rsp, d->cmd_resp, n);
	ret = n;
out:
	d->cmd_wait_tag = 0;
	mutex_unlock(&d->cmd_lock);
	return ret;
}

/* ------------------------------------------------------- session layer */

static void unohd_sas_decode(struct unohd *d, const u8 *p, int len)
{
	int off = 3;			/* message_nb, message_length */

	while (off + 4 <= len) {
		u16 tag = p[off] << 8 | p[off + 1];
		int tl = p[off + 2] << 8 | p[off + 3];

		if (off + 4 + tl > len)
			break;

		if (tag == SMIT_TUNER_LOCK_RSP && tl >= 4) {
			u32 rc = get_unaligned_be32(p + off + 4);

			d->locked = (rc == 0);
			dbg(d, "<- tuner_lock result=%u\n", rc);
		} else if (tag == SMIT_TUNER_STATUS_RSP && tl >= 16) {
			const u8 *v = p + off + 4;

			d->locked = v[13] != 0;
			d->strength = v[14];
			d->quality = v[15];
			dbg(d, "<- status lock=%u strength=%u quality=%u\n",
			    v[13], v[14], v[15]);
		}

		if (d->cmd_wait_tag == tag) {
			d->cmd_resp_len = min_t(int, tl, sizeof(d->cmd_resp));
			memcpy(d->cmd_resp, p + off + 4, d->cmd_resp_len);
			complete(&d->cmd_done);
		}

		off += 4 + tl;
	}
}

/* MJD, per EN 300 468 annex C. */
static void unohd_send_date_time(struct unohd *d, u16 ssnb)
{
	struct tm tm;
	u8 b[7];
	long mjd;
	int Y, M, D, L;

	time64_to_tm(ktime_get_real_seconds(), 0, &tm);
	Y = tm.tm_year + 1900;
	M = tm.tm_mon + 1;
	D = tm.tm_mday;
	L = (M == 1 || M == 2) ? 1 : 0;
	mjd = 14956 + D + ((Y - 1900 - L) * 36525 / 100)
			+ ((M + 1 + L * 12) * 306001 / 10000);

	b[0] = mjd >> 8;
	b[1] = mjd;
	b[2] = ((tm.tm_hour / 10) << 4) | (tm.tm_hour % 10);
	b[3] = ((tm.tm_min / 10) << 4) | (tm.tm_min % 10);
	b[4] = ((tm.tm_sec / 10) << 4) | (tm.tm_sec % 10);
	b[5] = 0;
	b[6] = 0;
	unohd_send_apdu(d, ssnb, 0x9f8441, b, sizeof(b));
}

static void unohd_on_apdu(struct unohd *d, u16 ssnb, u32 tag,
			  const u8 *p, int len)
{
	u8 b[16];

	switch (tag) {
	case 0x9f8010:		/* profile_enq -> profile_reply */
	{
		int i, n = 0;
		static const u32 res[] = {
			RID_RM, RID_APPINFO, RID_CA, RID_DATE,
			RID_MMI, RID_SMIT, RID_HOST,
		};
		u8 rb[sizeof(res)];

		for (i = 0; i < (int)ARRAY_SIZE(res); i++) {
			rb[n++] = res[i] >> 24;
			rb[n++] = res[i] >> 16;
			rb[n++] = res[i] >> 8;
			rb[n++] = res[i];
		}
		unohd_send_apdu(d, ssnb, 0x9f8011, rb, n);
		break;
	}
	case 0x9f8011:		/* profile_reply -> profile_changed */
		unohd_send_apdu(d, ssnb, 0x9f8012, NULL, 0);
		break;
	case 0x9f8440:		/* date_time_enq */
		if (d->dt_interval != (len ? p[0] : 0))
			dev_info(&d->udev->dev,
				 "date_time response_interval = %u s\n",
				 len ? p[0] : 0);
		d->dt_ssnb = ssnb;
		d->dt_interval = len ? p[0] : 0;
		d->dt_next = jiffies + msecs_to_jiffies(d->dt_interval * 1000);
		unohd_send_date_time(d, ssnb);
		break;
	case 0x9f9a00:		/* sas_connect_rqst from the module */
		if (len >= 8) {
			memcpy(b, p, 8);
			b[8] = 0;
			unohd_send_apdu(d, ssnb, 0x9f9a01, b, 9);
		}
		break;
	case 0x9f9a01:		/* sas_connect_cnf */
		if (len && p[len - 1] == 0 && !d->sas_connected) {
			d->sas_ssnb = ssnb;
			d->sas_connected = true;
			d->last_cmd = jiffies;
			schedule_delayed_work(&d->keepalive,
					      msecs_to_jiffies(UNOHD_KEEPALIVE_MS));
			dev_info(&d->udev->dev,
				 "SAS connected on session %u\n", ssnb);
		}
		break;
	case 0x9f9a07:		/* sas_async_msg */
		unohd_sas_decode(d, p, len);
		break;
	default:
		break;
	}
}

static void unohd_on_session_opened(struct unohd *d, u16 ssnb, u32 rid)
{
	static const u8 sas_app_id[8] = "SMiTZBJL";

	switch (rid) {
	case RID_RM:
		unohd_send_apdu(d, ssnb, 0x9f8010, NULL, 0);	/* profile_enq */
		break;
	case RID_APPINFO:
		unohd_send_apdu(d, ssnb, 0x9f8020, NULL, 0);
		break;
	case RID_CA:
		unohd_send_apdu(d, ssnb, 0x9f8030, NULL, 0);
		break;
	case RID_SMIT:
		if (!d->sas_requested) {
			d->sas_requested = true;
			unohd_send_apdu(d, ssnb, 0x9f9a00,
					sas_app_id, sizeof(sas_app_id));
		}
		break;
	}
}

static void unohd_on_spdu(struct unohd *d, const u8 *b, int n)
{
	int i;

	if (n < 2)
		return;

	switch (b[0]) {
	case S_OPEN_SS_REQ: {
		u32 rid;
		u16 ssnb;

		if (n < 6)
			return;
		rid = get_unaligned_be32(b + 2);

		/*
		 * Refuse CI+ content protection.  Accepting it and then failing
		 * to authenticate wedges the module's SAS application: it keeps
		 * ACKing tuner commands at the USB level and executing none.
		 */
		if (rid == RID_SMIT_CP) {
			dbg(d, "<- open_session_request %08x — refusing\n", rid);
			unohd_send_open_ss_rsp(d, rid, 0, 0xf0);
			return;
		}
		if (d->nsess >= UNOHD_MAX_SESS)
			return;

		ssnb = d->next_ssnb++;
		d->sess[d->nsess].ssnb = ssnb;
		d->sess[d->nsess].rid = rid;
		d->sess[d->nsess].open = true;
		d->nsess++;
		unohd_send_open_ss_rsp(d, rid, ssnb, 0x00);
		unohd_on_session_opened(d, ssnb, rid);
		break;
	}
	case S_CREATE_SS_RSP: {
		u32 rid;
		u16 ssnb;

		if (n < 9 || b[2] != 0 || d->nsess >= UNOHD_MAX_SESS)
			return;
		rid = get_unaligned_be32(b + 3);
		ssnb = b[7] << 8 | b[8];
		d->sess[d->nsess].ssnb = ssnb;
		d->sess[d->nsess].rid = rid;
		d->sess[d->nsess].open = true;
		d->nsess++;
		unohd_on_session_opened(d, ssnb, rid);
		break;
	}
	case S_CLOSE_SS_REQ: {
		u16 ssnb;

		if (n < 4)
			return;
		ssnb = b[2] << 8 | b[3];
		for (i = 0; i < d->nsess; i++)
			if (d->sess[i].ssnb == ssnb)
				d->sess[i].open = false;
		unohd_send_close_ss_rsp(d, ssnb, 0x00);
		break;
	}
	case S_SESSION_NUMBER: {
		u16 ssnb;
		u32 tag;
		int len, lf;

		if (n < 7)
			return;
		ssnb = b[2] << 8 | b[3];
		tag = (u32)b[4] << 16 | b[5] << 8 | b[6];
		lf = unohd_parse_lenfield(b + 7, n - 7, &len);
		if (lf < 0)
			return;
		if (len > n - 7 - lf)
			len = n - 7 - lf;
		unohd_on_apdu(d, ssnb, tag, b + 7 + lf, len);
		break;
	}
	default:
		dbg(d, "<- unhandled SPDU tag 0x%02x (%d bytes)\n", b[0], n);
		break;
	}
}

/*
 * Keep the module's session alive across idle periods.  Skipped whenever a
 * real command went out recently, so a streaming adapter -- which polls
 * status constantly -- adds no traffic at all.
 */
static void unohd_keepalive_work(struct work_struct *w)
{
	struct unohd *d = container_of(to_delayed_work(w), struct unohd,
				       keepalive);
	static const u8 gsta[4] = "GSTA";

	if (d->disconnected)
		return;

	if (time_after(jiffies,
		       d->last_cmd + msecs_to_jiffies(UNOHD_KEEPALIVE_MS)))
		unohd_sas_cmd(d, SMIT_TUNER_STATUS, gsta, sizeof(gsta),
			      SMIT_TUNER_STATUS_RSP, NULL, 0, 2000);

	schedule_delayed_work(&d->keepalive,
			      msecs_to_jiffies(UNOHD_KEEPALIVE_MS));
}

/* ------------------------------------------------------------ threads */

static int unohd_register_dvb(struct unohd *d);

static int unohd_pump_thread(void *arg)
{
	struct unohd *d = arg;
	unsigned long deadline = jiffies + msecs_to_jiffies(30000);
	int ret, actual;

	while (!kthread_should_stop()) {
		ret = usb_interrupt_msg(d->udev,
					usb_rcvintpipe(d->udev, EP_CMD_IN),
					d->rxbuf, 2048, &actual, 400);
		if (!ret && actual > 0)
			unohd_on_spdu(d, d->rxbuf, actual);
		else if (ret && ret != -ETIMEDOUT && ret != -EAGAIN)
			break;

		if (d->dt_interval && time_after(jiffies, d->dt_next)) {
			d->dt_next = jiffies +
				msecs_to_jiffies(d->dt_interval * 1000);
			unohd_send_date_time(d, d->dt_ssnb);
		}

		if (!d->dvb_registered && d->sas_connected && !d->dvb_failed) {
			if (unohd_register_dvb(d)) {
				/*
				 * Registration allocates an adapter number and
				 * unwinds on failure.  Retrying it once per
				 * received packet would churn through those,
				 * so give up and say so.
				 */
				d->dvb_failed = true;
				dev_err(&d->udev->dev,
					"DVB registration failed\n");
			}
		} else if (!d->dvb_registered && !d->dvb_failed &&
			   time_after(jiffies, deadline)) {
			dev_err(&d->udev->dev,
				"module never completed the SAS handshake\n");
			deadline = jiffies + msecs_to_jiffies(30000);
		}
	}
	return 0;
}

static int unohd_ts_thread(void *arg)
{
	struct unohd *d = arg;
	int ret, actual;

	while (!kthread_should_stop()) {
		ret = usb_bulk_msg(d->udev, usb_rcvbulkpipe(d->udev, EP_TS_IN),
				   d->ts_buf, TS_BUFSZ, &actual, 500);
		if (!ret && actual > 0)
			dvb_dmx_swfilter(&d->demux, d->ts_buf, actual);
		else if (ret && ret != -ETIMEDOUT)
			usleep_range(1000, 2000);
	}
	return 0;
}

/* --------------------------------------------------------- front end */

static int unohd_fe_set_frontend(struct dvb_frontend *fe)
{
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct unohd *d = fe->demodulator_priv;
	u32 khz = c->frequency / 1000;
	u8 bw = c->bandwidth_hz ? c->bandwidth_hz / 1000000 : 8;
	u8 std = (c->delivery_system == SYS_DVBT2) ? 3 : 0;
	u8 plp = 0;
	u8 b[16];
	int ret;

	/*
	 * The PLP field is not optional.  0xFFFF is not "any PLP" — it routes
	 * no PLP to the output, and a DVB-T2 mux then locks perfectly and
	 * delivers nothing.  Default to PLP 0 when no stream id is set.
	 */
	if (c->stream_id != NO_STREAM_ID_FILTER)
		plp = c->stream_id & 0xff;

	memset(b, 0, sizeof(b));
	b[0] = khz;
	b[1] = khz >> 8;
	b[2] = khz >> 16;
	b[3] = khz >> 24;
	b[10] = plp;
	b[11] = 0;
	b[12] = bw;
	b[13] = std;		/* advisory: the demod auto-detects T vs T2 */

	dev_info(&d->udev->dev, "tune %u kHz bw %u %s plp %u\n",
		 khz, bw, std == 3 ? "DVB-T2" : "DVB-T", plp);

	d->locked = false;
	ret = unohd_sas_cmd(d, SMIT_TUNER_LOCK, b, sizeof(b),
			    SMIT_TUNER_LOCK_RSP, NULL, 0, 5000);
	return ret < 0 ? ret : 0;
}

static int unohd_fe_get_frontend(struct dvb_frontend *fe,
				 struct dtv_frontend_properties *c)
{
	return 0;			/* cache already holds what we set */
}

static int unohd_fe_read_status(struct dvb_frontend *fe,
				enum fe_status *status)
{
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct unohd *d = fe->demodulator_priv;
	static const u8 gsta[4] = "GSTA";

	unohd_sas_cmd(d, SMIT_TUNER_STATUS, gsta, sizeof(gsta),
		      SMIT_TUNER_STATUS_RSP, NULL, 0, 2000);

	*status = d->locked ? (FE_HAS_SIGNAL | FE_HAS_CARRIER | FE_HAS_VITERBI |
			       FE_HAS_SYNC | FE_HAS_LOCK) : 0;

	/*
	 * The module reports strength and quality as 0..100 with no stated
	 * units, so both are published on the relative scale rather than
	 * invented dBm/dB figures.  Everything else it does not report at all,
	 * and saying so is better than publishing a zero that looks like data.
	 */
	c->strength.len = 1;
	c->cnr.len = 1;
	if (d->locked) {
		c->strength.stat[0].scale = FE_SCALE_RELATIVE;
		c->strength.stat[0].uvalue = min_t(u32, d->strength, 100) * 655;
		c->cnr.stat[0].scale = FE_SCALE_RELATIVE;
		c->cnr.stat[0].uvalue = min_t(u32, d->quality, 100) * 655;
	} else {
		c->strength.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		c->cnr.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
	}

	c->pre_bit_error.len = 1;
	c->pre_bit_error.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
	c->post_bit_error.len = 1;
	c->post_bit_error.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
	c->block_error.len = 1;
	c->block_error.stat[0].scale = FE_SCALE_NOT_AVAILABLE;

	return 0;
}

static int unohd_fe_read_signal_strength(struct dvb_frontend *fe, u16 *st)
{
	struct unohd *d = fe->demodulator_priv;

	*st = (u16)d->strength * 655;
	return 0;
}

static int unohd_fe_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	struct unohd *d = fe->demodulator_priv;

	*snr = (u16)d->quality * 655;
	return 0;
}

static int unohd_fe_read_ber(struct dvb_frontend *fe, u32 *ber)
{
	*ber = 0;
	return 0;
}

static int unohd_fe_read_ucblocks(struct dvb_frontend *fe, u32 *ucb)
{
	*ucb = 0;
	return 0;
}

static int unohd_fe_get_tune_settings(struct dvb_frontend *fe,
				      struct dvb_frontend_tune_settings *s)
{
	s->min_delay_ms = 800;
	return 0;
}

static const struct dvb_frontend_ops unohd_fe_ops = {
	.delsys = { SYS_DVBT, SYS_DVBT2 },
	.info = {
		.name			= "SMiT SM1670 (WinTV-UnoHD)",
		.frequency_min_hz	= 174 * 1000 * 1000,
		.frequency_max_hz	= 862 * 1000 * 1000,
		.frequency_stepsize_hz	= 166667,
		.caps = FE_CAN_FEC_AUTO | FE_CAN_QAM_AUTO |
			FE_CAN_TRANSMISSION_MODE_AUTO |
			FE_CAN_GUARD_INTERVAL_AUTO |
			FE_CAN_HIERARCHY_AUTO | FE_CAN_MUTE_TS |
			FE_CAN_2G_MODULATION | FE_CAN_MULTISTREAM,
	},
	.set_frontend		= unohd_fe_set_frontend,
	.get_frontend		= unohd_fe_get_frontend,
	.read_status		= unohd_fe_read_status,
	.read_signal_strength	= unohd_fe_read_signal_strength,
	.read_snr		= unohd_fe_read_snr,
	.read_ber		= unohd_fe_read_ber,
	.read_ucblocks		= unohd_fe_read_ucblocks,
	.get_tune_settings	= unohd_fe_get_tune_settings,
};

/* -------------------------------------------------------------- demux */

/*
 * The only safe place to touch ts_task.  disconnect() and stop_feed() both
 * want to stop the thread and will happily race to do it twice: disconnect
 * tears down the DVB objects, and dvb_dmxdev_release() then runs stop_feed
 * on behalf of an application that still has dvr0 open.  Stopping a task
 * struct twice underflows its refcount and then dereferences NULL inside
 * kthread_stop(), which is exactly what unplugging mid-stream used to do.
 */
static void unohd_stop_ts_locked(struct unohd *d)
{
	struct task_struct *t = d->ts_task;

	lockdep_assert_held(&d->feed_lock);
	if (!t)
		return;
	d->ts_task = NULL;
	kthread_stop(t);
}

static int unohd_start_feed(struct dvb_demux_feed *feed)
{
	struct unohd *d = feed->demux->priv;
	int ret = 0;

	mutex_lock(&d->feed_lock);
	if (d->disconnected) {
		mutex_unlock(&d->feed_lock);
		return -ENODEV;
	}
	if (d->feed_count++ == 0) {
		usb_clear_halt(d->udev, usb_rcvbulkpipe(d->udev, EP_TS_IN));
		d->ts_task = kthread_run(unohd_ts_thread, d, "unohd-ts/%s",
					 d->udev->devpath);
		if (IS_ERR(d->ts_task)) {
			ret = PTR_ERR(d->ts_task);
			d->ts_task = NULL;
			d->feed_count--;
		}
	}
	mutex_unlock(&d->feed_lock);
	return ret;
}

static int unohd_stop_feed(struct dvb_demux_feed *feed)
{
	struct unohd *d = feed->demux->priv;

	mutex_lock(&d->feed_lock);
	if (--d->feed_count <= 0) {
		d->feed_count = 0;
		unohd_stop_ts_locked(d);
	}
	mutex_unlock(&d->feed_lock);
	return 0;
}

static int unohd_register_dvb(struct unohd *d)
{
	short adapter_nr[] = { -1 };
	int ret;

	ret = dvb_register_adapter(&d->adap, "WinTV-UnoHD", THIS_MODULE,
				   &d->udev->dev, adapter_nr);
	if (ret < 0)
		return ret;

	d->demux.dmx.capabilities = DMX_TS_FILTERING | DMX_SECTION_FILTERING;
	d->demux.priv = d;
	d->demux.filternum = 256;
	d->demux.feednum = 256;
	d->demux.start_feed = unohd_start_feed;
	d->demux.stop_feed = unohd_stop_feed;
	ret = dvb_dmx_init(&d->demux);
	if (ret < 0)
		goto err_adapter;

	d->dmxdev.filternum = 256;
	d->dmxdev.demux = &d->demux.dmx;
	ret = dvb_dmxdev_init(&d->dmxdev, &d->adap);
	if (ret < 0)
		goto err_dmx;

	ret = dvb_net_init(&d->adap, &d->dvbnet, &d->demux.dmx);
	if (ret < 0)
		goto err_dmxdev;

	memcpy(&d->fe.ops, &unohd_fe_ops, sizeof(struct dvb_frontend_ops));
	d->fe.demodulator_priv = d;
	ret = dvb_register_frontend(&d->adap, &d->fe);
	if (ret < 0)
		goto err_net;

	d->dvb_registered = true;
	dev_info(&d->udev->dev, "registered DVB adapter %d\n", d->adap.num);
	return 0;

err_net:
	dvb_net_release(&d->dvbnet);
err_dmxdev:
	dvb_dmxdev_release(&d->dmxdev);
err_dmx:
	dvb_dmx_release(&d->demux);
err_adapter:
	dvb_unregister_adapter(&d->adap);
	return ret;
}

static void unohd_unregister_dvb(struct unohd *d)
{
	if (!d->dvb_registered)
		return;
	/*
	 * Teardown order as the in-tree usb drivers do it.
	 *
	 * DVB_FE_DEVICE_REMOVED first: dvb_frontend_ioctl() and
	 * dvb_frontend_open() both check fe->exit and return -ENODEV, which is
	 * what stops dvb_core calling our ops with a demodulator_priv that is
	 * about to go away.
	 *
	 * dvb_frontend_detach() does NOT wait for an application to close
	 * frontend0 -- dvb_register_frontend() leaves two references and each
	 * open() takes another, so unregister+detach here can drop the count to
	 * one, not zero.  Whether it is then safe to free d is decided in
	 * unohd_put().
	 */
	d->fe.exit = DVB_FE_DEVICE_REMOVED;

	dvb_net_release(&d->dvbnet);
	dvb_dmxdev_release(&d->dmxdev);
	dvb_dmx_release(&d->demux);
	dvb_unregister_frontend(&d->fe);
	dvb_frontend_detach(&d->fe);
	dvb_unregister_adapter(&d->adap);
	d->dvb_registered = false;
}

/*
 * dvb_core holds a reference to struct dvb_frontend for as long as an
 * application has frontend0 open, and dvb_frontend_release() dereferences both
 * fe and fe->dvb (which is &d->adap) on the close.  Both live inside struct
 * unohd.  Freeing d in disconnect() while an fd was still open was a
 * use-after-free: a general protection fault on a non-canonical address in
 * __wake_up() from dvb_frontend_release(), reproduced on demand.
 *
 * The sanctioned escape is fe->ops.release, which __dvb_frontend_free() calls
 * once the last reference goes.  We cannot use it.  CONFIG_MEDIA_ATTACH is set
 * on this kernel, and dvb_frontend_invoke_release() then follows ops.release
 * with dvb_detach(), i.e. symbol_put_addr(), which would module_put() this
 * module for a reference dvb_attach() never took.  That hook belongs to
 * separately built demod modules; a self-contained driver is not one.
 *
 * So disconnect() frees d only once dvb_core has demonstrably let go, and
 * otherwise parks it here.  dvbdev.c sets dvbdevfops->owner = adap->module, so
 * an open device node pins this module and module_exit cannot run until every
 * fd has been closed -- which is what makes draining the list there safe.
 */
static DEFINE_MUTEX(unohd_zombie_lock);
static LIST_HEAD(unohd_zombies);

static void unohd_free(struct unohd *d)
{
	usb_put_dev(d->udev);
	kfree(d->ts_buf);
	kfree(d->rxbuf);
	kfree(d->txbuf);
	kfree(d);
}

static void unohd_put(struct unohd *d)
{
	/*
	 * Zero covers both "never registered" (kzalloc) and "registered, and
	 * dvb_core has already dropped its last reference".  Anything else and
	 * dvb_core can still reach into d, so it has to outlive us.
	 */
	if (kref_read(&d->fe.refcount) == 0) {
		unohd_free(d);
		return;
	}

	dev_info(&d->udev->dev,
		 "frontend still open; deferring teardown to module unload\n");
	mutex_lock(&unohd_zombie_lock);
	list_add(&d->zombie, &unohd_zombies);
	mutex_unlock(&unohd_zombie_lock);
}

static void unohd_drain_zombies(void)
{
	struct unohd *d, *tmp;

	mutex_lock(&unohd_zombie_lock);
	list_for_each_entry_safe(d, tmp, &unohd_zombies, zombie) {
		list_del(&d->zombie);
		unohd_free(d);
	}
	mutex_unlock(&unohd_zombie_lock);
}

/* ---------------------------------------------------------- usb glue */

static struct usb_driver unohd_driver;

/*
 * The module announces its CI sessions exactly once, immediately after
 * enumeration, and is mute forever afterwards — a host that merely opens the
 * device late sees nothing, and host-initiated create_session gets no reply
 * either (measured: attaching without a reset and issuing create_session
 * yields no response at all).  So the first probe resets the device and lets
 * the second probe do the real work.
 *
 * usb_reset_device on this device is established as safe and reversible; see
 * docs/how-it-works.md.  The serial number and timestamp below tell a post-reset
 * probe apart from a cold one; both are reinitialised by a module reload.
 */
/*
 * "Have I just reset this one?" is per-device state and must not be kept in a
 * single global slot.  With two dongles the slot ping-pongs: probe(A) records
 * A and resets it, probe(B) overwrites the slot with B and resets it, and when
 * A re-enumerates the slot still says B -- so A is judged not-fresh and reset
 * again, for ever.  One record per device, keyed by bus and port path, which
 * are what survive a reset (the serial is kept only to make the log readable,
 * and would not distinguish two units that report none).
 */
#define UNOHD_RESET_TIMEOUT_MS	60000

struct unohd_reset_rec {
	struct list_head node;
	unsigned int busnum;
	char devpath[32];
	unsigned long when;
};

static DEFINE_MUTEX(unohd_reset_lock);
static LIST_HEAD(unohd_reset_recs);

static bool unohd_rec_matches(const struct unohd_reset_rec *r,
			      struct usb_device *udev)
{
	return r->busnum == udev->bus->busnum &&
	       !strcmp(r->devpath, udev->devpath);
}

static bool unohd_freshly_reset(struct usb_device *udev)
{
	struct unohd_reset_rec *r, *tmp;
	bool fresh = false;

	mutex_lock(&unohd_reset_lock);

	list_for_each_entry_safe(r, tmp, &unohd_reset_recs, node) {
		/* Drop anything stale, whoever it belongs to. */
		if (time_after(jiffies, r->when +
			       msecs_to_jiffies(UNOHD_RESET_TIMEOUT_MS))) {
			list_del(&r->node);
			kfree(r);
			continue;
		}
		if (unohd_rec_matches(r, udev)) {
			/*
			 * Consume it: this is the post-reset probe, and a
			 * later cold replug of the same port must reset again.
			 */
			list_del(&r->node);
			kfree(r);
			fresh = true;
		}
	}

	if (!fresh) {
		r = kzalloc(sizeof(*r), GFP_KERNEL);
		if (r) {
			r->busnum = udev->bus->busnum;
			strscpy(r->devpath, udev->devpath, sizeof(r->devpath));
			r->when = jiffies;
			list_add(&r->node, &unohd_reset_recs);
		}
		/*
		 * If the allocation failed we report not-fresh and reset, and
		 * the next probe will try to allocate again.  Reporting fresh
		 * would skip the reset the device needs.
		 */
	}

	mutex_unlock(&unohd_reset_lock);
	return fresh;
}

static void unohd_drop_reset_recs(void)
{
	struct unohd_reset_rec *r, *tmp;

	mutex_lock(&unohd_reset_lock);
	list_for_each_entry_safe(r, tmp, &unohd_reset_recs, node) {
		list_del(&r->node);
		kfree(r);
	}
	mutex_unlock(&unohd_reset_lock);
}

static int unohd_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *alt = intf->cur_altsetting;
	struct unohd *d;
	struct usb_interface *ts_intf;
	int ret;

	/* Bind once, on the command interface; take the media one ourselves. */
	if (alt->desc.bInterfaceNumber != 0)
		return -ENODEV;

	if (!unohd_freshly_reset(udev)) {
		dev_info(&udev->dev,
			 "resetting so the CI module re-announces its sessions\n");
		usb_set_intfdata(intf, NULL);
		usb_queue_reset_device(intf);
		return 0;
	}

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	/*
	 * A parked struct unohd outlives the USB device, and dvb_core still
	 * reaches fe->dvb->device, which is &udev->dev, from its release path.
	 */
	d->udev = usb_get_dev(udev);
	d->intf_cmd = intf;
	d->next_ssnb = 1;
	mutex_init(&d->tx_lock);
	mutex_init(&d->cmd_lock);
	mutex_init(&d->feed_lock);
	init_completion(&d->cmd_done);
	INIT_DELAYED_WORK(&d->keepalive, unohd_keepalive_work);
	d->last_cmd = jiffies;

	d->txbuf = kzalloc(2048, GFP_KERNEL);
	d->rxbuf = kzalloc(2048, GFP_KERNEL);
	d->ts_buf = kzalloc(TS_BUFSZ, GFP_KERNEL);
	if (!d->txbuf || !d->rxbuf || !d->ts_buf) {
		ret = -ENOMEM;
		goto err_free;
	}

	/*
	 * Claim the media interface BEFORE bringing the session layer up.
	 * Claiming it afterwards leaves EP 0x84 permanently silent.
	 */
	ts_intf = usb_ifnum_to_if(udev, 1);
	if (!ts_intf) {
		dev_err(&udev->dev, "no media interface\n");
		ret = -ENODEV;
		goto err_free;
	}
	ret = usb_driver_claim_interface(&unohd_driver, ts_intf, d);
	if (ret) {
		dev_err(&udev->dev, "cannot claim media interface: %d\n", ret);
		goto err_free;
	}
	d->intf_ts = ts_intf;

	usb_set_intfdata(intf, d);

	d->pump_task = kthread_run(unohd_pump_thread, d, "unohd-spdu/%s",
				   udev->devpath);
	if (IS_ERR(d->pump_task)) {
		ret = PTR_ERR(d->pump_task);
		d->pump_task = NULL;
		goto err_release;
	}

	dev_info(&udev->dev, "WinTV-UnoHD: bringing up EN 50221 session layer\n");
	return 0;

err_release:
	usb_set_intfdata(intf, NULL);
	usb_driver_release_interface(&unohd_driver, ts_intf);
err_free:
	/* Nothing was registered with dvb_core, so nothing can still hold d. */
	unohd_free(d);
	return ret;
}

static void unohd_disconnect(struct usb_interface *intf)
{
	struct unohd *d = usb_get_intfdata(intf);

	if (!d)
		return;
	/* Called for both interfaces; only tear down from the command one. */
	if (intf->cur_altsetting->desc.bInterfaceNumber != 0) {
		usb_set_intfdata(intf, NULL);
		return;
	}

	/*
	 * Order matters.  Stop feeding the demux first, then stop the session
	 * pump, then unregister -- dvb_dmxdev_release() blocks until the last
	 * application closes its fd, and that close comes back through
	 * stop_feed(), which must find nothing left to do.
	 */
	mutex_lock(&d->feed_lock);
	d->disconnected = true;
	d->feed_count = 0;
	unohd_stop_ts_locked(d);
	mutex_unlock(&d->feed_lock);

	cancel_delayed_work_sync(&d->keepalive);

	if (d->pump_task) {
		kthread_stop(d->pump_task);
		d->pump_task = NULL;
	}

	unohd_unregister_dvb(d);

	usb_set_intfdata(intf, NULL);
	if (d->intf_ts) {
		usb_set_intfdata(d->intf_ts, NULL);
		usb_driver_release_interface(&unohd_driver, d->intf_ts);
	}

	unohd_put(d);
}

static const struct usb_device_id unohd_table[] = {
	{ USB_DEVICE(UNOHD_VID, UNOHD_PID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, unohd_table);

static struct usb_driver unohd_driver = {
	.name		= DRIVER_NAME,
	.probe		= unohd_probe,
	.disconnect	= unohd_disconnect,
	.id_table	= unohd_table,
};

static int __init unohd_init(void)
{
	return usb_register(&unohd_driver);
}

static void __exit unohd_exit(void)
{
	usb_deregister(&unohd_driver);
	unohd_drain_zombies();
	unohd_drop_reset_recs();
}

module_init(unohd_init);
module_exit(unohd_exit);

MODULE_AUTHOR("Jake Smith");
MODULE_DESCRIPTION("SMiT SM1670 (Hauppauge WinTV-UnoHD) DVB-T/T2 driver");
MODULE_VERSION("0.1.0");
MODULE_LICENSE("GPL");
