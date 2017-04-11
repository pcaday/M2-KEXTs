/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 1999 Apple Computer, Inc.  All rights reserved.
 *
 *  DRI: Josh de Cesare
 *
 */

#ifndef _M2VIA_H
#define _M2VIA_H


#define kVIADeviceTypeCuda     (0)
#define kVIADeviceTypePMU      (1)

#define kAuxControlOffset      (0x01600)
#define kT1CounterLowOffset    (0x00800)
#define kT1CounterHightOffset  (0x00A00)
#define kT1LatchLowOffset      (0x00C00)
#define kT1LatchHighOffset     (0x00E00)

class M2VIADevice;
class M2VIAInterruptController;

class M2VIA : public IOService
{
  OSDeclareDefaultStructors(M2VIA);
  
private:
  IOLogicalAddress            viaBaseAddress;
  int                         viaDeviceType;
  M2VIADevice              *viaDevice;
  
public:
  virtual bool start(IOService *provider);
  virtual M2VIADevice *createNub(void);
};

class M2VIADevice : public IOService
{
  OSDeclareDefaultStructors(M2VIADevice);
public: 
};

#endif
