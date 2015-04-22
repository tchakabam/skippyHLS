# skippyHLS 

## skippyHLS: A Fresh HTTP Live Streaming Solution

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



