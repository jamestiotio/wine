/*
 * GStreamer muxer backend
 *
 * Copyright 2023 Ziqing Hui for CodeWeavers
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

/*
 * wg_muxer will autoplug gstreamer muxer and parser elements.
 * It creates a pipeline like this:
 *
 *                     -------------------       -------
 *     [my_src 1] ==> |parser 1 (optional)| ==> |       |
 *                     -------------------      |       |
 *                                              |       |
 *                     -------------------      |       |
 *     [my_src 2] ==> |parser 2 (optional)| ==> |       |
 *                     -------------------      |       |
 *                                              | muxer | ==> [my_sink]
 *                                              |       |
 *     [ ...... ]                               |       |
 *                                              |       |
 *                                              |       |
 *                     -------------------      |       |
 *     [my_src n] ==> |parser n (optional)| ==> |       |
 *                     -------------------       -------
 */

#if 0
#pragma makedep unix
#endif

#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"

#include "unix_private.h"

#include "wine/list.h"

struct wg_muxer
{
    GstElement *container, *muxer;
    GstPad *my_sink;
    GstCaps *my_sink_caps;

    pthread_mutex_t mutex;
    guint64 offset;

    struct list streams;
};

struct wg_muxer_stream
{
    struct wg_muxer *muxer;
    struct wg_format format;
    uint32_t id;

    GstPad *my_src;
    GstCaps *my_src_caps, *parser_src_caps;
    GstElement *parser;
    GstSegment segment;

    struct list entry;
};

static struct wg_muxer *get_muxer(wg_muxer_t muxer)
{
    return (struct wg_muxer *)(ULONG_PTR)muxer;
}

static struct wg_muxer_stream *muxer_get_stream_by_id(struct wg_muxer *muxer, DWORD id)
{
    struct wg_muxer_stream *stream;

    LIST_FOR_EACH_ENTRY(stream, &muxer->streams, struct wg_muxer_stream, entry)
    {
        if (stream->id == id)
            return stream;
    }

    return NULL;
}

static bool muxer_try_muxer_factory(struct wg_muxer *muxer, GstElementFactory *muxer_factory)
{
    struct wg_muxer_stream *stream;

    GST_INFO("Trying %"GST_PTR_FORMAT".", muxer_factory);

    LIST_FOR_EACH_ENTRY(stream, &muxer->streams, struct wg_muxer_stream, entry)
    {
        GstCaps *caps = stream->parser ? stream->parser_src_caps : stream->my_src_caps;

        if (!gst_element_factory_can_sink_any_caps(muxer_factory, caps))
        {
            GST_INFO("%"GST_PTR_FORMAT" cannot sink stream %u %p, caps %"GST_PTR_FORMAT,
                    muxer_factory, stream->id, stream, caps);
            return false;
        }
    }

    return true;
}

static GstElement *muxer_find_muxer(struct wg_muxer *muxer)
{
    /* Some muxers are formatter, eg. id3mux. */
    GstElementFactoryListType muxer_type = GST_ELEMENT_FACTORY_TYPE_MUXER | GST_ELEMENT_FACTORY_TYPE_FORMATTER;
    GstElement *element = NULL;
    GList *muxers, *tmp;

    GST_DEBUG("muxer %p.", muxer);

    muxers = find_element_factories(muxer_type, GST_RANK_NONE, NULL, muxer->my_sink_caps);

    for (tmp = muxers; tmp && !element; tmp = tmp->next)
    {
        GstElementFactory *factory = GST_ELEMENT_FACTORY(tmp->data);
        if (muxer_try_muxer_factory(muxer, factory))
            element = factory_create_element(factory);
    }

    gst_plugin_feature_list_free(muxers);

    if (!element)
        GST_WARNING("Failed to find any compatible muxer element.");

    return element;
}

static gboolean muxer_sink_query_cb(GstPad *pad, GstObject *parent, GstQuery *query)
{
    struct wg_muxer *muxer = gst_pad_get_element_private(pad);

    GST_DEBUG("pad %p, parent %p, query %p, muxer %p, type \"%s\".",
            pad, parent, query, muxer, gst_query_type_get_name(query->type));

    switch (query->type)
    {
        case GST_QUERY_SEEKING:
            gst_query_set_seeking(query, GST_FORMAT_BYTES, true, 0, -1);
            return true;
        default:
            GST_WARNING("Ignoring \"%s\" query.", gst_query_type_get_name(query->type));
            return gst_pad_query_default(pad, parent, query);
    }
}

static gboolean muxer_sink_event_cb(GstPad *pad, GstObject *parent, GstEvent *event)
{
    struct wg_muxer *muxer = gst_pad_get_element_private(pad);
    const GstSegment *segment;

    GST_DEBUG("pad %p, parent %p, event %p, muxer %p, type \"%s\".",
            pad, parent, event, muxer, GST_EVENT_TYPE_NAME(event));

    switch (event->type)
    {
        case GST_EVENT_SEGMENT:
            pthread_mutex_lock(&muxer->mutex);

            gst_event_parse_segment(event, &segment);
            if (segment->format != GST_FORMAT_BYTES)
            {
                pthread_mutex_unlock(&muxer->mutex);
                GST_FIXME("Unhandled segment format \"%s\".", gst_format_get_name(segment->format));
                break;
            }
            muxer->offset = segment->start;

            pthread_mutex_unlock(&muxer->mutex);
            break;

        default:
            GST_WARNING("Ignoring \"%s\" event.", GST_EVENT_TYPE_NAME(event));
            break;
    }

    gst_event_unref(event);
    return TRUE;
}

static GstFlowReturn muxer_sink_chain_cb(GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
    GST_FIXME("Stub.");
    return GST_FLOW_ERROR;
}

static void stream_free(struct wg_muxer_stream *stream)
{
    if (stream->parser_src_caps)
        gst_caps_unref(stream->parser_src_caps);
    gst_object_unref(stream->my_src);
    gst_caps_unref(stream->my_src_caps);
    free(stream);
}

NTSTATUS wg_muxer_create(void *args)
{
    struct wg_muxer_create_params *params = args;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    GstPadTemplate *template = NULL;
    struct wg_muxer *muxer;

    /* Create wg_muxer object. */
    if (!(muxer = calloc(1, sizeof(*muxer))))
        return STATUS_NO_MEMORY;
    list_init(&muxer->streams);
    muxer->offset = GST_BUFFER_OFFSET_NONE;
    pthread_mutex_init(&muxer->mutex, NULL);
    if (!(muxer->container = gst_bin_new("wg_muxer")))
        goto out;

    /* Create sink pad. */
    if (!(muxer->my_sink_caps = gst_caps_from_string(params->format)))
    {
        GST_ERROR("Failed to get caps from format string: \"%s\".", params->format);
        goto out;
    }
    if (!(template = gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, muxer->my_sink_caps)))
        goto out;
    muxer->my_sink = gst_pad_new_from_template(template, "wg_muxer_sink");
    if (!muxer->my_sink)
        goto out;
    gst_pad_set_element_private(muxer->my_sink, muxer);
    gst_pad_set_query_function(muxer->my_sink, muxer_sink_query_cb);
    gst_pad_set_event_function(muxer->my_sink, muxer_sink_event_cb);
    gst_pad_set_chain_function(muxer->my_sink, muxer_sink_chain_cb);

    gst_object_unref(template);

    GST_INFO("Created winegstreamer muxer %p.", muxer);
    params->muxer = (wg_transform_t)(ULONG_PTR)muxer;

    return STATUS_SUCCESS;

out:
    if (muxer->my_sink)
        gst_object_unref(muxer->my_sink);
    if (template)
        gst_object_unref(template);
    if (muxer->my_sink_caps)
        gst_caps_unref(muxer->my_sink_caps);
    if (muxer->container)
        gst_object_unref(muxer->container);
    pthread_mutex_destroy(&muxer->mutex);
    free(muxer);

    return status;
}

NTSTATUS wg_muxer_destroy(void *args)
{
    struct wg_muxer *muxer = get_muxer(*(wg_muxer_t *)args);
    struct wg_muxer_stream *stream, *next;

    LIST_FOR_EACH_ENTRY_SAFE(stream, next, &muxer->streams, struct wg_muxer_stream, entry)
    {
        list_remove(&stream->entry);
        stream_free(stream);
    }

    gst_object_unref(muxer->my_sink);
    gst_caps_unref(muxer->my_sink_caps);
    gst_element_set_state(muxer->container, GST_STATE_NULL);
    gst_object_unref(muxer->container);

    pthread_mutex_destroy(&muxer->mutex);

    free(muxer);

    return S_OK;
}

NTSTATUS wg_muxer_add_stream(void *args)
{
    struct wg_muxer_add_stream_params *params = args;
    struct wg_muxer *muxer = get_muxer(params->muxer);
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    GstPadTemplate *template = NULL;
    struct wg_muxer_stream *stream;
    char src_pad_name[64];

    GST_DEBUG("muxer %p, stream %u, format %p.", muxer, params->stream_id, params->format);

    /* Create stream object. */
    if (!(stream = calloc(1, sizeof(*stream))))
        return STATUS_NO_MEMORY;
    stream->muxer = muxer;
    stream->format = *params->format;
    stream->id = params->stream_id;

    /* Create stream my_src pad. */
    if (!(stream->my_src_caps = wg_format_to_caps(params->format)))
        goto out;
    if (!(template = gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, stream->my_src_caps)))
        goto out;
    sprintf(src_pad_name, "wg_muxer_stream_src_%u", stream->id);
    if (!(stream->my_src = gst_pad_new_from_template(template, src_pad_name)))
        goto out;
    gst_pad_set_element_private(stream->my_src, stream);

    /* Create parser. */
    if ((stream->parser = find_element(GST_ELEMENT_FACTORY_TYPE_PARSER, stream->my_src_caps, NULL)))
    {
        GstPad *parser_src;

        if (!gst_bin_add(GST_BIN(muxer->container), stream->parser)
                || !link_src_to_element(stream->my_src, stream->parser))
            goto out;

        parser_src = gst_element_get_static_pad(stream->parser, "src");
        stream->parser_src_caps = gst_pad_query_caps(parser_src, NULL);
        GST_INFO("Created parser %"GST_PTR_FORMAT" for stream %u %p.",
                stream->parser, stream->id, stream);
        gst_object_unref(parser_src);
    }

    /* Add to muxer stream list. */
    list_add_tail(&muxer->streams, &stream->entry);

    gst_object_unref(template);

    GST_INFO("Created winegstreamer muxer stream %p.", stream);

    return STATUS_SUCCESS;

out:
    if (stream->parser)
        gst_object_unref(stream->parser);
    if (stream->my_src)
        gst_object_unref(stream->my_src);
    if (template)
        gst_object_unref(template);
    if (stream->my_src_caps)
        gst_caps_unref(stream->my_src_caps);
    free(stream);

    return status;
}

NTSTATUS wg_muxer_start(void *args)
{
    struct wg_muxer *muxer = get_muxer(*(wg_muxer_t *)args);
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    struct wg_muxer_stream *stream;

    GST_DEBUG("muxer %p.", muxer);

    /* Create muxer element. */
    if (!(muxer->muxer = muxer_find_muxer(muxer))
            || !gst_bin_add(GST_BIN(muxer->container), muxer->muxer))
        return status;

    /* Link muxer element to my_sink */
    if (!link_element_to_sink(muxer->muxer, muxer->my_sink)
            || !gst_pad_set_active(muxer->my_sink, 1))
        return status;

    /* Link each stream to muxer element. */
    LIST_FOR_EACH_ENTRY(stream, &muxer->streams, struct wg_muxer_stream, entry)
    {
        bool link_ok = stream->parser ?
                gst_element_link(stream->parser, muxer->muxer) :
                link_src_to_element(stream->my_src, muxer->muxer);

        if (!link_ok)
            return status;
    }

    /* Set to pause state. */
    if (gst_element_set_state(muxer->container, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE
            || gst_element_get_state(muxer->container, NULL, NULL, -1) == GST_STATE_CHANGE_FAILURE)
        return status;

    /* Active stream my_src pad and push events to prepare for streaming. */
    LIST_FOR_EACH_ENTRY(stream, &muxer->streams, struct wg_muxer_stream, entry)
    {
        char buffer[64];

        sprintf(buffer, "wg_muxer_stream_src_%u", stream->id);
        gst_segment_init(&stream->segment, GST_FORMAT_BYTES);
        if (!gst_pad_set_active(stream->my_src, 1))
            return status;
        if (!push_event(stream->my_src, gst_event_new_stream_start(buffer))
                || !push_event(stream->my_src, gst_event_new_caps(stream->my_src_caps))
                || !push_event(stream->my_src, gst_event_new_segment(&stream->segment)))
            return status;
    }

    GST_DEBUG("Started muxer %p.", muxer);

    return STATUS_SUCCESS;
}

NTSTATUS wg_muxer_push_sample(void *args)
{
    struct wg_muxer_push_sample_params *params = args;
    struct wg_muxer *muxer = get_muxer(params->muxer);
    struct wg_sample *sample = params->sample;
    struct wg_muxer_stream *stream;
    GstFlowReturn ret;
    GstBuffer *buffer;

    if (!(stream = muxer_get_stream_by_id(muxer, params->stream_id)))
        return STATUS_NOT_FOUND;

    /* Create sample data buffer. */
    if (!(buffer = gst_buffer_new_and_alloc(sample->size))
            || !gst_buffer_fill(buffer, 0, wg_sample_data(sample), sample->size))
    {
        GST_ERROR("Failed to allocate input buffer.");
        return STATUS_NO_MEMORY;
    }

    GST_INFO("Copied %u bytes from sample %p to buffer %p.", sample->size, sample, buffer);

    /* Set sample properties. */
    if (sample->flags & WG_SAMPLE_FLAG_HAS_PTS)
        GST_BUFFER_PTS(buffer) = sample->pts * 100;
    if (sample->flags & WG_SAMPLE_FLAG_HAS_DURATION)
        GST_BUFFER_DURATION(buffer) = sample->duration * 100;
    if (!(sample->flags & WG_SAMPLE_FLAG_SYNC_POINT))
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    if (sample->flags & WG_SAMPLE_FLAG_DISCONTINUITY)
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);

    /* Push sample data buffer to stream src pad. */
    if ((ret = gst_pad_push(stream->my_src, buffer)) < 0)
    {
        GST_ERROR("Failed to push buffer %p to pad %s, reason %s.",
                buffer, gst_pad_get_name(stream->my_src), gst_flow_get_name(ret));
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}
