#ifndef __SINGER_AUDIO_DEFINES_H
#define __SINGER_AUDIO_DEFINES_H 1


#include <IOKit/audio/IOAudioControl.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>
#include <IOKit/audio/IOAudioDefines.h>

#include <IOKit/IOLib.h>

/* Debugging */
#pragma mark Debugging

#define VERBOSE 1

#if VERBOSE
	#define VerboseIOLog(x...) IOLog(x)
#else
	#define VerboseIOLog(x...) { }
#endif

/* FIFO buffer defines */

#define FIFO_BUF_SIZE 0x2000
#define FIFO_BUF_ALIGN 0x2000

/* Register layout */
#pragma mark Register layout


#define singerMaxVolumeLocal 15
#define singerMinVolumeLocal 0


// Register accessors

#define REG_V(r, off, type) (*((volatile type *) (((char *) r) + off)))
#define REG_VU8(r, off) REG_V(r, off, UInt8)
#define REG_VU16(r, off) REG_V(r, off, UInt16)
#define REG_VU32(r, off) REG_V(r, off, UInt32)
#define REG_VP(r, off) REG_V(r, off, void *)

#define lFifo8(r) REG_VU8(r, 0)
#define rFifo8(r) REG_VU8(r, 0x400)
#define lrFifo8(r) REG_VU16(r, 0x3FF)

#define r0x800(r) REG_VU8(r, 0x800)
#define r0x801(r) REG_VU8(r, 0x801)
#define r0x803(r) REG_VU8(r, 0x803)
#define r0x804(r) REG_VU8(r, 0x804)
#define r0x806(r) REG_VU8(r, 0x806)
#define r0x80A(r) REG_VU8(r, 0x80A)

#define inIntEnable(r) REG_VU8(r, 0xF09)
#define outIntEnable(r) REG_VU8(r, 0xF29)
#define r0xF40(r) REG_VU8(r, 0xF40)
#define fifoBufPhys(r) REG_VP(r, 0xF44)
#define auxDataA(r) REG_VU16(r, 0xF48)
#define auxDataB(r) REG_VU16(r, 0xF4A)
#define r0xF4E(r) REG_VU16(r, 0xF4E)
#define lFifo16(r) REG_VU16(r, 0x1000)
#define rFifo16(r) REG_VU16(r, 0x1800)
#define lrFifo16(r) REG_VU32(r, 0x17FE)

// Register definitions

#define r0x804_fifoOutEmpty 0x4		/* what's the value? */

#define auxDataA_muteMask 0x0400
#define auxDataA_muteShift 10

#define auxDataB_rightVolMask 0x00F0
#define auxDataB_rightVolShift 4
#define auxDataB_leftVolMask 0x0F00
#define auxDataB_leftVolShift 8


#endif /* __SINGER_AUDIO_DEFINES_H */