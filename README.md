# skippyHLS 

## skippyHLS: A Fresh HTTP Live Streaming Solution

Our HLS solution currently integrates with GStreamer as a demux-style element, similarly to the legacy gst-hlsdemux element that comes with gst-plugins-bad. 

We tried to solve substantial issues that came with the legacy element as robustness towards varying network conditions, connection retrial, handling of expiring media URLs (403s) etc. At the same time we tried to simplifiy some concepts to better target our needs and requirements, while (currently) sticking to the HLS standard.

With a high-speed connection we currently achieve a time-to-play of ~1000 ms, our aim is to achieve as low as 800 ms with further optimizations.

Future developments are looking at improving things further, integrating more tightly with an HTTP client library etc.

## Dependencies

* GStreamer 1.x (known to work with 1.2 - 1.5)
* GLib (comes with GStreamer framework in accordance)
* GNU-compatible C-compiler/toolchain of your choice

More info on installing GStreamer on all current platforms can be found here: http://gstreamer.freedesktop.org/download/

On OSX you can also install pkg (see link above) which will result in "Framework" install. See further instructions below to build with an OSX Framework install.

There are binaries for Windows and it may be possible to build the present project from within CygWin environments.

On OSX you might want to install GStreamer/GLib using brew:

Install base framework
```
brew install gstreamer gst-plugins-base
```

Install plugins:
```
brew install gst-plugins-good gst-plugins-bad gst-plugins-ugly
```

On GNU/Linux flavors that have apt you could try ...
```
apt-get install gstreamer gst-plugins-good gst-plugins-bad gst-plugins-ugly
```
... to achieve likely things.

## Build

Run
```
make
```

Note for OSX install with framework package: If you have not installed GStreamer via `brew` then your header files are not located in `/usr/local/...`. But our Makefile can be smart about that if you do ...
```
export OSX_GSTREAMER=yes
```
... which will enable you to build in OSX command line with a Framework pkg install.

## Test

No unit tests owned by this project currently, but working on it. 

NOTE: Building a shared GStreamer plugin library that can be scanned by the factory at init, could enable this to be used by the `gst-launch` tool as well (without the need to build a standalone program to run it).

## Usage

Currently we only compile to a static library in the bundled Makefile. But feel free to integrate the code into your project as it fits.

To register the plugin to the GStreamer core element factory, call the `skippy_hlsdemux_setup` once. That will enable the exported element to be used as an HLS demuxer in a GStreamer pipeline.

## Contribute

We're happy about useful pull-requests!

## What about non-GStreamer things?

We currently tightly integrate with GStreamer as a multimedia framework. Future will show if it makes sense to decouple the implementation further from the integration glue-code (our framework-scope would then become GLib) and enable us to integrate with further multimedia frameworks as VideoLAN (VLC player), Google/Android ExoPlayer, Windows stuff, etc ... Sticking to GLib might enable mixed usage of C with [Vala](https://wiki.gnome.org/Projects/Vala) in the project as a modern very-high level functional language.


