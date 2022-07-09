# kplay

[English](https://gitee.com/wksuper/kplay/blob/master/README.md) | [简体中文](https://gitee.com/wksuper/kplay/blob/master/README-cn.md)

## 介绍

kplay是一个纯控制台的wav文件播放器，可以实时调音。

主要功能：

- 音量调节
- 左右声道平衡调节
- 音调调节
- 节拍调节
- 输出保存成文件
- 跨平台（在GNU-Linux和MacOS上工作地很好)

## 依赖

1. kplay是一个基于百灵鸟的应用，依赖于 ***百灵鸟*** 库。先访问<https://gitee.com/wksuper/lark-release>以安装之。
2. kplay运行时需要 ***SoundTouch*** 库。运行kplay前要安装它。

```bash
$ sudo apt-get install libsoundtouch-dev
```

## 编译和安装

```bash
$ cd kplay
$ mkdir build && cd build
$ cmake ..
$ make
$ sudo make install
$ sudo ldconfig
```

## 运行时截图

![截图](./resources/screenshot.png)

## 功能键

![功能键](./resources/keys.png)
