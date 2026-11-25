/** @file
  ARM Generic Timer Virtual Counter library for Xiaomi Redmi Note 3 (Kenzo).
  Qualcomm MSM8956 TZ/LK may leave CNTFRQ_EL0 == 0, which causes ArmPkg
  TimerDxe to compute mTimerTicks == 0 and infinite-loop on the first IRQ.
  Fall back to 19,200,000 Hz (verified from kenzo.dtb clock-frequency).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmGenericTimerCounterLib.h>
#include <Library/ArmLib.h>

#define KENZO_ARCH_TIMER_FREQ_HZ  19200000U

VOID
EFIAPI
ArmGenericTimerEnableTimer (
  VOID
  )
{
  UINTN  TimerCtrlReg;

  TimerCtrlReg  = ArmReadCntvCtl ();
  TimerCtrlReg |= ARM_ARCH_TIMER_ENABLE;
  ArmWriteCntvCtl (TimerCtrlReg);
}

VOID
EFIAPI
ArmGenericTimerReenableTimer (
  VOID
  )
{
}

VOID
EFIAPI
ArmGenericTimerDisableTimer (
  VOID
  )
{
  UINTN  TimerCtrlReg;

  TimerCtrlReg  = ArmReadCntvCtl ();
  TimerCtrlReg &= ~ARM_ARCH_TIMER_ENABLE;
  ArmWriteCntvCtl (TimerCtrlReg);
}

VOID
EFIAPI
ArmGenericTimerSetTimerFreq (
  IN   UINTN  FreqInHz
  )
{
  ArmWriteCntFrq (FreqInHz);
}

UINTN
EFIAPI
ArmGenericTimerGetTimerFreq (
  VOID
  )
{
  UINTN  Freq = ArmReadCntFrq ();

  return (Freq != 0) ? Freq : KENZO_ARCH_TIMER_FREQ_HZ;
}

UINTN
EFIAPI
ArmGenericTimerGetTimerVal (
  VOID
  )
{
  return ArmReadCntvTval ();
}

VOID
EFIAPI
ArmGenericTimerSetTimerVal (
  IN   UINTN  Value
  )
{
  ArmWriteCntvTval (Value);
}

UINT64
EFIAPI
ArmGenericTimerGetSystemCount (
  VOID
  )
{
  return ArmReadCntvCt ();
}

UINTN
EFIAPI
ArmGenericTimerGetTimerCtrlReg (
  VOID
  )
{
  return ArmReadCntvCtl ();
}

VOID
EFIAPI
ArmGenericTimerSetTimerCtrlReg (
  UINTN  Value
  )
{
  ArmWriteCntvCtl (Value);
}

UINT64
EFIAPI
ArmGenericTimerGetCompareVal (
  VOID
  )
{
  return ArmReadCntvCval ();
}

VOID
EFIAPI
ArmGenericTimerSetCompareVal (
  IN   UINT64  Value
  )
{
  ArmWriteCntvCval (Value);
}
