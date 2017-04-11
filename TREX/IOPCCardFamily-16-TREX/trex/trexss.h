#ifndef _TREX_SS_H
#define _TREX_SS_H 1

#define MAP_TREX_DONE_ATTR

extern "C" {


// Return types from init_trex
enum {
    eTREXNoMoreSockets = -1,		// No more sockets in socket table
    eTREXRegisterFailed = -2		// Registration of SS entry points failed
};

int init_trex(IOPCCardBridge *pccard_nub, IOPCIDevice *bridge_nub, void *device_regs, void *socket_windows);
int trex_get_ranges(unsigned int socket, unsigned long *range_list, unsigned int *num_ranges);

}

#endif