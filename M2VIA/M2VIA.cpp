/*
 * Modified verison of AppleVIA for M2. Unlike AppleVIA, M2VIA does no interrupt handling.
 */
 
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
 */


#include <ppc/proc_reg.h>

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>

#include "M2VIA.h"

#define Verbose_IOLog(x...) { }
//#define Verbose_IOLog(x...) IOLog(x...)

extern "C" {
extern void PE_Determine_Clock_Speeds(unsigned int via_addr,
				      int num_speeds,
				      unsigned long *speed_list);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define super IOService

OSDefineMetaClassAndStructors(M2VIA, IOService);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

bool M2VIA::start(IOService *provider)
{
  M2VIADevice     *nub;
  IOMemoryMap        *viaMemoryMap;
  int                numSpeeds = 0;
  unsigned long      *speedList = 0;
  
  Verbose_IOLog("M2VIA::start() entered\n");
  
  // Call super's start.
  if (!super::start(provider))
    return false;
  
  // Figure out what kind of via device nub to make.
  if (IODTMatchNubWithKeys(provider, "'via-cuda'"))
    viaDeviceType = kVIADeviceTypeCuda;
  else if (IODTMatchNubWithKeys(provider, "'via-pmu'"))
    viaDeviceType = kVIADeviceTypePMU;
  else viaDeviceType = -1;  // This should not happen.
  
	Verbose_IOLog("M2VIA::start() viaDeviceType = %d\n", viaDeviceType);
	
  // get the via's base address
  viaMemoryMap = provider->mapDeviceMemoryWithIndex(0);
  if (viaMemoryMap == 0) return false;
  viaBaseAddress = viaMemoryMap->getVirtualAddress();
  viaMemoryMap->release();

  Verbose_IOLog("M2VIA::start() calculating speeds\n");

  // Calculate the bus and cpu speeds if needed.
  if (provider->getProperty("BusSpeedCorrect") == 0) {
    callPlatformFunction("GetDefaultBusSpeeds", false,
                         &numSpeeds, &speedList, 0, 0);
    PE_Determine_Clock_Speeds(viaBaseAddress, numSpeeds, speedList);
  }

  // In M2, all interrupt-handling is centralized in WhitneyInterruptController
/*
  // Allocate the interruptController instance.
  interruptController = new M2VIAInterruptController;
  if (interruptController == NULL) return false;
  
  // call the interruptController's init method.
  error = interruptController->initInterruptController(provider,
						       viaBaseAddress);
  if (error != kIOReturnSuccess) return false;
  
  handler = interruptController->getInterruptHandlerAddress();
  provider->registerInterrupt(0, interruptController, handler, 0);
  
  provider->enableInterrupt(0);
  
  // Register the interrupt controller so clients can find it.
  interruptControllerName = (OSSymbol *)OSSymbol::withCStringNoCopy(kInterruptControllerName);
  getPlatform()->registerInterruptController(interruptControllerName,
					     interruptController);
*/

  Verbose_IOLog("M2VIA::start() creating child nub\n");

  nub = createNub();
  if (nub == 0) return false;
  
  nub->attach(this);
  nub->registerService();
  
	Verbose_IOLog("M2VIA::start() exited\n");

  return true;
}

M2VIADevice *M2VIA::createNub(void)
{
  bool              err;
  OSSymbol          *name = 0;
  OSArray           *deviceMemoryArray;
  OSDictionary      *dict = 0;
  M2VIADevice    *nub = 0;
  
  do {
    // create the deviceMemory array.
    deviceMemoryArray = getProvider()->getDeviceMemory();
    if (deviceMemoryArray == 0) continue;
    
    // create the name for the viaDevice nub
    if (viaDeviceType == kVIADeviceTypeCuda)
      name = (OSSymbol *)OSSymbol::withCStringNoCopy("cuda");
    else if (viaDeviceType == kVIADeviceTypePMU)
      name = (OSSymbol *)OSSymbol::withCStringNoCopy("pmu");
    else name = 0;
    if (name == 0) continue;
    
    // Create the dictionary for the viaDevice nub.
    dict = OSDictionary::withCapacity(1);
    if (dict == 0) continue;
    
    // add the interrupt numbers and parents to the dictionary.
    err = !dict->setObject("name", name);
    if (err) continue;
    
    // Create the viaDevice nub
    nub = new M2VIADevice;
    if (nub == 0) continue;
    
    if (!nub->init(dict)) {
      nub->release();
      nub = 0;
      continue;
    }
    
    // set the nub's name.
    nub->setName(name);
    
    // set the nub's deviceMemory.
    nub->setDeviceMemory(deviceMemoryArray);
    
  } while(false);
  
  if(name) name->release();
  if(dict) dict->release();
  
  return nub;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef  super
#define super IOService

OSDefineMetaClassAndStructors(M2VIADevice, IOService);

// null device

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

