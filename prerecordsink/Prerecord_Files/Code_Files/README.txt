 Prerecordsink

Prerecordsink is a Gstreamer Plugin which will save the videostream before the interrupt comes.

Factory Details:
  Rank                     none (0)
  Long-name                prerecordsink
  Klass                    FIXME:Generic
  Description              FIXME:Generic Prerecord Element
  Author                   Mayur Dongre <<mdongre@phoenix.tech>>

Plugin Details:
  Name                     prerecordsink
  Description              Prerecords the Video before the Interrupt arrives
  Filename                 /home/sarekar/Plugin/gst-template/build/gst-plugin/libgstprerecordsink.so
  Version                  1.19.0.1
  License                  LGPL
  Source module            gst-template-plugin
  Binary package           GStreamer template Plug-ins
  Origin URL               https://gstreamer.freedesktop.org

GObject
 +----GInitiallyUnowned
       +----GstObject
             +----GstElement
                   +----Gstprerecordsink

Pad Templates:
  SINK template: 'sink'
    Availability: Always
    Capabilities:
      ANY
  
  SRC template: 'src'
    Availability: Always
    Capabilities:
      ANY

Element has no clocking capabilities.
Element has no URI handling capabilities.

Pads:
  SINK: 'sink'
    Pad Template: 'sink'
  SRC: 'src'
    Pad Template: 'src'

Element Properties:
  buffering           : Enable buffering
                        flags: readable, writable
                        Boolean. Default: false
  fifosize            : Duration in milliseconds for the video in FIFO
                        flags: readable, writable
                        Unsigned Integer. Range: 1 - 4294967295 Default: 1024 
  location            : File location to save the video stream
                        flags: readable, writable
                        String. Default: null
  name                : The name of the object
                        flags: readable, writable, 0x2000
                        String. Default: "prerecordsink0"
  parent              : The parent of the object
                        flags: readable, writable, 0x2000
                        Object of type "GstObject"
  silent              : Produce verbose output :Mayur's Plugin
                        flags: readable, writable
                        Boolean. Default: false**
