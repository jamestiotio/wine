/*
 * winegstreamer Unix library interface
 *
 * Copyright 2020-2021 Zebediah Figura for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_WINEGSTREAMER_UNIX_PRIVATE_H
#define __WINE_WINEGSTREAMER_UNIX_PRIVATE_H

#include "unixlib.h"

#include <stdbool.h>
#include <gst/gst.h>

/* unixlib.c */

GST_DEBUG_CATEGORY_EXTERN(wine) DECLSPEC_HIDDEN;
#define GST_CAT_DEFAULT wine

extern NTSTATUS wg_init_gstreamer(void *args) DECLSPEC_HIDDEN;

extern GstStreamType stream_type_from_caps(GstCaps *caps) DECLSPEC_HIDDEN;
extern GstElement *create_element(const char *name, const char *plugin_set) DECLSPEC_HIDDEN;
GstElement *factory_create_element(GstElementFactory *factory) DECLSPEC_HIDDEN;
extern GList *find_element_factories(GstElementFactoryListType type, GstRank min_rank,
        GstCaps *element_sink_caps, GstCaps *element_src_caps) DECLSPEC_HIDDEN;
extern GstElement *find_element(GstElementFactoryListType type,
        GstCaps *element_sink_caps, GstCaps *element_src_caps) DECLSPEC_HIDDEN;
extern bool append_element(GstElement *container, GstElement *element, GstElement **first, GstElement **last) DECLSPEC_HIDDEN;
extern bool link_src_to_sink(GstPad *src_pad, GstPad *sink_pad) DECLSPEC_HIDDEN;
extern bool link_src_to_element(GstPad *src_pad, GstElement *element) DECLSPEC_HIDDEN;
extern bool link_element_to_sink(GstElement *element, GstPad *sink_pad) DECLSPEC_HIDDEN;
extern bool push_event(GstPad *pad, GstEvent *event) DECLSPEC_HIDDEN;

/* wg_format.c */

extern void wg_format_from_caps(struct wg_format *format, const GstCaps *caps) DECLSPEC_HIDDEN;
extern bool wg_format_compare(const struct wg_format *a, const struct wg_format *b) DECLSPEC_HIDDEN;
extern GstCaps *wg_format_to_caps(const struct wg_format *format) DECLSPEC_HIDDEN;

/* wg_transform.c */

extern NTSTATUS wg_transform_create(void *args) DECLSPEC_HIDDEN;
extern NTSTATUS wg_transform_destroy(void *args) DECLSPEC_HIDDEN;
extern NTSTATUS wg_transform_set_output_format(void *args) DECLSPEC_HIDDEN;
extern NTSTATUS wg_transform_push_data(void *args) DECLSPEC_HIDDEN;
extern NTSTATUS wg_transform_read_data(void *args) DECLSPEC_HIDDEN;
extern NTSTATUS wg_transform_get_status(void *args) DECLSPEC_HIDDEN;
extern NTSTATUS wg_transform_drain(void *args) DECLSPEC_HIDDEN;
extern NTSTATUS wg_transform_flush(void *args) DECLSPEC_HIDDEN;

/* wg_muxer.c */

extern NTSTATUS wg_muxer_create(void *args) DECLSPEC_HIDDEN;
extern NTSTATUS wg_muxer_destroy(void *args) DECLSPEC_HIDDEN;
extern NTSTATUS wg_muxer_add_stream(void *args) DECLSPEC_HIDDEN;
extern NTSTATUS wg_muxer_start(void *args) DECLSPEC_HIDDEN;
extern NTSTATUS wg_muxer_push_sample(void *args) DECLSPEC_HIDDEN;

/* wg_allocator.c */

static inline BYTE *wg_sample_data(struct wg_sample *sample)
{
    return (BYTE *)(UINT_PTR)sample->data;
}

/* wg_allocator_release_sample can be used to release any sample that was requested. */
typedef struct wg_sample *(*wg_allocator_request_sample_cb)(gsize size, void *context);
extern GstAllocator *wg_allocator_create(void) DECLSPEC_HIDDEN;
extern void wg_allocator_destroy(GstAllocator *allocator) DECLSPEC_HIDDEN;
extern void wg_allocator_provide_sample(GstAllocator *allocator, struct wg_sample *sample) DECLSPEC_HIDDEN;
extern void wg_allocator_release_sample(GstAllocator *allocator, struct wg_sample *sample,
        bool discard_data) DECLSPEC_HIDDEN;

#endif /* __WINE_WINEGSTREAMER_UNIX_PRIVATE_H */
