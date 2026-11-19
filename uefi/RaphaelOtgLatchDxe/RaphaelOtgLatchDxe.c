/** @file
  RaphaelOtgLatchDxe - force the pm8150b OTG (USB VBUS boost) on at boot.

  Redmi K20 Pro / Mi 9T Pro "raphael", Snapdragon 855 (SM8150), PMIC pm8150b.

  WHY
    Under Windows the closed Qualcomm Type-C/charger driver does not assert the
    OTG 5V boost, so bus-powered USB-OTG devices get no power. The boost itself
    lives in the pm8150b SMB charger (DCDC peripheral) and is reached over SPMI.
    This driver drives the boost ON directly from UEFI, before Windows loads.

  WHAT IT DOES  (the "software OTG enable" path, same as mainline
  qcom_usb_vbus-regulator.c for "qcom,pm8150b-vbus-reg"):
    1. OTG_CFG (0x1153): clear OTG_EN_SRC_CFG (BIT1) -> software owns VBUS.
    2. (optional) OTG_CURRENT_LIMIT_CFG (0x1152): set source current limit.
    3. CMD_OTG (0x1140): set OTG_EN (BIT0) -> boost on.

  DIAGNOSTICS
    The arbiter enforces per-EE ownership on writes. Rather than guess UEFI's EE
    and refuse, this version ATTEMPTS the writes and VERIFIES by reading OTG_EN
    back, logging every step. The log goes both to the UART (DEBUG) and to a file
    "raphael-otg.log" in the root of every FAT volume (the ESP), so it can be read
    from Arch by mounting the esp partition - no UART cable needed.

  SAFETY / LIMITATIONS  (read the README)
    - One-shot latch: boost stays on until cleared or reboot. DO NOT plug a
      charger while latched - two 5V sources fight on VBUS. Reboot to charge.
    - Never fails the boot: all error paths log and return.
**/

#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Protocol/SimpleFileSystem.h>

#define RAPHAEL_OTG_SET_CURRENT_LIMIT  1

//
// SM8150 SPMI PMIC-arbiter v5 region bases (from raphael sm8150.dtsi).
//
#define PMIC_ARB_CORE_BASE    0x0C440000u
#define PMIC_ARB_CHNLS_BASE   0x0C600000u
#define PMIC_ARB_OBSRVR_BASE  0x0E600000u
#define PMIC_ARB_CNFG_BASE    0x0C40A000u
#define PMIC_ARB_EE           0u            // observer read-channel EE

#define PMIC_ARB_VERSION_OFF      0x00u
#define PMIC_ARB_FEATURES_OFF     0x04u
#define PMIC_ARB_VERSION_V5_MIN   0x50000000u
#define PMIC_ARB_VERSION_V6_MAX   0x70000000u
#define APID_MAP_OFF(n)           (0x0900u + 4u * (n))   // in core
#define OWNER_TABLE_OFF(n)        (0x0700u + 4u * (n))   // in cnfg

#define OBS_CH_OFF(n)             (0x80u    * (UINT32)(n))
#define RW_CH_OFF(n)              (0x10000u * (UINT32)(n))

#define PA_CMD     0x00u
#define PA_STATUS  0x08u
#define PA_WDATA0  0x10u
#define PA_RDATA0  0x18u

#define OP_EXT_WRITEL    0u
#define OP_EXT_READL     1u
#define CMD_OPCODE_SHIFT 27u

#define ST_DONE     0x01u
#define ST_ERRMASK  0x0Eu

#define PMIC_ARB_MAX_PERIPHS  512u
#define SPMI_POLL_COUNT       1000u

#define CHG_SID        2u
#define CHG_PERIPH     0x11u
#define CHG_PPID       (((CHG_SID) << 8) | (CHG_PERIPH))   // 0x211

#define OFF_CMD_OTG          0x40u   // 0x1140
#define OFF_OTG_CURR_LIMIT   0x52u   // 0x1152
#define OFF_OTG_CFG          0x53u   // 0x1153

#define OTG_EN_BIT              0x01u
#define OTG_EN_SRC_CFG_BIT      0x02u
#define OTG_CURRENT_LIMIT_MASK  0x07u
#define OTG_CURRENT_LIMIT_1500MA 0x02u

//
// ---- log buffer (flushed to the ESP) --------------------------------------
//
STATIC CHAR8  mLog[4096];
STATIC UINTN  mLogLen = 0;

STATIC
VOID
EFIAPI
LogA (
  IN CONST CHAR8  *Fmt,
  ...
  )
{
  CHAR8    Tmp[192];
  VA_LIST  Args;
  UINTN    n;

  VA_START (Args, Fmt);
  AsciiVSPrint (Tmp, sizeof (Tmp), Fmt, Args);
  VA_END (Args);

  n = AsciiStrLen (Tmp);
  if (mLogLen + n + 1 < sizeof (mLog)) {
    CopyMem (mLog + mLogLen, Tmp, n);
    mLogLen += n;
    mLog[mLogLen] = '\0';
  }
  DEBUG ((DEBUG_ERROR, "%a", Tmp));
}

STATIC
VOID
FlushLogToEsp (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Count = 0;
  EFI_HANDLE  *Handles = NULL;
  UINTN       i;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  for (i = 0; i < Count; i++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
    EFI_FILE_PROTOCOL                *Root;
    EFI_FILE_PROTOCOL                *File;
    UINTN                            Len;

    if (EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs))) {
      continue;
    }
    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }
    Status = Root->Open (
                     Root,
                     &File,
                     L"raphael-otg.log",
                     EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                     0
                     );
    if (!EFI_ERROR (Status)) {
      Len = mLogLen;
      File->Write (File, &Len, mLog);
      File->Close (File);
    }
    Root->Close (Root);
  }

  gBS->FreePool (Handles);
}

//
// ---- SPMI PMIC-arb v5 primitives ------------------------------------------
//
STATIC
UINT16
SpmiFindApid (
  IN  UINT16  Ppid,
  OUT UINT32  *OwnerOut
  )
{
  UINT32  Count;
  UINT32  i;
  UINT16  Found      = 0xFFFF;
  UINT32  FoundOwner = 0xFFFFFFFF;

  Count = MmioRead32 (PMIC_ARB_CORE_BASE + PMIC_ARB_FEATURES_OFF) & 0x7FFu;
  if ((Count == 0) || (Count > PMIC_ARB_MAX_PERIPHS)) {
    Count = PMIC_ARB_MAX_PERIPHS;
  }

  for (i = 0; i < Count; i++) {
    UINT32  Map = MmioRead32 (PMIC_ARB_CORE_BASE + APID_MAP_OFF (i));
    UINT32  Owner;

    if (Map == 0) {
      continue;
    }
    if (((Map >> 8) & 0xFFFu) != Ppid) {
      continue;
    }
    Owner      = MmioRead32 (PMIC_ARB_CNFG_BASE + OWNER_TABLE_OFF (i)) & 0x7u;
    Found      = (UINT16)i;
    FoundOwner = Owner;
    if (Owner == PMIC_ARB_EE) {
      break;
    }
  }

  *OwnerOut = FoundOwner;
  return Found;
}

STATIC
EFI_STATUS
SpmiReadByte (
  IN  UINT16  Apid,
  IN  UINT8   Off,
  OUT UINT8   *Val
  )
{
  UINT32  Base = PMIC_ARB_OBSRVR_BASE + OBS_CH_OFF (Apid);
  UINT32  Cmd  = (OP_EXT_READL << CMD_OPCODE_SHIFT) | ((UINT32)Off << 4);
  UINT32  n;

  MmioWrite32 (Base + PA_CMD, Cmd);

  for (n = 0; n < SPMI_POLL_COUNT; n++) {
    UINT32  St = MmioRead32 (Base + PA_STATUS);
    if ((St & ST_DONE) != 0) {
      if ((St & ST_ERRMASK) != 0) {
        return EFI_DEVICE_ERROR;
      }
      *Val = (UINT8)MmioRead32 (Base + PA_RDATA0);
      return EFI_SUCCESS;
    }
    gBS->Stall (1);
  }
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SpmiWriteByte (
  IN  UINT16  Apid,
  IN  UINT8   Off,
  IN  UINT8   Val
  )
{
  UINT32  Base = PMIC_ARB_CHNLS_BASE + RW_CH_OFF (Apid);
  UINT32  Cmd  = (OP_EXT_WRITEL << CMD_OPCODE_SHIFT) | ((UINT32)Off << 4);
  UINT32  n;

  MmioWrite32 (Base + PA_WDATA0, (UINT32)Val);
  MmioWrite32 (Base + PA_CMD, Cmd);

  for (n = 0; n < SPMI_POLL_COUNT; n++) {
    UINT32  St = MmioRead32 (Base + PA_STATUS);
    if ((St & ST_DONE) != 0) {
      if ((St & ST_ERRMASK) != 0) {
        return EFI_DEVICE_ERROR;
      }
      return EFI_SUCCESS;
    }
    gBS->Stall (1);
  }
  return EFI_TIMEOUT;
}

//
// Attempt a masked write and report before/after + status in the log.
//
STATIC
VOID
SpmiTryRmw (
  IN  UINT16       Apid,
  IN  UINT8        Off,
  IN  UINT8        Mask,
  IN  UINT8        NewBits,
  IN  CONST CHAR8  *Name
  )
{
  EFI_STATUS  Status;
  UINT8       Cur = 0;
  UINT8       Next;

  Status = SpmiReadByte (Apid, Off, &Cur);
  if (EFI_ERROR (Status)) {
    LogA ("  %a (0x%02x): read FAIL %r\n", Name, Off, Status);
    return;
  }
  Next   = (UINT8)((Cur & ~Mask) | (NewBits & Mask));
  Status = SpmiWriteByte (Apid, Off, Next);
  LogA ("  %a (0x%02x): 0x%02x -> 0x%02x write %r\n", Name, Off, Cur, Next, Status);
}

//
// ---- the latch ------------------------------------------------------------
//
STATIC
VOID
DoOtgLatch (
  VOID
  )
{
  UINT32  Version;
  UINT16  Apid;
  UINT32  Owner = 0xFFFFFFFF;
  UINT8   CfgBack = 0;
  UINT8   CmdBack = 0;

  mLogLen  = 0;
  mLog[0]  = '\0';

  Version = MmioRead32 (PMIC_ARB_CORE_BASE + PMIC_ARB_VERSION_OFF);
  LogA ("RAPHAEL-OTG: PMIC-arb version 0x%08x\n", Version);

  if ((Version < PMIC_ARB_VERSION_V5_MIN) || (Version >= PMIC_ARB_VERSION_V6_MAX)) {
    LogA ("RAPHAEL-OTG: not a v5 arbiter - abort\n");
    goto Done;
  }

  Apid = SpmiFindApid (CHG_PPID, &Owner);
  if (Apid == 0xFFFF) {
    LogA ("RAPHAEL-OTG: charger PPID 0x%03x NOT FOUND in APID map\n", CHG_PPID);
    goto Done;
  }
  LogA ("RAPHAEL-OTG: PPID 0x%03x -> APID %u, owner EE %u (our read-EE %u)\n",
        CHG_PPID, Apid, Owner, PMIC_ARB_EE);

  //
  // Attempt regardless of owner; the readback below is the real test.
  //
  SpmiTryRmw (Apid, OFF_OTG_CFG, OTG_EN_SRC_CFG_BIT, 0, "OTG_CFG clr SRC_CFG");
 #if RAPHAEL_OTG_SET_CURRENT_LIMIT
  SpmiTryRmw (Apid, OFF_OTG_CURR_LIMIT, OTG_CURRENT_LIMIT_MASK, OTG_CURRENT_LIMIT_1500MA, "OTG_CURR_LIMIT");
 #endif
  SpmiTryRmw (Apid, OFF_CMD_OTG, OTG_EN_BIT, OTG_EN_BIT, "CMD_OTG set OTG_EN");

  //
  // Verify what actually stuck.
  //
  SpmiReadByte (Apid, OFF_OTG_CFG, &CfgBack);
  SpmiReadByte (Apid, OFF_CMD_OTG, &CmdBack);
  LogA ("RAPHAEL-OTG: readback OTG_CFG=0x%02x CMD_OTG=0x%02x OTG_EN=%u -> VBUS %a\n",
        CfgBack, CmdBack, CmdBack & OTG_EN_BIT,
        (CmdBack & OTG_EN_BIT) ? "LATCHED ON" : "still OFF");

Done:
  FlushLogToEsp ();
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  DoOtgLatch ();
}

EFI_STATUS
EFIAPI
RaphaelOtgLatchDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   ReadyToBootEvent;

  Status = EfiCreateEventReadyToBootEx (
             TPL_CALLBACK,
             OnReadyToBoot,
             NULL,
             &ReadyToBootEvent
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RAPHAEL-OTG: ReadyToBoot reg failed (%r); running now\n", Status));
    DoOtgLatch ();
  }

  return EFI_SUCCESS;
}
