/*
   Copyright (C) 2010-2012 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>
   Richard Hughes <rhughes@redhat.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include "config.h"

#ifdef USE_USBREDIR
#include <glib/gi18n-lib.h>
#ifdef USE_LZ4
#include <lz4.h>
#endif
#ifdef USE_POLKIT
#include "usb-acl-helper.h"
#endif
#include "channel-usbredir-priv.h"
#include "usb-device-manager-priv.h"
#include "usbutil.h"

#include "common/log.h"
#include "spice-client.h"
#include "spice-common.h"

#include "spice-channel-priv.h"

/**
 * SECTION:channel-usbredir
 * @short_description: usb redirection
 * @title: USB Redirection Channel
 * @section_id:
 * @stability: Stable
 * @include: spice-client.h
 *
 * The Spice protocol defines a set of messages to redirect USB devices
 * from the Spice client to the VM. This channel handles these messages.
 */

#define COMPRESS_THRESHOLD 1000
enum SpiceUsbredirChannelState {
    STATE_DISCONNECTED,
#ifdef USE_POLKIT
    STATE_WAITING_FOR_ACL_HELPER,
#endif
    STATE_CONNECTED,
    STATE_DISCONNECTING,
};

struct _SpiceUsbredirChannelPrivate {
    SpiceUsbDevice *device;
    SpiceUsbBackend *context;
    SpiceUsbBackendChannel *host;
    enum SpiceUsbredirChannelState state;
#ifdef USE_POLKIT
    GTask *task;
    SpiceUsbAclHelper *acl_helper;
#endif
    GMutex device_connect_mutex;
};

static void channel_set_handlers(SpiceChannelClass *klass);
static void spice_usbredir_channel_up(SpiceChannel *channel);
static void spice_usbredir_channel_dispose(GObject *obj);
static void spice_usbredir_channel_finalize(GObject *obj);
static void usbredir_handle_msg(SpiceChannel *channel, SpiceMsgIn *in);


G_DEFINE_TYPE_WITH_PRIVATE(SpiceUsbredirChannel, spice_usbredir_channel, SPICE_TYPE_CHANNEL)


/* ------------------------------------------------------------------ */

static void spice_usbredir_channel_init(SpiceUsbredirChannel *channel)
{
    channel->priv = spice_usbredir_channel_get_instance_private(channel);
    g_mutex_init(&channel->priv->device_connect_mutex);
}

static void _channel_reset_finish(SpiceUsbredirChannel *channel, gboolean migrating)
{
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    spice_usbredir_channel_lock(channel);

    spice_usb_backend_channel_delete(priv->host);
    priv->host = NULL;

    /* Call set_context to re-create the host */
    spice_usbredir_channel_set_context(channel, priv->context);

    spice_usbredir_channel_unlock(channel);

    SPICE_CHANNEL_CLASS(spice_usbredir_channel_parent_class)->channel_reset(SPICE_CHANNEL(channel), migrating);
}

static void _channel_reset_cb(GObject *gobject,
                              GAsyncResult *result,
                              gpointer user_data)
{
    SpiceChannel *spice_channel =  SPICE_CHANNEL(gobject);
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(spice_channel);
    gboolean migrating = GPOINTER_TO_UINT(user_data);
    GError *err = NULL;

    _channel_reset_finish(channel, migrating);

    spice_usbredir_channel_disconnect_device_finish(channel, result, &err);
}

static void spice_usbredir_channel_reset(SpiceChannel *c, gboolean migrating)
{
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(c);
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    /* Host isn't running, just reset */
    if (!priv->host) {
        SPICE_CHANNEL_CLASS(spice_usbredir_channel_parent_class)->channel_reset(c, migrating);
        return;
    }

    /* Host is running, so we might need to disconnect the usb devices async.
     * This should not block channel_reset() otherwise we might run in reconnection
     * problems such as https://bugzilla.redhat.com/show_bug.cgi?id=1625550
     * No operation from here on should rely on SpiceChannel as its coroutine
     * might be terminated. */
    
    if (priv->state == STATE_CONNECTED) {
        /* FIXME: We should chain-up parent's channel-reset here */
        spice_usbredir_channel_disconnect_device_async(channel, NULL,
            _channel_reset_cb, GUINT_TO_POINTER(migrating));
        return;
    }

    _channel_reset_finish(channel, migrating);
}

static void spice_usbredir_channel_class_init(SpiceUsbredirChannelClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    SpiceChannelClass *channel_class = SPICE_CHANNEL_CLASS(klass);

    gobject_class->dispose       = spice_usbredir_channel_dispose;
    gobject_class->finalize      = spice_usbredir_channel_finalize;
    channel_class->channel_up    = spice_usbredir_channel_up;
    channel_class->channel_reset = spice_usbredir_channel_reset;

    channel_set_handlers(SPICE_CHANNEL_CLASS(klass));
}

static void spice_usbredir_channel_dispose(GObject *obj)
{
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(obj);

    spice_usbredir_channel_disconnect_device(channel);

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_usbredir_channel_parent_class)->dispose)
        G_OBJECT_CLASS(spice_usbredir_channel_parent_class)->dispose(obj);
}

/*
 * Note we don't unref our device / acl_helper / result references in our
 * finalize. The reason for this is that depending on our state at dispose
 * time they are either:
 * 1) Already unreferenced
 * 2) Will be unreferenced by the disconnect_device call from dispose
 * 3) Will be unreferenced by spice_usbredir_channel_open_acl_cb
 *
 * Now the last one may seem like an issue, since what will happen if
 * spice_usbredir_channel_open_acl_cb will run after finalization?
 *
 * This will never happens since the GTask created before we
 * get into the STATE_WAITING_FOR_ACL_HELPER takes a reference to its
 * source object, which is our SpiceUsbredirChannel object, so
 * the finalize won't hapen until spice_usbredir_channel_open_acl_cb runs,
 * and unrefs priv->result which will in turn unref ourselve once the
 * complete_in_idle call it does has completed. And once
 * spice_usbredir_channel_open_acl_cb has run, all references we hold have
 * been released even in the 3th scenario.
 */
static void spice_usbredir_channel_finalize(GObject *obj)
{
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(obj);

    if (channel->priv->host)
        spice_usb_backend_channel_delete(channel->priv->host);
    g_mutex_clear(&channel->priv->device_connect_mutex);

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_usbredir_channel_parent_class)->finalize)
        G_OBJECT_CLASS(spice_usbredir_channel_parent_class)->finalize(obj);
}

static void channel_set_handlers(SpiceChannelClass *klass)
{
    static const spice_msg_handler handlers[] = {
        [ SPICE_MSG_SPICEVMC_DATA ] = usbredir_handle_msg,
        [ SPICE_MSG_SPICEVMC_COMPRESSED_DATA ] = usbredir_handle_msg,
    };

    spice_channel_set_handlers(klass, handlers, G_N_ELEMENTS(handlers));
}

/* ------------------------------------------------------------------ */
/* private api                                                        */

G_GNUC_INTERNAL
void spice_usbredir_channel_set_context(SpiceUsbredirChannel *channel,
                                        SpiceUsbBackend      *context)
{
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    g_return_if_fail(priv->host == NULL);

    priv->context = context;
    priv->host = spice_usb_backend_channel_new(context, channel);
    if (!priv->host)
        g_error("Out of memory initializing redirection support");

#ifdef USE_LZ4
    spice_channel_set_capability(channel, SPICE_SPICEVMC_CAP_DATA_COMPRESS_LZ4);
#endif
}

static gboolean spice_usbredir_channel_open_device(
    SpiceUsbredirChannel *channel, GError **err)
{
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    g_return_val_if_fail(priv->state == STATE_DISCONNECTED
#ifdef USE_POLKIT
                         || priv->state == STATE_WAITING_FOR_ACL_HELPER
#endif
                         , FALSE);

    if (!spice_usb_backend_channel_attach(priv->host, priv->device, err)) {
        if (*err == NULL) {
            g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                "Error attaching device: (no error information)");
        }
        return FALSE;
    }

    priv->state = STATE_CONNECTED;

    return TRUE;
}

#ifdef USE_POLKIT
static void spice_usbredir_channel_open_acl_cb(
    GObject *gobject, GAsyncResult *acl_res, gpointer user_data)
{
    SpiceUsbAclHelper *acl_helper = SPICE_USB_ACL_HELPER(gobject);
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(user_data);
    SpiceUsbredirChannelPrivate *priv = channel->priv;
    GError *err = NULL;

    g_return_if_fail(acl_helper == priv->acl_helper);
    g_return_if_fail(priv->state == STATE_WAITING_FOR_ACL_HELPER ||
                     priv->state == STATE_DISCONNECTING);

    spice_usb_acl_helper_open_acl_finish(acl_helper, acl_res, &err);
    if (!err && priv->state == STATE_DISCONNECTING) {
        err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                  "USB redirection channel connect cancelled");
    }
    if (!err) {
        spice_usbredir_channel_open_device(channel, &err);
    }
    if (err) {
        g_clear_pointer(&priv->device, spice_usb_backend_device_unref);
        priv->state  = STATE_DISCONNECTED;
        g_task_return_error(priv->task, err);
    } else {
        g_task_return_boolean(priv->task, TRUE);
    }

    g_clear_object(&priv->acl_helper);
    g_object_set(spice_channel_get_session(SPICE_CHANNEL(channel)),
                 "inhibit-keyboard-grab", FALSE, NULL);

    g_clear_object(&priv->task);
}
#endif

static void
_open_device_async_cb(GTask *task,
                      gpointer object,
                      gpointer task_data,
                      GCancellable *cancellable)
{
    GError *err = NULL;
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(object);
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    spice_usbredir_channel_lock(channel);

    if (!spice_usbredir_channel_open_device(channel, &err)) {
        g_clear_pointer(&priv->device, spice_usb_backend_device_unref);
    }

    spice_usbredir_channel_unlock(channel);

    if (err) {
        g_task_return_error(task, err);
    } else {
        g_task_return_boolean(task, TRUE);
    }
}

G_GNUC_INTERNAL
void spice_usbredir_channel_connect_device_async(SpiceUsbredirChannel *channel,
                                                 SpiceUsbDevice *device,
                                                 GCancellable *cancellable,
                                                 GAsyncReadyCallback callback,
                                                 gpointer user_data)
{
    SpiceUsbredirChannelPrivate *priv = channel->priv;
#ifdef USE_POLKIT
    const UsbDeviceInformation *info = spice_usb_backend_device_get_info(device);
#endif
    GTask *task;

    g_return_if_fail(SPICE_IS_USBREDIR_CHANNEL(channel));
    g_return_if_fail(device != NULL);

    CHANNEL_DEBUG(channel, "connecting device %04x:%04x (%p) to channel %p",
                  spice_usb_device_get_vid(device),
                  spice_usb_device_get_pid(device),
                  device, channel);

    task = g_task_new(channel, cancellable, callback, user_data);

    if (!priv->host) {
        g_task_return_new_error(task,
                            SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            "Error libusb context not set");
        goto done;
    }

    if (priv->state != STATE_DISCONNECTED) {
        g_task_return_new_error(task,
                            SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            "Error channel is busy");
        goto done;
    }

    priv->device = spice_usb_backend_device_ref(device);
#ifdef USE_POLKIT
    if (info->bus != BUS_NUMBER_FOR_EMULATED_USB) {
        priv->task = task;
        priv->state  = STATE_WAITING_FOR_ACL_HELPER;
        priv->acl_helper = spice_usb_acl_helper_new();
        g_object_set(spice_channel_get_session(SPICE_CHANNEL(channel)),
                    "inhibit-keyboard-grab", TRUE, NULL);
        spice_usb_acl_helper_open_acl_async(priv->acl_helper,
                                            info->bus,
                                            info->address,
                                            cancellable,
                                            spice_usbredir_channel_open_acl_cb,
                                            channel);
        return;
    }
#endif
    g_task_run_in_thread(task, _open_device_async_cb);

done:
    g_object_unref(task);
}

G_GNUC_INTERNAL
gboolean spice_usbredir_channel_connect_device_finish(
                                               SpiceUsbredirChannel *channel,
                                               GAsyncResult         *res,
                                               GError              **err)
{
    GTask *task = G_TASK(res);

    g_return_val_if_fail(g_task_is_valid(task, channel), FALSE);

    return g_task_propagate_boolean(task, err);
}

G_GNUC_INTERNAL
void spice_usbredir_channel_disconnect_device(SpiceUsbredirChannel *channel)
{
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    CHANNEL_DEBUG(channel, "disconnecting device from usb channel %p", channel);

    spice_usbredir_channel_lock(channel);

    switch (priv->state) {
    case STATE_DISCONNECTED:
    case STATE_DISCONNECTING:
        break;
#ifdef USE_POLKIT
    case STATE_WAITING_FOR_ACL_HELPER:
        priv->state = STATE_DISCONNECTING;
        /* We're still waiting for the acl helper -> cancel it */
        spice_usb_acl_helper_cancel(priv->acl_helper);
        break;
#endif
    case STATE_CONNECTED:
        /* This also closes the libusb handle we passed from open_device */
        spice_usb_backend_channel_detach(priv->host);
        g_clear_pointer(&priv->device, spice_usb_backend_device_unref);
        priv->state  = STATE_DISCONNECTED;
        break;
    }

    spice_usbredir_channel_unlock(channel);
}

static void
_disconnect_device_thread(GTask *task,
                          gpointer object,
                          gpointer task_data,
                          GCancellable *cancellable)
{
    spice_usbredir_channel_disconnect_device(SPICE_USBREDIR_CHANNEL(object));
    g_task_return_boolean(task, TRUE);
}

G_GNUC_INTERNAL
gboolean spice_usbredir_channel_disconnect_device_finish(
                                               SpiceUsbredirChannel *channel,
                                               GAsyncResult         *res,
                                               GError              **err)
{
    return g_task_propagate_boolean(G_TASK(res), err);
}

G_GNUC_INTERNAL
void spice_usbredir_channel_disconnect_device_async(SpiceUsbredirChannel *channel,
                                                    GCancellable *cancellable,
                                                    GAsyncReadyCallback callback,
                                                    gpointer user_data)
{
    GTask* task = g_task_new(channel, cancellable, callback, user_data);

    g_return_if_fail(channel != NULL);
    g_task_run_in_thread(task, _disconnect_device_thread);
    g_object_unref(task);
}

#ifdef USE_LZ4
static SpiceUsbDevice *
spice_usbredir_channel_get_spice_usb_device(SpiceUsbredirChannel *channel)
{
    return channel->priv->device;
}
#endif

G_GNUC_INTERNAL
SpiceUsbDevice *spice_usbredir_channel_get_device(SpiceUsbredirChannel *channel)
{
    return channel->priv->device;
}

G_GNUC_INTERNAL
void spice_usbredir_channel_get_guest_filter(
                          SpiceUsbredirChannel               *channel,
                          const struct usbredirfilter_rule  **rules_ret,
                          int                                *rules_count_ret)
{
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    g_return_if_fail(priv->host != NULL);

    spice_usb_backend_channel_get_guest_filter(priv->host, rules_ret, rules_count_ret);
}

/* ------------------------------------------------------------------ */
/* callbacks (any context)                                            */

static void usbredir_free_write_cb_data(uint8_t *data, void *user_data)
{
    SpiceUsbredirChannel *channel = user_data;
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    spice_usb_backend_return_write_data(priv->host, data);
}

#ifdef USE_LZ4
static int try_write_compress_LZ4(SpiceUsbredirChannel *channel, uint8_t *data, int count)
{
    SpiceChannelPrivate *c;
    SpiceMsgOut *msg_out_compressed;
    int bound, compressed_data_count;
    uint8_t *compressed_buf;
    SpiceMsgCompressedData compressed_data_msg = {
        .type = SPICE_DATA_COMPRESSION_TYPE_LZ4,
        .uncompressed_size = count
    };

    c = SPICE_CHANNEL(channel)->priv;
    if (g_socket_get_family(c->sock) == G_SOCKET_FAMILY_UNIX) {
        /* AF_LOCAL socket - data will not be compressed */
        return FALSE;
    }
    if (count <= COMPRESS_THRESHOLD) {
        /* Not enough data to compress */
        return FALSE;
    }
    if (!spice_channel_test_capability(SPICE_CHANNEL(channel),
                                       SPICE_SPICEVMC_CAP_DATA_COMPRESS_LZ4)) {
        /* No server compression capability - data will not be compressed */
        return FALSE;
    }
    if (spice_usb_device_is_isochronous(spice_usbredir_channel_get_spice_usb_device(channel))) {
        /* Don't compress - one of the device endpoints is isochronous */
        return FALSE;
    }
    bound = LZ4_compressBound(count);
    if (bound == 0) {
        /* Invalid bound - data will not be compressed */
        return FALSE;
    }

    compressed_buf = g_malloc(bound);
    compressed_data_count = LZ4_compress_default((char*)data,
                                                 (char*)compressed_buf,
                                                 count,
                                                 bound);
    if (compressed_data_count > 0 && compressed_data_count < count) {
        compressed_data_msg.compressed_data = compressed_buf;
        msg_out_compressed = spice_msg_out_new(SPICE_CHANNEL(channel),
                                               SPICE_MSGC_SPICEVMC_COMPRESSED_DATA);
        msg_out_compressed->marshallers->msg_SpiceMsgCompressedData(msg_out_compressed->marshaller,
                                                                    &compressed_data_msg);
        spice_marshaller_add_by_ref_full(msg_out_compressed->marshaller,
                                         compressed_data_msg.compressed_data,
                                         compressed_data_count,
                                         (spice_marshaller_item_free_func)g_free,
                                         NULL);
        spice_msg_out_send(msg_out_compressed);
        return TRUE;
    }

    /* if not - free & fallback to sending the message uncompressed */
    g_free(compressed_buf);
    return FALSE;
}
#endif

G_GNUC_INTERNAL
int spice_usbredir_write(SpiceUsbredirChannel *channel, uint8_t *data, int count)
{
    SpiceMsgOut *msg_out;

#ifdef USE_LZ4
    if (try_write_compress_LZ4(channel, data, count)) {
        spice_usb_backend_return_write_data(channel->priv->host, data);
        return count;
    }
#endif
    msg_out = spice_msg_out_new(SPICE_CHANNEL(channel),
                                SPICE_MSGC_SPICEVMC_DATA);
    spice_marshaller_add_by_ref_full(msg_out->marshaller, data, count,
                                     usbredir_free_write_cb_data, channel);
    spice_msg_out_send(msg_out);

    return count;
}

G_GNUC_INTERNAL
void spice_usbredir_channel_lock(SpiceUsbredirChannel *channel)
{
    g_mutex_lock(&channel->priv->device_connect_mutex);
}

G_GNUC_INTERNAL
void spice_usbredir_channel_unlock(SpiceUsbredirChannel *channel)
{
    g_mutex_unlock(&channel->priv->device_connect_mutex);
}

/* --------------------------------------------------------------------- */

typedef struct device_error_data {
    SpiceUsbredirChannel *channel;
    SpiceUsbDevice *device;
    GError *error;
    struct coroutine *caller;
} device_error_data;

/* main context */
static gboolean device_error(gpointer user_data)
{
    device_error_data *data = user_data;
    SpiceUsbredirChannel *channel = data->channel;
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    /* Check that the device has not changed before we manage to run */
    if (data->device == priv->device) {
        SpiceUsbDeviceManager *manager =
            spice_usb_device_manager_get(spice_channel_get_session(SPICE_CHANNEL(channel)), NULL);
        spice_usbredir_channel_disconnect_device(channel);
        spice_usb_device_manager_device_error(manager, data->device, data->error);
    }

    coroutine_yieldto(data->caller, NULL);
    return FALSE;
}

/* --------------------------------------------------------------------- */
/* coroutine context                                                     */
static void spice_usbredir_channel_up(SpiceChannel *c)
{
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(c);
    SpiceUsbredirChannelPrivate *priv = channel->priv;

    g_return_if_fail(priv->host != NULL);
    /* Flush any pending writes */
    spice_usb_backend_channel_flush_writes(priv->host);
}

static int try_handle_compressed_msg(SpiceMsgCompressedData *compressed_data_msg,
                                     uint8_t **buf,
                                     int *size) {
    int decompressed_size = 0;
    char *decompressed = NULL;

    if (compressed_data_msg->uncompressed_size == 0) {
        spice_warning("Invalid uncompressed_size");
        return FALSE;
    }

    switch (compressed_data_msg->type) {
#ifdef USE_LZ4
    case SPICE_DATA_COMPRESSION_TYPE_LZ4:
        decompressed = g_malloc(compressed_data_msg->uncompressed_size);
        decompressed_size = LZ4_decompress_safe ((char*)compressed_data_msg->compressed_data,
                                                 decompressed,
                                                 compressed_data_msg->compressed_size,
                                                 compressed_data_msg->uncompressed_size);
        break;
#endif
    default:
        spice_warning("Unknown Compression Type");
        return FALSE;
    }
    if (decompressed_size != compressed_data_msg->uncompressed_size) {
        spice_warning("Decompress Error decompressed_size=%d expected=%u",
                      decompressed_size, compressed_data_msg->uncompressed_size);
        g_free(decompressed);
        return FALSE;
    }

    *size = decompressed_size;
    *buf = (uint8_t*)decompressed;
    return TRUE;

}

static void usbredir_handle_msg(SpiceChannel *c, SpiceMsgIn *in)
{
    SpiceUsbredirChannel *channel = SPICE_USBREDIR_CHANNEL(c);
    SpiceUsbredirChannelPrivate *priv = channel->priv;
    int r = 0, size;
    uint8_t *buf;

    g_return_if_fail(priv->host != NULL);

    if (spice_msg_in_type(in) == SPICE_MSG_SPICEVMC_COMPRESSED_DATA) {
        SpiceMsgCompressedData *compressed_data_msg = spice_msg_in_parsed(in);
        if (try_handle_compressed_msg(compressed_data_msg, &buf, &size)) {
            /* uncompressed ok*/
        } else {
            buf = NULL;
            r = USB_REDIR_ERROR_READ_PARSE;
        }
    } else { /* Regular SPICE_MSG_SPICEVMC_DATA msg */
        buf = spice_msg_in_raw(in, &size);
    }

    spice_usbredir_channel_lock(channel);
    if (r == 0)
        r = spice_usb_backend_read_guest_data(priv->host, buf, size);
    if (r != 0 && priv->device != NULL) {
        SpiceUsbDevice *device = priv->device;
        device_error_data err_data;
        gchar *desc;
        GError *err;

        desc = spice_usb_device_get_description(device, NULL);
        err = spice_usb_backend_get_error_details(r, desc);
        g_free(desc);

        CHANNEL_DEBUG(c, "%s", err->message);

        err_data.channel = channel;
        err_data.caller = coroutine_self();
        err_data.device = spice_usb_backend_device_ref(device);
        err_data.error = err;
        spice_usbredir_channel_unlock(channel);
        g_spice_idle_add(device_error, &err_data);
        coroutine_yield(NULL);

        spice_usb_backend_device_unref(err_data.device);

        g_error_free(err);
    } else {
        spice_usbredir_channel_unlock(channel);
    }
    if (spice_msg_in_type(in) == SPICE_MSG_SPICEVMC_COMPRESSED_DATA) {
        g_free(buf);
    }
}

#else
#include "spice-client.h"

G_DEFINE_TYPE(SpiceUsbredirChannel, spice_usbredir_channel, SPICE_TYPE_CHANNEL)

static void spice_usbredir_channel_init(SpiceUsbredirChannel *channel)
{
}

static void spice_usbredir_channel_class_init(SpiceUsbredirChannelClass *klass)
{
}

#endif /* USE_USBREDIR */
