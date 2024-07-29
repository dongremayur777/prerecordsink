/* GStreamer
 * Copyright (C) 1999,2000 Erik Walthinsen <omega@cse.ogi.edu>
 *                    2000 Wim Taymans <wtay@chello.be>
 *
 * gstprerecordsink.h:
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */


#ifndef __GST_PRERECORD_SINK_H__
#define __GST_PRERECORD_SINK_H__

#include <stdio.h>

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

G_BEGIN_DECLS



#define GST_TYPE_PRERECORD_SINK \
  (gst_prerecord_sink_get_type())
#define GST_PRERECORD_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PRERECORD_SINK,GstPrerecordSink))
#define GST_PRERECORD_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PRERECORD_SINK,GstPrerecordSinkClass))
#define GST_IS_PRERECORD_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PRERECORD_SINK))
#define GST_IS_PRERECORD_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PRERECORD_SINK))
#define GST_PRERECORD_SINK_CAST(obj) ((GstPrerecordSink *)(obj))

typedef struct _GstPrerecordSink GstPrerecordSink;
typedef struct _GstPrerecordSinkClass GstPrerecordSinkClass;

/**
 * GstPrerecordSinkBufferMode:
 * @GST_PRERECORD_SINK_BUFFER_MODE_DEFAULT: Default buffering
 * @GST_PRERECORD_SINK_BUFFER_MODE_FULL: Fully buffered
 * @GST_PRERECORD_SINK_BUFFER_MODE_LINE: Line buffered
 * @GST_PRERECORD_SINK_BUFFER_MODE_UNBUFFERED: Unbuffered
 *
 * Prerecord read buffering mode.
 */
typedef enum {
  GST_PRERECORD_SINK_BUFFERING_PRERECORD    = -1,
  GST_PRERECORD_SINK_BUFFERING_RECORDING       = _IOFBF,
  GST_PRERECORD_SINK_BUFFERING_POSTRECORD       = _IOLBF,
} GstPrerecordSinkBuffering;



typedef enum {
  GST_PRERECORD_SINK_BUFFER_MODE_DEFAULT    = -1,
  GST_PRERECORD_SINK_BUFFER_MODE_FULL       = _IOFBF,
  GST_PRERECORD_SINK_BUFFER_MODE_LINE       = _IOLBF,
  GST_PRERECORD_SINK_BUFFER_MODE_UNBUFFERED = _IONBF
} GstPrerecordSinkBufferMode;


/**
 * GstPrerecordSink:
 *
 * Opaque #GstPrerecordSink structure.
 */



typedef struct _BufferListNode {
    GstBufferList *buffer_list;
    struct _BufferListNode *next;
} BufferListNode;

typedef struct _BufferListFIFO {
    BufferListNode *front;
    BufferListNode *rear;
} BufferListFIFO;

// Function to initialize an empty FIFO
BufferListFIFO *initializeFIFO() {
    BufferListFIFO *fifo = (BufferListFIFO *)malloc(sizeof(BufferListFIFO));
    if (fifo == NULL) {
        fprintf(stderr, "Memory allocation error for FIFO initialization\n");
        exit(EXIT_FAILURE);
    }

    fifo->front = fifo->rear = NULL;
    return fifo;
}



guint getFIFOSize(BufferListFIFO *fifo) {
    guint size = 0;
    BufferListNode *current = fifo->front;

    while (current != NULL) {
        size++;
        current = current->next;
    }

    return size;
}


// Function to check if the FIFO is empty
int is_fifo_empty(BufferListFIFO *fifo) {
    return fifo->front == NULL;
}


// Function to push a GstBufferList onto the FIFO
void push(BufferListFIFO *fifo, GstBufferList *buffer_list) {
    BufferListNode *new_node = (BufferListNode *)malloc(sizeof(BufferListNode));
    if (new_node == NULL) {
        fprintf(stderr, "Memory allocation error for new node\n");
        exit(EXIT_FAILURE);
    }

    new_node->buffer_list = buffer_list;
    new_node->next = NULL;

    if (is_fifo_empty(fifo)) {  // If the FIFO is empty
        fifo->front = fifo->rear = new_node;
    } else {
        fifo->rear->next = new_node;  // Append to the existing FIFO
        fifo->rear = new_node;
    }
}

// Function to pop a GstBufferList from the FIFO
GstBufferList *pop(BufferListFIFO *fifo) {
    if (is_fifo_empty(fifo)) {
        return NULL;  // FIFO is empty
    } else {
        BufferListNode *temp = fifo->front;
        GstBufferList *buffer_list = temp->buffer_list;  // Store the buffer_list

        fifo->front = fifo->front->next;
        if (fifo->front == NULL) {  // If the FIFO becomes empty
            fifo->rear = NULL;
        }

        free(temp);  // Free the node memory
        return buffer_list;  // Return the buffer_list
    }
}



GstBufferList *popNth(BufferListFIFO *fifo, guint n) {
    if (is_fifo_empty(fifo)) {
        return NULL;  // FIFO is empty
    } else {
        BufferListNode *current = fifo->front;
        BufferListNode *prev = NULL;
        guint count = 0;

        // Traverse the FIFO to find the nth element
        while (current != NULL && count != n) {
            prev = current;
            current = current->next;
            count++;
        }

        // If nth element is found
        if (current != NULL) {
            GstBufferList *buffer_list = current->buffer_list;
            
            // Remove the nth element from the FIFO
            if (prev != NULL) {
                prev->next = current->next;
            } else {
                fifo->front = current->next;
            }
            
            // If the removed element is the last element, update the rear pointer
            if (current == fifo->rear) {
                fifo->rear = prev;
            }

            // Free the node memory
            free(current);
            
            return buffer_list; // Return the buffer_list
        } else {
            return NULL; // nth element not found
        }
    }
}




// Function to free the memory occupied by the FIFO
void freeFIFO(BufferListFIFO *fifo) {
    while (!is_fifo_empty(fifo)) {
        pop(fifo);
    }
    free(fifo);
}




struct _GstPrerecordSink {
  GstBaseSink parent;

  /*< private >*/
  gchar *prerecordname;
  gchar *uri;
  FILE *prerecord;

  gboolean seekable;
  guint64 current_pos;

  gint    buffer_mode;
  guint   buffer_size;

  /* For default buffer mode */
  GstBufferList *buffer_list;

  /* For full buffer mode */
  guint8 *buffer;
  gsize   allocated_buffer_size;

  /* For default/full buffer mode */
  gsize current_buffer_size;

  gboolean append;
  gboolean o_sync;
  gint max_transient_error_timeout;
  gint pre_record;
  gint post_record;
  gint buffering;
  gboolean flushing;
  BufferListFIFO *fifo;

  
};

struct _GstPrerecordSinkClass {
  GstBaseSinkClass parent_class;
};

G_GNUC_INTERNAL GType gst_prerecord_sink_get_type (void);

G_END_DECLS

#endif /* __GST_PRERECORD_SINK_H__ */
