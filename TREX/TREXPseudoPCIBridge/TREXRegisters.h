#ifndef _TREX_REGISTERS_H
#define _TREX_REGISTERS_H 1


// Socket offsets

#define socket0 0x00
#define socket1 0x10
#define sockOffStep socket1
#define regsSize sockOffStep

// Facts of Life
#define kNumSockets 2

// Register numbers
#define rCfg0 0x1
#define rCfg1 0x2
#define rIntFlag 0x3
#define rIntEnable 0x4
#define rInputs 0x5
#define rMemTiming 0x6
#define rEvents 0x9
#define rIOTiming 0xA
#define rReset 0xF

// +1 defines
#define mAttr 0x20
#define mIOIntf 0x10
#define mMasterIntEnable 0x04	// guess
#define mResetSignal 0x02
#define mOutput 0x01		// output enable for the pins

/* bits to set for I/O interface, clear for memory interface */
#define mSetForIO (mIOIntf | 0x40)

// +2 defines
#define mVppMask 0xC
#define mVcc5V 0x2 		// Actually a 2-bit field (mask 0x3), but this works

/* values for mVpp for some chips */
#define mVpp0V_12 0x0
#define mVpp5V_12 0x4
#define mVpp12V_12 0x8

/* values for mVpp for other chips */
#define mVpp0V_14_15 0x0
#define mVpp5V_14_15 0x8
#define mVpp12V_14_15 0x4

// rIntFlag (0x3) defines
#define mIntFlag 0x01

// rIntEnable (0x4) and rEvents (0x9) defines
#define mInsert 0x80
#define mAccessErr 0x40
#define mWP 0x20
#define mRDYBSY 0x10
#define mCD 0x4
#define mBVD 0x1

#define mIRQ mRDYBSY
#define mSTSCHG mBVD

#define mAllInts 0xFF
#define mAllIntsExceptIOIRQ (mAllInts & ~mIRQ)
#define ioStatusChgMask (mSTSCHG | mAccessErr)

// rInputs (0x5) defines

/* 0x80 to 0x10 as in rIntEnable and rEvents.  */

/* One of these is apparently CD1, the other CD2. Not sure of the one with which
		CDa corresponds. Both use inverted logic. */
#define mCDa 0x8
#define mCDb 0x4

/* Battery voltage detects. Again, these use inverted logic. */
#define mBVD2 0x2
#define mBVD1 0x1

/* Higher-level accessors */
#define mCardPresent(inputs) (!(inputs & (mCDa | mCDb)))
#define mCardAbsent(inputs) (!(~inputs & (mCDa | mCDb))) 
#define mBVD2Asserted(inputs) (!(inputs & mBVD2))
#define mBVD1Asserted(inputs) (!(inputs & mBVD1))

// rMemTiming (0x6) defines
#define mMemTimingHighMask 0xC0
#define mMemSpeedMask 0x3F

#define mMemTimingHighBits 0x40

// rIOTiming (0xA) defines
#define mIOTimingHighMask 0x30
#define mIOSpeedMask 0xF

#define mIOTimingHighBits 0x20

// Window offsets
#define woSocketStep 0x08000000
#define woSocket0 (0 * woSocketStep)
#define woSocket1 (1 * woSocketStep)
#define windowsSize woSocketStep

#define woMemWin 0x00000000
#define woIOWin 0x04000000

// Window lengths
#define ioSpaceSize 0x00010000
#define TREX_io_win_size ioSpaceSize
#define TREX_mem_win_size 0x04000000

// Timing constants
#define timingQuantum ((devtype == 0x14) ? 40 : 60)	
		// in nanoseconds: probably equal to one cycle of the I/O bus


#endif