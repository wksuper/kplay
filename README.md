# kplay

[English](https://gitee.com/wksuper/kplay/blob/master/README.md) | [简体中文](https://gitee.com/wksuper/kplay/blob/master/README-cn.md)

## Introduction

kplay is a pure console wav file player that can tune the sound in real-time.

Main features

- Volume adjustment
- Left right channel balance adjustment
- Pitch adjustment
- Tempo adjustment
- Saving output to file
- Cross platform (Works well on GNU-Linux and MacOS)

## Dependency

1. kplay is a lark-based application depending on the ***lark*** library. Visit <https://gitee.com/wksuper/lark-release> to get it installed first.
2. The ***SoundTouch*** library is needed by kplay at runtime. Install it before running kplay.

```bash
$ sudo apt-get install libsoundtouch-dev
```

## Build and Install

```bash
$ cd kplay
$ mkdir build && cd build
$ cmake ..
$ make
$ sudo make install
$ sudo ldconfig
```

## Running Screenshot

![screenshot](./resources/screenshot.png)

## Function Keys

![function keys](./resources/keys.png)
