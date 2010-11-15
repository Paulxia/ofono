/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <net/if.h>
#include <errno.h>
#include <glib.h>

#include "message.h"
#include "common.h"
#include "modem.h"
#include "socket.h"

struct _GIsiServiceMux {
	GIsiModem *modem;
	GSList *pending;
	GIsiVersion version;
	uint8_t resource;
	uint8_t last_utid;
	unsigned subscriptions;
	unsigned registrations;
	gboolean reachable;
	gboolean version_pending;
};
typedef struct _GIsiServiceMux GIsiServiceMux;

struct _GIsiModem {
	unsigned index;
	GHashTable *services;
	gboolean subs_source;
	int req_fd;
	int ind_fd;
	guint req_watch;
	guint ind_watch;
	GIsiNotifyFunc debug;
	void *ddata;
};

struct _GIsiPending {
	enum GIsiMessageType type;
	GIsiServiceMux *service;
	guint timeout;
	GIsiNotifyFunc notify;
	GDestroyNotify destroy;
	void *data;
	uint8_t utid;
	uint8_t msgid;
};

static const struct sockaddr_pn namesrv = {
	.spn_family = AF_PHONET,
	.spn_resource = PN_NAMESERVICE,
};

static const struct sockaddr_pn commgr = {
	.spn_family = AF_PHONET,
	.spn_resource = PN_COMMGR,
};

static void service_finalize(gpointer value);

static GIsiServiceMux *service_get(GIsiModem *modem, uint8_t resource)
{
	GIsiServiceMux *mux;
	int key = resource;

	mux = g_hash_table_lookup(modem->services, GINT_TO_POINTER(key));
	if (mux)
		return mux;

	mux = g_try_new0(GIsiServiceMux, 1);
	if (!mux)
		return NULL;

	g_hash_table_insert(modem->services, GINT_TO_POINTER(key), mux);

	mux->modem = modem;
	mux->resource = resource;
	mux->version.major = -1;
	mux->version.minor = -1;
	mux->reachable = FALSE;
	mux->version_pending = FALSE;

	return mux;
}

static gint utid_equal(gconstpointer a, gconstpointer b)
{
	const GIsiPending *pa = a;
	const GIsiPending *pb = b;

	return pa->utid - pb->utid;
}

static void pending_dispatch(GIsiPending *pend, GIsiMessage *msg)
{
	if (!pend->notify)
		return;

	pend->notify(msg, pend->data);
}

static void service_dispatch(GIsiServiceMux *mux, GIsiMessage *msg,
				gboolean is_indication)
{
	uint8_t msgid = g_isi_msg_id(msg);
	uint8_t utid = g_isi_msg_utid(msg);

	GSList *l = mux->pending;

	while (l) {
		GSList *next = l->next;
		GIsiPending *pend = l->data;
		msg->private = pend;

		/*
		 * REQs, NTFs and INDs are dispatched on message ID.  While
		 * INDs have the unique transaction ID set to zero, NTFs
		 * typically mirror the UTID of the request that set up the
		 * session, and REQs can naturally have any transaction ID.
		 *
		 * RESPs are dispatched on unique transaction ID, explicitly
		 * ignoring the msgid.  A RESP also completes a transaction,
		 * so it needs to be removed after being notified of.
		 *
		 * Version query responses are dispatched in a similar fashion
		 * as RESPs, but based on the pending type and the message ID.
		 * Some of these may be synthesized, but nevertheless need to
		 * be removed.
		 */
		if (pend->type < GISI_MESSAGE_TYPE_RESP && pend->msgid == msgid) {

			pending_dispatch(pend, msg);

		} else if (pend->type == GISI_MESSAGE_TYPE_RESP &&
				!is_indication && pend->utid == utid) {

			pending_dispatch(pend, msg);
			pend->notify = NULL;

			g_isi_pending_remove(pend);
			break;

		} else if (pend->type == GISI_MESSAGE_TYPE_COMMON &&
				msgid == COMMON_MESSAGE &&
				pend->msgid == COMM_ISI_VERSION_GET_REQ) {

			pending_dispatch(pend, msg);
			pend->notify = NULL;

			g_isi_pending_remove(pend);

		}
		l = next;
	}
}

static void common_message_decode(GIsiServiceMux *mux, GIsiMessage *msg)
{
	uint8_t code;
	uint8_t major;
	uint8_t minor;

	if (!g_isi_msg_data_get_byte(msg, 0, &code))
		return;

	switch (code) {
	case COMM_ISI_VERSION_GET_RESP:

		if (g_isi_msg_data_get_byte(msg, 1, &major) &&
				g_isi_msg_data_get_byte(msg, 2, &minor)) {
			mux->version.major = major;
			mux->version.minor = minor;
		}
		mux->version_pending = FALSE;
		break;

	case COMM_ISA_ENTITY_NOT_REACHABLE_RESP:
		mux->reachable = FALSE;
		msg->error = ENOENT;
		break;

	case 0x00:
		/*
		 * PN_SIM doesn't support ISI version, but sends a
		 * 0x00 message as a response.  Work around this modem
		 * wart.
		 */
		mux->version_pending = FALSE;
		break;
	}
	msg->version = &mux->version;
}

static gboolean isi_callback(GIOChannel *channel, GIOCondition cond,
				gpointer data)
{
	GIsiModem *modem = data;
	int len;
	int fd;

	if (cond & (G_IO_NVAL|G_IO_HUP)) {
		g_warning("Unexpected event on PhoNet channel %p", channel);
		return FALSE;
	}

	fd = g_io_channel_unix_get_fd(channel);
	len = g_isi_phonet_peek_length(channel);

	if (len > 0) {
		struct sockaddr_pn addr;
		uint32_t buf[(len + 3) / 4];

		GIsiServiceMux *mux;
		GIsiMessage msg;
		unsigned key;

		len = g_isi_phonet_read(channel, buf, len, &addr);
		if (len < 2)
			return TRUE;

		msg.addr = &addr;
		msg.error = 0;
		msg.data = buf;
		msg.len = len;
		msg.version = &mux->version;

		if (modem->debug)
			modem->debug(&msg, modem->ddata);

		key = addr.spn_resource;
		mux = g_hash_table_lookup(modem->services, GINT_TO_POINTER(key));
		if (!mux)
			return TRUE;

		/*
		 * For now, assume any message received means that the
		 * resource is reachable
		 */
		mux->reachable = TRUE;

		if (g_isi_msg_id(&msg) == COMMON_MESSAGE)
			common_message_decode(mux, &msg);

		service_dispatch(mux, &msg, fd == modem->ind_fd);
	}
	return TRUE;
}

GIsiModem *g_isi_modem_create(unsigned index)
{
	GIsiModem *modem;
	GIOChannel *inds;
	GIOChannel *reqs;

	if (index == 0) {
		errno = ENODEV;
		return NULL;
	}

	modem = g_try_new0(GIsiModem, 1);
	if (!modem) {
		errno = ENOMEM;
		return NULL;
	}

	inds = g_isi_phonet_new(index);
	reqs = g_isi_phonet_new(index);

	if (!inds || !reqs) {
		g_free(modem);
		return NULL;
	}

	modem->req_fd = g_io_channel_unix_get_fd(reqs);
	modem->req_watch = g_io_add_watch(reqs,
					G_IO_IN|G_IO_ERR|G_IO_HUP|G_IO_NVAL,
					isi_callback, modem);
	modem->ind_fd = g_io_channel_unix_get_fd(inds);
	modem->ind_watch = g_io_add_watch(inds,
					G_IO_IN|G_IO_ERR|G_IO_HUP|G_IO_NVAL,
					isi_callback, modem);

	g_io_channel_unref(reqs);
	g_io_channel_unref(inds);

	modem->index = index;
	modem->services = g_hash_table_new_full(g_direct_hash, NULL,
						NULL, service_finalize);

	return modem;
}

GIsiModem *g_isi_modem_create_by_name(const char *name)
{
	return g_isi_modem_create(if_nametoindex(name));
}

static void service_name_register(GIsiServiceMux *mux)
{
	uint8_t msg[] = {
		0, PNS_NAME_ADD_REQ, 0, 0,
		0, 0, 0, mux->resource,	/* 32-bit Big-Endian name */
		0, 0,			/* device/object */
		0, 0,			/* filler */
	};
	uint16_t object = 0;

	if (ioctl(mux->modem->req_fd, SIOCPNGETOBJECT, &object) < 0) {
		g_warning("ioctl(SIOCPNGETOBJECT): %s", strerror(errno));
		return;
	}

	/* Fill in the object ID */
	msg[8] = object >> 8;
	msg[9] = object & 0xFF;

	sendto(mux->modem->req_fd, msg, sizeof(msg), MSG_NOSIGNAL,
		(void *)&namesrv, sizeof(namesrv));
}

static void service_name_deregister(GIsiServiceMux *mux)
{
	const uint8_t msg[] = {
		0, PNS_NAME_REMOVE_REQ, 0, 0,
		0, 0, 0, mux->resource,
	};

	sendto(mux->modem->req_fd, msg, sizeof(msg), MSG_NOSIGNAL,
		(void *)&namesrv, sizeof(namesrv));
}

static gboolean modem_subs_update(gpointer data)
{
	GHashTableIter iter;
	gpointer keyptr, value;

	GIsiModem *modem = data;
	uint8_t msg[3 + 256] = {
		0, PNS_SUBSCRIBED_RESOURCES_IND,
		0,
	};
	uint8_t count = 0;

	modem->subs_source = 0;

	g_hash_table_iter_init(&iter, modem->services);

	while (g_hash_table_iter_next(&iter, &keyptr, &value)) {
		GIsiServiceMux *mux = value;

		if (mux->subscriptions > 0) {
			msg[3 + count] = mux->resource;
			count++;
		}
	}
	msg[2] = count;

	sendto(modem->ind_fd, msg, 3 + msg[2], MSG_NOSIGNAL, (void *)&commgr,
		sizeof(commgr));

	return FALSE;
}


static void modem_subs_update_when_idle(GIsiModem *modem)
{
	if (modem->subs_source > 0)
		return;

	modem->subs_source = g_idle_add(modem_subs_update, modem);
}

static uint8_t service_next_utid(GIsiServiceMux *mux)
{
	if (mux->last_utid == 0x00 || mux->last_utid == 0xFF)
		return 1;

	return mux->last_utid + 1;
}

static void service_subs_incr(GIsiServiceMux *mux)
{
	GIsiModem *modem = mux->modem;

	mux->subscriptions++;

	if (mux->subscriptions == 1)
		modem_subs_update_when_idle(modem);
}

static void service_subs_decr(GIsiServiceMux *mux)
{
	GIsiModem *modem = mux->modem;

	if (mux->subscriptions == 0)
		return;

	mux->subscriptions--;

	if (mux->subscriptions == 0)
		modem_subs_update_when_idle(modem);
}

static void service_regs_incr(GIsiServiceMux *mux)
{
	mux->registrations++;

	if (mux->registrations == 1)
		service_name_register(mux);
}

static void service_regs_decr(GIsiServiceMux *mux)
{
	if (mux->registrations == 0)
		return;

	mux->registrations--;

	if (mux->registrations == 0)
		service_name_deregister(mux);
}

static void pending_destroy(gpointer value, gpointer user)
{
	GIsiPending *op = value;

	if (!op)
		return;

	if (op->timeout > 0)
		g_source_remove(op->timeout);

	if (op->destroy)
		op->destroy(op->data);

	g_free(op);
}

static void service_finalize(gpointer value)
{
	GIsiServiceMux *mux = value;
	GIsiModem *modem = mux->modem;

	if (mux->subscriptions > 0)
		modem_subs_update_when_idle(modem);

	if (mux->registrations > 0)
		service_name_deregister(mux);

	g_slist_foreach(mux->pending, pending_destroy, NULL);
	g_slist_free(mux->pending);
	g_free(mux);
}

void g_isi_modem_destroy(GIsiModem *modem)
{
	if (!modem)
		return;

	g_hash_table_remove_all(modem->services);

	if (modem->subs_source > 0) {
		g_source_remove(modem->subs_source);
		modem_subs_update(modem);
	}

	g_hash_table_unref(modem->services);

	if (modem->ind_watch)
		g_source_remove(modem->ind_watch);

	if (modem->req_watch)
		g_source_remove(modem->req_watch);

	g_free(modem);
}

unsigned g_isi_modem_index(GIsiModem *modem)
{
	return modem ? modem->index : 0;
}

GIsiPending *g_isi_request_send(GIsiModem *modem, uint8_t resource,
					const void *__restrict buf, size_t len,
					unsigned timeout, GIsiNotifyFunc notify,
					void *data, GDestroyNotify destroy)
{
	struct sockaddr_pn dst = {
		.spn_family = AF_PHONET,
		.spn_resource = resource,
	};

	return g_isi_request_sendto(modem, &dst, buf, len, timeout, notify,
					data, destroy);
};

GIsiPending *g_isi_request_vsend(GIsiModem *modem, uint8_t resource,
					const struct iovec *__restrict iov,
					size_t iovlen, unsigned timeout,
					GIsiNotifyFunc notify, void *data,
					GDestroyNotify destroy)
{
	struct sockaddr_pn dst = {
		.spn_family = AF_PHONET,
		.spn_resource = resource,
	};

	return g_isi_request_vsendto(modem, &dst, iov, iovlen, timeout, notify,
					data, destroy);
}

GIsiPending *g_isi_request_sendto(GIsiModem *modem, struct sockaddr_pn *dst,
					const void *__restrict buf, size_t len,
					unsigned timeout, GIsiNotifyFunc notify,
					void *data, GDestroyNotify destroy)
{
	const struct iovec iov = {
		.iov_base = (void *)buf,
		.iov_len = len,
	};

	return g_isi_request_vsendto(modem, dst, &iov, 1, timeout, notify, data,
					destroy);
}

static void vdebug(struct sockaddr_pn *dst,
			const struct iovec *__restrict iov, size_t iovlen,
			size_t total_len, GIsiNotifyFunc debug, void *data)
{
	uint8_t buffer[total_len];
	uint8_t *ptr = buffer;
	GIsiMessage msg = {
		.addr = dst,
		.data = (const void *)buffer,
		.len = total_len,
	};
	size_t i;

	for (i = 0; i < iovlen; i++) {
		memcpy(ptr, iov[i].iov_base, iov[i].iov_len);
		ptr += iov[i].iov_len;
	}

	debug(&msg, data);
}

static gboolean resp_timeout(gpointer data)
{
	GIsiPending *resp = data;
	GIsiMessage msg = {
		.error = ETIMEDOUT,
		.private = resp,
	};

	pending_dispatch(resp, &msg);
	resp->notify = NULL;

	g_isi_pending_remove(resp);
	return FALSE;
}

GIsiPending *g_isi_request_vsendto(GIsiModem *modem, struct sockaddr_pn *dst,
					const struct iovec *__restrict iov,
					size_t iovlen, unsigned timeout,
					GIsiNotifyFunc notify, void *data,
					GDestroyNotify destroy)
{
	struct iovec _iov[1 + iovlen];
	struct msghdr msg = {
		.msg_name = (void *)dst,
		.msg_namelen = sizeof(struct sockaddr_pn),
		.msg_iov = _iov,
		.msg_iovlen = 1 + iovlen,
		.msg_control = NULL,
		.msg_controllen = 0,
		.msg_flags = 0,
	};
	ssize_t ret;
	size_t i, len;

	GIsiServiceMux *mux;
	GIsiPending *resp;

	if (!modem) {
		errno = EINVAL;
		return NULL;
	}

	mux = service_get(modem, dst->spn_resource);
	if (!mux) {
		errno = ENOMEM;
		return NULL;
	}

	resp = g_try_new0(GIsiPending, 1);
	if (!resp) {
		errno = ENOMEM;
		return NULL;
	}

	resp->type = GISI_MESSAGE_TYPE_RESP;
	resp->utid = service_next_utid(mux);
	resp->service = mux;
	resp->notify = notify;
	resp->destroy = destroy;
	resp->data = data;

	if (g_slist_find_custom(mux->pending, resp, utid_equal)) {
		/*
		 * FIXME: perhaps retry with randomized access after
		 * initial miss. Although if the rate at which
		 * requests are sent is so high that the unique
		 * transaction ID wraps, it's likely there is
		 * something wrong and we might as well fail here.
		 */
		errno = EBUSY;
		goto error;
	}

	_iov[0].iov_base = &resp->utid;
	_iov[0].iov_len = 1;

	for (i = 0, len = 1; i < iovlen; i++) {
		_iov[1 + i] = iov[i];
		len += iov[i].iov_len;
	}

	if (modem->debug)
		vdebug(dst, _iov, 1 + iovlen, len, modem->debug, modem->ddata);

	ret = sendmsg(modem->req_fd, &msg, MSG_NOSIGNAL);
	if (ret == -1)
		goto error;

	if (ret != (ssize_t)len) {
		errno = EMSGSIZE;
		goto error;
	}

	mux->pending = g_slist_prepend(mux->pending, resp);

	if (timeout)
		resp->timeout = g_timeout_add_seconds(timeout, resp_timeout, resp);

	mux->last_utid = resp->utid;
	return resp;

error:
	g_free(resp);
	return NULL;
}

uint8_t g_isi_request_utid(GIsiPending *resp)
{
	return resp ? resp->utid : 0;
}

GIsiPending *g_isi_pending_from_msg(const GIsiMessage *msg)
{
	return msg ? msg->private : NULL;
}

void g_isi_pending_remove(GIsiPending *op)
{
	if (!op)
		return;

	op->service->pending = g_slist_remove(op->service->pending, op);

	if (op->type == GISI_MESSAGE_TYPE_IND)
		service_subs_decr(op->service);

	if (op->type == GISI_MESSAGE_TYPE_REQ)
		service_regs_decr(op->service);

	if (op->type == GISI_MESSAGE_TYPE_RESP && op->notify) {
		GIsiMessage msg = {
			.error = ESHUTDOWN,
			.private = op,
		};
		op->notify(&msg, op->data);
		op->notify = NULL;
	}

	pending_destroy(op, NULL);
}

GIsiPending *g_isi_ntf_subscribe(GIsiModem *modem, uint8_t resource,
					uint8_t msgid, GIsiNotifyFunc notify,
					void *data, GDestroyNotify destroy)
{
	GIsiServiceMux *mux;
	GIsiPending *ntf;

	mux = service_get(modem, resource);
	if (!mux) {
		errno = ENOMEM;
		return NULL;
	}

	ntf = g_try_new0(GIsiPending, 1);
	if (!ntf) {
		errno = ENOMEM;
		return NULL;
	}

	ntf->type = GISI_MESSAGE_TYPE_NTF;
	ntf->service = mux;
	ntf->notify = notify;
	ntf->data = data;
	ntf->destroy = destroy;
	ntf->msgid = msgid;

	mux->pending = g_slist_append(mux->pending, ntf);

	return ntf;
}

GIsiPending *g_isi_service_bind(GIsiModem *modem, uint8_t resource,
				uint8_t msgid, GIsiNotifyFunc notify,
				void *data, GDestroyNotify destroy)
{
	GIsiServiceMux *mux;
	GIsiPending *srv;

	mux = service_get(modem, resource);
	if (!mux) {
		errno = ENOMEM;
		return NULL;
	}

	srv = g_try_new0(GIsiPending, 1);
	if (!srv) {
		errno = ENOMEM;
		return NULL;
	}

	srv->type = GISI_MESSAGE_TYPE_REQ;
	srv->service = mux;
	srv->notify = notify;
	srv->data = data;
	srv->destroy = destroy;
	srv->msgid = msgid;

	mux->pending = g_slist_append(mux->pending, srv);

	service_regs_incr(mux);

	return srv;
}

GIsiPending *g_isi_ind_subscribe(GIsiModem *modem, uint8_t resource,
					uint8_t msgid, GIsiNotifyFunc notify,
					void *data, GDestroyNotify destroy)
{
	GIsiServiceMux *mux;
	GIsiPending *ind;

	mux = service_get(modem, resource);
	if (!mux) {
		errno = ENOMEM;
		return NULL;
	}

	ind = g_try_new0(GIsiPending, 1);
	if (!ind) {
		errno = ENOMEM;
		return NULL;
	}

	ind->type = GISI_MESSAGE_TYPE_IND;
	ind->service = mux;
	ind->notify = notify;
	ind->data = data;
	ind->destroy = destroy;
	ind->msgid = msgid;

	mux->pending = g_slist_append(mux->pending, ind);

	service_subs_incr(mux);

	return ind;
}

int g_isi_response_send(GIsiModem *modem, const GIsiMessage *req,
				const void *__restrict buf, size_t len)
{
	const struct iovec iov = {
		.iov_base = (void *)buf,
		.iov_len = len,
	};

	return g_isi_response_vsend(modem, req, &iov, 1);
}

int g_isi_response_vsend(GIsiModem *modem, const GIsiMessage *req,
				const struct iovec *__restrict iov,
				size_t iovlen)
{
	struct iovec _iov[1 + iovlen];
	uint8_t utid;
	size_t i;

	utid = g_isi_msg_utid(req);

	_iov[0].iov_base = &utid;
	_iov[0].iov_len = 1;

	for (i = 0; i < iovlen; i++)
		_iov[1 + i] = iov[i];

	return g_isi_modem_vsendto(modem, req->addr, _iov, 1 + iovlen);
}

int g_isi_modem_sendto(GIsiModem *modem, struct sockaddr_pn *dst,
			const void *__restrict buf, size_t len)
{
	const struct iovec iov = {
		.iov_base = (void *)buf,
		.iov_len = len,
	};

	return g_isi_modem_vsendto(modem, dst, &iov, 1);
}

int g_isi_modem_vsendto(GIsiModem *modem, struct sockaddr_pn *dst,
				const struct iovec *__restrict iov,
				size_t iovlen)
{
	struct msghdr msg = {
		.msg_name = (void *)dst,
		.msg_namelen = sizeof(struct sockaddr_pn),
		.msg_iov = (struct iovec *)iov,
		.msg_iovlen = iovlen,
		.msg_control = NULL,
		.msg_controllen = 0,
		.msg_flags = 0,
	};
	ssize_t ret;
	size_t i, len;
	GIsiServiceMux *mux;

	if (!modem)
		return -EINVAL;

	mux = service_get(modem, dst->spn_resource);
	if (!mux)
		return -ENOMEM;

	for (i = 0, len = 0; i < iovlen; i++)
		len += iov[i].iov_len;

	if (modem->debug)
		vdebug(dst, iov, iovlen, len, modem->debug, modem->ddata);

	ret = sendmsg(modem->req_fd, &msg, MSG_NOSIGNAL);
	if (ret == -1)
		return -errno;

	if (ret != (ssize_t)len)
		return -EMSGSIZE;

	return 0;
}

void g_isi_modem_set_debug(GIsiModem *modem, GIsiNotifyFunc debug, void *data)
{
	if (!modem)
		return;

	modem->debug = debug;
	modem->ddata = data;
}

static int version_get_send(GIsiModem *modem, GIsiPending *ping)
{
	GIsiServiceMux *mux = ping->service;
	const struct sockaddr_pn dst = {
		.spn_family = AF_PHONET,
		.spn_resource = mux->resource,
	};
	uint8_t msg[] = {
		ping->utid,	/* UTID */
		COMMON_MESSAGE,
		COMM_ISI_VERSION_GET_REQ,
		0,		/* Filler */
	};
	ssize_t ret;

	if (g_slist_find_custom(mux->pending, ping, utid_equal))
		return -EBUSY;

	ret = sendto(modem->req_fd, msg, sizeof(msg), MSG_NOSIGNAL,
			(void *)&dst, sizeof(dst));

	if (ret == -1)
		return -errno;

	if (ret != (ssize_t)sizeof(msg))
		return -EMSGSIZE;

	mux->last_utid = ping->utid;
	mux->version_pending = TRUE;
	return 0;
}

static gboolean reachable_notify(gpointer data)
{
	GIsiPending *pong = data;
	GIsiServiceMux *mux = pong->service;

	struct sockaddr_pn addr = {
		.spn_resource = mux->resource,
	};
	GIsiMessage msg = {
		.version = &mux->version,
		.private = pong,
		.addr = &addr,
	};

	pending_dispatch(pong, &msg);
	pong->notify = NULL;

	g_isi_pending_remove(pong);
	return FALSE;
}

GIsiPending *g_isi_resource_ping(GIsiModem *modem, uint8_t resource,
					GIsiNotifyFunc notify, void *data,
					GDestroyNotify destroy)
{
	GIsiServiceMux *mux;
	GIsiPending *ping;
	int ret;

	mux = service_get(modem, resource);
	if (!mux) {
		errno = ENOMEM;
		return NULL;
	}

	ping = g_try_new0(GIsiPending, 1);
	if (!ping) {
		errno = ENOMEM;
		return NULL;
	}

	ping->type = GISI_MESSAGE_TYPE_COMMON;
	ping->utid = service_next_utid(mux);
	ping->service = mux;
	ping->notify = notify;
	ping->data = data;
	ping->destroy = destroy;
	ping->msgid = COMM_ISI_VERSION_GET_REQ;

	if (mux->reachable) {
		g_idle_add(reachable_notify, ping);
		return ping;
	}

	if (!mux->version_pending) {
		ret = version_get_send(modem, ping);
		if (ret < 0) {
			g_free(ping);
			errno = ret;
			return NULL;
		}
		mux->last_utid = ping->utid;
	}

	ping->timeout = g_timeout_add_seconds(COMMON_TIMEOUT, resp_timeout,
						ping);
	mux->pending = g_slist_prepend(mux->pending, ping);
	mux->version_pending = TRUE;

	return ping;
}
