# kvmonitor

A simple server that mixes audio from one or more RTSP streams (e.g. IP cameras) and then re-exports them as a web page.

Open said web page in any old browser (on your computer, your phone, whatever), start it going, and leave it running in the background.

## What's it for?

I use this as a baby monitor.
* I have an Ubiquiti Unifi camera setup.
* Each camera can be configured to export an RTSP stream on the local network.
* I have two kids. Each has a camera in their room.
* I can leave the audio stream running in the background on my phone while I walk around the house, or when the phone is charging on my nightstand.

## Why not just use an off-the-shelf baby monitor?

Because:
1. Wireless baby monitors that do not use the network don't have enough range to reach across my house.
2. Most network-able baby monitors phone home to a cloud service. I want an all-local setup.

## How does it work?

* The server uses ffmpeg to pull from the RTSP streams. It continuously decodes each camera's audio channel into a ring buffer (even when no clients are connected -- it's pretty cheap TBH).
* When a client connects, the audio channels are mixed and then encoded with ffmpeg.
* Some overcomplicated JavaScript tricks the browser into not buffering. (Otherwise, by default the browser's `<audio>` element implementation wants to buffer like 10 seconds before it will start playing, wtf.)
