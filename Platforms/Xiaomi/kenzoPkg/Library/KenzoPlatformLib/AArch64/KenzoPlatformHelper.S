//
//  Copyright (c) 2012-2013, ARM Limited. All rights reserved.
//
//  SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>
#include <Library/ArmLib.h>

ASM_FUNC(ArmPlatformPeiBootAction)
  // Save incoming FDT address (passed in x0 by LittleKernel aboot)
  adrp  x9, ASM_PFX(mIncomingFdtAddress)
  str   x0, [x9, #:lo12:ASM_PFX(mIncomingFdtAddress)]
  ret
