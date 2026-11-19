/*++

  OtgVbus.sys - keep the pm8150b OTG (USB VBUS boost) asserted under Windows.

  Redmi K20 Pro / Mi 9T Pro "raphael", Snapdragon 855 (SM8150), PMIC pm8150b.

  WHY
    The UEFI driver (RaphaelOtgLatchDxe) proves VBUS can be latched on: at
    ReadyToBoot OTG_EN reads back 1 and the OTG device gets power (keyboard
    backlight lit through the UEFI logo). But when Windows loads, the closed
    Qualcomm Type-C/charger driver (qcpmictcc / ACPI\QCOM0582) re-clears OTG_EN
    during init, so VBUS drops at the login screen and bus-powered OTG devices
    die. The role switch (host mode) already works - VBUS is the only missing
    piece. We do NOT replace the closed driver (it does the CC detection host
    mode needs); we run alongside it and re-assert OTG_EN.

  WHAT
    A tiny software-only (non-PnP) kernel driver. It maps the SM8150 SPMI
    PMIC-arbiter v5 channel for the charger (APID 267 = PPID 0x211) and a system
    thread re-asserts OTG_EN (pm8150b DCDC CMD_OTG, off 0x40, bit0) every second,
    writing only when it finds the bit cleared. Same register sequence the UEFI
    driver verified on this exact phone.

  ADDRESSES (confirmed from raphael's DTS + the UEFI driver's ESP log)
    PMIC-arb v5: chnls 0x0C600000 (+0x10000*apid), obsrvr 0x0E600000 (+0x80*apid).
    APID 267: chnls channel = 0x0D6B0000, obsrvr channel = 0x0E608580.
    Channel regs: CMD +0x00, STATUS +0x08, WDATA0 +0x10, RDATA0 +0x18.
    Write opcode EXT_WRITEL=0, read opcode EXT_READL=1, opcode<<27, off<<4, bc=0.

  RISKS (see README)
    - Direct MMIO to a channel the closed driver also uses: the arbiter
      serializes each command, but a write that lands mid-transaction of the
      other driver could disturb it. Window is tiny (us) and we poke ~1/s.
    - If Windows drives OTG_EN=0 continuously we ping-pong (VBUS flickers);
      if it clears once at init, our re-assert wins and holds.
    - One-shot latch semantics still apply: do NOT charge the phone over the
      port while VBUS is held on.
    UNTESTED until built and loaded.

--*/

#include <ntddk.h>

//
// PMIC-arb v5 channel physical addresses for the charger APID (267).
//
#define CHNLS_PA    0x0D6B0000ULL    // 0x0C600000 + 267 * 0x10000
#define OBSRVR_PA   0x0E608580ULL    // 0x0E600000 + 267 * 0x80
#define CH_LEN      0x20

#define PA_CMD      0x00
#define PA_STATUS   0x08
#define PA_WDATA0   0x10
#define PA_RDATA0   0x18

#define OP_EXT_WRITEL     0u
#define OP_EXT_READL      1u
#define CMD_OPCODE_SHIFT  27u
#define ST_DONE           0x01u
#define ST_ERRMASK        0x0Eu

#define OFF_CMD_OTG         0x40u
#define OFF_OTG_CFG         0x53u
#define OTG_EN_BIT          0x01u
#define OTG_EN_SRC_CFG_BIT  0x02u

#define SPMI_POLL           1000
#define REASSERT_PERIOD_MS  1000

static volatile UCHAR *gChnls;
static volatile UCHAR *gObsrvr;
static PKTHREAD        gThread;
static BOOLEAN         gStop;
static KEVENT          gStopEvt;

#define OTG_LOG(fmt, ...) \
  DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "OTGVBUS: " fmt "\n", ##__VA_ARGS__)

//
// Read one byte of a charger register via the observer channel.
//
static
NTSTATUS
SpmiReadByte (
  _In_  UCHAR  Off,
  _Out_ UCHAR *Val
  )
{
  ULONG  Cmd = (OP_EXT_READL << CMD_OPCODE_SHIFT) | ((ULONG)Off << 4);
  int    i;

  WRITE_REGISTER_ULONG((PULONG)(gObsrvr + PA_CMD), Cmd);

  for (i = 0; i < SPMI_POLL; i++) {
    ULONG St = READ_REGISTER_ULONG((PULONG)(gObsrvr + PA_STATUS));
    if (St & ST_DONE) {
      if (St & ST_ERRMASK) {
        return STATUS_DEVICE_DATA_ERROR;
      }
      *Val = (UCHAR)READ_REGISTER_ULONG((PULONG)(gObsrvr + PA_RDATA0));
      return STATUS_SUCCESS;
    }
    KeStallExecutionProcessor(1);
  }
  return STATUS_IO_TIMEOUT;
}

//
// Write one byte of a charger register via the read/write channel.
//
static
NTSTATUS
SpmiWriteByte (
  _In_ UCHAR  Off,
  _In_ UCHAR  Val
  )
{
  ULONG  Cmd = (OP_EXT_WRITEL << CMD_OPCODE_SHIFT) | ((ULONG)Off << 4);
  int    i;

  WRITE_REGISTER_ULONG((PULONG)(gChnls + PA_WDATA0), (ULONG)Val);
  WRITE_REGISTER_ULONG((PULONG)(gChnls + PA_CMD), Cmd);

  for (i = 0; i < SPMI_POLL; i++) {
    ULONG St = READ_REGISTER_ULONG((PULONG)(gChnls + PA_STATUS));
    if (St & ST_DONE) {
      if (St & ST_ERRMASK) {
        return STATUS_DEVICE_DATA_ERROR;
      }
      return STATUS_SUCCESS;
    }
    KeStallExecutionProcessor(1);
  }
  return STATUS_IO_TIMEOUT;
}

//
// Ensure software owns VBUS (OTG_EN_SRC_CFG clear) and OTG_EN is set.
// Writes only when a bit is found wrong, to minimise bus traffic.
//
static
VOID
EnsureOtg (
  VOID
  )
{
  UCHAR  v;

  if (NT_SUCCESS(SpmiReadByte(OFF_OTG_CFG, &v)) && (v & OTG_EN_SRC_CFG_BIT)) {
    SpmiWriteByte(OFF_OTG_CFG, (UCHAR)(v & ~OTG_EN_SRC_CFG_BIT));
    OTG_LOG("OTG_CFG had HW gating set, cleared (was 0x%02x)", v);
  }

  if (NT_SUCCESS(SpmiReadByte(OFF_CMD_OTG, &v)) && !(v & OTG_EN_BIT)) {
    NTSTATUS st = SpmiWriteByte(OFF_CMD_OTG, (UCHAR)(v | OTG_EN_BIT));
    OTG_LOG("re-asserted OTG_EN (was 0x%02x, status 0x%08x)", v, (ULONG)st);
  }
}

static
VOID
ReassertThread (
  _In_ PVOID Context
  )
{
  LARGE_INTEGER  Timeout;

  UNREFERENCED_PARAMETER(Context);

  while (!gStop) {
    EnsureOtg();
    Timeout.QuadPart = -(LONGLONG)REASSERT_PERIOD_MS * 10000;  // relative, 100ns units
    KeWaitForSingleObject(&gStopEvt, Executive, KernelMode, FALSE, &Timeout);
  }
  PsTerminateSystemThread(STATUS_SUCCESS);
}

static
VOID
OtgVbusUnload (
  _In_ PDRIVER_OBJECT DriverObject
  )
{
  UNREFERENCED_PARAMETER(DriverObject);

  gStop = TRUE;
  KeSetEvent(&gStopEvt, IO_NO_INCREMENT, FALSE);

  if (gThread != NULL) {
    KeWaitForSingleObject(gThread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(gThread);
    gThread = NULL;
  }
  if (gChnls != NULL) {
    MmUnmapIoSpace((PVOID)gChnls, CH_LEN);
    gChnls = NULL;
  }
  if (gObsrvr != NULL) {
    MmUnmapIoSpace((PVOID)gObsrvr, CH_LEN);
    gObsrvr = NULL;
  }
  OTG_LOG("unloaded");
}

NTSTATUS
DriverEntry (
  _In_ PDRIVER_OBJECT  DriverObject,
  _In_ PUNICODE_STRING RegistryPath
  )
{
  PHYSICAL_ADDRESS  Pa;
  HANDLE            ThreadHandle;
  NTSTATUS          Status;

  UNREFERENCED_PARAMETER(RegistryPath);

  DriverObject->DriverUnload = OtgVbusUnload;

  Pa.QuadPart = CHNLS_PA;
  gChnls = (volatile UCHAR *)MmMapIoSpace(Pa, CH_LEN, MmNonCached);
  Pa.QuadPart = OBSRVR_PA;
  gObsrvr = (volatile UCHAR *)MmMapIoSpace(Pa, CH_LEN, MmNonCached);

  if ((gChnls == NULL) || (gObsrvr == NULL)) {
    if (gChnls != NULL)  { MmUnmapIoSpace((PVOID)gChnls, CH_LEN);  gChnls = NULL; }
    if (gObsrvr != NULL) { MmUnmapIoSpace((PVOID)gObsrvr, CH_LEN); gObsrvr = NULL; }
    OTG_LOG("MmMapIoSpace failed");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  KeInitializeEvent(&gStopEvt, NotificationEvent, FALSE);
  gStop = FALSE;

  EnsureOtg();  // assert immediately, then the thread keeps it up

  Status = PsCreateSystemThread(
             &ThreadHandle,
             THREAD_ALL_ACCESS,
             NULL, NULL, NULL,
             ReassertThread,
             NULL
             );
  if (NT_SUCCESS(Status)) {
    ObReferenceObjectByHandle(
      ThreadHandle, THREAD_ALL_ACCESS, *PsThreadType,
      KernelMode, (PVOID *)&gThread, NULL
      );
    ZwClose(ThreadHandle);
  } else {
    OTG_LOG("PsCreateSystemThread failed 0x%08x (one-shot assert only)", (ULONG)Status);
  }

  OTG_LOG("loaded; charger APID 267, chnls 0x%llx obsrvr 0x%llx", (UINT64)CHNLS_PA, (UINT64)OBSRVR_PA);
  return STATUS_SUCCESS;
}
