// Minimal UEFI definitions (UEFI 2.x), only what the monitor uses.
// Built with clang --target=x86_64-unknown-windows, so the MS x64 ABI that
// UEFI expects is the default calling convention.
#ifndef EFI_H
#define EFI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef uint16_t CHAR16;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;
typedef uint64_t EFI_TPL;

#define EFI_SUCCESS             0
#define EFI_ERR(n)              (0x8000000000000000ULL | (n))
#define EFI_LOAD_ERROR          EFI_ERR(1)
#define EFI_INVALID_PARAMETER   EFI_ERR(2)
#define EFI_UNSUPPORTED         EFI_ERR(3)
#define EFI_BUFFER_TOO_SMALL    EFI_ERR(5)
#define EFI_NOT_READY           EFI_ERR(6)
#define EFI_DEVICE_ERROR        EFI_ERR(7)
#define EFI_WRITE_PROTECTED     EFI_ERR(8)
#define EFI_NO_MEDIA            EFI_ERR(12)
#define EFI_MEDIA_CHANGED       EFI_ERR(13)
#define EFI_NOT_FOUND           EFI_ERR(14)
#define EFI_ALREADY_STARTED     EFI_ERR(20)
#define EFI_ERROR(s)            ((int64_t)(s) < 0)

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

//--------------------------------------------------------------------+
// Console
//--------------------------------------------------------------------+

typedef struct {
    uint16_t ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (*Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, bool);
    EFI_STATUS (*ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, EFI_INPUT_KEY *);
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct {
    int32_t MaxMode;
    int32_t Mode;
    int32_t Attribute;
    int32_t CursorColumn;
    int32_t CursorRow;
    bool CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (*Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, bool);
    EFI_STATUS (*OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, const CHAR16 *);
    EFI_STATUS (*TestString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, const CHAR16 *);
    EFI_STATUS (*QueryMode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, uint64_t, uint64_t *, uint64_t *);
    EFI_STATUS (*SetMode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, uint64_t);
    EFI_STATUS (*SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, uint64_t);
    EFI_STATUS (*ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
    EFI_STATUS (*SetCursorPosition)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, uint64_t, uint64_t);
    EFI_STATUS (*EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, bool);
    SIMPLE_TEXT_OUTPUT_MODE *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

//--------------------------------------------------------------------+
// Memory map
//--------------------------------------------------------------------+

enum {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode,
    EfiBootServicesData, EfiRuntimeServicesCode, EfiRuntimeServicesData,
    EfiConventionalMemory, EfiUnusableMemory, EfiACPIReclaimMemory,
    EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
    EfiPalCode, EfiPersistentMemory, EfiMaxMemoryType
};

enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress };

typedef struct {
    uint32_t Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

//--------------------------------------------------------------------+
// Boot services
//--------------------------------------------------------------------+

enum { AllHandles, ByRegisterNotify, ByProtocol };

#define EVT_TIMER   0x80000000
enum { TimerCancel, TimerPeriodic, TimerRelative };

typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_TPL (*RaiseTPL)(EFI_TPL);
    void (*RestoreTPL)(EFI_TPL);
    EFI_STATUS (*AllocatePages)(uint32_t Type, uint32_t MemoryType, uint64_t Pages, EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS, uint64_t);
    EFI_STATUS (*GetMemoryMap)(uint64_t *MapSize, EFI_MEMORY_DESCRIPTOR *Map, uint64_t *MapKey,
                               uint64_t *DescSize, uint32_t *DescVersion);
    EFI_STATUS (*AllocatePool)(uint32_t PoolType, uint64_t Size, void **Buffer);
    EFI_STATUS (*FreePool)(void *);
    EFI_STATUS (*CreateEvent)(uint32_t Type, EFI_TPL, void *NotifyFunction, void *NotifyContext, EFI_EVENT *);
    EFI_STATUS (*SetTimer)(EFI_EVENT, uint32_t Type, uint64_t TriggerTime);
    EFI_STATUS (*WaitForEvent)(uint64_t NumberOfEvents, EFI_EVENT *, uint64_t *Index);
    EFI_STATUS (*SignalEvent)(EFI_EVENT);
    EFI_STATUS (*CloseEvent)(EFI_EVENT);
    EFI_STATUS (*CheckEvent)(EFI_EVENT);
    EFI_STATUS (*InstallProtocolInterface)(EFI_HANDLE *, const EFI_GUID *, uint32_t InterfaceType, void *);
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE, const EFI_GUID *, void **);
    void *Reserved;
    void *RegisterProtocolNotify;
    EFI_STATUS (*LocateHandle)(uint32_t SearchType, const EFI_GUID *, void *SearchKey, uint64_t *BufferSize, EFI_HANDLE *);
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    EFI_STATUS (*Exit)(EFI_HANDLE, EFI_STATUS, uint64_t, CHAR16 *);
    void *UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE, uint64_t MapKey);
    void *GetNextMonotonicCount;
    EFI_STATUS (*Stall)(uint64_t Microseconds);
    EFI_STATUS (*SetWatchdogTimer)(uint64_t Timeout, uint64_t WatchdogCode, uint64_t DataSize, CHAR16 *);
    void *ConnectController;
    void *DisconnectController;
    EFI_STATUS (*OpenProtocol)(EFI_HANDLE, const EFI_GUID *, void **, EFI_HANDLE Agent, EFI_HANDLE Controller, uint32_t Attributes);
    EFI_STATUS (*CloseProtocol)(EFI_HANDLE, const EFI_GUID *, EFI_HANDLE Agent, EFI_HANDLE Controller);
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (*LocateHandleBuffer)(uint32_t SearchType, const EFI_GUID *, void *SearchKey, uint64_t *NoHandles, EFI_HANDLE **Buffer);
    EFI_STATUS (*LocateProtocol)(const EFI_GUID *, void *Registration, void **Interface);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void (*CopyMem)(void *, const void *, uint64_t);
    void (*SetMem)(void *, uint64_t, uint8_t);
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

enum { EfiResetCold, EfiResetWarm, EfiResetShutdown };

typedef struct {
    uint16_t Year;
    uint8_t Month, Day, Hour, Minute, Second, Pad1;
    uint32_t Nanosecond;
    int16_t TimeZone;
    uint8_t Daylight, Pad2;
} EFI_TIME;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (*GetTime)(EFI_TIME *Time, void *Capabilities);
    EFI_STATUS (*SetTime)(EFI_TIME *Time);
    void *GetWakeupTime;
    void *SetWakeupTime;
    void *SetVirtualAddressMap;
    void *ConvertPointer;
    EFI_STATUS (*GetVariable)(const CHAR16 *, const EFI_GUID *, uint32_t *Attributes, uint64_t *DataSize, void *Data);
    void *GetNextVariableName;
    EFI_STATUS (*SetVariable)(const CHAR16 *, const EFI_GUID *, uint32_t Attributes, uint64_t DataSize, const void *Data);
    void *GetNextHighMonotonicCount;
    void (*ResetSystem)(uint32_t ResetType, EFI_STATUS, uint64_t DataSize, void *ResetData);
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    uint64_t NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

//--------------------------------------------------------------------+
// Graphics Output Protocol
//--------------------------------------------------------------------+

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}}

enum { PixelRedGreenBlueReserved8BitPerColor, PixelBlueGreenRedReserved8BitPerColor, PixelBitMask, PixelBltOnly };

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelFormat;
    uint32_t PixelInformation[4];
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    uint64_t SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    uint64_t FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, uint32_t, uint64_t *, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    EFI_STATUS (*SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, uint32_t);
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

//--------------------------------------------------------------------+
// Simple Network Protocol
//--------------------------------------------------------------------+

#define EFI_SIMPLE_NETWORK_RECEIVE_UNICAST   0x01
#define EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST 0x02
#define EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST 0x04

#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL 0x01
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL       0x02
#define EFI_OPEN_PROTOCOL_BY_DRIVER          0x10
#define EFI_OPEN_PROTOCOL_EXCLUSIVE          0x20
#define EFI_OPEN_PROTOCOL_BY_DRIVER          0x10
#define EFI_NATIVE_INTERFACE 0

#define EFI_DRIVER_BINDING_PROTOCOL_GUID     {0x18a031ab, 0xb443, 0x4d1a, {0xa5, 0xc0, 0x0c, 0x09, 0x26, 0x1e, 0x9f, 0x71}}

typedef struct EFI_DRIVER_BINDING_PROTOCOL {
    EFI_STATUS (*Supported)(struct EFI_DRIVER_BINDING_PROTOCOL *, EFI_HANDLE Controller, void *RemainingDevicePath);
    EFI_STATUS (*Start)(struct EFI_DRIVER_BINDING_PROTOCOL *, EFI_HANDLE Controller, void *RemainingDevicePath);
    EFI_STATUS (*Stop)(struct EFI_DRIVER_BINDING_PROTOCOL *, EFI_HANDLE Controller, uint64_t NumberOfChildren,
                       EFI_HANDLE *ChildHandleBuffer);
    uint32_t Version;
    EFI_HANDLE ImageHandle;
    EFI_HANDLE DriverBindingHandle;
} EFI_DRIVER_BINDING_PROTOCOL;

#define EFI_SIMPLE_NETWORK_PROTOCOL_GUID \
    {0xa19832b9, 0xac25, 0x11d3, {0x9a, 0x2d, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}

typedef struct {
    uint8_t Addr[32];
} EFI_MAC_ADDRESS;

enum { EfiSimpleNetworkStopped, EfiSimpleNetworkStarted, EfiSimpleNetworkInitialized };

typedef struct {
    uint32_t State;
    uint32_t HwAddressSize;
    uint32_t MediaHeaderSize;
    uint32_t MaxPacketSize;
    uint32_t NvRamSize;
    uint32_t NvRamAccessSize;
    uint32_t ReceiveFilterMask;
    uint32_t ReceiveFilterSetting;
    uint32_t MaxMCastFilterCount;
    uint32_t MCastFilterCount;
    EFI_MAC_ADDRESS MCastFilter[16];
    EFI_MAC_ADDRESS CurrentAddress;
    EFI_MAC_ADDRESS BroadcastAddress;
    EFI_MAC_ADDRESS PermanentAddress;
    uint8_t IfType;
    bool MacAddressChangeable;
    bool MultipleTxSupported;
    bool MediaPresentSupported;
    bool MediaPresent;
} EFI_SIMPLE_NETWORK_MODE;

typedef struct EFI_SIMPLE_NETWORK_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (*Start)(struct EFI_SIMPLE_NETWORK_PROTOCOL *);
    EFI_STATUS (*Stop)(struct EFI_SIMPLE_NETWORK_PROTOCOL *);
    EFI_STATUS (*Initialize)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, uint64_t ExtraRx, uint64_t ExtraTx);
    void *Reset;
    EFI_STATUS (*Shutdown)(struct EFI_SIMPLE_NETWORK_PROTOCOL *);
    EFI_STATUS (*ReceiveFilters)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, uint32_t Enable, uint32_t Disable,
                                 bool ResetMCastFilter, uint64_t MCastFilterCnt, EFI_MAC_ADDRESS *MCastFilter);
    void *StationAddress;
    void *Statistics;
    void *MCastIpToMac;
    void *NvData;
    EFI_STATUS (*GetStatus)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, uint32_t *InterruptStatus, void **TxBuf);
    EFI_STATUS (*Transmit)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, uint64_t HeaderSize, uint64_t BufferSize, void *Buffer,
                           EFI_MAC_ADDRESS *Src, EFI_MAC_ADDRESS *Dest, uint16_t *Protocol);
    EFI_STATUS (*Receive)(struct EFI_SIMPLE_NETWORK_PROTOCOL *, uint64_t *HeaderSize, uint64_t *BufferSize, void *Buffer,
                          EFI_MAC_ADDRESS *Src, EFI_MAC_ADDRESS *Dest, uint16_t *Protocol);
    EFI_EVENT WaitForPacket;
    EFI_SIMPLE_NETWORK_MODE *Mode;
} EFI_SIMPLE_NETWORK_PROTOCOL;

//--------------------------------------------------------------------+
// PXE Base Code (only Mode, to learn the address PXE got)
//--------------------------------------------------------------------+

#define EFI_PXE_BASE_CODE_PROTOCOL_GUID \
    {0x03c4e603, 0xac28, 0x11d3, {0x9a, 0x2d, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}

typedef union {
    uint32_t Addr[4];
    uint8_t v4[4];
    uint8_t v6[16];
} EFI_IP_ADDRESS;

typedef struct {
    bool Started;
    bool Ipv6Available;
    bool Ipv6Supported;
    bool UsingIpv6;
    bool BisSupported;
    bool BisDetected;
    bool AutoArp;
    bool SendGUID;
    bool DhcpDiscoverValid;
    bool DhcpAckReceived;
    bool ProxyOfferReceived;
    bool PxeDiscoverValid;
    bool PxeReplyReceived;
    bool PxeBisReplyReceived;
    bool IcmpErrorReceived;
    bool TftpErrorReceived;
    bool MakeCallbacks;
    uint8_t TTL;
    uint8_t ToS;
    EFI_IP_ADDRESS StationIp;
    EFI_IP_ADDRESS SubnetMask;
    uint8_t DhcpDiscover[1472];
    uint8_t DhcpAck[1472];          // BOOTP layout: siaddr at offset 20
    // (the rest of the mode is not needed)
} EFI_PXE_BASE_CODE_MODE;

typedef struct {
    uint64_t Revision;
    void *Start, *Stop, *Dhcp, *Discover, *Mtftp, *UdpWrite, *UdpRead, *SetIpFilter, *Arp, *SetParameters,
         *SetStationIp, *SetPackets;
    EFI_PXE_BASE_CODE_MODE *Mode;
} EFI_PXE_BASE_CODE_PROTOCOL;

//--------------------------------------------------------------------+
// Loaded image
//--------------------------------------------------------------------+

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}

typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    uint32_t LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType;
    uint32_t ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

//--------------------------------------------------------------------+
// USB I/O
//--------------------------------------------------------------------+

#define EFI_USB_IO_PROTOCOL_GUID \
    {0x2B2F68D6, 0x0CD2, 0x44cf, {0x8E, 0x8B, 0xBB, 0xA2, 0x0B, 0x1B, 0x5B, 0x75}}

typedef struct __attribute__((packed)) {
    uint8_t RequestType, Request;
    uint16_t Value, Index, Length;
} USB_DEVICE_REQUEST;

typedef struct __attribute__((packed)) {
    uint8_t Length, DescriptorType, InterfaceNumber, AlternateSetting, NumEndpoints;
    uint8_t InterfaceClass, InterfaceSubClass, InterfaceProtocol, Interface;
} USB_INTERFACE_DESCRIPTOR;

typedef struct __attribute__((packed)) {
    uint8_t Length, DescriptorType, EndpointAddress, Attributes;
    uint16_t MaxPacketSize;
    uint8_t Interval;
} USB_ENDPOINT_DESCRIPTOR;

typedef EFI_STATUS (*USB_ASYNC_CB)(void *Data, uint64_t DataLength, void *Context, uint32_t Status);

typedef struct EFI_USB_IO_PROTOCOL {
    EFI_STATUS (*UsbControlTransfer)(struct EFI_USB_IO_PROTOCOL *, USB_DEVICE_REQUEST *, uint32_t Direction,
                                     uint32_t Timeout, void *Data, uint64_t DataLength, uint32_t *Status);
    void *UsbBulkTransfer;
    EFI_STATUS (*UsbAsyncInterruptTransfer)(struct EFI_USB_IO_PROTOCOL *, uint8_t Endpoint, bool IsNewTransfer,
                                            uint64_t PollingInterval, uint64_t DataLength, USB_ASYNC_CB Cb, void *Ctx);
    void *UsbSyncInterruptTransfer;
    void *UsbIsochronousTransfer;
    void *UsbAsyncIsochronousTransfer;
    void *UsbGetDeviceDescriptor;
    void *UsbGetConfigDescriptor;
    EFI_STATUS (*UsbGetInterfaceDescriptor)(struct EFI_USB_IO_PROTOCOL *, USB_INTERFACE_DESCRIPTOR *);
    EFI_STATUS (*UsbGetEndpointDescriptor)(struct EFI_USB_IO_PROTOCOL *, uint8_t Index, USB_ENDPOINT_DESCRIPTOR *);
    void *UsbGetStringDescriptor;
    void *UsbGetSupportedLanguages;
    void *UsbPortReset;
} EFI_USB_IO_PROTOCOL;

enum { EfiUsbDataIn, EfiUsbDataOut, EfiUsbNoData };

//--------------------------------------------------------------------+
// Block I/O, device paths, partitions
//--------------------------------------------------------------------+

#define EFI_BLOCK_IO_PROTOCOL_GUID     {0x964e5b21, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}

typedef struct {
    uint32_t MediaId;
    bool RemovableMedia;
    bool MediaPresent;
    bool LogicalPartition;
    bool ReadOnly;
    bool WriteCaching;
    uint32_t BlockSize;
    uint32_t IoAlign;
    uint64_t LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef struct EFI_BLOCK_IO_PROTOCOL {
    uint64_t Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    EFI_STATUS (*Reset)(struct EFI_BLOCK_IO_PROTOCOL *, bool ExtendedVerification);
    EFI_STATUS (*ReadBlocks)(struct EFI_BLOCK_IO_PROTOCOL *, uint32_t MediaId, uint64_t Lba, uint64_t BufferSize, void *Buffer);
    EFI_STATUS (*WriteBlocks)(struct EFI_BLOCK_IO_PROTOCOL *, uint32_t MediaId, uint64_t Lba, uint64_t BufferSize, const void *Buffer);
    EFI_STATUS (*FlushBlocks)(struct EFI_BLOCK_IO_PROTOCOL *);
} EFI_BLOCK_IO_PROTOCOL;

#define EFI_DEVICE_PATH_PROTOCOL_GUID     {0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}

typedef struct __attribute__((packed)) {
    uint8_t Type, SubType;
    uint16_t Length;
} EFI_DEVICE_PATH;

#define DP_HARDWARE 1
#define DP_ACPI 2
#define DP_MESSAGING 3
#define DP_MEDIA 4
#define DP_END 0x7f
#define DP_MSG_USB 5
#define DP_MSG_MAC 11
#define DP_MSG_IPV4 12
#define DP_MSG_SD 26
#define DP_MSG_EMMC 29
#define DP_MEDIA_HARDDRIVE 1
#define DP_MEDIA_FILEPATH 4

typedef struct __attribute__((packed)) {
    EFI_DEVICE_PATH Header;
    uint32_t PartitionNumber;
    uint64_t PartitionStart;
    uint64_t PartitionSize;
    uint8_t Signature[16];
    uint8_t MBRType;            // 1 MBR, 2 GPT
    uint8_t SignatureType;      // 1 MBR id, 2 GUID
} HARDDRIVE_DEVICE_PATH;

static inline EFI_DEVICE_PATH *dp_next(EFI_DEVICE_PATH *p) { return (EFI_DEVICE_PATH *)((uint8_t *)p + p->Length); }
static inline bool dp_end(const EFI_DEVICE_PATH *p) { return p->Type == DP_END && p->SubType == 0xff; }

#define EFI_OPEN_PROTOCOL_GET_PROTOCOL 0x02

#define EFI_GLOBAL_VARIABLE_GUID     {0x8be4df61, 0x93ca, 0x11d2, {0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c}}
#define EFI_VARIABLE_NON_VOLATILE 1
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 2
#define EFI_VARIABLE_RUNTIME_ACCESS 4

#define SMBIOS_TABLE_GUID     {0xeb9d2d31, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}
#define SMBIOS3_TABLE_GUID     {0xf2fd1544, 0x9794, 0x4a2c, {0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94}}

#define ACPI_20_TABLE_GUID \
    {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}}

#endif
