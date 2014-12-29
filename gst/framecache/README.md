Framecache: will trade RAM for sweet CPU.
=========================================

This is an initial proof of concept.

Test it:
--------

```
wget -O bbb1080.ogg http://mirrorblender.top-ix.org/peach/bigbuckbunny_movies/big_buck_bunny_1080p_stereo.ogg
```

Get the latest gst-devtools to have gst-validate. "smooth_seek.scenario" is in the same folder as thos README.

Compare :

```
gst-validate-1.0 filesrc location=bbb1080.ogg ! oggdemux ! theoradec ! videoscale ! video/x-raw, width=900, height=720 ! queue ! autovideosink --set-scenario smooth_seek.scenario
```

and 

```
gst-validate-1.0 filesrc location=bbb1080.ogg ! oggdemux ! theoradec ! videoscale ! video/x-raw, width=900, height=720 ! framecache ! queue ! autovideosink --set-scenario smooth_seek.scenario
```

to see what the point is, unless you run this on a supercomputer, in which case why are you even looking for a framecache? Add 10 thousand effects before
this element or stop reading this README and go back to your plans for world domination.

Design:
-------

Initial design proposal at https://bugzilla.gnome.org/show_bug.cgi?id=741754 . Hopefully this code dump will get the discussion going.

Briefly, the intent is to stay in passthrough mode until seeked, at which time a task is started on the source pad, that sends fragmented seeks
upstream and forwards buffers from the requested position. The task is paused and started at each new seek for convenience reasons.
Ideally, the element could be able to get back to passthrough in PLAYING, but the continued caching is arguably a feature.
All the upstream seeks are done on idle in the main thread, for the sake of sequencing, this will have to change.

TODO:
-----

Lots of things. Note that I will only consider merging pull requests containing original ASCII art, as the code is currently cruelly lacking
in this specific area.
