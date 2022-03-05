/*
   Copyright (C) 2010-2011 Red Hat, Inc.

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

#include <glib.h>
#include <gdk/gdk.h>

#ifdef HAVE_X11_XKBLIB_H
#include <X11/XKBlib.h>
#endif
#ifdef GDK_WINDOWING_X11
#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#endif
#ifdef G_OS_WIN32
#include <windows.h>
#include <gdk/gdkwin32.h>
#ifndef MAPVK_VK_TO_VSC /* may be undefined in older mingw-headers */
#define MAPVK_VK_TO_VSC 0
#endif
#endif

#ifdef HAVE_PHODAV_VIRTUAL
#include <libphodav/phodav.h>
#endif

#include <gtk/gtk.h>
#include <spice/vd_agent.h>
#include "desktop-integration.h"
#include "spice-common.h"
#include "spice-gtk-session.h"
#include "spice-gtk-session-priv.h"
#include "spice-session-priv.h"
#include "spice-util-priv.h"
#include "spice-channel-priv.h"

#define CLIPBOARD_LAST (VD_AGENT_CLIPBOARD_SELECTION_SECONDARY + 1)

struct _SpiceGtkSessionPrivate {
    SpiceSession            *session;
    /* Clipboard related */
    gboolean                auto_clipboard_enable;
    SpiceMainChannel        *main;
    GtkClipboard            *clipboard;
    GtkClipboard            *clipboard_primary;
    GtkTargetEntry          *clip_targets[CLIPBOARD_LAST];
    guint                   nclip_targets[CLIPBOARD_LAST];
    GdkAtom                 *atoms[CLIPBOARD_LAST];
    guint                   n_atoms[CLIPBOARD_LAST];
    gboolean                clip_hasdata[CLIPBOARD_LAST];
    gboolean                clip_grabbed[CLIPBOARD_LAST];
    gboolean                clipboard_by_guest[CLIPBOARD_LAST];
    guint                   clipboard_release_delay[CLIPBOARD_LAST];
    /* TODO: maybe add a way of restoring this? */
    GHashTable              *cb_shared_files;
    /* auto-usbredir related */
    gboolean                auto_usbredir_enable;
    int                     auto_usbredir_reqs;
    gboolean                pointer_grabbed;
    gboolean                keyboard_has_focus;
    gboolean                mouse_has_pointer;
    gboolean                sync_modifiers;
};

/**
 * SECTION:spice-gtk-session
 * @short_description: handles GTK connection details
 * @title: Spice GTK Session
 * @section_id:
 * @see_also: #SpiceSession, and the GTK widget #SpiceDisplay
 * @stability: Stable
 * @include: spice-client-gtk.h
 *
 * The #SpiceGtkSession class is the spice-client-gtk counter part of
 * #SpiceSession. It contains functionality which should be handled per
 * session rather then per #SpiceDisplay (one session can have multiple
 * displays), but which cannot live in #SpiceSession as it depends on
 * GTK. For example the clipboard functionality.
 *
 * There should always be a 1:1 relation between #SpiceGtkSession objects
 * and #SpiceSession objects. Therefor there is no spice_gtk_session_new,
 * instead there is spice_gtk_session_get() which ensures this 1:1 relation.
 *
 * Client and guest clipboards will be shared automatically if
 * #SpiceGtkSession:auto-clipboard is set to #TRUE. Alternatively, you
 * can send / receive clipboard data from client to guest with
 * spice_gtk_session_copy_to_guest() / spice_gtk_session_paste_from_guest().
 */

/* ------------------------------------------------------------------ */
/* Prototypes for private functions */
static void clipboard_release(SpiceGtkSession *self, guint selection);
static void clipboard_owner_change(GtkClipboard *clipboard,
                                   GdkEventOwnerChange *event,
                                   gpointer user_data);
static void channel_new(SpiceSession *session, SpiceChannel *channel,
                        gpointer user_data);
static void channel_destroy(SpiceSession *session, SpiceChannel *channel,
                            gpointer user_data);
static gboolean read_only(SpiceGtkSession *self);

G_DEFINE_TYPE_WITH_PRIVATE(SpiceGtkSession, spice_gtk_session, G_TYPE_OBJECT)

/* Properties */
enum {
    PROP_0,
    PROP_SESSION,
    PROP_AUTO_CLIPBOARD,
    PROP_AUTO_USBREDIR,
    PROP_POINTER_GRABBED,
    PROP_SYNC_MODIFIERS,
};

static guint32 get_keyboard_lock_modifiers(void)
{
    guint32 modifiers = 0;
/* Ignore GLib's too-new warnings */
    GdkKeymap *keyboard = gdk_keymap_get_for_display(gdk_display_get_default());

    if (gdk_keymap_get_caps_lock_state(keyboard)) {
        modifiers |= SPICE_INPUTS_CAPS_LOCK;
    }

    if (gdk_keymap_get_num_lock_state(keyboard)) {
        modifiers |= SPICE_INPUTS_NUM_LOCK;
    }

    if (gdk_keymap_get_scroll_lock_state(keyboard)) {
        modifiers |= SPICE_INPUTS_SCROLL_LOCK;
    }
    return modifiers;
}

static void spice_gtk_session_sync_keyboard_modifiers_for_channel(SpiceGtkSession *self,
                                                                  SpiceInputsChannel* inputs,
                                                                  gboolean force)
{
    guint32 guest_modifiers = 0, client_modifiers = 0;

    g_return_if_fail(SPICE_IS_INPUTS_CHANNEL(inputs));

    if (SPICE_IS_GTK_SESSION(self) && !self->priv->sync_modifiers) {
        SPICE_DEBUG("Syncing modifiers is disabled");
        return;
    }

    g_object_get(inputs, "key-modifiers", &guest_modifiers, NULL);
    client_modifiers = get_keyboard_lock_modifiers();

    if (force || client_modifiers != guest_modifiers) {
        CHANNEL_DEBUG(inputs, "client_modifiers:0x%x, guest_modifiers:0x%x",
                      client_modifiers, guest_modifiers);
        spice_inputs_channel_set_key_locks(inputs, client_modifiers);
    }
}

static void keymap_modifiers_changed(GdkKeymap *keymap, gpointer data)
{
    SpiceGtkSession *self = data;

    /* set_key_locks() is inherently racy, but no need to sync modifiers
     * if we have focus as the regular keypress/keyrelease will have set
     * the expected modifiers state in the guest.
     */
    if (self->priv->keyboard_has_focus) {
        return;
    }

    spice_gtk_session_sync_keyboard_modifiers(self);
}

static void guest_modifiers_changed(SpiceInputsChannel *inputs, gpointer data)
{
    SpiceGtkSession *self = data;

    spice_gtk_session_sync_keyboard_modifiers_for_channel(self, inputs, FALSE);
}

static void spice_gtk_session_init(SpiceGtkSession *self)
{
    SpiceGtkSessionPrivate *s;
    GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());

    s = self->priv = spice_gtk_session_get_instance_private(self);

    s->cb_shared_files =
        g_hash_table_new_full(g_file_hash,
                              (GEqualFunc)g_file_equal,
                              g_object_unref, /* unref GFile */
                              g_free /* free gchar * */
                             );
    s->clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    g_signal_connect(G_OBJECT(s->clipboard), "owner-change",
                     G_CALLBACK(clipboard_owner_change), self);
    s->clipboard_primary = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
    g_signal_connect(G_OBJECT(s->clipboard_primary), "owner-change",
                     G_CALLBACK(clipboard_owner_change), self);
    spice_g_signal_connect_object(keymap, "state-changed",
                                  G_CALLBACK(keymap_modifiers_changed), self, 0);
}

static void
spice_gtk_session_constructed(GObject *gobject)
{
    SpiceGtkSession *self;
    SpiceGtkSessionPrivate *s;
    GList *list;
    GList *it;

    self = SPICE_GTK_SESSION(gobject);
    s = self->priv;
    if (!s->session)
        g_error("SpiceGtKSession constructed without a session");

    g_signal_connect(s->session, "channel-new",
                     G_CALLBACK(channel_new), self);
    g_signal_connect(s->session, "channel-destroy",
                     G_CALLBACK(channel_destroy), self);
    list = spice_session_get_channels(s->session);
    for (it = g_list_first(list); it != NULL; it = g_list_next(it)) {
        channel_new(s->session, it->data, (gpointer*)self);
    }
    g_list_free(list);
}

static void spice_gtk_session_dispose(GObject *gobject)
{
    SpiceGtkSession *self = SPICE_GTK_SESSION(gobject);
    SpiceGtkSessionPrivate *s = self->priv;

    /* release stuff */
    if (s->clipboard) {
        g_signal_handlers_disconnect_by_func(s->clipboard,
                G_CALLBACK(clipboard_owner_change), self);
        s->clipboard = NULL;
    }

    if (s->clipboard_primary) {
        g_signal_handlers_disconnect_by_func(s->clipboard_primary,
                G_CALLBACK(clipboard_owner_change), self);
        s->clipboard_primary = NULL;
    }

    if (s->session) {
        g_signal_handlers_disconnect_by_func(s->session,
                                             G_CALLBACK(channel_new),
                                             self);
        g_signal_handlers_disconnect_by_func(s->session,
                                             G_CALLBACK(channel_destroy),
                                             self);
        s->session = NULL;
    }
    g_clear_pointer(&s->cb_shared_files, g_hash_table_destroy);

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_gtk_session_parent_class)->dispose)
        G_OBJECT_CLASS(spice_gtk_session_parent_class)->dispose(gobject);
}

static void clipboard_release_delay_remove(SpiceGtkSession *self, guint selection,
                                           gboolean release_if_delayed)
{
    SpiceGtkSessionPrivate *s = self->priv;

    if (!s->clipboard_release_delay[selection]) {
        return;
    }

    if (release_if_delayed) {
        SPICE_DEBUG("delayed clipboard release, sel:%u", selection);
        clipboard_release(self, selection);
    }

    g_spice_source_remove(s->clipboard_release_delay[selection]);
    s->clipboard_release_delay[selection] = 0;
}

static void spice_gtk_session_finalize(GObject *gobject)
{
    SpiceGtkSession *self = SPICE_GTK_SESSION(gobject);
    SpiceGtkSessionPrivate *s = self->priv;
    int i;

    /* release stuff */
    for (i = 0; i < CLIPBOARD_LAST; ++i) {
        g_clear_pointer(&s->clip_targets[i], g_free);
        clipboard_release_delay_remove(self, i, true);
        g_clear_pointer(&s->atoms[i], g_free);
        s->n_atoms[i] = 0;
    }

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_gtk_session_parent_class)->finalize)
        G_OBJECT_CLASS(spice_gtk_session_parent_class)->finalize(gobject);
}

static void spice_gtk_session_get_property(GObject    *gobject,
                                           guint       prop_id,
                                           GValue     *value,
                                           GParamSpec *pspec)
{
    SpiceGtkSession *self = SPICE_GTK_SESSION(gobject);
    SpiceGtkSessionPrivate *s = self->priv;

    switch (prop_id) {
    case PROP_SESSION:
        g_value_set_object(value, s->session);
	break;
    case PROP_AUTO_CLIPBOARD:
        g_value_set_boolean(value, s->auto_clipboard_enable);
        break;
    case PROP_AUTO_USBREDIR:
        g_value_set_boolean(value, s->auto_usbredir_enable);
        break;
    case PROP_POINTER_GRABBED:
        g_value_set_boolean(value, s->pointer_grabbed);
        break;
    case PROP_SYNC_MODIFIERS:
        g_value_set_boolean(value, s->sync_modifiers);
        break;
    default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
	break;
    }
}

static void spice_gtk_session_set_property(GObject      *gobject,
                                           guint         prop_id,
                                           const GValue *value,
                                           GParamSpec   *pspec)
{
    SpiceGtkSession *self = SPICE_GTK_SESSION(gobject);
    SpiceGtkSessionPrivate *s = self->priv;

    switch (prop_id) {
    case PROP_SESSION:
        s->session = g_value_get_object(value);
        break;
    case PROP_AUTO_CLIPBOARD:
        s->auto_clipboard_enable = g_value_get_boolean(value);
        break;
    case PROP_AUTO_USBREDIR: {
        SpiceDesktopIntegration *desktop_int;
        gboolean orig_value = s->auto_usbredir_enable;

        s->auto_usbredir_enable = g_value_get_boolean(value);
        if (s->auto_usbredir_enable == orig_value)
            break;

        if (s->auto_usbredir_reqs) {
            SpiceUsbDeviceManager *manager =
                spice_usb_device_manager_get(s->session, NULL);

            if (!manager)
                break;

            g_object_set(manager, "auto-connect", s->auto_usbredir_enable,
                         NULL);

            desktop_int = spice_desktop_integration_get(s->session);
            if (s->auto_usbredir_enable)
                spice_desktop_integration_inhibit_automount(desktop_int);
            else
                spice_desktop_integration_uninhibit_automount(desktop_int);
        }
        break;
    }
    case PROP_SYNC_MODIFIERS:
        s->sync_modifiers = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
        break;
    }
}

static void spice_gtk_session_class_init(SpiceGtkSessionClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->constructed  = spice_gtk_session_constructed;
    gobject_class->dispose      = spice_gtk_session_dispose;
    gobject_class->finalize     = spice_gtk_session_finalize;
    gobject_class->get_property = spice_gtk_session_get_property;
    gobject_class->set_property = spice_gtk_session_set_property;

    /**
     * SpiceGtkSession:session:
     *
     * #SpiceSession this #SpiceGtkSession is associated with
     *
     * Since: 0.8
     **/
    g_object_class_install_property
        (gobject_class, PROP_SESSION,
         g_param_spec_object("session",
                             "Session",
                             "SpiceSession",
                             SPICE_TYPE_SESSION,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS));

    /**
     * SpiceGtkSession:auto-clipboard:
     *
     * When this is true the clipboard gets automatically shared between host
     * and guest.
     *
     * Since: 0.8
     **/
    g_object_class_install_property
        (gobject_class, PROP_AUTO_CLIPBOARD,
         g_param_spec_boolean("auto-clipboard",
                              "Auto clipboard",
                              "Automatically relay clipboard changes between "
                              "host and guest.",
                              TRUE,
                              G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT |
                              G_PARAM_STATIC_STRINGS));

    /**
     * SpiceGtkSession:auto-usbredir:
     *
     * Automatically redirect newly plugged in USB devices. Note the auto
     * redirection only happens when a #SpiceDisplay associated with the
     * session had keyboard focus.
     *
     * Since: 0.8
     **/
    g_object_class_install_property
        (gobject_class, PROP_AUTO_USBREDIR,
         g_param_spec_boolean("auto-usbredir",
                              "Auto USB Redirection",
                              "Automatically redirect newly plugged in USB"
                              "Devices to the guest.",
                              FALSE,
                              G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT |
                              G_PARAM_STATIC_STRINGS));

    /**
     * SpiceGtkSession:pointer-grabbed:
     *
     * Returns %TRUE if the pointer is currently grabbed by this session.
     *
     * Since: 0.27
     **/
    g_object_class_install_property
        (gobject_class, PROP_POINTER_GRABBED,
         g_param_spec_boolean("pointer-grabbed",
                              "Pointer grabbed",
                              "Whether the pointer is grabbed",
                              FALSE,
                              G_PARAM_READABLE |
                              G_PARAM_STATIC_STRINGS));

    /**
     * SpiceGtkSession:sync-modifiers:
     *
     * Automatically sync modifiers (Caps, Num and Scroll locks) with the guest.
     *
     * Since: 0.32
     **/
    g_object_class_install_property
        (gobject_class, PROP_SYNC_MODIFIERS,
         g_param_spec_boolean("sync-modifiers",
                              "Sync modifiers",
                              "Automatically sync modifiers",
                              TRUE,
                              G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT |
                              G_PARAM_STATIC_STRINGS));
}

/* ---------------------------------------------------------------- */
/* private functions (clipboard related)                            */

static GtkClipboard* get_clipboard_from_selection(SpiceGtkSessionPrivate *s,
                                                  guint selection)
{
    if (selection == VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD) {
        return s->clipboard;
    } else if (selection == VD_AGENT_CLIPBOARD_SELECTION_PRIMARY) {
        return s->clipboard_primary;
    } else {
        g_warning("Unhandled clipboard selection: %u", selection);
        return NULL;
    }
}

static gint get_selection_from_clipboard(SpiceGtkSessionPrivate *s,
                                         GtkClipboard* cb)
{
    if (cb == s->clipboard) {
        return VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    } else if (cb == s->clipboard_primary) {
        return VD_AGENT_CLIPBOARD_SELECTION_PRIMARY;
    } else {
        g_warning("Unhandled clipboard");
        return -1;
    }
}

static const struct {
    const char  *xatom;
    uint32_t    vdagent;
} atom2agent[] = {
    {
        .vdagent = VD_AGENT_CLIPBOARD_UTF8_TEXT,
        .xatom   = "UTF8_STRING",
    },{
        .vdagent = VD_AGENT_CLIPBOARD_UTF8_TEXT,
        .xatom   = "text/plain;charset=utf-8"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_UTF8_TEXT,
        .xatom   = "STRING"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_UTF8_TEXT,
        .xatom   = "TEXT"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_UTF8_TEXT,
        .xatom   = "text/plain"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_PNG,
        .xatom   = "image/png"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_BMP,
        .xatom   = "image/bmp"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_BMP,
        .xatom   = "image/x-bmp"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_BMP,
        .xatom   = "image/x-MS-bmp"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_BMP,
        .xatom   = "image/x-win-bitmap"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_TIFF,
        .xatom   = "image/tiff"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_JPG,
        .xatom   = "image/jpeg"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_FILE_LIST,
        .xatom   = "text/uri-list"
    }
};

static GWeakRef* get_weak_ref(gpointer object)
{
    GWeakRef *weakref = g_new(GWeakRef, 1);
    g_weak_ref_init(weakref, object);
    return weakref;
}

static gpointer free_weak_ref(gpointer data)
{
    GWeakRef *weakref = data;
    gpointer object = g_weak_ref_get(weakref);

    g_weak_ref_clear(weakref);
    g_free(weakref);
    if (object != NULL) {
        /* The main reference still exists as object is not NULL, so we can
         * remove the strong reference given by g_weak_ref_get */
        g_object_unref(object);
    }
    return object;
}

#ifdef HAVE_PHODAV_VIRTUAL
static SpiceWebdavChannel *clipboard_get_open_webdav(SpiceSession *session)
{
    GList *list, *l;
    SpiceChannel *channel = NULL;
    gboolean open = FALSE;

    g_return_val_if_fail(session != NULL, NULL);

    list = spice_session_get_channels(session);
    for (l = g_list_first(list); l != NULL; l = g_list_next(l)) {
        channel = l->data;

        if (!SPICE_IS_WEBDAV_CHANNEL(channel)) {
            continue;
        }

        g_object_get(channel, "port-opened", &open, NULL);
        break;
    }

    g_list_free(list);
    return open ? SPICE_WEBDAV_CHANNEL(channel) : NULL;
}

static GdkAtom clipboard_find_atom(SpiceGtkSessionPrivate *s, guint selection, GdkAtom a)
{
    for (int i = 0; i < s->n_atoms[selection]; i++) {
        if (s->atoms[selection][i] == a) {
            return a;
        }
    }
    return GDK_NONE;
}
#endif

static void clipboard_get_targets(GtkClipboard *clipboard,
                                  GdkAtom *atoms,
                                  gint n_atoms,
                                  gpointer user_data)
{
    SpiceGtkSession *self = free_weak_ref(user_data);

    SPICE_DEBUG("%s:", __FUNCTION__);

    if (self == NULL)
        return;

    g_return_if_fail(SPICE_IS_GTK_SESSION(self));

    if (atoms == NULL) {
        SPICE_DEBUG("Retrieving the clipboard data has failed");
        return;
    }

    SpiceGtkSessionPrivate *s = self->priv;
    guint32 types[SPICE_N_ELEMENTS(atom2agent)] = { 0 };
    gint num_types;
    int a;
    int selection;

    if (s->main == NULL)
        return;

    selection = get_selection_from_clipboard(s, clipboard);
    g_return_if_fail(selection != -1);

    /* GTK+ does seem to cache atoms, but not for Wayland */
    g_free(s->atoms[selection]);
    s->atoms[selection] = g_memdup(atoms, n_atoms * sizeof(GdkAtom));
    s->n_atoms[selection] = n_atoms;

    if (s->clip_grabbed[selection]) {
        SPICE_DEBUG("Clipboard is already grabbed, re-grab: %d atoms", n_atoms);
    }

    /* Set all Atoms that matches our current protocol implementation */
    num_types = 0;
    for (a = 0; a < n_atoms; a++) {
        guint m;
        gchar *name = gdk_atom_name(atoms[a]);

        SPICE_DEBUG(" \"%s\"", name);

        for (m = 0; m < SPICE_N_ELEMENTS(atom2agent); m++) {
            guint t;

            if (strcasecmp(name, atom2agent[m].xatom) != 0) {
                continue;
            }

            if (atom2agent[m].vdagent == VD_AGENT_CLIPBOARD_FILE_LIST) {
#ifdef HAVE_PHODAV_VIRTUAL
                if (!clipboard_get_open_webdav(s->session)) {
                    SPICE_DEBUG("Received %s target, but the clipboard webdav channel "
                                "isn't available, skipping", atom2agent[m].xatom);
                    break;
                }
#else
                break;
#endif
            }

            /* check if type is already in list */
            for (t = 0; t < num_types; t++) {
                if (types[t] == atom2agent[m].vdagent) {
                    break;
                }
            }

            if (t == num_types) {
                /* add type to empty slot */
                types[t] = atom2agent[m].vdagent;
                num_types++;
            }
        }
        g_free(name);
    }

    if (num_types == 0) {
        SPICE_DEBUG("No GdkAtoms will be sent from %d", n_atoms);
        return;
    }

    s->clip_grabbed[selection] = TRUE;

    if (spice_main_channel_agent_test_capability(s->main, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND))
        spice_main_channel_clipboard_selection_grab(s->main, selection, types, num_types);

    /* Sending a grab causes the agent to do an implicit release */
    s->nclip_targets[selection] = 0;
}

/* Callback for every owner-change event for given @clipboard.
 * This event is triggered in different ways depending on the environment of
 * the Client, some examples:
 *
 * Situation 1: When another application on the client machine is holding and
 * changing the clipboard. If client is on Wayland, spice-gtk only receives the
 * related GtkClipboard::owner-changed event after focus-in event on Spice
 * widget; On X11, we will receive it at the moment the clipboard data has been
 * changed in by other application.
 *
 * Situation 2: When spice-gtk holds the focus and is changing the clipboard by
 * either setting new content information with gtk_clipboard_set_with_owner() or
 * clearing up old content with gtk_clipboard_clear(). The main difference between
 * Wayland and X11 is that on X11, gtk_clipboard_clear() sets the owner to none, which
 * emits owner-change event; On Wayland that does not happen as spice-gtk still is
 * the owner of the clipboard.
 */
static void clipboard_owner_change(GtkClipboard        *clipboard,
                                   GdkEventOwnerChange *event,
                                   gpointer            user_data)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(user_data));

    SpiceGtkSession *self = user_data;
    SpiceGtkSessionPrivate *s = self->priv;
    int selection;

    selection = get_selection_from_clipboard(s, clipboard);
    g_return_if_fail(selection != -1);

    if (s->main == NULL) {
        return;
    }

    g_clear_pointer(&s->atoms[selection], g_free);
    s->n_atoms[selection] = 0;

    if (event->reason != GDK_OWNER_CHANGE_NEW_OWNER) {
        if (s->clip_grabbed[selection]) {
            /* grab was sent to the agent, so release it */
            s->clip_grabbed[selection] = FALSE;
            if (spice_main_channel_agent_test_capability(s->main, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND)) {
                spice_main_channel_clipboard_selection_release(s->main, selection);
            }
        }
        s->clip_hasdata[selection] = FALSE;
        return;
    }

    /* This situation happens when clipboard is being set by us (grab message) */
    if (gtk_clipboard_get_owner(clipboard) == G_OBJECT(self)) {
        return;
    }

    s->clipboard_by_guest[selection] = FALSE;

#ifdef GDK_WINDOWING_X11
    if (!event->owner && GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
        s->clip_hasdata[selection] = FALSE;
        return;
    }
#endif

    s->clip_hasdata[selection] = TRUE;
    if (s->auto_clipboard_enable && !read_only(self))
        gtk_clipboard_request_targets(clipboard, clipboard_get_targets,
                                      get_weak_ref(self));
}

typedef struct
{
    SpiceGtkSession *self;
    GMainLoop *loop;
    GtkSelectionData *selection_data;
    guint info;
    guint selection;
} RunInfo;

static void clipboard_got_from_guest(SpiceMainChannel *main, guint selection,
                                     guint type, const guchar *data, guint size,
                                     gpointer user_data)
{
    RunInfo *ri = user_data;
    SpiceGtkSessionPrivate *s = ri->self->priv;
    gchar *conv = NULL;

    g_return_if_fail(selection == ri->selection);

    SPICE_DEBUG("clipboard got data");

    if (atom2agent[ri->info].vdagent == VD_AGENT_CLIPBOARD_UTF8_TEXT) {
        /* on windows, gtk+ would already convert to LF endings, but
           not on unix */
        if (spice_main_channel_agent_test_capability(s->main, VD_AGENT_CAP_GUEST_LINEEND_CRLF)) {
            conv = spice_dos2unix((gchar*)data, size);
            size = strlen(conv);
        }

        gtk_selection_data_set_text(ri->selection_data, conv ?: (gchar*)data, size);
    } else {
        gtk_selection_data_set(ri->selection_data,
            gdk_atom_intern_static_string(atom2agent[ri->info].xatom),
            8, data, size);
    }

    if (g_main_loop_is_running (ri->loop))
        g_main_loop_quit (ri->loop);

    g_free(conv);
}

static void clipboard_agent_connected(RunInfo *ri)
{
    g_warning("agent status changed, cancel clipboard request");

    if (g_main_loop_is_running(ri->loop))
        g_main_loop_quit(ri->loop);
}

static void clipboard_get(GtkClipboard *clipboard,
                          GtkSelectionData *selection_data,
                          guint info, gpointer user_data)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(user_data));

    RunInfo ri = { NULL, };
    SpiceGtkSession *self = user_data;
    SpiceGtkSessionPrivate *s = self->priv;
    gboolean agent_connected = FALSE;
    gulong clipboard_handler;
    gulong agent_handler;
    int selection;

    SPICE_DEBUG("clipboard get");

    selection = get_selection_from_clipboard(s, clipboard);
    g_return_if_fail(selection != -1);
    g_return_if_fail(info < SPICE_N_ELEMENTS(atom2agent));
    g_return_if_fail(s->main != NULL);

    if (s->clipboard_release_delay[selection]) {
        SPICE_DEBUG("not requesting data from guest during delayed release");
        return;
    }

    ri.selection_data = selection_data;
    ri.info = info;
    ri.loop = g_main_loop_new(spice_main_context(), FALSE);
    ri.selection = selection;
    ri.self = self;

    clipboard_handler = g_signal_connect(s->main, "main-clipboard-selection",
                                         G_CALLBACK(clipboard_got_from_guest),
                                         &ri);
    agent_handler = g_signal_connect_swapped(s->main, "notify::agent-connected",
                                     G_CALLBACK(clipboard_agent_connected),
                                     &ri);

    spice_main_channel_clipboard_selection_request(s->main, selection,
                                                   atom2agent[info].vdagent);


    g_object_get(s->main, "agent-connected", &agent_connected, NULL);
    if (!agent_connected) {
        SPICE_DEBUG("canceled clipboard_get, before running loop");
        goto cleanup;
    }

    /* This is modeled on the implementation of gtk_dialog_run() even though
     * these thread functions are deprecated and appears to be needed to avoid
     * dead-lock from gtk_dialog_run().
     */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_threads_leave();
    g_main_loop_run(ri.loop);
    gdk_threads_enter();
    G_GNUC_END_IGNORE_DEPRECATIONS

cleanup:
    g_clear_pointer(&ri.loop, g_main_loop_unref);
    g_signal_handler_disconnect(s->main, clipboard_handler);
    g_signal_handler_disconnect(s->main, agent_handler);
}

static void clipboard_clear(GtkClipboard *clipboard, gpointer user_data)
{
    SPICE_DEBUG("clipboard_clear");
    /* We watch for clipboard ownership changes and act on those, so we
       don't need to do anything here */
}

static gboolean clipboard_grab(SpiceMainChannel *main, guint selection,
                               guint32* types, guint32 ntypes,
                               gpointer user_data)
{
    g_return_val_if_fail(SPICE_IS_GTK_SESSION(user_data), FALSE);

    SpiceGtkSession *self = user_data;
    SpiceGtkSessionPrivate *s = self->priv;
    GtkTargetEntry targets[SPICE_N_ELEMENTS(atom2agent)];
    gboolean target_selected[SPICE_N_ELEMENTS(atom2agent)] = { FALSE, };
    gboolean found;
    GtkClipboard* cb;
    int m, n;
    int num_targets = 0;

    clipboard_release_delay_remove(self, selection, false);

    cb = get_clipboard_from_selection(s, selection);
    g_return_val_if_fail(cb != NULL, FALSE);

    for (n = 0; n < ntypes; ++n) {
        found = FALSE;
        for (m = 0; m < SPICE_N_ELEMENTS(atom2agent); m++) {
            if (atom2agent[m].vdagent == types[n] && !target_selected[m]) {
                found = TRUE;
                g_return_val_if_fail(num_targets < SPICE_N_ELEMENTS(atom2agent), FALSE);
                targets[num_targets].target = (gchar*)atom2agent[m].xatom;
                targets[num_targets].info = m;
                target_selected[m] = TRUE;
                num_targets++;
            }
        }
        if (!found) {
            g_warning("clipboard: couldn't find a matching type for: %u",
                      types[n]);
        }
    }

    g_free(s->clip_targets[selection]);
    s->nclip_targets[selection] = num_targets;
    s->clip_targets[selection] = g_memdup(targets, sizeof(GtkTargetEntry) * num_targets);
    /* Receiving a grab implies we've released our own grab */
    s->clip_grabbed[selection] = FALSE;

    if (read_only(self) ||
        !s->auto_clipboard_enable ||
        s->nclip_targets[selection] == 0) {
        return TRUE;
    }

    if (!gtk_clipboard_set_with_owner(cb,
                                      targets,
                                      num_targets,
                                      clipboard_get,
                                      clipboard_clear,
                                      G_OBJECT(self))) {
        g_warning("clipboard grab failed");
        return FALSE;
    }
    s->clipboard_by_guest[selection] = TRUE;
    s->clip_hasdata[selection] = FALSE;

    return TRUE;
}

static gboolean check_clipboard_size_limits(SpiceGtkSession *session,
                                            gint clipboard_len)
{
    int max_clipboard;

    g_object_get(session->priv->main, "max-clipboard", &max_clipboard, NULL);
    if (max_clipboard != -1 && clipboard_len > max_clipboard) {
        g_warning("discarded clipboard of size %d (max: %d)",
                  clipboard_len, max_clipboard);
        return FALSE;
    } else if (clipboard_len <= 0) {
        SPICE_DEBUG("discarding empty clipboard");
        return FALSE;
    }

    return TRUE;
}

/* This will convert line endings if needed (between Windows/Unix conventions),
 * and will make sure 'len' does not take into account any trailing \0 as this could
 * cause some confusion guest side.
 * The 'len' argument will be modified by this function to the length of the modified
 * string
 */
static char *fixup_clipboard_text(SpiceGtkSession *self, const char *text, int *len)
{
    char *conv = NULL;

    if (spice_main_channel_agent_test_capability(self->priv->main,
                                                 VD_AGENT_CAP_GUEST_LINEEND_CRLF)) {
        conv = spice_unix2dos(text, *len);
        *len = strlen(conv);
    } else {
        /* On Windows, with some versions of gtk+, GtkSelectionData::length
         * will include the final '\0'. When a string with this trailing '\0'
         * is pasted in some linux applications, it will be pasted as <NIL> or
         * as an invisible character, which is unwanted. Ensure the length we
         * send to the agent does not include any trailing '\0'
         * This is gtk+ bug https://bugzilla.gnome.org/show_bug.cgi?id=734670
         */
        *len = strlen(text);
    }

    return conv;
}

static void clipboard_received_text_cb(GtkClipboard *clipboard,
                                       const gchar *text,
                                       gpointer user_data)
{
    SpiceGtkSession *self = free_weak_ref(user_data);
    char *conv = NULL;
    int len = 0;
    int selection;
    const guchar *data = NULL;

    if (self == NULL)
        return;

    selection = get_selection_from_clipboard(self->priv, clipboard);
    g_return_if_fail(selection != -1);

    if (text == NULL) {
        SPICE_DEBUG("Failed to retrieve clipboard text");
        goto notify_agent;
    }

    g_return_if_fail(SPICE_IS_GTK_SESSION(self));

    len = strlen(text);
    if (!check_clipboard_size_limits(self, len)) {
        SPICE_DEBUG("Failed size limits of clipboard text (%d bytes)", len);
        goto notify_agent;
    }

    /* gtk+ internal utf8 newline is always LF, even on windows */
    conv = fixup_clipboard_text(self, text, &len);
    if (!check_clipboard_size_limits(self, len)) {
        SPICE_DEBUG("Failed size limits of clipboard text (%d bytes)", len);
        goto notify_agent;
    }

    data = (const guchar *) (conv != NULL ? conv : text);
notify_agent:
    spice_main_channel_clipboard_selection_notify(self->priv->main, selection,
                                                  VD_AGENT_CLIPBOARD_UTF8_TEXT,
                                                  data,
                                                  (data != NULL) ? len : 0);
    g_free(conv);
}

#ifdef HAVE_PHODAV_VIRTUAL
/* returns path to @file under @root in clipboard phodav server, or NULL on error */
static gchar *clipboard_webdav_share_file(PhodavVirtualDir *root, GFile *file)
{
    gchar *uuid;
    PhodavVirtualDir *dir;
    GError *err = NULL;

    /* separate directory is created for each file,
     * as we want to preserve the original filename and avoid conflicts */
    for (guint i = 0; i < 8; i++) {
        uuid = g_uuid_string_random();
        gchar *dir_path = g_strdup_printf(SPICE_WEBDAV_CLIPBOARD_FOLDER_PATH "/%s", uuid);
        dir = phodav_virtual_dir_new_dir(root, dir_path, &err);
        g_free(dir_path);
        if (!err) {
            break;
        }
        g_clear_pointer(&uuid, g_free);
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
            g_warning("failed to create phodav virtual dir: %s", err->message);
            g_error_free(err);
            return NULL;
        }
        g_clear_error(&err);
    }

    if (!dir) {
        g_warning("failed to create phodav virtual dir: all attempts failed");
        return NULL;
    }

    phodav_virtual_dir_attach_real_child(dir, file);
    g_object_unref(dir);

    gchar *base = g_file_get_basename(file);
    gchar *path = g_strdup_printf(SPICE_WEBDAV_CLIPBOARD_FOLDER_PATH "/%s/%s", uuid, base);
    g_free(uuid);
    g_free(base);

    return path;
}

/* join all strings in @strv into a new char array,
 * including all terminating NULL-chars */
static gchar *strv_concat(gchar **strv, gsize *size_out)
{
    gchar **str_p, *arr, *curr;

    g_return_val_if_fail(strv && size_out, NULL);

    for (str_p = strv, *size_out = 0; *str_p != NULL; str_p++) {
        *size_out += strlen(*str_p) + 1;
    }

    arr = g_malloc(*size_out);

    for (str_p = strv, curr = arr; *str_p != NULL; str_p++) {
        curr = g_stpcpy(curr, *str_p) + 1;
    }

    return arr;
}

/* if not done alreay, share all files in @uris using the webdav server
 * and return a new buffer with VD_AGENT_CLIPBOARD_FILE_LIST data */
static gchar *strv_uris_transform_to_data(SpiceGtkSessionPrivate *s,
    gchar **uris, gsize *size_out, GdkDragAction action)
{
    SpiceWebdavChannel *webdav;
    /* if there's version mismatch between spice-client-gtk and spice-client-glib,
     * "webdav-server" property might not be present, so phodav must be initialized to NULL */
    PhodavServer *phodav = NULL;
    PhodavVirtualDir *root;

    gchar **uri_ptr, *path, **paths, *data;
    GFile *file;
    guint n;

    *size_out = 0;

    if (!uris || g_strv_length(uris) < 1) {
        return NULL;
    }

    webdav = clipboard_get_open_webdav(s->session);
    if (!webdav) {
        SPICE_DEBUG("Received uris, but no webdav channel");
        return NULL;
    }

    g_object_get(s->session, "webdav-server", &phodav, NULL);
    if (!phodav) {
        return NULL;
    }
    g_object_get(phodav, "root-file", &root, NULL);
    g_object_unref(phodav);

    paths = g_new0(gchar *, g_strv_length(uris) + 2);

    paths[0] = action == GDK_ACTION_MOVE ? "cut" : "copy";
    n = 1;

    for (uri_ptr = uris; *uri_ptr != NULL; uri_ptr++) {
        file = g_file_new_for_uri(*uri_ptr);

        /* clipboard data is usually requested multiple times for no obvious reasons
         * (clipboar managers to blame?), we don't want to create multiple dirs for the same file */
        path = g_hash_table_lookup(s->cb_shared_files, file);
        if (path) {
            SPICE_DEBUG("found %s with path %s", *uri_ptr, path);
            g_object_unref(file);
        } else {
            path = clipboard_webdav_share_file(root, file);
            g_return_val_if_fail(path != NULL, NULL);
            SPICE_DEBUG("publishing %s under %s", *uri_ptr, path);
            /* file and path gets freed once the hash table gets destroyed */
            g_hash_table_insert(s->cb_shared_files, file, path);
        }
        paths[n] = path;
        n++;
    }

    g_object_unref(root);
    data = strv_concat(paths, size_out);
    g_free(paths);

    return data;
}

static GdkAtom a_gnome, a_mate, a_nautilus, a_uri_list, a_kde_cut;

static void init_uris_atoms()
{
    if (a_gnome != GDK_NONE) {
        return;
    }
    a_gnome = gdk_atom_intern_static_string("x-special/gnome-copied-files");
    a_mate = gdk_atom_intern_static_string("x-special/mate-copied-files");
    a_nautilus = gdk_atom_intern_static_string("UTF8_STRING");
    a_uri_list = gdk_atom_intern_static_string("text/uri-list");
    a_kde_cut = gdk_atom_intern_static_string("application/x-kde-cutselection");
}

static GdkAtom clipboard_select_uris_atom(SpiceGtkSessionPrivate *s, guint selection)
{
    init_uris_atoms();
    if (clipboard_find_atom(s, selection, a_gnome)) {
        return a_gnome;
    }
    if (clipboard_find_atom(s, selection, a_mate)) {
        return a_mate;
    }
    if (clipboard_find_atom(s, selection, a_nautilus)) {
        return a_nautilus;
    }
    return clipboard_find_atom(s, selection, a_uri_list);
}

/* common handler for "x-special/gnome-copied-files" and "x-special/mate-copied-files" */
static gchar *x_special_copied_files_transform_to_data(SpiceGtkSessionPrivate *s,
    GtkSelectionData *selection_data, gsize *size_out)
{
    const gchar *text;
    gchar **lines, *data = NULL;
    GdkDragAction action;

    *size_out = 0;

    text = (gchar *)gtk_selection_data_get_data(selection_data);
    if (!text) {
        return NULL;
    }
    lines = g_strsplit(text, "\n", -1);
    if (g_strv_length(lines) < 2) {
        goto err;
    }

    if (!g_strcmp0(lines[0], "cut")) {
        action = GDK_ACTION_MOVE;
    } else if (!g_strcmp0(lines[0], "copy")) {
        action = GDK_ACTION_COPY;
    } else {
        goto err;
    }

    data = strv_uris_transform_to_data(s, &lines[1], size_out, action);
err:
    g_strfreev(lines);
    return data;
}

/* used with newer Nautilus */
static gchar *nautilus_uris_transform_to_data(SpiceGtkSessionPrivate *s,
    GtkSelectionData *selection_data, gsize *size_out, gboolean *retry_out)
{
    gchar **lines, *text, *data = NULL;
    guint n_lines;
    GdkDragAction action;

    *size_out = 0;

    text = (gchar *)gtk_selection_data_get_text(selection_data);
    if (!text) {
        return NULL;
    }
    lines = g_strsplit(text, "\n", -1);
    g_free(text);
    n_lines = g_strv_length(lines);

    if (n_lines < 4) {
        *retry_out = TRUE;
        goto err;
    }

    if (g_strcmp0(lines[0], "x-special/nautilus-clipboard")) {
        *retry_out = TRUE;
        goto err;
    }

    if (!g_strcmp0(lines[1], "cut")) {
        action = GDK_ACTION_MOVE;
    } else if (!g_strcmp0(lines[1], "copy")) {
        action = GDK_ACTION_COPY;
    } else {
        goto err;
    }

    /* the list of uris must end with \n,
     * so there must be an empty string after the split */
    if (g_strcmp0(lines[n_lines-1], "")) {
        goto err;
    }
    g_clear_pointer(&lines[n_lines-1], g_free);

    data = strv_uris_transform_to_data(s, &lines[2], size_out, action);
err:
    g_strfreev(lines);
    return data;
}

static GdkDragAction kde_get_clipboard_action(SpiceGtkSessionPrivate *s, GtkClipboard *clipboard)
{
    GtkSelectionData *selection_data;
    GdkDragAction action;
    const guchar *data;

    /* this uses another GMainLoop, basically the same mechanism
     * as we use in clipboard_get(), so it doesn't block */
    selection_data = gtk_clipboard_wait_for_contents(clipboard, a_kde_cut);
    data = gtk_selection_data_get_data(selection_data);
    if (data && data[0] == '1') {
        action = GDK_ACTION_MOVE;
    } else {
        action = GDK_ACTION_COPY;
    }
    gtk_selection_data_free(selection_data);

    return action;
}

static void clipboard_received_uri_contents_cb(GtkClipboard *clipboard,
                                               GtkSelectionData *selection_data,
                                               gpointer user_data)
{
    SpiceGtkSession *self = free_weak_ref(user_data);
    SpiceGtkSessionPrivate *s;
    guint selection;

    if (!self) {
        return;
    }
    s = self->priv;

    selection = get_selection_from_clipboard(s, clipboard);
    g_return_if_fail(selection != -1);

    init_uris_atoms();
    GdkAtom type = gtk_selection_data_get_data_type(selection_data);
    gchar *data;
    gsize len;

    if (type == a_gnome || type == a_mate) {
        /* used by old Nautilus + many other file managers  */
        data = x_special_copied_files_transform_to_data(s, selection_data, &len);
    } else if (type == a_nautilus) {
        gboolean retry = FALSE;
        data = nautilus_uris_transform_to_data(s, selection_data, &len, &retry);

        if (retry && clipboard_find_atom(s, selection, a_uri_list) != GDK_NONE) {
            /* it's not Nautilus, so we give it one more try with the generic uri-list target */
            gtk_clipboard_request_contents(clipboard, a_uri_list,
                clipboard_received_uri_contents_cb, get_weak_ref(self));
            return;
        }
    } else if (type == a_uri_list) {
        GdkDragAction action = GDK_ACTION_COPY;
        gchar **uris = gtk_selection_data_get_uris(selection_data);

        /* KDE uses a separate atom to distinguish between copy and move operation */
        if (clipboard_find_atom(s, selection, a_kde_cut) != GDK_NONE) {
            action = kde_get_clipboard_action(s, clipboard);
        }

        data = strv_uris_transform_to_data(s, uris, &len, action);
        g_strfreev(uris);
    } else {
        g_warning("received uris in unsupported type");
        data = NULL;
        len = 0;
    }

    spice_main_channel_clipboard_selection_notify(s->main, selection,
        VD_AGENT_CLIPBOARD_FILE_LIST, (guchar *)data, len);
    g_free(data);
}
#endif

static void clipboard_received_cb(GtkClipboard *clipboard,
                                  GtkSelectionData *selection_data,
                                  gpointer user_data)
{
    SpiceGtkSession *self = free_weak_ref(user_data);

    if (self == NULL)
        return;

    g_return_if_fail(SPICE_IS_GTK_SESSION(self));

    SpiceGtkSessionPrivate *s = self->priv;
    gint len = 0, m;
    guint32 type = VD_AGENT_CLIPBOARD_NONE;
    gchar* name;
    GdkAtom atom;
    int selection;

    selection = get_selection_from_clipboard(s, clipboard);
    g_return_if_fail(selection != -1);

    len = gtk_selection_data_get_length(selection_data);
    if (!check_clipboard_size_limits(self, len)) {
        return;
    } else {
        atom = gtk_selection_data_get_data_type(selection_data);
        name = gdk_atom_name(atom);
        for (m = 0; m < SPICE_N_ELEMENTS(atom2agent); m++) {
            if (strcasecmp(name, atom2agent[m].xatom) == 0) {
                break;
            }
        }

        if (m >= SPICE_N_ELEMENTS(atom2agent)) {
            g_warning("clipboard_received for unsupported type: %s", name);
        } else {
            type = atom2agent[m].vdagent;
        }

        g_free(name);
    }

    const guchar *data = gtk_selection_data_get_data(selection_data);

    /* text should be handled through clipboard_received_text_cb(), not
     * clipboard_received_cb().
     */
    g_warn_if_fail(type != VD_AGENT_CLIPBOARD_UTF8_TEXT);

    spice_main_channel_clipboard_selection_notify(s->main, selection, type, data, len);
}

static gboolean clipboard_request(SpiceMainChannel *main, guint selection,
                                  guint type, gpointer user_data)
{
    g_return_val_if_fail(SPICE_IS_GTK_SESSION(user_data), FALSE);

    SpiceGtkSession *self = user_data;
    SpiceGtkSessionPrivate *s = self->priv;
    GdkAtom atom;
    GtkClipboard* cb;
    int m;

    cb = get_clipboard_from_selection(s, selection);
    g_return_val_if_fail(cb != NULL, FALSE);
    g_return_val_if_fail(s->clipboard_by_guest[selection] == FALSE, FALSE);
    g_return_val_if_fail(s->clip_grabbed[selection], FALSE);

    if (read_only(self))
        return FALSE;

    if (type == VD_AGENT_CLIPBOARD_UTF8_TEXT) {
        gtk_clipboard_request_text(cb, clipboard_received_text_cb,
                                   get_weak_ref(self));
    } else if (type == VD_AGENT_CLIPBOARD_FILE_LIST) {
#ifdef HAVE_PHODAV_VIRTUAL
        atom = clipboard_select_uris_atom(s, selection);
        if (atom == GDK_NONE) {
            return FALSE;
        }
        gtk_clipboard_request_contents(cb, atom,
            clipboard_received_uri_contents_cb, get_weak_ref(self));
#else
        return FALSE;
#endif
    } else {
        for (m = 0; m < SPICE_N_ELEMENTS(atom2agent); m++) {
            if (atom2agent[m].vdagent == type)
                break;
        }

        g_return_val_if_fail(m < SPICE_N_ELEMENTS(atom2agent), FALSE);

        atom = gdk_atom_intern_static_string(atom2agent[m].xatom);
        gtk_clipboard_request_contents(cb, atom, clipboard_received_cb,
                                       get_weak_ref(self));
    }

    return TRUE;
}

static void clipboard_release(SpiceGtkSession *self, guint selection)
{
    SpiceGtkSessionPrivate *s = self->priv;
    GtkClipboard* clipboard = get_clipboard_from_selection(s, selection);

    g_return_if_fail(clipboard != NULL);

    s->nclip_targets[selection] = 0;

    if (!s->clipboard_by_guest[selection])
        return;
    gtk_clipboard_clear(clipboard);
    s->clipboard_by_guest[selection] = FALSE;
}

typedef struct SpiceGtkClipboardRelease {
    SpiceGtkSession *self;
    guint selection;
} SpiceGtkClipboardRelease;

static gboolean clipboard_release_timeout(gpointer user_data)
{
    SpiceGtkClipboardRelease *rel = user_data;

    clipboard_release_delay_remove(rel->self, rel->selection, true);

    return G_SOURCE_REMOVE;
}

/*
 * The agents send release between two grabs. This may trigger
 * clipboard managers trying to grab the clipboard. We end up with two
 * sides, client and remote, racing for the clipboard grab, and
 * believing each other is the owner.
 *
 * Workaround this problem by delaying the release event by 0.5 sec,
 * unless the no-release-on-regrab capability is present.
 */
#define CLIPBOARD_RELEASE_DELAY 500 /* ms */

static void clipboard_release_delay(SpiceMainChannel *main, guint selection,
                                    gpointer user_data)
{
    SpiceGtkSession *self = SPICE_GTK_SESSION(user_data);
    SpiceGtkSessionPrivate *s = self->priv;
    GtkClipboard* clipboard = get_clipboard_from_selection(s, selection);
    SpiceGtkClipboardRelease *rel;

    if (!clipboard) {
        return;
    }

    clipboard_release_delay_remove(self, selection, true);

    if (spice_main_channel_agent_test_capability(s->main,
                                                 VD_AGENT_CAP_CLIPBOARD_NO_RELEASE_ON_REGRAB)) {
        clipboard_release(self, selection);
        return;
    }

    rel = g_new0(SpiceGtkClipboardRelease, 1);
    rel->self = self;
    rel->selection = selection;
    s->clipboard_release_delay[selection] =
        g_spice_timeout_add_full(G_PRIORITY_DEFAULT, CLIPBOARD_RELEASE_DELAY,
                                 clipboard_release_timeout, rel, g_free);

}

static void channel_new(SpiceSession *session, SpiceChannel *channel,
                        gpointer user_data)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(user_data));

    SpiceGtkSession *self = user_data;
    SpiceGtkSessionPrivate *s = self->priv;

    if (SPICE_IS_MAIN_CHANNEL(channel)) {
        SPICE_DEBUG("Changing main channel from %p to %p", s->main, channel);
        s->main = SPICE_MAIN_CHANNEL(channel);
        g_signal_connect(channel, "main-clipboard-selection-grab",
                         G_CALLBACK(clipboard_grab), self);
        g_signal_connect(channel, "main-clipboard-selection-request",
                         G_CALLBACK(clipboard_request), self);
        g_signal_connect(channel, "main-clipboard-selection-release",
                         G_CALLBACK(clipboard_release_delay), self);
    }
    if (SPICE_IS_INPUTS_CHANNEL(channel)) {
        spice_g_signal_connect_object(channel, "inputs-modifiers",
                                      G_CALLBACK(guest_modifiers_changed), self, 0);
        spice_gtk_session_sync_keyboard_modifiers_for_channel(self, SPICE_INPUTS_CHANNEL(channel), TRUE);
    }
}

static void channel_destroy(SpiceSession *session, SpiceChannel *channel,
                            gpointer user_data)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(user_data));

    SpiceGtkSession *self = user_data;
    SpiceGtkSessionPrivate *s = self->priv;
    guint i;

    if (SPICE_IS_MAIN_CHANNEL(channel) && SPICE_MAIN_CHANNEL(channel) == s->main) {
        s->main = NULL;
        for (i = 0; i < CLIPBOARD_LAST; ++i) {
            if (s->clipboard_by_guest[i]) {
                GtkClipboard *cb = get_clipboard_from_selection(s, i);
                if (cb)
                    gtk_clipboard_clear(cb);
                s->clipboard_by_guest[i] = FALSE;
            }
            s->clip_grabbed[i] = FALSE;
            s->nclip_targets[i] = 0;
        }
    }
}

static gboolean read_only(SpiceGtkSession *self)
{
    return spice_session_get_read_only(self->priv->session);
}

/* ---------------------------------------------------------------- */
/* private functions (usbredir related)                             */
G_GNUC_INTERNAL
void spice_gtk_session_request_auto_usbredir(SpiceGtkSession *self,
                                             gboolean state)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(self));

    SpiceGtkSessionPrivate *s = self->priv;
    SpiceDesktopIntegration *desktop_int;
    SpiceUsbDeviceManager *manager;

    if (state) {
        s->auto_usbredir_reqs++;
        if (s->auto_usbredir_reqs != 1)
            return;
    } else {
        g_return_if_fail(s->auto_usbredir_reqs > 0);
        s->auto_usbredir_reqs--;
        if (s->auto_usbredir_reqs != 0)
            return;
    }

    if (!s->auto_usbredir_enable)
        return;

    manager = spice_usb_device_manager_get(s->session, NULL);
    if (!manager)
        return;

    g_object_set(manager, "auto-connect", state, NULL);

    desktop_int = spice_desktop_integration_get(s->session);
    if (state)
        spice_desktop_integration_inhibit_automount(desktop_int);
    else
        spice_desktop_integration_uninhibit_automount(desktop_int);
}

/* ------------------------------------------------------------------ */
/* public functions                                                   */

/**
 * spice_gtk_session_get:
 * @session: #SpiceSession for which to get the #SpiceGtkSession
 *
 * Gets the #SpiceGtkSession associated with the passed in #SpiceSession.
 * A new #SpiceGtkSession instance will be created the first time this
 * function is called for a certain #SpiceSession.
 *
 * Note that this function returns a weak reference, which should not be used
 * after the #SpiceSession itself has been unref-ed by the caller.
 *
 * Returns: (transfer none): a weak reference to the #SpiceGtkSession associated with the passed in #SpiceSession
 *
 * Since 0.8
 **/
SpiceGtkSession *spice_gtk_session_get(SpiceSession *session)
{
    g_return_val_if_fail(SPICE_IS_SESSION(session), NULL);

    SpiceGtkSession *self;
    static GMutex mutex;

    g_mutex_lock(&mutex);
    self = g_object_get_data(G_OBJECT(session), "spice-gtk-session");
    if (self == NULL) {
        self = g_object_new(SPICE_TYPE_GTK_SESSION, "session", session, NULL);
        g_object_set_data_full(G_OBJECT(session), "spice-gtk-session", self, g_object_unref);
    }
    g_mutex_unlock(&mutex);

    return SPICE_GTK_SESSION(self);
}

/**
 * spice_gtk_session_copy_to_guest:
 * @self: #SpiceGtkSession
 *
 * Copy client-side clipboard to guest clipboard.
 *
 * Since 0.8
 **/
void spice_gtk_session_copy_to_guest(SpiceGtkSession *self)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(self));
    g_return_if_fail(read_only(self) == FALSE);

    SpiceGtkSessionPrivate *s = self->priv;
    int selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;

    if (s->clip_hasdata[selection] && !s->clip_grabbed[selection]) {
        gtk_clipboard_request_targets(s->clipboard, clipboard_get_targets,
                                      get_weak_ref(self));
    }
}

/**
 * spice_gtk_session_paste_from_guest:
 * @self: #SpiceGtkSession
 *
 * Copy guest clipboard to client-side clipboard.
 *
 * Since 0.8
 **/
void spice_gtk_session_paste_from_guest(SpiceGtkSession *self)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(self));
    g_return_if_fail(read_only(self) == FALSE);

    SpiceGtkSessionPrivate *s = self->priv;
    int selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;

    if (s->nclip_targets[selection] == 0) {
        g_warning("Guest clipboard is not available.");
        return;
    }

    if (!gtk_clipboard_set_with_owner(s->clipboard, s->clip_targets[selection], s->nclip_targets[selection],
                                      clipboard_get, clipboard_clear, G_OBJECT(self))) {
        g_warning("Clipboard grab failed");
        return;
    }
    s->clipboard_by_guest[selection] = TRUE;
    s->clip_hasdata[selection] = FALSE;
}

G_GNUC_INTERNAL
void spice_gtk_session_sync_keyboard_modifiers(SpiceGtkSession *self)
{
    GList *l = NULL, *channels = spice_session_get_channels(self->priv->session);

    for (l = channels; l != NULL; l = l->next) {
        if (SPICE_IS_INPUTS_CHANNEL(l->data)) {
            SpiceInputsChannel *inputs = SPICE_INPUTS_CHANNEL(l->data);
            spice_gtk_session_sync_keyboard_modifiers_for_channel(self, inputs, TRUE);
        }
    }
    g_list_free(channels);
}

G_GNUC_INTERNAL
void spice_gtk_session_set_pointer_grabbed(SpiceGtkSession *self, gboolean grabbed)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(self));

    self->priv->pointer_grabbed = grabbed;
    g_object_notify(G_OBJECT(self), "pointer-grabbed");
}

G_GNUC_INTERNAL
gboolean spice_gtk_session_get_pointer_grabbed(SpiceGtkSession *self)
{
    g_return_val_if_fail(SPICE_IS_GTK_SESSION(self), FALSE);

    return self->priv->pointer_grabbed;
}

G_GNUC_INTERNAL
void spice_gtk_session_set_keyboard_has_focus(SpiceGtkSession *self,
                                                gboolean keyboard_has_focus)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(self));

    self->priv->keyboard_has_focus = keyboard_has_focus;
}

G_GNUC_INTERNAL
void spice_gtk_session_set_mouse_has_pointer(SpiceGtkSession *self,
                                                gboolean mouse_has_pointer)
{
   g_return_if_fail(SPICE_IS_GTK_SESSION(self));
   self->priv->mouse_has_pointer = mouse_has_pointer;
}

G_GNUC_INTERNAL
gboolean spice_gtk_session_get_keyboard_has_focus(SpiceGtkSession *self)
{
    g_return_val_if_fail(SPICE_IS_GTK_SESSION(self), FALSE);

    return self->priv->keyboard_has_focus;
}

G_GNUC_INTERNAL
gboolean spice_gtk_session_get_mouse_has_pointer(SpiceGtkSession *self)
{
    g_return_val_if_fail(SPICE_IS_GTK_SESSION(self), FALSE);

    return self->priv->mouse_has_pointer;
}
