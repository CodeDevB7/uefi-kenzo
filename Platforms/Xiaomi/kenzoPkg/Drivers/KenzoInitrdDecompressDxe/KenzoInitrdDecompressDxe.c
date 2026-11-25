/** @file
  Decompresses an LZMA-packed ("KLZM") FAT32 initrd from LittleKernel's
  0x82000000..0x83000000 16 MiB window into high DRAM (0xA0000000) and
  updates /chosen/linux,initrd-{start,end} in-place before UniXNURamDiskDxe.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>

#define KENZO_KLZM_MAGIC          0x4D5A4C4BU  // "KLZM"
#define KENZO_HIGH_INITRD_BASE    0xA0000000ULL
#define KENZO_HIGH_INITRD_MAX     0x10000000U  // 256 MiB protected window

// MSM8956 GCC SDCC1 (eMMC @ 0x07824900) and SDCC2 (microSD @ 0x07864900) clocks
// Verified directly against Kenzo stock Linux kernel (gcc-msm8976).
#define GCC_SDCC1_APPS_CMD_RCGR   0x01842004ULL
#define GCC_SDCC1_APPS_CFG_RCGR   0x01842008ULL
#define GCC_SDCC1_APPS_CBCR       0x01842018ULL
#define GCC_SDCC1_AHB_CBCR        0x0184201CULL
#define GCC_SDCC1_ICE_CORE_CBCR   0x0185D014ULL

#define GCC_SDCC2_APPS_CMD_RCGR   0x01843004ULL
#define GCC_SDCC2_APPS_CFG_RCGR   0x01843008ULL
#define GCC_SDCC2_APPS_CBCR       0x01843018ULL
#define GCC_SDCC2_AHB_CBCR        0x0184301CULL

#define SDCC1_CORE_BASE           0x07824000ULL
#define SDCC1_HC_BASE             0x07824900ULL
#define SDCC2_CORE_BASE           0x07864000ULL
#define SDCC2_HC_BASE             0x07864900ULL

#define SMEM_BASE_ADDR            0x86300000ULL
#define SMEM_HEAP_TOC_OFFSET      0x000000D0ULL
#define APCS_IPC_REG              0x0B011008ULL
#define TLMM_SDC2_HDRV_PULL_CTL   0x0110A000ULL

typedef struct {
  UINT32  Allocated;
  UINT32  Offset;
  UINT32  Size;
  UINT32  AuxBase;
} SMEM_HEAP_ENTRY;

typedef struct {
  CHAR8   Name[20];
  UINT32  Cid;
  UINT32  Ctype;
  UINT32  RefCount;
} SMD_ALLOC_ELM;

typedef struct {
  volatile UINT32  State;
  volatile UINT32  FDsr;
  volatile UINT32  FCts;
  volatile UINT32  FCd;
  volatile UINT32  FRi;
  volatile UINT32  FHead;
  volatile UINT32  FTail;
  volatile UINT32  FState;
  volatile UINT32  FBlockReadIntr;
  volatile UINT32  Tail;
  volatile UINT32  Head;
} SMD_HALF_CH_WORD;

STATIC
UINT64
SmemGetItem (
  IN  UINT32  ItemId,
  OUT UINT32  *SizeOut
  )
{
  CONST SMEM_HEAP_ENTRY  *Toc;
  UINT32                 Base;

  if (ItemId >= 512) {
    return 0;
  }
  Toc = (CONST SMEM_HEAP_ENTRY *)(UINTN)(SMEM_BASE_ADDR + SMEM_HEAP_TOC_OFFSET);
  if (Toc[ItemId].Allocated == 0) {
    return 0;
  }
  if (SizeOut != NULL) {
    *SizeOut = Toc[ItemId].Size;
  }
  Base = Toc[ItemId].AuxBase & 0xFFFFFFFCU;
  return (UINT64)(Base ? Base : (UINT32)SMEM_BASE_ADDR) + Toc[ItemId].Offset;
}

STATIC
VOID
SmdRingRpm (
  VOID
  )
{
  MemoryFence ();
  MmioWrite32 (APCS_IPC_REG, BIT0);
  MemoryFence ();
}

STATIC
VOID
SmdWriteRing (
  IN UINT64      FifoBase,
  IN UINT32      FifoSize,
  IN OUT UINT32  *Head,
  IN CONST VOID  *Src,
  IN UINT32      Len
  )
{
  CONST UINT32  *W = (CONST UINT32 *)Src;
  UINT32        Words = Len / 4;
  UINT32        Idx = *Head;
  UINT32        I;

  for (I = 0; I < Words; I++) {
    MmioWrite32 (FifoBase + Idx, W[I]);
    Idx += 4;
    if (Idx >= FifoSize) {
      Idx = 0;
    }
  }
  *Head = Idx;
}

STATIC
VOID
RpmSendLdoVote (
  IN SMD_HALF_CH_WORD  *Tx,
  IN SMD_HALF_CH_WORD  *Rx,
  IN UINT64            TxFifo,
  IN UINT32            FifoSize,
  IN UINT32            MsgId,
  IN UINT32            LdoId,
  IN UINT32            MicroVolts,
  IN UINT32            MilliAmps
  )
{
  UINT32  Pkt[24];
  UINT32  Head;
  UINTN   Spin;

  ZeroMem (Pkt, sizeof (Pkt));
  // 20-byte SMD packet header (5 words)
  Pkt[0] = 76U;           // Payload bytes (8 + 20 + 48)
  // 8-byte qcom_rpm_header
  Pkt[5] = 0x00716572U;   // "req\0"
  Pkt[6] = 68U;           // Request body bytes (20 + 48)
  // 20-byte qcom_rpm_request
  Pkt[7]  = MsgId;
  Pkt[8]  = 0U;           // ACTIVE_SET
  Pkt[9]  = 0x616F646CU;  // "ldoa"
  Pkt[10] = LdoId;
  Pkt[11] = 48U;          // 4 KVPs (12 bytes each)
  // KVP 1: swen = 1
  Pkt[12] = 0x6E657773U;  // "swen"
  Pkt[13] = 4U;
  Pkt[14] = 1U;
  // KVP 2: uv = MicroVolts
  Pkt[15] = 0x00007675U;  // "uv\0\0"
  Pkt[16] = 4U;
  Pkt[17] = MicroVolts;
  // KVP 3: ma = MilliAmps
  Pkt[18] = 0x0000616DU;  // "ma\0\0"
  Pkt[19] = 4U;
  Pkt[20] = MilliAmps;
  // KVP 4: lsmd = 1 (HPM High Power Mode)
  Pkt[21] = 0x646D736CU;  // "lsmd"
  Pkt[22] = 4U;
  Pkt[23] = 1U;

  Head = Tx->Head;
  SmdWriteRing (TxFifo, FifoSize, &Head, Pkt, sizeof (Pkt));
  MemoryFence ();
  Tx->Head  = Head;
  Tx->FHead = 1U;
  Rx->FTail = 1U;
  SmdRingRpm ();

  // Wait for RPM to consume TX (Tx->Tail == Head) and reply on RX (Rx->Head != Rx->Tail)
  for (Spin = 0; Spin < 5000; Spin++) {
    if ((Tx->Tail == Head) && (Rx->Head != Rx->Tail)) {
      Rx->Tail  = Rx->Head;
      Rx->FTail = 1U;
      SmdRingRpm ();
      break;
    }
    MicroSecondDelay (100);
  }
}

STATIC
VOID
RpmEnableSdcc2Power (
  VOID
  )
{
  UINT32            TblSize;
  UINT64            TblAddr;
  CONST SMD_ALLOC_ELM *Elm;
  UINT32            Count;
  UINT32            I;
  INT32             Cid;
  UINT32            InfoSize;
  UINT32            FifoSize;
  UINT64            InfoAddr;
  UINT64            FifoAddr;
  SMD_HALF_CH_WORD  *Tx;
  SMD_HALF_CH_WORD  *Rx;
  UINTN             Spin;

  TblSize = 0;
  TblAddr = SmemGetItem (13, &TblSize);
  if ((TblAddr == 0) || (TblSize < sizeof (SMD_ALLOC_ELM))) {
    DEBUG ((DEBUG_ERROR, "RPM_SMD: SMEM item 13 missing (init=%08x)\n",
            MmioRead32 (SMEM_BASE_ADDR + 0xC0)));
    return;
  }

  Cid   = -1;
  Elm   = (CONST SMD_ALLOC_ELM *)(UINTN)TblAddr;
  Count = TblSize / sizeof (SMD_ALLOC_ELM);
  for (I = 0; I < Count; I++) {
    if ((AsciiStrnCmp (Elm[I].Name, "rpm_requests", 12) == 0) &&
        ((Elm[I].Ctype & 0xFFU) == 0x0FU))
    {
      Cid = (INT32)Elm[I].Cid;
      break;
    }
  }
  if (Cid < 0) {
    DEBUG ((DEBUG_ERROR, "RPM_SMD: rpm_requests channel not found in %u entries\n", Count));
    return;
  }

  InfoSize = 0;
  FifoSize = 0;
  InfoAddr = SmemGetItem (14 + (UINT32)Cid, &InfoSize);
  FifoAddr = SmemGetItem (338 + (UINT32)Cid, &FifoSize);
  if ((InfoAddr == 0) || (FifoAddr == 0) || (FifoSize < 256)) {
    DEBUG ((DEBUG_ERROR, "RPM_SMD: cid=%d info=%lx(%u) fifo=%lx(%u) invalid\n",
            Cid, InfoAddr, InfoSize, FifoAddr, FifoSize));
    return;
  }

  FifoSize /= 2; // First half = TX (APPS->RPM), second half = RX (RPM->APPS)
  Tx = (SMD_HALF_CH_WORD *)(UINTN)InfoAddr;
  Rx = Tx + 1;

  // Open SMD channel if not already OPENED (state 2)
  if (Tx->State != 2U) {
    Tx->FDsr   = 1;
    Tx->FCts   = 1;
    Tx->FCd    = 1;
    Tx->FState = 1;
    Tx->State  = 1U; // OPENING
    SmdRingRpm ();
    for (Spin = 0; Spin < 2000; Spin++) {
      if ((Rx->State == 1U) || (Rx->State == 2U)) {
        break;
      }
      MicroSecondDelay (100);
    }
    Tx->State  = 2U; // OPENED
    Tx->FState = 1;
    SmdRingRpm ();
    for (Spin = 0; Spin < 2000; Spin++) {
      if (Rx->State == 2U) {
        break;
      }
      MicroSecondDelay (100);
    }
  }

  DEBUG ((DEBUG_ERROR, "RPM_SMD: cid=%d info=%lx fifo=%lx(%u) tx=%u rx=%u\n",
          Cid, InfoAddr, FifoAddr, FifoSize, Tx->State, Rx->State));

  if ((Tx->State == 2U) && (Rx->State == 2U)) {
    UINT32  Sdc1Tlmm = MmioRead32 (0x01109000ULL);
    UINT32  Sdc2Before = MmioRead32 (TLMM_SDC2_HDRV_PULL_CTL);
    // Drain any stale RX bytes left by aboot first
    Rx->Tail = Rx->Head;
    // Enable PM8950 LDO11 (2.95V SD VDD, 800mA HPM) and LDO12 (2.95V SD VDD-IO, 22mA HPM)
    RpmSendLdoVote (Tx, Rx, FifoAddr, FifoSize, 1U, 11U, 2950000U, 800U);
    RpmSendLdoVote (Tx, Rx, FifoAddr, FifoSize, 2U, 12U, 2950000U, 22U);
    MicroSecondDelay (30000);
    // Configure SDC2 TLMM pad drive/pull: CLK=16mA/no-pull, CMD=10mA/pull-up, DATA=10mA/pull-up
    MmioWrite32 (TLMM_SDC2_HDRV_PULL_CTL, 0x00001FE4U);
    DEBUG ((DEBUG_ERROR, "RPM_SMD: LDO11+12 HPM (tx=%u/%u rx=%u/%u tlmm1=%08x tlmm2=%08x->%08x)\n",
            Tx->Tail, Tx->Head, Rx->Tail, Rx->Head, Sdc1Tlmm, Sdc2Before, MmioRead32 (TLMM_SDC2_HDRV_PULL_CTL)));
  }
}

STATIC
VOID
EnableGccBranch (
  IN UINT64  CbcrAddr
  )
{
  UINT32  Val;
  UINTN   Spin;

  Val = MmioRead32 (CbcrAddr);
  if ((Val & BIT0) == 0) {
    MmioWrite32 (CbcrAddr, Val | BIT0);
  }

  for (Spin = 0; Spin < 50000; Spin++) {
    if ((MmioRead32 (CbcrAddr) & BIT31) == 0) {
      break;
    }
  }
}

STATIC
VOID
EnableGccSdcc (
  IN CONST CHAR8  *Name,
  IN UINT64       CmdRcgr,
  IN UINT64       CfgRcgr,
  IN UINT64       AppsCbcr,
  IN UINT64       AhbCbcr
  )
{
  UINT32  AhbBefore;
  UINT32  AppsBefore;
  UINTN   Spin;

  AhbBefore  = MmioRead32 (AhbCbcr);
  AppsBefore = MmioRead32 (AppsCbcr);

  EnableGccBranch (AhbCbcr);
  EnableGccBranch (AppsCbcr);

  // If APPS branch still reports CLK_OFF, switch RCG cleanly to 19.2 MHz XO (src=0, div=0).
  if ((MmioRead32 (AppsCbcr) & BIT31) != 0) {
    MmioWrite32 (CfgRcgr, 0);
    MmioWrite32 (CmdRcgr, MmioRead32 (CmdRcgr) | BIT0);
    for (Spin = 0; Spin < 50000; Spin++) {
      if ((MmioRead32 (CmdRcgr) & BIT0) == 0) {
        break;
      }
    }
    EnableGccBranch (AppsCbcr);
  }

  DEBUG ((
    DEBUG_ERROR,
    "KENZO_SDCC %a: AHB %08x->%08x APPS %08x->%08x RCG=%08x/%08x\n",
    Name,
    AhbBefore,
    MmioRead32 (AhbCbcr),
    AppsBefore,
    MmioRead32 (AppsCbcr),
    MmioRead32 (CmdRcgr),
    MmioRead32 (CfgRcgr)
    ));
}

STATIC
BOOLEAN
SdhciSimpleCmd (
  IN  UINT64  Hc,
  IN  UINT32  Cmd,
  IN  UINT32  Arg,
  IN  UINT16  Flags,
  OUT UINT32  *RespOut
  )
{
  UINTN   Spin;
  UINT16  Norm;
  UINT16  Err;

  MmioWrite32 (Hc + 0x38, 0);
  MmioWrite32 (Hc + 0x34, 0xFFFFFFFFU);
  MmioWrite32 (Hc + 0x30, 0xFFFFFFFFU);
  // Qualcomm SDCC4 requires >=3 SDCLK cycles (~50-100us at 400kHz) for INT_STAT clear to sync.
  MicroSecondDelay (100);
  MmioWrite8  (Hc + 0x2E, 0x0EU);
  for (Spin = 0; Spin < 10000; Spin++) {
    if ((MmioRead32 (Hc + 0x24) & 0x3U) == 0) {
      break;
    }
    MicroSecondDelay (10);
  }
  MmioWrite32 (Hc + 0x08, Arg);
  MmioWrite32 (Hc + 0x0C, ((UINT32)(((Cmd & 0x3FU) << 8) | Flags) << 16));
  Norm = 0;
  Err  = 0;
  for (Spin = 0; Spin < 20000; Spin++) {
    Norm = MmioRead16 (Hc + 0x30);
    if ((Norm & 0x8000U) != 0) {
      Err = MmioRead16 (Hc + 0x32);
      break;
    }
    if ((Norm & 0x0001U) != 0) {
      break;
    }
    MicroSecondDelay (10);
  }
  *RespOut = MmioRead32 (Hc + 0x10);
  MmioWrite32 (Hc + 0x30, 0xFFFFFFFFU);
  MicroSecondDelay (100);
  if ((Err != 0) || ((Norm & 0x0001U) == 0)) {
    MmioWrite8 (Hc + 0x2F, 0x06U);
    for (Spin = 0; Spin < 5000; Spin++) {
      if ((MmioRead8 (Hc + 0x2F) & 0x06U) == 0) {
        break;
      }
      MicroSecondDelay (10);
    }
    DEBUG ((DEBUG_ERROR, "  CMD%u fail norm=%04x err=%04x ps=%08x resp=%08x\n", Cmd, Norm, Err, MmioRead32 (Hc + 0x24), *RespOut));
    return FALSE;
  }
  return TRUE;
}

STATIC
VOID
AckPwrctl (
  IN UINT64  Core
  )
{
  UINT32  St;
  UINT32  Ctl;
  UINTN   Spin;

  for (Spin = 0; Spin < 500; Spin++) {
    St = MmioRead32 (Core + 0xDC);
    if (St != 0) {
      MmioWrite32 (Core + 0xE0, St);
      // BUS_SUCCESS = BIT0 (0x1), IO_SUCCESS = BIT2 (0x4). NEVER set BIT1 (BUS_FAIL)!
      Ctl = 0;
      if ((St & 0x03U) != 0) {
        Ctl |= 0x01U;
      }
      if ((St & 0x0CU) != 0) {
        Ctl |= 0x04U;
      }
      if (Ctl != 0) {
        MmioWrite32 (Core + 0xE8, Ctl);
      }
      MicroSecondDelay (100);
      return;
    }
    MicroSecondDelay (10);
  }
}

STATIC
VOID
SetSdhciClock (
  IN UINT64  Hc,
  IN UINT16  Div
  )
{
  UINTN   Spin;
  UINT16  Clk;

  MmioWrite16 (Hc + 0x2C, 0);
  MicroSecondDelay (100);
  Clk = (UINT16)(((Div & 0xFFU) << 8) | (((Div >> 8) & 0x03U) << 6) | 0x0001U);
  MmioWrite16 (Hc + 0x2C, Clk);
  for (Spin = 0; Spin < 5000; Spin++) {
    if ((MmioRead16 (Hc + 0x2C) & 0x0002U) != 0) {
      break;
    }
    MicroSecondDelay (10);
  }
  MmioWrite16 (Hc + 0x2C, (UINT16)(Clk | 0x0004U));
  MicroSecondDelay (1000);
}

STATIC
BOOLEAN
SdhciReadSectorPio (
  IN  UINT64  Hc,
  IN  UINT32  Lba,
  OUT UINT8   *Buf512
  )
{
  UINTN   Spin;
  UINT16  Norm;
  UINT16  Err;
  BOOLEAN BufRead;
  BOOLEAN XferDone;
  UINT32  W;

  MmioWrite32 (Hc + 0x38, 0);
  MmioWrite32 (Hc + 0x34, 0xFFFFFFFFU);
  MmioWrite32 (Hc + 0x30, 0xFFFFFFFFU);
  MicroSecondDelay (100);
  MmioWrite8  (Hc + 0x2E, 0x0EU);
  for (Spin = 0; Spin < 10000; Spin++) {
    if ((MmioRead32 (Hc + 0x24) & 0x3U) == 0) {
      break;
    }
    MicroSecondDelay (10);
  }
  MmioWrite16 (Hc + 0x04, 512);
  MmioWrite16 (Hc + 0x06, 1);
  MmioWrite32 (Hc + 0x08, Lba);
  MmioWrite32 (Hc + 0x0C, 0x0010U | ((UINT32)((17U << 8) | 0x003AU) << 16)); // Read PIO + CMD17

  Norm     = 0;
  Err      = 0;
  BufRead  = FALSE;
  XferDone = FALSE;
  for (Spin = 0; Spin < 50000; Spin++) {
    Norm = MmioRead16 (Hc + 0x30);
    if ((Norm & 0x8000U) != 0) {
      Err = MmioRead16 (Hc + 0x32);
      break;
    }
    if (!BufRead && ((Norm & 0x0020U) != 0)) {
      MmioWrite16 (Hc + 0x30, 0x0020U);
      for (W = 0; W < 128; W++) {
        ((UINT32 *)Buf512)[W] = MmioRead32 (Hc + 0x20);
      }
      BufRead = TRUE;
    }
    if (BufRead && ((Norm & 0x0002U) != 0)) {
      MmioWrite16 (Hc + 0x30, 0x0002U);
      XferDone = TRUE;
      break;
    }
    MicroSecondDelay (10);
  }
  MmioWrite32 (Hc + 0x30, 0xFFFFFFFFU);
  if ((Err != 0) || !BufRead || !XferDone) {
    DEBUG ((DEBUG_ERROR, "  CMD17(%u) fail norm=%04x err=%04x ps=%08x\n", Lba, Norm, Err, MmioRead32 (Hc + 0x24)));
    return FALSE;
  }
  return TRUE;
}

STATIC
VOID
ProbeSdccController (
  IN CONST CHAR8  *Name,
  IN UINT64       Core,
  IN UINT64       Hc,
  IN BOOLEAN      IsEmmc
  )
{
  UINT32  HcModeBefore;
  UINT32  Pwrctl;
  UINT32  Resp;
  UINTN   Spin;
  UINT8   Sec[512];

  // 1. Switch SDCC1 RCG to clean 19.2 MHz XO so 400 kHz / 9.6 MHz DS needs no HS200/DLL tuning.
  if (IsEmmc) {
    MmioWrite32 (GCC_SDCC1_APPS_CFG_RCGR, 0);
    MmioWrite32 (GCC_SDCC1_APPS_CMD_RCGR, MmioRead32 (GCC_SDCC1_APPS_CMD_RCGR) | BIT0);
    for (Spin = 0; Spin < 50000; Spin++) {
      if ((MmioRead32 (GCC_SDCC1_APPS_CMD_RCGR) & BIT0) == 0) {
        break;
      }
    }
  }

  HcModeBefore = MmioRead32 (Core + 0x78);
  Pwrctl       = MmioRead32 (Core + 0xDC);
  AckPwrctl (Core);

  // 2. Full Qualcomm core reset + HC_MODE_EN (matches Linux sdhci-msm & U-Boot msm_sdhci)
  MmioWrite32 (Core + 0x00, MmioRead32 (Core + 0x00) | BIT7); // CORE_SW_RST
  for (Spin = 0; Spin < 50000; Spin++) {
    if ((MmioRead32 (Core + 0x00) & BIT7) == 0) {
      break;
    }
    MicroSecondDelay (10);
  }
  MmioWrite32 (Core + 0x78, 0x00000001U); // HC_MODE_EN
  MmioWrite32 (Core + 0x78, 0x00002001U); // HC_MODE_EN | FF_CLK_SW_RST_DIS
  AckPwrctl (Core);

  // 3. Standard SDHCI SW_RST_ALL + Bus Power ON + Ack PWRCTL!
  MmioWrite8 (Hc + 0x2F, 0x01U);
  for (Spin = 0; Spin < 10000; Spin++) {
    if ((MmioRead8 (Hc + 0x2F) & 0x01U) == 0) {
      break;
    }
    MicroSecondDelay (10);
  }
  AckPwrctl (Core);

  // Clear Qualcomm vendor DLL/HS200 select-in mux for clean 1-bit DS
  MmioWrite32 (Hc + 0x10C, MmioRead32 (Hc + 0x10C) & ~(0xFU << 18));
  MmioWrite8  (Hc + 0x28, 0x00U); // 1-bit DS
  MmioWrite16 (Hc + 0x3E, 0x0000U);
  MmioWrite8  (Hc + 0x29, IsEmmc ? 0x0BU : 0x0FU); // 1.8V eMMC / 3.3V SD + Bus Power ON
  AckPwrctl (Core);
  SetSdhciClock (Hc, 0x0018U); // 19.2 MHz / (2*24) = 400 kHz

  DEBUG ((
    DEBUG_ERROR,
    "PROBE %a: HC_MODE %x->%x PWRCTL=%x->%x PS=%08x CLK=%04x PWR=%02x\n",
    Name,
    HcModeBefore,
    MmioRead32 (Core + 0x78),
    Pwrctl,
    MmioRead32 (Core + 0xDC),
    MmioRead32 (Hc + 0x24),
    MmioRead16 (Hc + 0x2C),
    MmioRead8  (Hc + 0x29)
    ));

  if (IsEmmc) {
    Resp = 0;
    SdhciSimpleCmd (Hc, 0, 0, 0x00U, &Resp);
    MicroSecondDelay (5000);
    for (Spin = 0; Spin < 200; Spin++) {
      if (SdhciSimpleCmd (Hc, 1, 0x40FF8080U, 0x02U, &Resp) && ((Resp & 0x80000000U) != 0)) {
        break;
      }
      MicroSecondDelay (2000);
    }
    DEBUG ((DEBUG_ERROR, "  eMMC CMD1 OCR=%08x (tries=%u)\n", Resp, (UINT32)Spin));
    if ((Resp & 0x80000000U) != 0) {
      if (SdhciSimpleCmd (Hc, 2, 0, 0x09U, &Resp) &&
          SdhciSimpleCmd (Hc, 3, 2U << 16, 0x1AU, &Resp) &&
          SdhciSimpleCmd (Hc, 7, 2U << 16, 0x1BU, &Resp))
      {
        SdhciSimpleCmd (Hc, 16, 512, 0x1AU, &Resp);
        SetSdhciClock (Hc, 0x0001U); // 9.6 MHz DS
        SdhciSimpleCmd (Hc, 13, 2U << 16, 0x1AU, &Resp);
        DEBUG ((DEBUG_ERROR, "  eMMC TRAN! CMD13=%08x\n", Resp));
        if (SdhciReadSectorPio (Hc, 1, Sec)) {
          DEBUG ((
            DEBUG_ERROR,
            "  eMMC LBA1 SIG: '%c%c%c%c%c%c%c%c' first=%lx last=%lx\n",
            Sec[0], Sec[1], Sec[2], Sec[3], Sec[4], Sec[5], Sec[6], Sec[7],
            *(UINT64 *)(Sec + 40), *(UINT64 *)(Sec + 48)
            ));
        }
      }
    }
  } else {
    // Full SDHC/SDXC microSD init (CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 -> CMD7 -> CMD17(0))
    UINT32  Rca;

    Resp = 0;
    SdhciSimpleCmd (Hc, 0, 0, 0x00U, &Resp);
    MicroSecondDelay (5000);
    if (SdhciSimpleCmd (Hc, 8, 0x1AAU, 0x1AU, &Resp)) {
      DEBUG ((DEBUG_ERROR, "  SDCC2 microSD CMD8 OK! resp=%08x\n", Resp));
      for (Spin = 0; Spin < 200; Spin++) {
        if (!SdhciSimpleCmd (Hc, 55, 0, 0x1AU, &Resp)) {
          break;
        }
        if (SdhciSimpleCmd (Hc, 41, 0x40FF8000U, 0x02U, &Resp) && ((Resp & 0x80000000U) != 0)) {
          break;
        }
        MicroSecondDelay (5000);
      }
      DEBUG ((DEBUG_ERROR, "  SDCC2 ACMD41 OCR=%08x (tries=%u)\n", Resp, (UINT32)Spin));
      if ((Resp & 0x80000000U) != 0) {
        if (SdhciSimpleCmd (Hc, 2, 0, 0x09U, &Resp) &&
            SdhciSimpleCmd (Hc, 3, 0, 0x1AU, &Resp))
        {
          Rca = Resp & 0xFFFF0000U;
          if (SdhciSimpleCmd (Hc, 7, Rca, 0x1BU, &Resp)) {
            SdhciSimpleCmd (Hc, 16, 512, 0x1AU, &Resp);
            SetSdhciClock (Hc, 0x0001U); // 9.6 MHz DS
            SdhciSimpleCmd (Hc, 13, Rca, 0x1AU, &Resp);
            DEBUG ((DEBUG_ERROR, "  SDCC2 microSD TRAN! RCA=%04x CMD13=%08x\n", Rca >> 16, Resp));
            if (SdhciReadSectorPio (Hc, 0, Sec)) {
              DEBUG ((
                DEBUG_ERROR,
                "  SDCC2 LBA0 MBR: %02x %02x (part0 type=%02x start=%08x len=%08x)\n",
                Sec[510], Sec[511], Sec[450],
                ReadUnaligned32 ((CONST UINT32 *)(Sec + 454)),
                ReadUnaligned32 ((CONST UINT32 *)(Sec + 458))
                ));
            }
          }
        }
      }
    }
  }
}

STATIC
VOID
KenzoEnableSdccClocks (
  VOID
  )
{
  EnableGccSdcc ("SDCC1", GCC_SDCC1_APPS_CMD_RCGR, GCC_SDCC1_APPS_CFG_RCGR, GCC_SDCC1_APPS_CBCR, GCC_SDCC1_AHB_CBCR);
  EnableGccBranch (GCC_SDCC1_ICE_CORE_CBCR);
  EnableGccSdcc ("SDCC2", GCC_SDCC2_APPS_CMD_RCGR, GCC_SDCC2_APPS_CFG_RCGR, GCC_SDCC2_APPS_CBCR, GCC_SDCC2_AHB_CBCR);
  RpmEnableSdcc2Power ();
}

// Must match KenzoPlatformMem.c / UniXNURamDiskDxe.c
STATIC CONST EFI_GUID  mEntryArgHobGuid = {
  0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 }
};

RETURN_STATUS
EFIAPI
LzmaUefiDecompressGetInfo (
  IN  CONST VOID  *Source,
  IN  UINT32      SourceSize,
  OUT UINT32      *DestinationSize,
  OUT UINT32      *ScratchSize
  );

RETURN_STATUS
EFIAPI
LzmaUefiDecompress (
  IN CONST VOID  *Source,
  IN UINTN       SourceSize,
  IN OUT VOID    *Destination,
  IN OUT VOID    *Scratch
  );

STATIC
UINT64
ReadCells (
  IN CONST VOID  *Data,
  IN UINT32      Cells
  )
{
  if (Cells == 2) {
    return Fdt64ToCpu (ReadUnaligned64 (Data));
  }

  return Fdt32ToCpu (ReadUnaligned32 (Data));
}

STATIC
VOID
WriteCells (
  OUT VOID    *Data,
  IN  UINT32  Cells,
  IN  UINT64  Value
  )
{
  if (Cells == 2) {
    WriteUnaligned64 (Data, CpuToFdt64 (Value));
  } else {
    WriteUnaligned32 (Data, CpuToFdt32 ((UINT32)Value));
  }
}

EFI_STATUS
EFIAPI
KenzoInitrdDecompressEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  UINT64             DtbAddr;
  VOID               *Fdt;
  INT32              Chosen;
  CONST VOID         *StartProp;
  CONST VOID         *EndProp;
  INT32              StartLen;
  INT32              EndLen;
  UINT64             Start;
  UINT64             End;
  UINTN              CompSize;
  UINT32             DestSize;
  UINT32             ScratchSize;
  VOID               *Scratch;
  RETURN_STATUS      RetStatus;

  KenzoEnableSdccClocks ();

  Hob = GetFirstGuidHob (&mEntryArgHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (UINT64))) {
    return EFI_SUCCESS;
  }

  DtbAddr = ReadUnaligned64 ((CONST UINT64 *)GET_GUID_HOB_DATA (Hob));
  if ((DtbAddr == 0) || ((DtbAddr & 3) != 0)) {
    return EFI_SUCCESS;
  }

  Fdt = (VOID *)(UINTN)DtbAddr;
  if (FdtCheckHeader (Fdt) != 0) {
    return EFI_SUCCESS;
  }

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    return EFI_SUCCESS;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLen);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLen);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLen != 4) && (StartLen != 8)) || ((EndLen != 4) && (EndLen != 8)))
  {
    return EFI_SUCCESS;
  }

  Start = ReadCells (StartProp, StartLen / 4);
  End   = ReadCells (EndProp, EndLen / 4);
  if ((End <= Start) || ((End - Start) < 18) ||
      (ReadUnaligned32 ((CONST UINT32 *)(UINTN)Start) != KENZO_KLZM_MAGIC))
  {
    // Diagnostic UEFI-only boot (no KLZM ramdisk): probe SDCC1 & SDCC2 and hold on screen.
    ProbeSdccController ("SDCC1", SDCC1_CORE_BASE, SDCC1_HC_BASE, TRUE);
    ProbeSdccController ("SDCC2", SDCC2_CORE_BASE, SDCC2_HC_BASE, FALSE);
    DEBUG ((DEBUG_ERROR, "KENZO_SDCC: diagnostic probe complete (holding screen)\n"));
    for (;;) {
      MicroSecondDelay (1000000);
    }
  }

  CompSize = (UINTN)(End - Start - 4);
  RetStatus = LzmaUefiDecompressGetInfo (
                (CONST VOID *)(UINTN)(Start + 4),
                (UINT32)CompSize,
                &DestSize,
                &ScratchSize
                );
  if (RETURN_ERROR (RetStatus) || (DestSize == 0) || (DestSize > KENZO_HIGH_INITRD_MAX)) {
    DEBUG ((DEBUG_ERROR, "KENZO_INITRD: bad LZMA info %r (DestSize=0x%x)\n", RetStatus, DestSize));
    return EFI_SUCCESS;
  }

  Scratch = AllocatePool (ScratchSize);
  if (Scratch == NULL) {
    DEBUG ((DEBUG_ERROR, "KENZO_INITRD: OOM for scratch 0x%x\n", ScratchSize));
    return EFI_SUCCESS;
  }

  RetStatus = LzmaUefiDecompress (
                (CONST VOID *)(UINTN)(Start + 4),
                CompSize,
                (VOID *)(UINTN)KENZO_HIGH_INITRD_BASE,
                Scratch
                );
  FreePool (Scratch);

  if (RETURN_ERROR (RetStatus)) {
    DEBUG ((DEBUG_ERROR, "KENZO_INITRD: LZMA decompress failed: %r\n", RetStatus));
    return EFI_SUCCESS;
  }

  // Update /chosen/linux,initrd-{start,end} in-place for UniXNURamDiskDxe
  WriteCells ((VOID *)StartProp, StartLen / 4, KENZO_HIGH_INITRD_BASE);
  WriteCells ((VOID *)EndProp, EndLen / 4, KENZO_HIGH_INITRD_BASE + DestSize);

  return EFI_SUCCESS;
}
