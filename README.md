# PowerBook 1400/5300 Kernel Extensions for OS X

This is a set of OS X kernel extensions for the on-board I/O of the PowerBook 1400 (and just maybe the 5300). These machines lack Open Firmware, so an OF [implementation](https://www.github.com/pcaday/TurboOF) is required. Mac OS X 10.0 - 10.2 can be successfully booted and the full 64MB of RAM is suggested, although not required at least for 10.0. Note that 10.2 has a bug in the kernel that causes it to hang on processors without thermal monitoring, which includes the 1400's 603e.

