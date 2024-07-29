

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../../gst/gst-i18n-lib.h"

#include <gst/gst.h>
#include <glib/gstdio.h>
#include <stdio.h> /* for fseeko() */
#ifdef HAVE_STDIO_EXT_H
#include <stdio_ext.h> /* for __fbufsize, for debugging */
#endif
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include "gstprerecordsink.h"
#include <string.h>
#include <gst/gstminiobject.h>
#include <sys/types.h>
#include <fcntl.h>

#ifdef G_OS_WIN32
#include <io.h> /* lseek, open, close, read */
#undef lseek
#define lseek _lseeki64
#undef off_t
#define off_t guint64
#undef ftruncate
#define ftruncate _chsize
#undef fsync
#define fsync _commit
#ifdef _MSC_VER        /* Check if we are using MSVC, fileno is deprecated in favour */
#define fileno _fileno /* of _fileno */
#endif
#endif

#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "gstelements_private.h"
#include "gstprerecordsink.h"
#include "gstcoreelementselements.h"

#define GST_BUFFER_DURATION(buf) (GST_BUFFER_CAST(buf)->duration)
#define factor 1.95
#define tenrten 100000000000

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE("sink",
                                                                   GST_PAD_SINK,
                                                                   GST_PAD_ALWAYS,
                                                                   GST_STATIC_CAPS_ANY);

#define GST_TYPE_PRERECORD_SINK_BUFFER_MODE (gst_prerecord_sink_buffer_mode_get_type())
#define GST_TYPE_PRERECORD_SINK_BUFFERING (gst_prerecord_sink_buffering_get_type())


static GType
gst_prerecord_sink_buffering_get_type(void)
{
  static GType buffering_type = 0;
  static const GEnumValue buffering[] = {
      {GST_PRERECORD_SINK_BUFFERING_PRERECORD, "Prerecording", "default"},
      {GST_PRERECORD_SINK_BUFFERING_RECORDING, "Recording", "Recording/SinglePress"},
      {GST_PRERECORD_SINK_BUFFERING_POSTRECORD, "Postrecording", "PostRecord/DoublePress"},
      {0, NULL, NULL},
  };

  if (!buffering_type)
  {
    buffering_type =
        g_enum_register_static("GstPrerecordSinkBuffering", buffering);
  }
  return buffering_type;
}



static GType
gst_prerecord_sink_buffer_mode_get_type(void)
{
  static GType buffer_mode_type = 0;
  static const GEnumValue buffer_mode[] = {
      {GST_PRERECORD_SINK_BUFFER_MODE_DEFAULT, "Default buffering", "default"},
      {GST_PRERECORD_SINK_BUFFER_MODE_FULL, "Fully buffered", "full"},
      {GST_PRERECORD_SINK_BUFFER_MODE_LINE, "Line buffered (deprecated, like full)",
       "line"},
      {GST_PRERECORD_SINK_BUFFER_MODE_UNBUFFERED, "Unbuffered", "unbuffered"},
      {0, NULL, NULL},
  };

  if (!buffer_mode_type)
  {
    buffer_mode_type =
        g_enum_register_static("GstPrerecordSinkBufferMode", buffer_mode);
  }
  return buffer_mode_type;
}

GST_DEBUG_CATEGORY_STATIC(gst_prerecord_sink_debug);
#define GST_CAT_DEFAULT gst_prerecord_sink_debug

#define DEFAULT_LOCATION NULL
#define DEFAULT_BUFFER_MODE GST_PRERECORD_SINK_BUFFER_MODE_DEFAULT
#define DEFAULT_BUFFERING GST_PRERECORD_SINK_BUFFERING_PRERECORD
#define DEFAULT_BUFFER_SIZE 64 * 1024
#define DEFAULT_APPEND FALSE
#define DEFAULT_O_SYNC FALSE
#define DEFAULT_MAX_TRANSIENT_ERROR_TIMEOUT 0
#define DEFAULT_PRE_RECORD 30
#define DEFAULT_POST_RECORD 30
#define DEFAULT_BUFFERING GST_PRERECORD_SINK_BUFFERING_PRERECORD

enum
{
  PROP_0,
  PROP_LOCATION,
  PROP_BUFFER_MODE,
  PROP_BUFFER_SIZE,
  PROP_APPEND,
  PROP_O_SYNC,
  PROP_MAX_TRANSIENT_ERROR_TIMEOUT,
  PROP_LAST,
  PROP_PRE_RECORD,
  PROP_POST_RECORD,
  PROP_BUFFERING
};

static FILE *
gst_fopen(const gchar *prerecordname, const gchar *mode, gboolean o_sync)
{
  FILE *retval;
#ifdef G_OS_WIN32
  retval = g_fopen(prerecordname, mode);
  return retval;
#else
  int fd;
  int flags = O_CREAT | O_WRONLY;

  if (strcmp(mode, "wb") == 0)
    flags |= O_TRUNC;
  else if (strcmp(mode, "ab") == 0)
    flags |= O_APPEND;
  else
    g_assert_not_reached();

  if (o_sync)
    flags |= O_SYNC;

  fd = open(prerecordname, flags, 0666);

  if (fd < 0)
    return NULL;

  retval = fdopen(fd, mode);
  return retval;
#endif
}

static void gst_prerecord_sink_dispose(GObject *object);

static void gst_prerecord_sink_set_property(GObject *object, guint prop_id,
                                            const GValue *value, GParamSpec *pspec);
static void gst_prerecord_sink_get_property(GObject *object, guint prop_id,
                                            GValue *value, GParamSpec *pspec);

static gboolean gst_prerecord_sink_open_prerecord(GstPrerecordSink *sink);
static void gst_prerecord_sink_close_prerecord(GstPrerecordSink *sink);

static gboolean gst_prerecord_sink_start(GstBaseSink *sink);
static gboolean gst_prerecord_sink_stop(GstBaseSink *sink);
static gboolean gst_prerecord_sink_event(GstBaseSink *sink, GstEvent *event);
static GstFlowReturn gst_prerecord_sink_render(GstBaseSink *sink,
                                               GstBuffer *buffer);
static GstFlowReturn gst_prerecord_sink_render_list(GstBaseSink *sink,
                                                    GstBufferList *list);
static gboolean gst_prerecord_sink_unlock(GstBaseSink *sink);
static gboolean gst_prerecord_sink_unlock_stop(GstBaseSink *sink);

static gboolean gst_prerecord_sink_do_seek(GstPrerecordSink *prerecordsink,
                                           guint64 new_offset);
static gboolean gst_prerecord_sink_get_current_offset(GstPrerecordSink *prerecordsink,
                                                      guint64 *p_pos);

static gboolean gst_prerecord_sink_query(GstBaseSink *bsink, GstQuery *query);

static void gst_prerecord_sink_uri_handler_init(gpointer g_iface,
                                                gpointer iface_data);

static GstFlowReturn gst_prerecord_sink_flush_buffer(GstPrerecordSink *prerecordsink);

#define _do_init                                                                    \
  G_IMPLEMENT_INTERFACE(GST_TYPE_URI_HANDLER, gst_prerecord_sink_uri_handler_init); \
  GST_DEBUG_CATEGORY_INIT(gst_prerecord_sink_debug, "prerecordsink", 0, "prerecordsink element");
#define gst_prerecord_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(GstPrerecordSink, gst_prerecord_sink, GST_TYPE_BASE_SINK,
                        _do_init);
GST_ELEMENT_REGISTER_DEFINE(prerecordsink, "prerecordsink", GST_RANK_PRIMARY,
                            GST_TYPE_PRERECORD_SINK);

static void
gst_prerecord_sink_class_init(GstPrerecordSinkClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);
  GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS(klass);

  gobject_class->dispose = gst_prerecord_sink_dispose;

  gobject_class->set_property = gst_prerecord_sink_set_property;
  gobject_class->get_property = gst_prerecord_sink_get_property;

  g_object_class_install_property(gobject_class, PROP_LOCATION,
                                  g_param_spec_string("location", "Prerecord Location",
                                                      "Location of the prerecord to write", NULL,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_BUFFER_MODE,
                                  g_param_spec_enum("buffer-mode", "Buffering mode",
                                                    "The buffering mode to use", GST_TYPE_PRERECORD_SINK_BUFFER_MODE,
                                                    DEFAULT_BUFFER_MODE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_BUFFER_SIZE,
                                  g_param_spec_uint("buffer-size", "Buffering size",
                                                    "Size of buffer in number of bytes for line or full buffer-mode", 0,
                                                    G_MAXUINT, DEFAULT_BUFFER_SIZE,
                                                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstPrerecordSink:append
   *
   * Append to an already existing prerecord.
   */
  g_object_class_install_property(gobject_class, PROP_APPEND,
                                  g_param_spec_boolean("append", "Append",
                                                       "Append to an already existing prerecord", DEFAULT_APPEND,
                                                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_O_SYNC,
                                  g_param_spec_boolean("o-sync", "Synchronous IO",
                                                       "Open the prerecord with O_SYNC for enabling synchronous IO",
                                                       DEFAULT_O_SYNC, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_PRE_RECORD,
                                  g_param_spec_int("pre-record", "Pre record",
                                                   "Time in seconds for Prerecording", G_MININT, G_MAXINT, DEFAULT_PRE_RECORD,
                                                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_POST_RECORD,
                                  g_param_spec_int("post-record", "Post record",
                                                   "Time in seconds for Postrecording", G_MININT, G_MAXINT, DEFAULT_POST_RECORD,
                                                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_BUFFERING,
                                  g_param_spec_enum("buffering", "Buffering",
                                                       "Precord / Record /Postrecord", GST_TYPE_PRERECORD_SINK_BUFFERING,
                                                       DEFAULT_BUFFERING,G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class,
                                  PROP_MAX_TRANSIENT_ERROR_TIMEOUT,
                                  g_param_spec_int("max-transient-error-timeout",
                                                   "Max Transient Error Timeout",
                                                   "Retry up to this many ms on transient errors (currently EACCES)", 0,
                                                   G_MAXINT, DEFAULT_MAX_TRANSIENT_ERROR_TIMEOUT,
                                                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata(gstelement_class,
                                        "Prerecord Sink",
                                        "Sink/Prerecord", "Write stream to a prerecord",
                                        "Mayur Dongre@latest <mdongre at phoenix dot tech>");
  gst_element_class_add_static_pad_template(gstelement_class, &sinktemplate);

  gstbasesink_class->start = GST_DEBUG_FUNCPTR(gst_prerecord_sink_start);
  gstbasesink_class->stop = GST_DEBUG_FUNCPTR(gst_prerecord_sink_stop);
  gstbasesink_class->query = GST_DEBUG_FUNCPTR(gst_prerecord_sink_query);
  gstbasesink_class->render = GST_DEBUG_FUNCPTR(gst_prerecord_sink_render);
  gstbasesink_class->render_list =
      GST_DEBUG_FUNCPTR(gst_prerecord_sink_render_list);
  gstbasesink_class->event = GST_DEBUG_FUNCPTR(gst_prerecord_sink_event);
  gstbasesink_class->unlock = GST_DEBUG_FUNCPTR(gst_prerecord_sink_unlock);
  gstbasesink_class->unlock_stop =
      GST_DEBUG_FUNCPTR(gst_prerecord_sink_unlock_stop);

  if (sizeof(off_t) < 8)
  {
    GST_LOG("No large prerecord support, sizeof (off_t) = %" G_GSIZE_FORMAT "!",
            sizeof(off_t));
  }

  gst_type_mark_as_plugin_api(GST_TYPE_PRERECORD_SINK_BUFFER_MODE, 0);
}

static void
gst_prerecord_sink_init(GstPrerecordSink *prerecordsink)
{
  prerecordsink->prerecordname = NULL;
  prerecordsink->prerecord = NULL;
  prerecordsink->current_pos = 0;
  prerecordsink->buffer_mode = DEFAULT_BUFFER_MODE;
  prerecordsink->buffer_size = DEFAULT_BUFFER_SIZE;
  prerecordsink->pre_record = DEFAULT_PRE_RECORD;
  prerecordsink->post_record = DEFAULT_POST_RECORD;
  prerecordsink->buffering = DEFAULT_BUFFERING;
  prerecordsink->append = FALSE;

  gst_base_sink_set_sync(GST_BASE_SINK(prerecordsink), FALSE);
}

static void
gst_prerecord_sink_dispose(GObject *object)
{
  GstPrerecordSink *sink = GST_PRERECORD_SINK(object);

  G_OBJECT_CLASS(parent_class)->dispose(object);

  g_free(sink->uri);
  sink->uri = NULL;
  g_free(sink->prerecordname);
  sink->prerecordname = NULL;
}

static gboolean
gst_prerecord_sink_set_location(GstPrerecordSink *sink, const gchar *location,
                                GError **error)
{
  if (sink->prerecord)
    goto was_open;

  g_free(sink->prerecordname);
  g_free(sink->uri);
  if (location != NULL)
  {
    /* we store the prerecordname as we received it from the application. On Windows
     * this should be in UTF8 */
    sink->prerecordname = g_strdup(location);
    sink->uri = gst_filename_to_uri(location, NULL);
    GST_INFO_OBJECT(sink, "prerecordname : %s", sink->prerecordname);
    GST_INFO_OBJECT(sink, "uri      : %s", sink->uri);
  }
  else
  {
    sink->prerecordname = NULL;
    sink->uri = NULL;
  }

  return TRUE;

  /* ERRORS */
was_open:
{
  g_warning("Changing the `location' property on prerecordsink when a prerecord is "
            "open is not supported.");
  g_set_error(error, GST_URI_ERROR, GST_URI_ERROR_BAD_STATE,
              "Changing the 'location' property on prerecordsink when a prerecord is "
              "open is not supported");
  return FALSE;
}
}

static void
gst_prerecord_sink_set_property(GObject *object, guint prop_id,
                                const GValue *value, GParamSpec *pspec)
{
  GstPrerecordSink *sink = GST_PRERECORD_SINK(object);

  // Assuming 'filter' is the pointer to your prerecordsink element

  // Set default codec settings for video (H.264)

  switch (prop_id)
  {
  case PROP_LOCATION:
    gst_prerecord_sink_set_location(sink, g_value_get_string(value), NULL);
    break;
  case PROP_BUFFER_MODE:
    sink->buffer_mode = g_value_get_enum(value);
    break;
  case PROP_BUFFER_SIZE:
    sink->buffer_size = g_value_get_uint(value);
    break;
  case PROP_APPEND:
    sink->append = g_value_get_boolean(value);
    break;
  case PROP_O_SYNC:
    sink->o_sync = g_value_get_boolean(value);
    break;
  case PROP_MAX_TRANSIENT_ERROR_TIMEOUT:
    sink->max_transient_error_timeout = g_value_get_int(value);
    break;
  case PROP_PRE_RECORD:
    sink->pre_record = g_value_get_int(value);
  case PROP_POST_RECORD:
    sink->post_record = g_value_get_int(value);
    break;
  case PROP_BUFFERING:
    sink->buffering = g_value_get_enum(value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void
gst_prerecord_sink_get_property(GObject *object, guint prop_id, GValue *value,
                                GParamSpec *pspec)
{
  GstPrerecordSink *sink = GST_PRERECORD_SINK(object);

  switch (prop_id)
  {
  case PROP_LOCATION:
    g_value_set_string(value, sink->prerecordname);
    break;
  case PROP_BUFFER_MODE:
    g_value_set_enum(value, sink->buffer_mode);
    break;
  case PROP_BUFFER_SIZE:
    g_value_set_uint(value, sink->buffer_size);
    break;
  case PROP_APPEND:
    g_value_set_boolean(value, sink->append);
    break;
  case PROP_O_SYNC:
    g_value_set_boolean(value, sink->o_sync);
    break;
  case PROP_MAX_TRANSIENT_ERROR_TIMEOUT:
    g_value_set_int(value, sink->max_transient_error_timeout);
    break;
  case PROP_PRE_RECORD:
    g_value_set_int(value, sink->pre_record);
    break;
  case PROP_POST_RECORD:
    g_value_set_int(value, sink->post_record);
    break;
  case PROP_BUFFERING:
    g_value_set_enum(value, sink->buffering);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static gboolean
gst_prerecord_sink_open_prerecord(GstPrerecordSink *sink)
{
  /* open the prerecord */
  if (sink->prerecordname == NULL || sink->prerecordname[0] == '\0')
    goto no_prerecordname;

  if (sink->append)
    sink->prerecord = gst_fopen(sink->prerecordname, "ab", sink->o_sync);
  else
    sink->prerecord = gst_fopen(sink->prerecordname, "wb", sink->o_sync);
  if (sink->prerecord == NULL)
    goto open_failed;

  sink->current_pos = 0;
  /* try to seek in the prerecord to figure out if it is seekable */
  sink->seekable = gst_prerecord_sink_do_seek(sink, 0);

  if (sink->buffer)
    g_free(sink->buffer);
  sink->buffer = NULL;
  if (sink->buffer_list)
    gst_buffer_list_unref(sink->buffer_list);
  sink->buffer_list = NULL;

  if (sink->buffer_mode != GST_PRERECORD_SINK_BUFFER_MODE_UNBUFFERED)
  {
    if (sink->buffer_size == 0)
    {
      sink->buffer_size = DEFAULT_BUFFER_SIZE;
      g_object_notify(G_OBJECT(sink), "buffer-size");
    }

    if (sink->buffer_mode == GST_PRERECORD_SINK_BUFFER_MODE_FULL)
    {
      sink->buffer = g_malloc(sink->buffer_size);
      sink->allocated_buffer_size = sink->buffer_size;
    }
    else
    {
      sink->buffer_list = gst_buffer_list_new();
    }
    sink->current_buffer_size = 0;
  }

  GST_DEBUG_OBJECT(sink, "opened prerecord %s, seekable %d",
                   sink->prerecordname, sink->seekable);

  return TRUE;

  /* ERRORS */
no_prerecordname:
{
  GST_ELEMENT_ERROR(sink, RESOURCE, NOT_FOUND,
                    (_("No prerecord name specified for writing.")), (NULL));
  return FALSE;
}
open_failed:
{
  GST_ELEMENT_ERROR(sink, RESOURCE, OPEN_WRITE,
                    (_("Could not open prerecord \"%s\" for writing."), sink->prerecordname),
                    GST_ERROR_SYSTEM);
  return FALSE;
}
}

static void
gst_prerecord_sink_close_prerecord(GstPrerecordSink *sink)
{
  if (sink->prerecord)
  {
    if (gst_prerecord_sink_flush_buffer(sink) != GST_FLOW_OK)
      GST_ELEMENT_ERROR(sink, RESOURCE, CLOSE,
                        (_("Error closing prerecord \"%s\"."), sink->prerecordname), NULL);

    if (fclose(sink->prerecord) != 0)
      GST_ELEMENT_ERROR(sink, RESOURCE, CLOSE,
                        (_("Error closing prerecord \"%s\"."), sink->prerecordname), GST_ERROR_SYSTEM);

    GST_DEBUG_OBJECT(sink, "closed prerecord");
    sink->prerecord = NULL;
  }

  if (sink->buffer)
  {
    g_free(sink->buffer);
    sink->buffer = NULL;
  }
  sink->allocated_buffer_size = 0;

  if (sink->buffer_list)
  {
    gst_buffer_list_unref(sink->buffer_list);
    sink->buffer_list = NULL;
  }
  sink->current_buffer_size = 0;
}

static gboolean
gst_prerecord_sink_query(GstBaseSink *bsink, GstQuery *query)
{
  gboolean res;
  GstPrerecordSink *self;
  GstFormat format;

  self = GST_PRERECORD_SINK(bsink);

  switch (GST_QUERY_TYPE(query))
  {
  case GST_QUERY_POSITION:
    gst_query_parse_position(query, &format, NULL);

    switch (format)
    {
    case GST_FORMAT_DEFAULT:
    case GST_FORMAT_BYTES:
      gst_query_set_position(query, GST_FORMAT_BYTES,
                             self->current_pos + self->current_buffer_size);
      res = TRUE;
      break;
    default:
      res = FALSE;
      break;
    }
    break;

  case GST_QUERY_FORMATS:
    gst_query_set_formats(query, 2, GST_FORMAT_DEFAULT, GST_FORMAT_BYTES);
    res = TRUE;
    break;

  case GST_QUERY_URI:
    gst_query_set_uri(query, self->uri);
    res = TRUE;
    break;

  case GST_QUERY_SEEKING:
    gst_query_parse_seeking(query, &format, NULL, NULL, NULL);
    if (format == GST_FORMAT_BYTES || format == GST_FORMAT_DEFAULT)
    {
      gst_query_set_seeking(query, GST_FORMAT_BYTES, self->seekable, 0, -1);
    }
    else
    {
      gst_query_set_seeking(query, format, FALSE, 0, -1);
    }
    res = TRUE;
    break;

  default:
    res = GST_BASE_SINK_CLASS(parent_class)->query(bsink, query);
    break;
  }
  return res;
}

#ifdef HAVE_FSEEKO
#define __GST_STDIO_SEEK_FUNCTION "fseeko"
#elif defined(G_OS_UNIX) || defined(G_OS_WIN32)
#define __GST_STDIO_SEEK_FUNCTION "lseek"
#else
#define __GST_STDIO_SEEK_FUNCTION "fseek"
#endif

static gboolean
gst_prerecord_sink_do_seek(GstPrerecordSink *prerecordsink, guint64 new_offset)
{
  GST_DEBUG_OBJECT(prerecordsink, "Seeking to offset %" G_GUINT64_FORMAT " using " __GST_STDIO_SEEK_FUNCTION, new_offset);

  if (gst_prerecord_sink_flush_buffer(prerecordsink) != GST_FLOW_OK)
    goto flush_buffer_failed;

#ifdef HAVE_FSEEKO
  if (fseeko(prerecordsink->prerecord, (off_t)new_offset, SEEK_SET) != 0)
    goto seek_failed;
#elif defined(G_OS_UNIX) || defined(G_OS_WIN32)
  if (lseek(fileno(prerecordsink->prerecord), (off_t)new_offset,
            SEEK_SET) == (off_t)-1)
    goto seek_failed;
#else
  if (fseek(prerecordsink->prerecord, (long)new_offset, SEEK_SET) != 0)
    goto seek_failed;
#endif

  /* adjust position reporting after seek;
   * presumably this should basically yield new_offset */
  gst_prerecord_sink_get_current_offset(prerecordsink, &prerecordsink->current_pos);

  return TRUE;

  /* ERRORS */
flush_buffer_failed:
{
  GST_DEBUG_OBJECT(prerecordsink, "Flushing buffer failed");
  return FALSE;
}
seek_failed:
{
  GST_DEBUG_OBJECT(prerecordsink, "Seeking failed: %s", g_strerror(errno));
  return FALSE;
}
}

/* handle events (search) */
static gboolean
gst_prerecord_sink_event(GstBaseSink *sink, GstEvent *event)
{
  GstEventType type;
  GstPrerecordSink *prerecordsink;

  prerecordsink = GST_PRERECORD_SINK(sink);

  type = GST_EVENT_TYPE(event);

  switch (type)
  {
  case GST_EVENT_SEGMENT:
  {
    const GstSegment *segment;

    gst_event_parse_segment(event, &segment);

    if (segment->format == GST_FORMAT_BYTES)
    {
      /* only try to seek and fail when we are going to a different
       * position */
      if (prerecordsink->current_pos + prerecordsink->current_buffer_size !=
          segment->start)
      {
        /* FIXME, the seek should be performed on the pos field, start/stop are
         * just boundaries for valid bytes offsets. We should also fill the prerecord
         * with zeroes if the new position extends the current EOF (sparse streams
         * and segment accumulation). */
        if (!gst_prerecord_sink_do_seek(prerecordsink, (guint64)segment->start))
          goto seek_failed;
      }
      else
      {
        GST_DEBUG_OBJECT(prerecordsink, "Ignored SEGMENT, no seek needed");
      }
    }
    else
    {
      GST_DEBUG_OBJECT(prerecordsink,
                       "Ignored SEGMENT event of format %u (%s)", (guint)segment->format,
                       gst_format_get_name(segment->format));
    }
    break;
  }
  case GST_EVENT_FLUSH_STOP:
    if (prerecordsink->current_pos != 0 && prerecordsink->seekable)
    {
      gst_prerecord_sink_do_seek(prerecordsink, 0);
      if (ftruncate(fileno(prerecordsink->prerecord), 0))
        goto truncate_failed;
    }
    if (prerecordsink->buffer_list)
    {
      gst_buffer_list_unref(prerecordsink->buffer_list);
      prerecordsink->buffer_list = gst_buffer_list_new();
    }
    prerecordsink->current_buffer_size = 0;
    break;
  case GST_EVENT_EOS:
    if (gst_prerecord_sink_flush_buffer(prerecordsink) != GST_FLOW_OK)
      goto flush_buffer_failed;
    break;
  default:
    break;
  }

  return GST_BASE_SINK_CLASS(parent_class)->event(sink, event);

  /* ERRORS */
seek_failed:
{
  GST_ELEMENT_ERROR(prerecordsink, RESOURCE, SEEK,
                    (_("Error while seeking in prerecord \"%s\"."), prerecordsink->prerecordname),
                    GST_ERROR_SYSTEM);
  gst_event_unref(event);
  return FALSE;
}
flush_buffer_failed:
{
  GST_ELEMENT_ERROR(prerecordsink, RESOURCE, WRITE,
                    (_("Error while writing to prerecord \"%s\"."), prerecordsink->prerecordname), NULL);
  gst_event_unref(event);
  return FALSE;
}
truncate_failed:
{
  GST_ELEMENT_ERROR(prerecordsink, RESOURCE, WRITE,
                    (_("Error while writing to prerecord \"%s\"."), prerecordsink->prerecordname),
                    GST_ERROR_SYSTEM);
  gst_event_unref(event);
  return FALSE;
}
}

static GstFlowReturn
render_buffer(GstPrerecordSink *prerecordsink, GstBuffer *buffer)
{
  GstFlowReturn flow;
  guint64 bytes_written = 0;
  guint64 skip = 0;

  for (;;)
  {
    flow =
        gst_writev_buffer(GST_OBJECT_CAST(prerecordsink),
                          fileno(prerecordsink->prerecord), NULL, buffer, &bytes_written, skip,
                          prerecordsink->max_transient_error_timeout, prerecordsink->current_pos,
                          &prerecordsink->flushing);

    prerecordsink->current_pos += bytes_written;
    skip += bytes_written;

    if (flow != GST_FLOW_FLUSHING)
      break;

    flow = gst_base_sink_wait_preroll(GST_BASE_SINK(prerecordsink));

    if (flow != GST_FLOW_OK)
      break;
  }

  return flow;
}

static gboolean
gst_prerecord_sink_get_current_offset(GstPrerecordSink *prerecordsink, guint64 *p_pos)
{
  off_t ret = -1;

  /* no need to flush internal buffer here as this is only called right
   * after a seek. If this changes then the buffer should be flushed here
   * too
   */

#ifdef HAVE_FTELLO
  ret = ftello(prerecordsink->prerecord);
#elif defined(G_OS_UNIX) || defined(G_OS_WIN32)
  ret = lseek(fileno(prerecordsink->prerecord), 0, SEEK_CUR);
#else
  ret = (off_t)ftell(prerecordsink->prerecord);
#endif

  if (ret != (off_t)-1)
    *p_pos = (guint64)ret;

  return (ret != (off_t)-1);
}

static GstFlowReturn
gst_file_sink_render_list_internal(GstPrerecordSink *sink,
                                   GstBufferList *buffer_list)
{
  GstFlowReturn flow;
  guint num_buffers;
  guint64 skip = 0;

  num_buffers = gst_buffer_list_length(buffer_list);
  if (num_buffers == 0)
    goto no_data;

  GST_DEBUG_OBJECT(sink,
                   "writing %u buffers at position %" G_GUINT64_FORMAT, num_buffers,
                   sink->current_pos);

  for (;;)
  {
    guint64 bytes_written = 0;

    flow =
        gst_writev_buffer_list(GST_OBJECT_CAST(sink), fileno(sink->prerecord),
                               NULL, buffer_list, &bytes_written, skip,
                               sink->max_transient_error_timeout, sink->current_pos, &sink->flushing);

    sink->current_pos += bytes_written;
    skip += bytes_written;

    if (flow != GST_FLOW_FLUSHING)
      break;

    flow = gst_base_sink_wait_preroll(GST_BASE_SINK(sink));

    if (flow != GST_FLOW_OK)
      return flow;
  }

  return flow;

no_data:
{
  GST_LOG_OBJECT(sink, "empty buffer list");
  return GST_FLOW_OK;
}
}



static GstFlowReturn
gst_prerecord_sink_render_list_internal(GstPrerecordSink *sink,
                                        GstBufferList *buffer_list)
{
  static gboolean first_buffers_written = FALSE; // Flag to track if the first buffer has been written directly
  static gint num=0;
  GstFlowReturn flow = GST_FLOW_OK;
  static double elapsed_time_seconds = 0;
  static double elapsed_time_seconds2 = 0;
  static gboolean timecaptured = FALSE;
  static gint cal=FALSE;
  static gboolean timecaptured2 = FALSE;
  static GstClockTime start_time; 
  static GstClockTime start_time2; 
 
 /* if (cal == 0)
    {
      cal = sink->pre_record;
    }
    */
   GstClock *clocks = gst_system_clock_obtain();
  if (clocks == NULL) {
    printf("Failed to obtain system clock.\n");
    return GST_FLOW_ERROR;
  }

  if (!GST_IS_BUFFER_LIST(buffer_list))
  {
    // It's not a valid GstBufferList
    printf("Buffer list is not a valid GstBufferList.\n");
    return GST_FLOW_ERROR;
  }

  printf("Buffer list type before pushing: GstBufferList\n");

    
  if (sink->buffering==0)
    {
      // Buffering disabled
      // Process each buffered GstBufferList

      printf("Processing buffered GstBufferLists.\n");
      while (sink->fifo != NULL && !is_fifo_empty(sink->fifo))
      {
        //printf("Entering POP.\n");
        GstBufferList *poppedBufferList = pop(sink->fifo);

        if (poppedBufferList != NULL && GST_IS_BUFFER_LIST(poppedBufferList))
        {
          printf("Copying Prerecorded Data\n");
          // Do something with poppedBufferList

          flow = gst_file_sink_render_list_internal(sink, poppedBufferList);

          if (flow != GST_FLOW_OK)
          {
            printf("Failed to write a buffered GstBufferList to the file.\n");

            return flow;
          }
        }
        else
        {
          printf("Popped BufferList is either NULL or not a GstBufferList\n");
          // Handle accordingly
        }
      }

      printf("Recording in Process\n");

      
      flow = gst_file_sink_render_list_internal(sink, buffer_list);
        
    }
    else if (sink->buffering==1)
    {
        if ((sink->post_record) > 0)
      {
        // Get the current time
          
          if(!timecaptured)
          {
          start_time = gst_clock_get_time(clocks);
          timecaptured=TRUE;
          }

          flow = gst_file_sink_render_list_internal(sink, buffer_list);
          
          double elapsed_time_seconds = (double)(gst_clock_get_time(clocks) - start_time) / GST_SECOND;

          printf("Postrecording Time: %.2f seconds\n", elapsed_time_seconds);

          
          if (elapsed_time_seconds >= sink->post_record)
          {
            printf("PostRecording Done\n");
            printf("Total Postrecording Time: %.2f seconds\n", elapsed_time_seconds);
            gst_object_unref(clocks); // Don't forget to unref the clock
            return GST_FLOW_EOS;     // Exit the function
          }
          first_buffers_written=FALSE;
          num=0;
      }
    }
    else
    {
    
    
      if (!first_buffers_written)
  {
	num++;
	
    GstBufferList *first_buffer_list = gst_buffer_list_copy_deep(buffer_list);
    
    guint num_buffers = gst_buffer_list_length(first_buffer_list);

    // Iterate through each buffer in the list
    for (guint i = 0; i < num_buffers; i++)
    {
        GstBuffer *buffer = gst_buffer_list_get(first_buffer_list, i);
        // Set PTS and DTS to GST_CLOCK_TIME_NONE
        GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    }

    // Write each buffer list individually
    flow = gst_file_sink_render_list_internal(sink, first_buffer_list);
    if(num > 1)
    {
    first_buffers_written = TRUE; // Update the flag after writing the first 10 buffer lists
    timecaptured = FALSE;
    timecaptured2 = FALSE;
  }
  }
  else
  {
  
   
      if (sink->fifo == NULL)
      {
        sink->fifo = initializeFIFO();  
        if (sink->fifo == NULL)
        {
          printf("Failed to initialize FIFO.\n");
          return GST_FLOW_ERROR;
        }
      }
      
      
      if(!timecaptured2)
          {
          start_time2 = gst_clock_get_time(clocks);
          timecaptured2=TRUE;
          }

  
     
      if (cal==TRUE)
      {
        printf("Entering.\n");
        

        GstBufferList *removedBufferList = pop(sink->fifo);

        GstBufferList *copyBufferList_remove = NULL;
        if (removedBufferList != NULL)
        {
            copyBufferList_remove = gst_buffer_list_copy_deep(removedBufferList);
            printf("Removing the Oldest BufferList from the FIFO.\n");
        }

        if (copyBufferList_remove != NULL)
        {
        
            g_clear_pointer(&copyBufferList_remove, gst_buffer_list_unref);
            g_clear_pointer(&removedBufferList, gst_buffer_list_unref);
      
        }
        cal=FALSE;
        printf("Cal False\n");
      }
      
      if(cal==FALSE)
      {
      double elapsed_time_seconds2 = (double)(gst_clock_get_time(clocks) - start_time2) / GST_SECOND;
      printf(" Before Elapsed2222 time Adding: %.6f seconds\n", elapsed_time_seconds2);
      
      if(elapsed_time_seconds2 > sink->pre_record)
      {
      cal=TRUE;
      printf("Cal True\n");
      }// Push the incoming buffer_list to the FIFO
      }
      GstBufferList *copyBufferList = gst_buffer_list_copy_deep(buffer_list);
      
    guint num_buffers_s = gst_buffer_list_length(copyBufferList);

    // Iterate through each buffer in the list
    for (guint i = 0; i < num_buffers_s; i++)
    {
        GstBuffer *buffers = gst_buffer_list_get(copyBufferList, i);
        // Set PTS and DTS to GST_CLOCK_TIME_NONE
        GST_BUFFER_PTS(buffers) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DTS(buffers) = GST_CLOCK_TIME_NONE;
    }
    
      push(sink->fifo, copyBufferList);
     printf("Pushed\n");
    //  double bufferListDuration = BufferListduration(copyBufferList);

     // g_clear_pointer(&copyBufferList, gst_buffer_list_unref);
  }
    }
 
  return flow;
}

static GstFlowReturn
gst_prerecord_sink_flush_buffer(GstPrerecordSink *prerecordsink)
{
  GstFlowReturn flow_ret = GST_FLOW_OK;

  // printf("Flushing out buffer of size %" G_GSIZE_FORMAT "\n", prerecordsink->current_buffer_size);

  if (prerecordsink->buffer && prerecordsink->current_buffer_size)
  {
    guint64 skip = 0;

    for (;;)
    {
      guint64 bytes_written = 0;

      flow_ret =
          gst_writev_mem(GST_OBJECT_CAST(prerecordsink), fileno(prerecordsink->prerecord),
                         NULL, prerecordsink->buffer, prerecordsink->current_buffer_size, &bytes_written,
                         skip, prerecordsink->max_transient_error_timeout, prerecordsink->current_pos,
                         &prerecordsink->flushing);

      prerecordsink->current_pos += bytes_written;
      skip += bytes_written;

      if (flow_ret != GST_FLOW_FLUSHING)
        break;

      flow_ret = gst_base_sink_wait_preroll(GST_BASE_SINK(prerecordsink));
      if (flow_ret != GST_FLOW_OK)
        break;
    }
  }
  else if (prerecordsink->buffer_list && prerecordsink->current_buffer_size)
  {
    guint length;

    length = gst_buffer_list_length(prerecordsink->buffer_list);

    printf("Length of BufferList %u\n", length);

    if (length > 0)
    {
      flow_ret =
          gst_prerecord_sink_render_list_internal(prerecordsink, prerecordsink->buffer_list);
      /* Remove all buffers from the list but keep the list. This ensures that
       * we don't re-allocate the array storing the buffers all the time */
      gst_buffer_list_remove(prerecordsink->buffer_list, 0, length);
    }
  }

  prerecordsink->current_buffer_size = 0;

  return flow_ret;
}

static gboolean
has_sync_after_buffer(GstBuffer **buffer, guint idx, gpointer user_data)
{
  if (GST_BUFFER_FLAG_IS_SET(*buffer, GST_BUFFER_FLAG_SYNC_AFTER))
  {
    gboolean *sync_after = user_data;

    *sync_after = TRUE;
    return FALSE;
  }

  return TRUE;
}

static gboolean
accumulate_size(GstBuffer **buffer, guint idx, gpointer user_data)
{
  guint *size = user_data;

  *size += gst_buffer_get_size(*buffer);

  return TRUE;
}

static GstFlowReturn
gst_prerecord_sink_render_list(GstBaseSink *bsink, GstBufferList *buffer_list)
{
  GstFlowReturn flow;
  GstPrerecordSink *sink;
  guint i, num_buffers;
  gboolean sync_after = FALSE;
  gint fsync_ret;

  sink = GST_PRERECORD_SINK_CAST(bsink);
  // printf("Inside Render List");
  num_buffers = gst_buffer_list_length(buffer_list);
  if (num_buffers == 0)
    goto no_data;

  gst_buffer_list_foreach(buffer_list, has_sync_after_buffer, &sync_after);

  if (sync_after || (!sink->buffer && !sink->buffer_list))
  {
    flow = gst_prerecord_sink_flush_buffer(sink);
    if (flow == GST_FLOW_OK)
      flow = gst_prerecord_sink_render_list_internal(sink, buffer_list);
  }
  else
  {
    guint size = 0;
    gst_buffer_list_foreach(buffer_list, accumulate_size, &size);

    // printf( "Queueing buffer list of %u bytes (%u buffers) at offset %" G_GUINT64_FORMAT, size, num_buffers,
    //                sink->current_pos + sink->current_buffer_size);

    if (sink->buffer)
    {
      flow = GST_FLOW_OK;
      for (i = 0; i < num_buffers && flow == GST_FLOW_OK; i++)
      {
        GstBuffer *buffer = gst_buffer_list_get(buffer_list, i);
        gsize buffer_size = gst_buffer_get_size(buffer);

        if (sink->current_buffer_size + buffer_size >
            sink->allocated_buffer_size)
        {
          flow = gst_prerecord_sink_flush_buffer(sink);
          if (flow != GST_FLOW_OK)
            return flow;
        }

        if (buffer_size > sink->allocated_buffer_size)
        {
          printf(
              "writing buffer ( %" G_GSIZE_FORMAT
              " bytes) at position %" G_GUINT64_FORMAT,
              buffer_size, sink->current_pos);

          flow = render_buffer(sink, buffer);
        }
        else
        {
          sink->current_buffer_size +=
              gst_buffer_extract(buffer, 0,
                                 sink->buffer + sink->current_buffer_size, buffer_size);
          flow = GST_FLOW_OK;
        }
      }
    }
    else
    {
      for (i = 0; i < num_buffers; ++i)
        gst_buffer_list_add(sink->buffer_list,
                            gst_buffer_ref(gst_buffer_list_get(buffer_list, i)));
      sink->current_buffer_size += size;

      if (sink->current_buffer_size > sink->buffer_size)
        flow = gst_prerecord_sink_flush_buffer(sink);
      else
        flow = GST_FLOW_OK;
    }
  }

  if (flow == GST_FLOW_OK && sync_after)
  {
    do
    {
      fsync_ret = fsync(fileno(sink->prerecord));
    } while (fsync_ret < 0 && errno == EINTR);
    if (fsync_ret)
    {
      GST_ELEMENT_ERROR(sink, RESOURCE, WRITE,
                        (_("Error while writing to prerecord \"%s\"."), sink->prerecordname),
                        ("%s", g_strerror(errno)));
      flow = GST_FLOW_ERROR;
    }
  }

  return flow;

no_data:
{
  GST_LOG_OBJECT(sink, "empty buffer list");
  return GST_FLOW_OK;
}
}

static GstFlowReturn
gst_prerecord_sink_render(GstBaseSink *sink, GstBuffer *buffer)
{
  GstPrerecordSink *prerecordsink;
  GstFlowReturn flow;
  guint8 n_mem;
  gboolean sync_after;
  gint fsync_ret;

  prerecordsink = GST_PRERECORD_SINK_CAST(sink);

  sync_after = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_SYNC_AFTER);

  n_mem = gst_buffer_n_memory(buffer);

  if (n_mem > 0 && (sync_after || (!prerecordsink->buffer && !prerecordsink->buffer_list)))
  {
    flow = gst_prerecord_sink_flush_buffer(prerecordsink);
    if (flow == GST_FLOW_OK)
    {
      flow = render_buffer(prerecordsink, buffer);
    }
  }
  else if (n_mem > 0)
  {
    gsize size = gst_buffer_get_size(buffer);

    GST_DEBUG_OBJECT(prerecordsink,
                     "Queueing buffer of %" G_GSIZE_FORMAT " bytes at offset %" G_GUINT64_FORMAT, size,
                     prerecordsink->current_pos + prerecordsink->current_buffer_size);

    if (prerecordsink->buffer)
    {
      if (prerecordsink->current_buffer_size + size >
          prerecordsink->allocated_buffer_size)
      {
        flow = gst_prerecord_sink_flush_buffer(prerecordsink);
        if (flow != GST_FLOW_OK)
          return flow;
      }

      if (size > prerecordsink->allocated_buffer_size)
      {
        GST_DEBUG_OBJECT(sink,
                         "writing buffer ( %" G_GSIZE_FORMAT
                         " bytes) at position %" G_GUINT64_FORMAT,
                         size, prerecordsink->current_pos);

        flow = render_buffer(prerecordsink, buffer);
      }
      else
      {
        prerecordsink->current_buffer_size +=
            gst_buffer_extract(buffer, 0,
                               prerecordsink->buffer + prerecordsink->current_buffer_size, size);
        flow = GST_FLOW_OK;
      }
    }
    else
    {
      prerecordsink->current_buffer_size += gst_buffer_get_size(buffer);
      gst_buffer_list_add(prerecordsink->buffer_list, gst_buffer_ref(buffer));

      if (prerecordsink->current_buffer_size > prerecordsink->buffer_size)
        flow = gst_prerecord_sink_flush_buffer(prerecordsink);
      else
        flow = GST_FLOW_OK;
    }
  }
  else
  {
    flow = GST_FLOW_OK;
  }

  if (flow == GST_FLOW_OK && sync_after)
  {
    do
    {
      fsync_ret = fsync(fileno(prerecordsink->prerecord));
    } while (fsync_ret < 0 && errno == EINTR);
    if (fsync_ret)
    {
      GST_ELEMENT_ERROR(prerecordsink, RESOURCE, WRITE,
                        (_("Error while writing to prerecord \"%s\"."), prerecordsink->prerecordname),
                        ("%s", g_strerror(errno)));
      flow = GST_FLOW_ERROR;
    }
  }

  return flow;
}

static gboolean
gst_prerecord_sink_start(GstBaseSink *basesink)
{
  GstPrerecordSink *prerecordsink;

  prerecordsink = GST_PRERECORD_SINK_CAST(basesink);

  g_atomic_int_set(&prerecordsink->flushing, FALSE);
  return gst_prerecord_sink_open_prerecord(prerecordsink);
}

static gboolean
gst_prerecord_sink_stop(GstBaseSink *basesink)
{
  GstPrerecordSink *prerecordsink;

  prerecordsink = GST_PRERECORD_SINK_CAST(basesink);

  gst_prerecord_sink_close_prerecord(prerecordsink);
  return TRUE;
}

static gboolean
gst_prerecord_sink_unlock(GstBaseSink *basesink)
{
  GstPrerecordSink *prerecordsink;

  prerecordsink = GST_PRERECORD_SINK_CAST(basesink);
  g_atomic_int_set(&prerecordsink->flushing, TRUE);

  return TRUE;
}

static gboolean
gst_prerecord_sink_unlock_stop(GstBaseSink *basesink)
{
  GstPrerecordSink *prerecordsink;

  prerecordsink = GST_PRERECORD_SINK_CAST(basesink);
  g_atomic_int_set(&prerecordsink->flushing, FALSE);

  return TRUE;
}

/*** GSTURIHANDLER INTERFACE *************************************************/

static GstURIType
gst_prerecord_sink_uri_get_type(GType type)
{
  return GST_URI_SINK;
}

static const gchar *const *
gst_prerecord_sink_uri_get_protocols(GType type)
{
  static const gchar *protocols[] = {"prerecord", NULL};

  return protocols;
}

static gchar *
gst_prerecord_sink_uri_get_uri(GstURIHandler *handler)
{
  GstPrerecordSink *sink = GST_PRERECORD_SINK(handler);

  /* FIXME: make thread-safe */
  return g_strdup(sink->uri);
}

static gboolean
gst_prerecord_sink_uri_set_uri(GstURIHandler *handler, const gchar *uri,
                               GError **error)
{
  gchar *location;
  gboolean ret;
  GstPrerecordSink *sink = GST_PRERECORD_SINK(handler);

  /* allow prerecord://localhost/foo/bar by stripping localhost but fail
   * for every other hostname */
  if (g_str_has_prefix(uri, "prerecord://localhost/"))
  {
    char *tmp;

    /* 16 == strlen ("prerecord://localhost") */
    tmp = g_strconcat("prerecord://", uri + 16, NULL);
    /* we use gst_uri_get_location() although we already have the
     * "location" with uri + 16 because it provides unescaping */
    location = gst_uri_get_location(tmp);
    g_free(tmp);
  }
  else if (strcmp(uri, "prerecord://") == 0)
  {
    /* Special case for "prerecord://" as this is used by some applications
     *  to test with gst_element_make_from_uri if there's an element
     *  that supports the URI protocol. */
    gst_prerecord_sink_set_location(sink, NULL, NULL);
    return TRUE;
  }
  else
  {
    location = gst_uri_get_location(uri);
  }

  if (!location)
  {
    g_set_error_literal(error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
                        "Prerecord URI without location");
    return FALSE;
  }

  if (!g_path_is_absolute(location))
  {
    g_set_error_literal(error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
                        "Prerecord URI location must be an absolute path");
    g_free(location);
    return FALSE;
  }

  ret = gst_prerecord_sink_set_location(sink, location, error);
  g_free(location);

  return ret;
}

static void
gst_prerecord_sink_uri_handler_init(gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *)g_iface;

  iface->get_type = gst_prerecord_sink_uri_get_type;
  iface->get_protocols = gst_prerecord_sink_uri_get_protocols;
  iface->get_uri = gst_prerecord_sink_uri_get_uri;
  iface->set_uri = gst_prerecord_sink_uri_set_uri;
}

