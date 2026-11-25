/** @file
  PlatformBootManagerLib implementation for Xiaomi Redmi Note 3 (Kenzo).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/CpuLib.h>
#include <Library/DebugLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  // Thin MAGENTA bar (rows 40..80, 32-bit BGRX at 0x83200000) proves BdsDxe started!
  {
    volatile UINT32  *Fb32 = (volatile UINT32 *)(UINTN)0x83200000ULL;
    UINTN            Idx;
    for (Idx = (1080 * 40); Idx < (1080 * 80); Idx++) {
      Fb32[Idx] = 0x00FF00FF;
    }
  }

  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  EfiBootManagerConnectAll ();
}

VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EfiBootManagerConnectAll ();
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (
  UINT16  TimeoutRemain
  )
{
  return;
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  DEBUG ((DEBUG_ERROR, "KenzoPlatformBootManager: Unable to boot any boot option\n"));
  return;
}

VOID
EFIAPI
PlatformBootManagerBdsEntry (
  VOID
  )
{
  return;
}

VOID
EFIAPI
PlatformBootManagerProcessBootCompletion (
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
  return;
}

VOID
EFIAPI
PlatformBootManagerPriorityBoot (
  UINT16  **BootNext
  )
{
  return;
}

VOID
EFIAPI
PlatformBootManagerDeadloop (
  VOID
  )
{
  while (TRUE) {
    CpuPause ();
  }
}

VOID
EFIAPI
PlatformBootManagerOnDemandConInConnect (
  VOID
  )
{
  return;
}
