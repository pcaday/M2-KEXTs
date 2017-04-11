#ifndef __EXTRAHEADERSTUFF
#define __EXTRAHEADERSTUFF 1


// from IOPCIDevice.h in 10.1 (xnu-201.42.3)
#ifndef kIOPCIDeviceStateOff
enum {
    kIOPCIDevicePowerStateCount = 3,
    kIOPCIDeviceOffState	= 0,
    kIOPCIDeviceOnState		= 2
};
#endif


#endif