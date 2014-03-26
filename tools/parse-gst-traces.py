#! /usr/bin/python
# GStreamer scheduling parser
# Analyze GST traces and shows the time spent for each buffer at each element level.
#
# Usage: GST_DEBUG=*:2,GST_SCHEDULING:5,GST_PERFORMANCE:5 gst-launch --gst-debug-no-color -e [PIPELINE] >foo 2>&1
#        ./parse-gst-traces.py foo
# Example of pipeline: videotestsrc num-buffers=200 ! "video/x-raw-yuv, format=(fourcc)NV12, width=1280, height=720, framerate=30/1" ! ducatih264enc profile=66 ! ducatih264dec! dri2videosink sync=false

import sys
import time
import fileinput
import re
import operator
from datetime import datetime

def string_to_time (d):
  return datetime.strptime(d[0:14], "%H:%M:%S.%f")

def main():
  tr = dict()
  cam = dict()
  dec = dict()
  ecs = 0

  ### Parse stdin or any file on the command line
  for line in fileinput.input():	

    # Filter out GST traces from other output
    m = re.match(r"([\w:.]+)[\s]+([\w]+)[\s]+([\w]+)[\s]+([\w]+)[\s]+(.*)",line)
    if m == None:
      print line
      continue
    (tsr,pid,adr,sev,msg) = m.groups()

    # Parse GST_SCHEDULING traces
    m = re.match(r"GST_SCHEDULING[\s]+gstpad.c:[\w]+:([\w_]+):<([A-Za-z0-9_]+)([0-9]+):([\w_]+)> calling chainfunction &[\w]+ with buffer ([\w]+), data ([\w]+), malloc ([\w()]+), ts ([\w:.]+), dur ([\w:.]+)", msg)
    if m != None:
      (func, elem, number, pad, gstBuffer, data, malloc, tsb, dur) = m.groups()
      if func !=  "gst_pad_push":
        continue
      if elem == "cam" and pad == "src" and gstBuffer in cam:
        tr[tsb] = [(cam[gstBuffer], "FillBufferDone")]
      elif elem == "ducatih264dec" and pad == "src":
        dec[gstBuffer] = tsb
      elif elem == "udpsrc" or elem == "recv_rtp_sink_" or elem == "rtpsession" or elem =="rtpssrcdemux":
        continue
      if tsb not in tr:
        tr[tsb] = []
      tr[tsb].append((tsr, "%s:<%s%s:%s>" % (func, elem, number, pad) ))
      continue

    # Parse omx_camera traces
    m = re.match(r"GST_PERFORMANCE gstomx_core.c:[\w]+:FillBufferDone:<cam> FillBufferDone: GstBuffer=([\w]+)", msg)
    if m != None:
      gstBuffer = m.group(1)
      cam[gstBuffer] = tsr
      continue

    # Parse encoder traces
    m = re.match(r"GST_PERFORMANCE gstducatividenc.c:[\w]+:gst_ducati_videnc_handle_frame:<ducatih264enc[\w]+> Encoded frame in ([\w]+) bytes", msg)
    if m != None:
       ecs += int(m.group(1))
       continue

    # Parse dri2videosink traces
    m = re.match(r"GST_PERFORMANCE gstdri2util.c:[\w]+:gst_dri2window_buffer_show:<dri2videosink[\w]+> (Before DRI2SwapBuffersVid|After DRI2WaitSBC), buf=([\w]+)", msg)
    if m != None:
      (ba, dri2_buf) = m.groups()
      if dri2_buf in dec:
        tr[dec[dri2_buf]].append((tsr, ba))
      continue

  ### End of stdin parsing

  # Display the results, frame per frame
  avg = cnt = 0
  for tsb, tfs in tr.iteritems():
    cnt +=1
    first = prev = string_to_time(tfs[0][0])
    print "\n*** Frame no: %d, timestamp: %s" % (cnt, tsb)
    for el in tfs:
      cur = string_to_time(el[0])
      if cur != prev:
        later = "(%6d us later)" % (cur - prev).microseconds
      else:
        later = "(    first event)"
      print "At %s %s Func: %s" % (el[0][5:14], later, el[1])
      prev = cur
    total = cur - first
    avg += total.microseconds
    print "*** Total: %6d us" % (total.microseconds)

  # Display the totals
  if (cnt != 0):
    if (ecs != 0):
      ecs = "%s KB" % (ecs / 1024)
    else:
      ecs = "N/A (lack of encoder traces)"
    print "\n=-= Encoded stream size: %s" % (ecs)
    print "\n=-= Average: %6d us on %d frames" % (avg / cnt, cnt)
  else:
    print "\n=-= No frame, the pipeline have failed"

  return 0
if __name__ == '__main__':
  main()



