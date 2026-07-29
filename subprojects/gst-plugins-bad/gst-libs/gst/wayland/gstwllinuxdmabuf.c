/* GStreamer Wayland Library
 *
 * Copyright (C) 2016 STMicroelectronics SA
 * Copyright (C) 2016 Fabien Dessenne <fabien.dessenne@st.com>
 * Copyright (C) 2022 Collabora Ltd.
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

#include "gstwllinuxdmabuf.h"

#include <gst/video/video-info-dma.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"

GST_DEBUG_CATEGORY (gst_wl_dmabuf_debug);
#define GST_CAT_DEFAULT gst_wl_dmabuf_debug

void
gst_wl_linux_dmabuf_init_once (void)
{
  static gsize _init = 0;

  if (g_once_init_enter (&_init)) {
    GST_DEBUG_CATEGORY_INIT (gst_wl_dmabuf_debug, "wl_dmabuf", 0,
        "wl_dmabuf library");

    g_once_init_leave (&_init, 1);
  }
}

typedef struct
{
  struct wl_buffer *wbuf;
} ConstructBufferData;

static gint
get_drm_stride (const GstVideoFormatInfo * finfo, const gint * strides,
    gint plane)
{
  gint stride = strides[plane];

  if (!GST_VIDEO_FORMAT_INFO_IS_TILED (finfo))
    return stride;

  return GST_VIDEO_TILE_X_TILES (stride) *
      GST_VIDEO_FORMAT_INFO_TILE_STRIDE (finfo, plane);
}

struct wl_buffer *
gst_wl_linux_dmabuf_construct_wl_buffer (GstBuffer * buf,
    GstWlDisplay * display, const GstVideoInfoDmaDrm * drm_info)
{
  GstMemory *mem;
  guint fourcc;
  guint64 modifier;
  guint i, width = 0, height = 0;
  GstVideoInfo info;
  const GstVideoFormatInfo *finfo;
  const gsize *offsets = NULL;
  const gint *strides = NULL;
  GstVideoMeta *vmeta;
  guint nplanes = 0, flags = 0;
  gfloat stride_scale = 1.0f;
  struct zwp_linux_buffer_params_v1 *params;
  ConstructBufferData data = { 0 };

  g_return_val_if_fail (gst_wl_display_check_format_for_dmabuf (display,
          drm_info->drm_fourcc, drm_info->drm_modifier), NULL);

  mem = gst_buffer_peek_memory (buf, 0);
  fourcc = drm_info->drm_fourcc;
  modifier = drm_info->drm_modifier;

  if (!gst_video_info_dma_drm_to_video_info (drm_info, &info)) {
    GST_ERROR_OBJECT (display, "GstVideoMeta is needed to carry DMABuf using "
        "'memory:DMABuf' caps feature.");
    data.wbuf = NULL;
    goto out;
  }

  vmeta = gst_buffer_get_video_meta (buf);
  if (vmeta) {
    finfo = drm_info->vinfo.finfo;
    width = vmeta->width;
    height = vmeta->height;
    nplanes = vmeta->n_planes;
    offsets = vmeta->offset;
    strides = vmeta->stride;
  } else {
    finfo = info.finfo;
    nplanes = GST_VIDEO_INFO_N_PLANES (&info);
    width = info.width;
    height = info.height;
    offsets = info.offset;
    strides = info.stride;
  }

  GST_DEBUG_OBJECT (display,
      "Creating wl_buffer from DMABuf of size %" G_GSSIZE_FORMAT
      " (%d x %d), DRM fourcc %" GST_FOURCC_FORMAT, gst_buffer_get_size (buf),
      width, height, GST_FOURCC_ARGS (fourcc));

  if (nplanes != 1) {
    /* HACK: Mali uses these formats instead */
    switch (fourcc) {
    case DRM_FORMAT_YUV420_8BIT:
    case DRM_FORMAT_YUV420_10BIT:
      nplanes = 1;
      stride_scale = 1.5;
      break;
    case DRM_FORMAT_YUYV:
      nplanes = 1;
      stride_scale = 2;
      break;
    }
  }

  /* Creation and configuration of planes  */
  params = zwp_linux_dmabuf_v1_create_params (gst_wl_display_get_dmabuf_v1
      (display));

  for (i = 0; i < nplanes; i++) {
    guint offset, stride, mem_idx, length;
    gsize skip;

    offset = offsets[i];
    stride = get_drm_stride (finfo, strides, i);
    stride *= stride_scale;
    if (gst_buffer_find_memory (buf, offset, 1, &mem_idx, &length, &skip)) {
      GstMemory *m = gst_buffer_peek_memory (buf, mem_idx);
      gint fd = gst_dmabuf_memory_get_fd (m);
      zwp_linux_buffer_params_v1_add (params, fd, i, m->offset + skip,
          stride, modifier >> 32, modifier & G_GUINT64_CONSTANT (0x0ffffffff));
    } else {
      GST_ERROR_OBJECT (mem->allocator, "memory does not seem to contain "
          "enough data for the specified format");
      zwp_linux_buffer_params_v1_destroy (params);
      data.wbuf = NULL;
      goto out;
    }
  }

  if (GST_BUFFER_FLAG_IS_SET (buf, GST_VIDEO_BUFFER_FLAG_INTERLACED)) {
    GST_DEBUG_OBJECT (mem->allocator, "interlaced buffer");
    flags = ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_INTERLACED;

    if (!GST_BUFFER_FLAG_IS_SET (buf, GST_VIDEO_BUFFER_FLAG_TFF)) {
      GST_DEBUG_OBJECT (mem->allocator, "with bottom field first");
      flags |= ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_BOTTOM_FIRST;
    }
  }

  data.wbuf =
      zwp_linux_buffer_params_v1_create_immed (params, width, height, fourcc,
      flags);
  zwp_linux_buffer_params_v1_destroy (params);

out:
  if (!data.wbuf) {
    GST_ERROR_OBJECT (mem->allocator, "can't create linux-dmabuf buffer");
  } else {
    GST_DEBUG_OBJECT (mem->allocator, "created linux_dmabuf wl_buffer (%p):"
        "%dx%d, fmt=%" GST_FOURCC_FORMAT ", %d planes",
        data.wbuf, width, height, GST_FOURCC_ARGS (fourcc), nplanes);
  }

  return data.wbuf;
}
