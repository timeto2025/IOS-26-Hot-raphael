/** @file
  RaphaelOtgLatchDxe - force the pm8150b OTG (USB VBUS boost) on at boot.

  Redmi K20 Pro / Mi 9T Pro "raphael", Snapdragon 855 (SM8150), PMIC pm8150b.

  WHY
    Under Windows the closed Qualcomm Type-C/charger driver does not assert the
    OTG 5V boost, so bus-powered USB-OTG devices get no power. The boost itself
    lives in the pm8150b SMB charger (DCDC peripheral) and is reached over SPMI.
    This driver drives the boost ON directly from UEFI, before Windows loads, so
    the result is independent of whatever the OS driver does.

  WHAT IT DOES  (the "software OTG enable" path, same as mainline
  qcom_usb_vbus-regulator.c for "qcom,pm8150b-vbus-reg"):
    1. OTG_CFG (0x1153): clear OTG_EN_SRC_CFG (BIT1) -> take VBUS off the
       hardware CC/ID gating so software owns it.
    2. (optional) OTG_CURRENT_LIMIT_CFG (0x1152): set the source current limit.
    3. CMD_OTG (0x1140): set OTG_EN (BIT0) -> boost on.

  HOW IT TALKS TO THE PMIC
    Direct MMIO to the SM8150 SPMI PMIC-arbiter v5. Region bases and the whole
    transaction format are taken from raphael's own kernel DTS and the mainline
    spmi-pmic-arb.c / u-boot spmi-msm.c:
      core   0x0C440000  (arbiter version, APID count, PPID->APID map @ 0x900)
      chnls  0x0C600000  (read/write channels, stride 0x10000 * apid)
      obsrvr 0x0E600000  (observer read channels, stride 0x80 * apid, ee 0)
      cnfg   0x0C40A000  (ownership table @ 0x700, owner = regval & 7)
    Reads use the observer channel (allowed from any EE). Writes use the RW
    channel and the arbiter ENFORCES ownership: a write is only accepted from the
    EE that owns the APID. We therefore check ownership first and refuse to write
    if UEFI's EE (0) is not the owner, instead of poking the PMIC blindly.

  SAFETY / LIMITATIONS  (read the README)
    - This is a one-shot latch: the boost stays on until it is cleared or the
      phone reboots. DO NOT plug a charger while the boost is latched - two 5V
      sources fight on VBUS. Reboot (or a future CC-aware version) before charging.
    - The driver never fails the boot: every error path just logs and returns
      EFI_SUCCESS.
    - UNTESTED until built and flashed. No part of this has run on hardware yet.
**/

#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>

//
// Set to 1 to also program the OTG source current limit (see table below).
// Left on at 1500 mA: plenty for a flash drive / small hub, well within the
// pm8150b boost rating. Set to 0 to leave the hardware default untouched.
//
#define RAPHAEL_OTG_SET_CURRENT_LIMIT  1

//
// SM8150 SPMI PMIC-arbiter v5 region bases (from raphael sm8150.dtsi spmi@c440000).
//
#define PMIC_ARB_CORE_BASE    0x0C440000u
#define PMIC_ARB_CHNLS_BASE   0x0C600000u
#define PMIC_ARB_OBSRVR_BASE  0x0E600000u
#define PMIC_ARB_CNFG_BASE    0x0C40A000u
#define PMIC_ARB_EE           0u            // UEFI/apps execution environment

// core region fields
#define PMIC_ARB_VERSION_OFF      0x00u
#define PMIC_ARB_FEATURES_OFF     0x04u     // bits[10:0] = number of APIDs
#define PMIC_ARB_VERSION_V5_MIN   0x50000000u
#define PMIC_ARB_VERSION_V6_MAX   0x70000000u
#define APID_MAP_OFF(n)           (0x0900u + 4u * (n))   // in core
#define OWNER_TABLE_OFF(n)        (0x0700u + 4u * (n))   // in cnfg

// per-apid channel strides (v5)
#define OBS_CH_OFF(n)             (0x80u    * (UINT32)(n))   // observer, ee 0
#define RW_CH_OFF(n)              (0x10000u * (UINT32)(n))   // read/write

// channel register offsets
#define PA_CMD     0x00u
#define PA_STATUS  0x08u
#define PA_WDATA0  0x10u
#define PA_RDATA0  0x18u

// command opcodes / format
#define OP_EXT_WRITEL   0u
#define OP_EXT_READL    1u
#define CMD_OPCODE_SHIFT 27u

// status bits
#define ST_DONE     0x01u
#define ST_ERRMASK  0x0Eu   // failure | error | dropped

#define PMIC_ARB_MAX_PERIPHS  512u
#define SPMI_POLL_COUNT       1000u   // * 1us

//
// Target: pm8150b SMB charger DCDC peripheral.
//
#define CHG_SID        2u
#define CHG_PERIPH     0x11u                           // dcdc@1100
#define CHG_PPID       (((CHG_SID) << 8) | (CHG_PERIPH))   // 0x211

// register offsets WITHIN the peripheral (reg & 0xFF); DCDC base is 0x1100
#define OFF_CMD_OTG          0x40u   // 0x1140
#define OFF_OTG_CURR_LIMIT   0x52u   // 0x1152
#define OFF_OTG_CFG          0x53u   // 0x1153

#define OTG_EN_BIT              0x01u   // CMD_OTG bit0
#define OTG_EN_SRC_CFG_BIT      0x02u   // OTG_CFG bit1 (hardware VBUS gating)
#define OTG_CURRENT_LIMIT_MASK  0x07u
#define OTG_CURRENT_LIMIT_1500MA 0x02u  // 0=500 1=1000 2=1500 3=2000 ...

STATIC
UINT16
SpmiFindApid (
  IN  UINT16  Ppid,
  OUT UINT32  *OwnerOut
  )
{
  UINT32  Count;
  UINT32  i;
  UINT16  Found;
  UINT32  FoundOwner;

  Found      = 0xFFFF;
  FoundOwner = 0xFFFFFFFF;

  Count = MmioRead32 (PMIC_ARB_CORE_BASE + PMIC_ARB_FEATURES_OFF) & 0x7FFu;
  if ((Count == 0) || (Count > PMIC_ARB_MAX_PERIPHS)) {
    Count = PMIC_ARB_MAX_PERIPHS;
  }

  for (i = 0; i < Count; i++) {
    UINT32  Map   = MmioRead32 (PMIC_ARB_CORE_BASE + APID_MAP_OFF (i));
    UINT32  Owner;

    if (Map == 0) {
      continue;
    }
    if (((Map >> 8) & 0xFFFu) != Ppid) {
      continue;
    }

    Owner = MmioRead32 (PMIC_ARB_CNFG_BASE + OWNER_TABLE_OFF (i)) & 0x7u;

    //
    // Several APIDs can map to one PPID; the writable one is the APID our EE
    // owns. Prefer an EE-owned match; otherwise remember the last match so the
    // caller can report who actually owns the charger.
    //
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

  MmioWrite32 (Base + PA_CMD, Cmd);           // byte count - 1 = 0 (1 byte)

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
  MmioWrite32 (Base + PA_CMD, Cmd);           // byte count - 1 = 0 (1 byte)

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
// Read-modify-write one bit field in a charger register (via the APID channel).
//
STATIC
EFI_STATUS
SpmiRmw (
  IN  UINT16  Apid,
  IN  UINT8   Off,
  IN  UINT8   Mask,
  IN  UINT8   NewBits
  )
{
  EFI_STATUS  Status;
  UINT8       Cur;
  UINT8       Next;

  Status = SpmiReadByte (Apid, Off, &Cur);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RAPHAEL-OTG: read off 0x%02x failed (%r)\n", Off, Status));
    return Status;
  }

  Next = (UINT8)((Cur & ~Mask) | (NewBits & Mask));
  if (Next == Cur) {
    DEBUG ((DEBUG_INFO, "RAPHAEL-OTG: off 0x%02x already 0x%02x\n", Off, Cur));
    return EFI_SUCCESS;
  }

  Status = SpmiWriteByte (Apid, Off, Next);
  DEBUG ((
    DEBUG_INFO,
    "RAPHAEL-OTG: off 0x%02x 0x%02x -> 0x%02x (%r)\n",
    Off, Cur, Next, Status
    ));
  return Status;
}

//
// The actual latch. Run from a ReadyToBoot callback (see entry point) so it
// happens after every other DXE/BDS driver - including UEFI's own PMIC/charger
// driver - has initialised, and nothing re-disables OTG_EN before the OS loads.
//
STATIC
VOID
DoOtgLatch (
  VOID
  )
{
  UINT32  Version;
  UINT16  Apid;
  UINT32  Owner;
  UINT8   Verify;

  Version = MmioRead32 (PMIC_ARB_CORE_BASE + PMIC_ARB_VERSION_OFF);
  DEBUG ((DEBUG_INFO, "RAPHAEL-OTG: PMIC-arb version 0x%08x\n", Version));

  if ((Version < PMIC_ARB_VERSION_V5_MIN) || (Version >= PMIC_ARB_VERSION_V6_MAX)) {
    DEBUG ((DEBUG_ERROR, "RAPHAEL-OTG: not a v5 arbiter, aborting (boot continues)\n"));
    return;
  }

  Apid = SpmiFindApid (CHG_PPID, &Owner);
  if (Apid == 0xFFFF) {
    DEBUG ((DEBUG_ERROR, "RAPHAEL-OTG: charger PPID 0x%03x not found in APID map\n", CHG_PPID));
    return;
  }
  DEBUG ((DEBUG_INFO, "RAPHAEL-OTG: charger PPID 0x%03x -> APID %u, owner EE %u\n", CHG_PPID, Apid, Owner));

  if (Owner != PMIC_ARB_EE) {
    DEBUG ((
      DEBUG_ERROR,
      "RAPHAEL-OTG: charger APID owned by EE %u, not %u - UEFI cannot write it; aborting\n",
      Owner, PMIC_ARB_EE
      ));
    return;
  }

  // 1. Take VBUS off the hardware CC/ID gating so software controls OTG_EN.
  SpmiRmw (Apid, OFF_OTG_CFG, OTG_EN_SRC_CFG_BIT, 0);

 #if RAPHAEL_OTG_SET_CURRENT_LIMIT
  // 2. Program the OTG source current limit.
  SpmiRmw (Apid, OFF_OTG_CURR_LIMIT, OTG_CURRENT_LIMIT_MASK, OTG_CURRENT_LIMIT_1500MA);
 #endif

  // 3. Enable the boost.
  SpmiRmw (Apid, OFF_CMD_OTG, OTG_EN_BIT, OTG_EN_BIT);

  // Verify.
  if (!EFI_ERROR (SpmiReadByte (Apid, OFF_CMD_OTG, &Verify))) {
    DEBUG ((
      DEBUG_ERROR,     // DEBUG_ERROR level only so it is visible at default mask
      "RAPHAEL-OTG: CMD_OTG readback 0x%02x, OTG_EN=%u (VBUS latched %a)\n",
      Verify,
      Verify & OTG_EN_BIT,
      (Verify & OTG_EN_BIT) ? "ON" : "OFF"
      ));
  }
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  //
  // ReadyToBoot can fire more than once (e.g. user returns to the boot menu and
  // boots again). Re-asserting the latch each time is harmless and idempotent.
  //
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

  //
  // Do the latch as late as possible - just before the OS is handed control -
  // so no later UEFI driver can turn OTG_EN back off behind us.
  //
  Status = EfiCreateEventReadyToBootEx (
             TPL_CALLBACK,
             OnReadyToBoot,
             NULL,
             &ReadyToBootEvent
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RAPHAEL-OTG: ReadyToBoot event registration failed (%r)\n", Status));
    //
    // Fall back to doing it now, at driver load, rather than not at all.
    //
    DoOtgLatch ();
  }

  return EFI_SUCCESS;
}
