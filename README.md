# Moonlight Wii

Moonlight Wii is a fork of [Moonlight WiiU](https://github.com/GaryOderNichts/moonlight-wiiu), a port of [Moonlight Embedded](https://github.com/moonlight-stream/moonlight-embedded), which is an open source client for [Sunshine](https://github.com/LizardByte/Sunshine) and NVIDIA GameStream. Moonlight allows you to stream your full collection of games and applications from your PC to other devices to play them remotely.

Special thanks to [WiiMC-SSLC](https://github.com/SuperrSonic/WiiMC-SSLC) for the accelerated FFmpeg and [Moonlight N3DS](https://github.com/zoeyjodon/moonlight-N3DS) for the UI.
> [!WARNING]
> Moonlight Wii is currently in a very early state and many things do not work perfectly yet. While I have tested this on real hardware quite a bit, it is possible it could fail in unexpected ways.
> This port was created almost entirely using a local AI model (Qwen 3.8 27B), code quality not guaranteed.
> 
> See https://github.com/lint-os/moonlight-wii/tree/master#Known-Issues first!

## Quick Start

> :information_source: A Wii LAN Adapter is highly recommended!

* Grab the latest version from the [releases page](https://github.com/lint-os/moonlight-wii/releases) and extract the moonlight folder to the apps folder of your SD Card.
* Enter the IP of your Sunshine/GFE server in the `moonlight.conf` file located at `sd:/wiiu/apps/moonlight`.
* Ensure your Sunshine/GFE server and Wii U are on the same network.
* If using GFE, turn on Shield Streaming in the GFE settings.
* Launch Moonliight
* Set the IP address in the settings.
* Pair Moonlight Wii with the server.
* Accept the pairing confirmation on your PC.
* Connect to the server with Moonlight Wii.
* Play games!

## Configuration
You can configure all of the documented settings in the `moonlight.conf` file located in `sd:/moonlight`.  
However, the GUI should be able to configure most of them.
Note that a lot of option are commented out by default, to edit them you need to remove the `#` in front of them.

## Supported controllers

* Wiimote

## Known Issues
* Random disconnects on the video stream, cannot reconnect after it fails until Moonliight is restarted
* Sometimes after starting the stream it performs poorly, disconnecting and reconnecting fixes it usually
* Wiimote only works as gamepad, no mouse support yet
* Wiimote drops inputs occasionally
* Audio seems to drop packets randomly
* Crashes without an SD card present
* Video resolutions selection is rather nonexistent currently
* Overscan is not enabled by default, you need to set it

## Troubleshooting

### The stream disconnects frequently/immediately
Depending on your network connection you need to adjust the configuration to find a stable bitrate and resolution.
Try something like this to get started:
```
width = 854
height = 480
fps = 30
```
```
bitrate = 1500
```
Then slowly increase the bitrate until the stream is no longer stable.


## See also

[Moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) is the shared codebase between different Moonlight implementations

## Contribute

1. Fork us
2. Write code
3. Send Pull Requests

## Building from source
Install the required dependencies: `(dkp-)pacman -S --needed wii-dev ppc-freetype ppc-libopus ppc-libexpat`. 

You will also need mbedtls from https://github.com/AndrewPiroli/wii-curl/releases

Run `make` to build moonlight.

### Using docker
You can also build moonlight-wiiu using the provided Dockerfile.  
Use `docker build -t moonlightbuilder .` to build the container.  
Then use `docker run -it --rm -v ${PWD}:/project moonlightbuilder make` to build moonlight.  
