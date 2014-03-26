#! /usr/bin/python
import gobject
gobject.threads_init()
import gst
import sys
from argparse import ArgumentParser
import time

class Playlist(object):
    def run(self):
        parser = ArgumentParser()
        parser.add_argument('--skip-after', type=int, default=0)
        parser.add_argument('-d', dest='vs',action='store_const', const='dri2videosink', default='dri2videosink')
        parser.add_argument('-k', dest='vs', action='store_const',  const='kmssink', default='')
        parser.add_argument('-p', dest='vs',action='store_const', const='pvrvideosink', default='')
        parser.add_argument('uris', nargs='+')
        args = parser.parse_args()
        
        self.uris = args.uris
        self.skip_after = args.skip_after
        self.playbin = playbin = gst.element_factory_make("playbin2")
        self.sink = gst.element_factory_make(args.vs)
        self.playbin.set_property('video-sink', self.sink)
        bus = playbin.get_bus()
        bus.add_signal_watch()
        bus.connect('message::eos', self.eos)
        bus.connect('message::error', self.error)

        self.current_index = -1
        self.next()

        self.loop = gobject.MainLoop()
        self.loop.run()
        playbin.set_state(gst.STATE_NULL)

    def eos(self, bus, message):
        print 'EOS'
        self.next()

    def error(self, bus, message):
        gerror, debug = message.parse_error()
        print 'ERROR', gerror.message, debug
        self.next()

    def next(self):
        if self.current_index >= 0:
            print '*** STOPPING ', self.uris[self.current_index]
        playbin = self.playbin
        playbin.set_state(gst.STATE_NULL)
        self.current_index += 1
        if self.current_index == len(self.uris):
            self.loop.quit()
            return
        time.sleep(2)

        playbin.props.uri = self.uris[self.current_index]
        print '*** STARTING ', self.uris[self.current_index]
        playbin.set_state(gst.STATE_PLAYING)
        if self.skip_after:
            gobject.timeout_add_seconds(self.skip_after, self.next)
        


if __name__ == '__main__':
    Playlist().run()

