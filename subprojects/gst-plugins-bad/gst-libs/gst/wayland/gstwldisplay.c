/* GStreamer Wayland Library
 *
 * Copyright (C) 2014 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <drm_fourcc.h>

#include "gstwldisplay.h"
#include "gstwlwindow.h"

#include "fullscreen-shell-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <linux/input.h>

#include <wayland-cursor.h>

#define GST_CAT_DEFAULT gst_wl_display_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

typedef struct _GstWlDisplayPrivate
{
  /* public objects */
  struct wl_display *display;
  struct wl_display *display_wrapper;
  struct wl_event_queue *queue;

  struct wl_list input_list;

  struct wl_cursor_theme *cursor_theme;
  struct wl_cursor *default_cursor;
  struct wl_surface *cursor_surface;
  struct wl_surface *touch_surface;

  /* globals */
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_subcompositor *subcompositor;
  struct xdg_wm_base *xdg_wm_base;
  struct zwp_fullscreen_shell_v1 *fullscreen_shell;
  struct wp_single_pixel_buffer_manager_v1 *single_pixel_buffer;
  struct wl_shm *shm;
  struct wp_viewporter *viewporter;
  struct zwp_linux_dmabuf_v1 *dmabuf;
  GArray *shm_formats;
  GArray *dmabuf_formats;
  GArray *dmabuf_modifiers;

  /* private */
  gboolean own_display;
  GThread *thread;
  GstPoll *wl_fd_poll;

  GRecMutex sync_mutex;

  GMutex buffers_mutex;
  GHashTable *buffers;
  gboolean shutting_down;
} GstWlDisplayPrivate;

G_DEFINE_TYPE_WITH_CODE (GstWlDisplay, gst_wl_display, G_TYPE_OBJECT,
    G_ADD_PRIVATE (GstWlDisplay)
    GST_DEBUG_CATEGORY_INIT (gst_wl_display_debug,
        "wldisplay", 0, "wldisplay library");
    );

void
gst_wl_display_set_touch_surface (GstWlDisplay * self,
    struct wl_surface *touch_surface)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  priv->touch_surface = touch_surface;
}

static void gst_wl_display_finalize (GObject * gobject);

struct input
{
  GstWlDisplay *display;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wl_touch *touch;

  void *pointer_focus;

  struct wl_list link;
};

static void
gst_wl_display_class_init (GstWlDisplayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = gst_wl_display_finalize;
}

static void
gst_wl_display_init (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  priv->shm_formats = g_array_new (FALSE, FALSE, sizeof (uint32_t));
  priv->dmabuf_formats = g_array_new (FALSE, FALSE, sizeof (uint32_t));
  priv->dmabuf_modifiers = g_array_new (FALSE, FALSE, sizeof (guint64));
  priv->wl_fd_poll = gst_poll_new (TRUE);
  priv->buffers = g_hash_table_new (g_direct_hash, g_direct_equal);
  g_mutex_init (&priv->buffers_mutex);
  g_rec_mutex_init (&priv->sync_mutex);

  gst_wl_linux_dmabuf_init_once ();
  gst_wl_shm_init_once ();
  gst_shm_allocator_init_once ();
  gst_wl_videoformat_init_once ();
}

static void
gst_wl_ref_wl_buffer (gpointer key, gpointer value, gpointer user_data)
{
  g_object_ref (value);
}

static void
input_destroy (struct input *input)
{
  if (input->touch)
    wl_touch_destroy (input->touch);
  if (input->pointer)
    wl_pointer_destroy (input->pointer);

  wl_list_remove (&input->link);
  wl_seat_destroy (input->seat);
  free (input);
}

static void
display_destroy_inputs (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  struct input *tmp;
  struct input *input;

  wl_list_for_each_safe (input, tmp, &priv->input_list, link)
      input_destroy (input);
}

static void
gst_wl_display_finalize (GObject * gobject)
{
  GstWlDisplay *self = GST_WL_DISPLAY (gobject);
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  gst_poll_set_flushing (priv->wl_fd_poll, TRUE);
  if (priv->thread)
    g_thread_join (priv->thread);

  display_destroy_inputs (self);

  if (priv->cursor_surface)
    wl_surface_destroy (priv->cursor_surface);

  if (priv->cursor_theme)
    wl_cursor_theme_destroy (priv->cursor_theme);

  /* to avoid buffers being unregistered from another thread
   * at the same time, take their ownership */
  g_mutex_lock (&priv->buffers_mutex);
  priv->shutting_down = TRUE;
  g_hash_table_foreach (priv->buffers, gst_wl_ref_wl_buffer, NULL);
  g_mutex_unlock (&priv->buffers_mutex);

  g_hash_table_foreach (priv->buffers,
      (GHFunc) gst_wl_buffer_force_release_and_unref, NULL);
  g_hash_table_remove_all (priv->buffers);

  g_array_unref (priv->shm_formats);
  g_array_unref (priv->dmabuf_formats);
  g_array_unref (priv->dmabuf_modifiers);
  gst_poll_free (priv->wl_fd_poll);
  g_hash_table_unref (priv->buffers);
  g_mutex_clear (&priv->buffers_mutex);
  g_rec_mutex_clear (&priv->sync_mutex);

  if (priv->viewporter)
    wp_viewporter_destroy (priv->viewporter);

  if (priv->shm)
    wl_shm_destroy (priv->shm);

  if (priv->dmabuf)
    zwp_linux_dmabuf_v1_destroy (priv->dmabuf);

  if (priv->xdg_wm_base)
    xdg_wm_base_destroy (priv->xdg_wm_base);

  if (priv->fullscreen_shell)
    zwp_fullscreen_shell_v1_release (priv->fullscreen_shell);

  if (priv->single_pixel_buffer)
    wp_single_pixel_buffer_manager_v1_destroy (priv->single_pixel_buffer);

  if (priv->compositor)
    wl_compositor_destroy (priv->compositor);

  if (priv->subcompositor)
    wl_subcompositor_destroy (priv->subcompositor);

  if (priv->registry)
    wl_registry_destroy (priv->registry);

  if (priv->display_wrapper)
    wl_proxy_wrapper_destroy (priv->display_wrapper);

  if (priv->queue)
    wl_event_queue_destroy (priv->queue);

  if (priv->own_display) {
    wl_display_flush (priv->display);
    wl_display_disconnect (priv->display);
  }

  G_OBJECT_CLASS (gst_wl_display_parent_class)->finalize (gobject);
}

static void
shm_format (void *data, struct wl_shm *wl_shm, uint32_t format)
{
  GstWlDisplay *self = data;
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  g_array_append_val (priv->shm_formats, format);
}

static const struct wl_shm_listener shm_listener = {
  shm_format
};

static void
dmabuf_format (void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
    uint32_t format)
{
}

static void
dmabuf_modifier (void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
    uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
  GstWlDisplay *self = data;
  guint64 modifier = (guint64) modifier_hi << 32 | modifier_lo;
  static uint32_t last_format = 0;

  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  if (gst_wl_dmabuf_format_to_video_format (format) != GST_VIDEO_FORMAT_UNKNOWN) {
    GstVideoFormat gst_format = gst_wl_dmabuf_format_to_video_format (format);
    const guint32 fourcc = gst_video_dma_drm_fourcc_from_format (gst_format);

    /*
     * Ignore unsupported formats along with implicit modifiers. Implicit
     * modifiers have been source of garbled output for many many years and it
     * was decided that we prefer disabling zero-copy over risking a bad output.
     */
    if (fourcc == DRM_FORMAT_INVALID || modifier == DRM_FORMAT_MOD_INVALID)
      return;

    if (last_format == 0) {
      GST_INFO ("===== All DMA Formats With Modifiers =====");
      GST_INFO ("| Gst Format   | DRM Format              |");
    }

    if (last_format != format) {
      GST_INFO ("|-----------------------------------------");
      last_format = format;
    }

    GST_INFO ("| %-12s | %-23s |",
        (modifier == 0) ? gst_video_format_to_string (gst_format) : "",
        gst_video_dma_drm_fourcc_to_string (fourcc, modifier));

    g_array_append_val (priv->dmabuf_formats, format);
    g_array_append_val (priv->dmabuf_modifiers, modifier);
  }
#else
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  if (modifier == DRM_FORMAT_MOD_INVALID)
    modifier = DRM_FORMAT_MOD_LINEAR;

  g_array_append_val (priv->dmabuf_formats, format);
  g_array_append_val (priv->dmabuf_modifiers, modifier);
#endif
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
  dmabuf_format,
  dmabuf_modifier,
};

gboolean
gst_wl_display_check_format_for_shm (GstWlDisplay * self,
    const GstVideoInfo * video_info)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  GstVideoFormat format = GST_VIDEO_INFO_FORMAT (video_info);
  enum wl_shm_format shm_fmt;
  GArray *formats;
  guint i;

  shm_fmt = gst_video_format_to_wl_shm_format (format);
  if (shm_fmt == (enum wl_shm_format) -1)
    return FALSE;

  formats = priv->shm_formats;
  for (i = 0; i < formats->len; i++) {
    if (g_array_index (formats, uint32_t, i) == shm_fmt)
      return TRUE;
  }

  return FALSE;
}

gboolean
gst_wl_display_check_format_for_dmabuf (GstWlDisplay * self,
    const guint fourcc, const guint64 modifier)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  GArray *formats, *modifiers;
  guint i;

  if (!priv->dmabuf)
    return FALSE;

  formats = priv->dmabuf_formats;
  modifiers = priv->dmabuf_modifiers;
  for (i = 0; i < formats->len; i++) {
    if (g_array_index (formats, uint32_t, i) == fourcc) {
      if (g_array_index (modifiers, guint64, i) == modifier) {
        return TRUE;
      }
    }
  }

  return FALSE;
}

static void
handle_xdg_wm_base_ping (void *user_data, struct xdg_wm_base *xdg_wm_base,
    uint32_t serial)
{
  xdg_wm_base_pong (xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
  handle_xdg_wm_base_ping
};

static void
display_set_cursor (GstWlDisplay *self, struct wl_pointer *pointer,
    uint32_t serial)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  struct wl_buffer *buffer;
  struct wl_cursor_image *image;

  if (!priv->default_cursor)
    return;

  if (!priv->cursor_surface) {
      priv->cursor_surface =
          wl_compositor_create_surface (priv->compositor);
      if (!priv->cursor_surface)
        return;
  }

  image = priv->default_cursor->images[0];
  buffer = wl_cursor_image_get_buffer (image);
  if (!buffer)
    return;

  wl_pointer_set_cursor (pointer, serial,
      priv->cursor_surface, image->hotspot_x, image->hotspot_y);
  wl_surface_attach (priv->cursor_surface, buffer, 0, 0);
  wl_surface_damage (priv->cursor_surface, 0, 0,
      image->width, image->height);
  wl_surface_commit (priv->cursor_surface);
}

static void
pointer_handle_enter (void *data, struct wl_pointer *pointer,
    uint32_t serial, struct wl_surface *surface,
    wl_fixed_t sx_w, wl_fixed_t sy_w)
{
  struct input *input = data;
  GstWlDisplay *self = input->display;
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  GstWlWindow *window;

  if (!surface) {
    /* enter event for a window we've just destroyed */
    return;
  }

  if (surface != priv->touch_surface) {
    /* Ignoring input event from other surfaces */
    return;
  }

  window = wl_surface_get_user_data (surface);
  if (!window || !gst_wl_window_is_toplevel (window)) {
    /* Ignoring input event from subsurface */
    return;
  }

  input->pointer_focus = window;
  display_set_cursor (self, pointer, serial);
}

static void
pointer_handle_leave (void *data, struct wl_pointer *pointer,
    uint32_t serial, struct wl_surface *surface)
{
  struct input *input = data;

  if (input->pointer_focus) {
    input->pointer_focus = NULL;
    wl_pointer_set_cursor (pointer, serial, NULL, 0, 0);
  }
}

static void
pointer_handle_motion (void *data, struct wl_pointer *pointer,
    uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
}

static void
pointer_handle_button (void *data, struct wl_pointer *pointer, uint32_t serial,
    uint32_t time, uint32_t button, uint32_t state)
{
  struct input *input = data;
  GstWlWindow *window;

  window = input->pointer_focus;
  if (!window)
    return;

  if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED)
    gst_wl_window_toplevel_move (window, input->seat, serial);
}

static void
pointer_handle_axis (void *data, struct wl_pointer *wl_pointer,
    uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
  pointer_handle_enter,
  pointer_handle_leave,
  pointer_handle_motion,
  pointer_handle_button,
  pointer_handle_axis,
};

static void
touch_handle_down (void *data, struct wl_touch *wl_touch,
    uint32_t serial, uint32_t time, struct wl_surface *surface,
    int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
  struct input *input = data;
  GstWlDisplay *self = input->display;
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  GstWlWindow *window;

  if (!surface) {
    /* enter event for a window we've just destroyed */
    return;
  }

  if (surface != priv->touch_surface) {
    /* Ignoring input event from other surfaces */
    return;
  }

  window = wl_surface_get_user_data (surface);
  if (!window || !gst_wl_window_is_toplevel (window)) {
    /* Ignoring input event from subsurface */
    return;
  }

  gst_wl_window_toplevel_move (window, input->seat, serial);
}

static void
touch_handle_up (void *data, struct wl_touch *wl_touch,
    uint32_t serial, uint32_t time, int32_t id)
{
}

static void
touch_handle_motion (void *data, struct wl_touch *wl_touch,
    uint32_t time, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
}

static void
touch_handle_frame (void *data, struct wl_touch *wl_touch)
{
}

static void
touch_handle_cancel (void *data, struct wl_touch *wl_touch)
{
}

static const struct wl_touch_listener touch_listener = {
  touch_handle_down,
  touch_handle_up,
  touch_handle_motion,
  touch_handle_frame,
  touch_handle_cancel,
};

static void
seat_handle_capabilities (void *data, struct wl_seat *seat,
    enum wl_seat_capability caps)
{
  struct input *input = data;

  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input->pointer) {
    input->pointer = wl_seat_get_pointer (seat);
    wl_pointer_add_listener (input->pointer, &pointer_listener, input);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && input->pointer) {
    wl_pointer_destroy (input->pointer);
    input->pointer = NULL;
  }

  if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !input->touch) {
    input->touch = wl_seat_get_touch (seat);
    wl_touch_add_listener (input->touch, &touch_listener, input);
  } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && input->touch) {
    wl_touch_destroy (input->touch);
    input->touch = NULL;
  }
}

static const struct wl_seat_listener seat_listener = {
  seat_handle_capabilities,
};

static void
display_add_input (GstWlDisplay *self, uint32_t id)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  struct input *input;

  input = calloc (1, sizeof (*input));
  if (input == NULL) {
    GST_ERROR ("Error out of memory");
    return;
  }

  input->display = self;

  input->seat = wl_registry_bind (priv->registry, id, &wl_seat_interface, 1);

  wl_seat_add_listener (input->seat, &seat_listener, input);
  wl_seat_set_user_data (input->seat, input);

  wl_list_insert (priv->input_list.prev, &input->link);
}

static void
registry_handle_global (void *data, struct wl_registry *registry,
    uint32_t id, const char *interface, uint32_t version)
{
  GstWlDisplay *self = data;
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  if (g_strcmp0 (interface, "wl_compositor") == 0) {
    priv->compositor = wl_registry_bind (registry, id, &wl_compositor_interface,
        MIN (version, 4));
  } else if (g_strcmp0 (interface, "wl_subcompositor") == 0) {
    priv->subcompositor =
        wl_registry_bind (registry, id, &wl_subcompositor_interface, 1);
  } else if (g_strcmp0 (interface, "xdg_wm_base") == 0) {
    priv->xdg_wm_base =
        wl_registry_bind (registry, id, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener (priv->xdg_wm_base, &xdg_wm_base_listener, self);
  } else if (g_strcmp0 (interface, "zwp_fullscreen_shell_v1") == 0) {
    priv->fullscreen_shell = wl_registry_bind (registry, id,
        &zwp_fullscreen_shell_v1_interface, 1);
  } else if (g_strcmp0 (interface, "wl_shm") == 0) {
    priv->shm = wl_registry_bind (registry, id, &wl_shm_interface, 1);
    wl_shm_add_listener (priv->shm, &shm_listener, self);

    priv->cursor_theme = wl_cursor_theme_load (NULL, 32, priv->shm);
    if (!priv->cursor_theme) {
      GST_ERROR ("Error loading default cursor theme");
    } else {
      priv->default_cursor =
          wl_cursor_theme_get_cursor (priv->cursor_theme, "left_ptr");
      if (!priv->default_cursor)
        GST_ERROR ("Error loading default left cursor pointer");
    }
  } else if (g_strcmp0 (interface, "wl_seat") == 0) {
    display_add_input (self, id);
  } else if (g_strcmp0 (interface, "wp_viewporter") == 0) {
    priv->viewporter =
        wl_registry_bind (registry, id, &wp_viewporter_interface, 1);
  } else if (g_strcmp0 (interface, "zwp_linux_dmabuf_v1") == 0) {
    priv->dmabuf =
        wl_registry_bind (registry, id, &zwp_linux_dmabuf_v1_interface, 3);
    zwp_linux_dmabuf_v1_add_listener (priv->dmabuf, &dmabuf_listener, self);
  } else if (g_strcmp0 (interface, "wp_single_pixel_buffer_manager_v1") == 0) {
    priv->single_pixel_buffer =
        wl_registry_bind (registry, id,
        &wp_single_pixel_buffer_manager_v1_interface, 1);
  }
}

static void
registry_handle_global_remove (void *data, struct wl_registry *registry,
    uint32_t name)
{
  /* temporarily do nothing */
}

static const struct wl_registry_listener registry_listener = {
  registry_handle_global,
  registry_handle_global_remove
};

static gpointer
gst_wl_display_thread_run (gpointer data)
{
  GstWlDisplay *self = data;
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  GstPollFD pollfd = GST_POLL_FD_INIT;

  pollfd.fd = wl_display_get_fd (priv->display);
  gst_poll_add_fd (priv->wl_fd_poll, &pollfd);
  gst_poll_fd_ctl_read (priv->wl_fd_poll, &pollfd, TRUE);

  /* main loop */
  while (1) {
    g_rec_mutex_lock (&priv->sync_mutex);
    while (wl_display_prepare_read_queue (priv->display, priv->queue) != 0)
      wl_display_dispatch_queue_pending (priv->display, priv->queue);
    g_rec_mutex_unlock (&priv->sync_mutex);
    wl_display_flush (priv->display);

    if (gst_poll_wait (priv->wl_fd_poll, GST_CLOCK_TIME_NONE) < 0) {
      gboolean normal = (errno == EBUSY);
      wl_display_cancel_read (priv->display);
      if (normal)
        break;
      else
        goto error;
    }
    if (wl_display_read_events (priv->display) == -1)
      goto error;

    g_rec_mutex_lock (&priv->sync_mutex);
    wl_display_dispatch_queue_pending (priv->display, priv->queue);
    g_rec_mutex_unlock (&priv->sync_mutex);
  }

  return NULL;

error:
  GST_ERROR ("Error communicating with the wayland server");
  return NULL;
}

GstWlDisplay *
gst_wl_display_new (const gchar * name, GError ** error)
{
  struct wl_display *display;

  display = wl_display_connect (name);

  if (!display) {
    *error = g_error_new (g_quark_from_static_string ("GstWlDisplay"), 0,
        "Failed to connect to the wayland display '%s'",
        name ? name : "(default)");
    return NULL;
  } else {
    return gst_wl_display_new_existing (display, TRUE, error);
  }
}

GstWlDisplay *
gst_wl_display_new_existing (struct wl_display *display,
    gboolean take_ownership, GError ** error)
{
  GstWlDisplay *self;
  GstWlDisplayPrivate *priv;
  GError *err = NULL;
  gint i;

  g_return_val_if_fail (display != NULL, NULL);

  self = g_object_new (GST_TYPE_WL_DISPLAY, NULL);
  priv = gst_wl_display_get_instance_private (self);
  priv->display = display;
  priv->display_wrapper = wl_proxy_create_wrapper (display);
  priv->own_display = take_ownership;

  wl_list_init (&priv->input_list);

#ifdef HAVE_WL_EVENT_QUEUE_NAME
  priv->queue = wl_display_create_queue_with_name (priv->display,
      "GStreamer display queue");
#else
  priv->queue = wl_display_create_queue (priv->display);
#endif
  wl_proxy_set_queue ((struct wl_proxy *) priv->display_wrapper, priv->queue);
  priv->registry = wl_display_get_registry (priv->display_wrapper);
  wl_registry_add_listener (priv->registry, &registry_listener, self);

  /* we need exactly 2 roundtrips to discover global objects and their state */
  for (i = 0; i < 2; i++) {
    if (wl_display_roundtrip_queue (priv->display, priv->queue) < 0) {
      *error = g_error_new (g_quark_from_static_string ("GstWlDisplay"), 0,
          "Error communicating with the wayland display");
      g_object_unref (self);
      return NULL;
    }
  }

  /* verify we got all the required interfaces */
#define VERIFY_INTERFACE_EXISTS(var, interface) \
  if (!priv->var) { \
    g_set_error (error, g_quark_from_static_string ("GstWlDisplay"), 0, \
        "Could not bind to " interface ". Either it is not implemented in " \
        "the compositor, or the implemented version doesn't match"); \
    g_object_unref (self); \
    return NULL; \
  }

  VERIFY_INTERFACE_EXISTS (compositor, "wl_compositor");
  VERIFY_INTERFACE_EXISTS (subcompositor, "wl_subcompositor");
  VERIFY_INTERFACE_EXISTS (shm, "wl_shm");

#undef VERIFY_INTERFACE_EXISTS

  /* We make the viewporter optional even though it may cause bad display.
   * This is so one can test wayland display on older compositor or on
   * compositor that don't implement this extension. */
  if (!priv->viewporter) {
    g_warning ("Wayland compositor is missing the ability to scale, video "
        "display may not work properly.");
  }

  if (!priv->dmabuf) {
    g_warning ("Could not bind to zwp_linux_dmabuf_v1");
  }

  if (!priv->xdg_wm_base && !priv->fullscreen_shell) {
    /* If wl_surface and wl_display are passed via GstContext
     * xdg_shell and zwp_fullscreen_shell are not used.
     * In this case is correct to continue.
     */
    g_warning ("Could not bind to either xdg_wm_base or zwp_fullscreen_shell, "
        "video display may not work properly.");
  }

  priv->thread = g_thread_try_new ("GstWlDisplay", gst_wl_display_thread_run,
      self, &err);
  if (err) {
    g_propagate_prefixed_error (error, err,
        "Failed to start thread for the display's events");
    g_object_unref (self);
    return NULL;
  }

  return self;
}

void
gst_wl_display_register_buffer (GstWlDisplay * self, gpointer gstmem,
    gpointer wlbuffer)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  g_assert (!priv->shutting_down);

  GST_TRACE_OBJECT (self, "registering GstWlBuffer %p to GstMem %p",
      wlbuffer, gstmem);

  g_mutex_lock (&priv->buffers_mutex);
  g_hash_table_replace (priv->buffers, gstmem, wlbuffer);
  g_mutex_unlock (&priv->buffers_mutex);
}

gpointer
gst_wl_display_lookup_buffer (GstWlDisplay * self, gpointer gstmem)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  gpointer wlbuffer;

  g_mutex_lock (&priv->buffers_mutex);
  wlbuffer = g_hash_table_lookup (priv->buffers, gstmem);
  g_mutex_unlock (&priv->buffers_mutex);
  return wlbuffer;
}

void
gst_wl_display_unregister_buffer (GstWlDisplay * self, gpointer gstmem)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  GST_TRACE_OBJECT (self, "unregistering GstWlBuffer owned by %p", gstmem);

  g_mutex_lock (&priv->buffers_mutex);
  if (G_LIKELY (!priv->shutting_down))
    g_hash_table_remove (priv->buffers, gstmem);
  g_mutex_unlock (&priv->buffers_mutex);
}

/* gst_wl_display_sync
 *
 * A syncronized version of `wl_display_sink` that ensures that the
 * callback will not be dispatched before the listener has been attached.
 */
struct wl_callback *
gst_wl_display_sync (GstWlDisplay * self,
    const struct wl_callback_listener *listener, gpointer data)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);
  struct wl_callback *callback;

  g_rec_mutex_lock (&priv->sync_mutex);

  callback = wl_display_sync (priv->display_wrapper);
  if (callback && listener)
    wl_callback_add_listener (callback, listener, data);

  g_rec_mutex_unlock (&priv->sync_mutex);

  return callback;
}

/* gst_wl_display_callback_destroy
 *
 * A syncronized version of `wl_callback_destroy` that ensures that the
 * once this function returns, the callback will either have already completed,
 * or will never be called.
 */
void
gst_wl_display_callback_destroy (GstWlDisplay * self,
    struct wl_callback **callback)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  g_rec_mutex_lock (&priv->sync_mutex);

  if (*callback) {
    wl_callback_destroy (*callback);
    *callback = NULL;
  }

  g_rec_mutex_unlock (&priv->sync_mutex);
}

struct wl_display *
gst_wl_display_get_display (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->display;
}

struct wl_event_queue *
gst_wl_display_get_event_queue (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->queue;
}

struct wl_compositor *
gst_wl_display_get_compositor (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->compositor;
}

struct wl_subcompositor *
gst_wl_display_get_subcompositor (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->subcompositor;
}

struct xdg_wm_base *
gst_wl_display_get_xdg_wm_base (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->xdg_wm_base;
}

struct zwp_fullscreen_shell_v1 *
gst_wl_display_get_fullscreen_shell_v1 (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->fullscreen_shell;
}

struct wp_viewporter *
gst_wl_display_get_viewporter (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->viewporter;
}

struct wl_shm *
gst_wl_display_get_shm (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->shm;
}

GArray *
gst_wl_display_get_shm_formats (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->shm_formats;
}

struct zwp_linux_dmabuf_v1 *
gst_wl_display_get_dmabuf_v1 (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->dmabuf;
}

GArray *
gst_wl_display_get_dmabuf_modifiers (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->dmabuf_modifiers;
}

GArray *
gst_wl_display_get_dmabuf_formats (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->dmabuf_formats;
}

struct wp_single_pixel_buffer_manager_v1 *
gst_wl_display_get_single_pixel_buffer_manager_v1 (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->single_pixel_buffer;
}

gboolean
gst_wl_display_has_own_display (GstWlDisplay * self)
{
  GstWlDisplayPrivate *priv = gst_wl_display_get_instance_private (self);

  return priv->own_display;
}
