/*++

  OtgVbus.sys - keep the pm8150b OTG (USB VBUS boost) asserted under Windows.

  Redmi K20 Pro / Mi 9T Pro "raphael", Snapdragon 855 (SM8150), PMIC pm8150b.

  The UEFI driver proved VBUS can be latched; rev1 of this driver proved we can
  re-assert OTG_EN from *inside Windows* too (the keyboard backlight now lights at
  the login screen, which it never did before). But it then goes dark again: the
  closed qcpmictcc keeps clearing OTG_EN, and a 1 s re-assert loses the race.

  rev2: re-assert fast (100 ms) to try to hold VBUS, and record counters into the
  driver's own service registry key so the dynamics can be read offline from Arch
  (hivex) - we need to know whether Windows clears OTG_EN continuously (race is
  unwinnable by polling) or only on events (fast re-assert holds).

  Registers (confirmed on-device): PMIC-arb v5, charger APID 267 (PPID 0x211),
  chnls 0x0D6B0000, obsrvr 0x0E608580; CMD +0, STATUS +8, WDATA0 +0x10, RDATA0
  +0x18; write op EXT_WRITEL=0, read op EXT_READL=1, opcode<<27, off<<4, bc=0;
  OTG_EN = CMD_OTG(off 0x40) bit0; OTG_EN_SRC_CFG = OTG_CFG(off 0x53) bit1.

  Risks: direct MMIO to a channel the closed driver also uses (arbiter serializes
  each command; a collision could disturb a transaction - window is us). Do NOT
  charge over the port while VBUS is held. UNTESTED knobs until loaded.

--*/

#include <ntddk.h>

#define CHNLS_PA    0x0D6B0000ULL
#define OBSRVR_PA   0x0E608580ULL
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
#define REASSERT_PERIOD_MS  100
#define REGLOG_EVERY_LOOPS  20       // ~ every 2 s

static volatile UCHAR *gChnls;
static volatile UCHAR *gObsrvr;
static PKTHREAD        gThread;
static BOOLEAN         gStop;
static KEVENT          gStopEvt;
static HANDLE          gKey;         // our service registry key, for counters

// counters (single writer: the re-assert thread)
static ULONG gLoops;
static ULONG gFoundCleared;
static ULONG gWriteOk;
static ULONG gWriteFail;
static ULONG gLastCmdOtg;
static ULONG gLastOtgCfg;

#define OTG_LOG(fmt, ...) \
  DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "OTGVBUS: " fmt "\n", ##__VA_ARGS__)

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

static
VOID
EnsureOtg (
  VOID
  )
{
  UCHAR  v;

  gLoops++;

  if (NT_SUCCESS(SpmiReadByte(OFF_OTG_CFG, &v))) {
    gLastOtgCfg = v;
    if (v & OTG_EN_SRC_CFG_BIT) {
      SpmiWriteByte(OFF_OTG_CFG, (UCHAR)(v & ~OTG_EN_SRC_CFG_BIT));
    }
  }

  if (NT_SUCCESS(SpmiReadByte(OFF_CMD_OTG, &v))) {
    gLastCmdOtg = v;
    if (!(v & OTG_EN_BIT)) {
      NTSTATUS st = SpmiWriteByte(OFF_CMD_OTG, (UCHAR)(v | OTG_EN_BIT));
      gFoundCleared++;
      if (NT_SUCCESS(st)) {
        gWriteOk++;
      } else {
        gWriteFail++;
      }
    }
  }
}

static
VOID
SetDword (
  _In_ PCWSTR Name,
  _In_ ULONG  Value
  )
{
  UNICODE_STRING  Vn;
  ULONG           Data = Value;

  if (gKey == NULL) {
    return;
  }
  RtlInitUnicodeString(&Vn, Name);
  ZwSetValueKey(gKey, &Vn, 0, REG_DWORD, &Data, sizeof(Data));
}

static
VOID
WriteCounters (
  VOID
  )
{
  SetDword(L"St_Loops",        gLoops);
  SetDword(L"St_FoundCleared", gFoundCleared);
  SetDword(L"St_WriteOk",      gWriteOk);
  SetDword(L"St_WriteFail",    gWriteFail);
  SetDword(L"St_LastCmdOtg",   gLastCmdOtg);
  SetDword(L"St_LastOtgCfg",   gLastOtgCfg);
}

static
VOID
ReassertThread (
  _In_ PVOID Context
  )
{
  LARGE_INTEGER  Timeout;
  ULONG          sinceLog = 0;

  UNREFERENCED_PARAMETER(Context);

  while (!gStop) {
    EnsureOtg();
    if (++sinceLog >= REGLOG_EVERY_LOOPS) {
      WriteCounters();
      sinceLog = 0;
    }
    Timeout.QuadPart = -(LONGLONG)REASSERT_PERIOD_MS * 10000;  // relative, 100ns units
    KeWaitForSingleObject(&gStopEvt, Executive, KernelMode, FALSE, &Timeout);
  }
  WriteCounters();
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
  if (gKey != NULL) {
    ZwClose(gKey);
    gKey = NULL;
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
  PHYSICAL_ADDRESS   Pa;
  HANDLE             ThreadHandle;
  NTSTATUS           Status;
  OBJECT_ATTRIBUTES  Oa;

  DriverObject->DriverUnload = OtgVbusUnload;

  // Open our service key for counter output (best-effort).
  InitializeObjectAttributes(&Oa, RegistryPath,
                             OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
  if (!NT_SUCCESS(ZwOpenKey(&gKey, KEY_SET_VALUE, &Oa))) {
    gKey = NULL;
  }

  Pa.QuadPart = CHNLS_PA;
  gChnls = (volatile UCHAR *)MmMapIoSpace(Pa, CH_LEN, MmNonCached);
  Pa.QuadPart = OBSRVR_PA;
  gObsrvr = (volatile UCHAR *)MmMapIoSpace(Pa, CH_LEN, MmNonCached);

  if ((gChnls == NULL) || (gObsrvr == NULL)) {
    if (gChnls != NULL)  { MmUnmapIoSpace((PVOID)gChnls, CH_LEN);  gChnls = NULL; }
    if (gObsrvr != NULL) { MmUnmapIoSpace((PVOID)gObsrvr, CH_LEN); gObsrvr = NULL; }
    if (gKey != NULL)    { ZwClose(gKey); gKey = NULL; }
    OTG_LOG("MmMapIoSpace failed");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  KeInitializeEvent(&gStopEvt, NotificationEvent, FALSE);
  gStop = FALSE;

  EnsureOtg();
  WriteCounters();

  Status = PsCreateSystemThread(
             &ThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL,
             ReassertThread, NULL
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

  OTG_LOG("loaded; charger APID 267, period %u ms", REASSERT_PERIOD_MS);
  return STATUS_SUCCESS;
}
