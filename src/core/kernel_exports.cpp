#include "core/kernel_exports.h"

namespace mc3 {
namespace {

#define F(ordinal, name) {ordinal, KernelExportKind::Function, #name}
#define D(ordinal, name) {ordinal, KernelExportKind::Data, #name}

// Retail xboxkrnl.exe exports, indexed by ordinal - 1.
constexpr KernelExport kExports[] = {
    F(1, AvGetSavedDataAddress), F(2, AvSendTVEncoderOption), F(3, AvSetDisplayMode), F(4, AvSetSavedDataAddress),
    F(5, DbgBreakPoint), F(6, DbgBreakPointWithStatus), F(7, DbgLoadImageSymbols), F(8, DbgPrint),
    F(9, HalReadSMCTrayState), F(10, DbgPrompt), F(11, DbgUnLoadImageSymbols), F(12, ExAcquireReadWriteLockExclusive),
    F(13, ExAcquireReadWriteLockShared), F(14, ExAllocatePool), F(15, ExAllocatePoolWithTag), D(16, ExEventObjectType),
    F(17, ExFreePool), F(18, ExInitializeReadWriteLock), F(19, ExInterlockedAddLargeInteger),
    F(20, ExInterlockedAddLargeStatistic), F(21, ExInterlockedCompareExchange64), D(22, ExMutantObjectType),
    F(23, ExQueryPoolBlockSize), F(24, ExQueryNonVolatileSetting), F(25, ExReadWriteRefurbInfo),
    F(26, ExRaiseException), F(27, ExRaiseStatus), F(28, ExReleaseReadWriteLock), F(29, ExSaveNonVolatileSetting),
    D(30, ExSemaphoreObjectType), D(31, ExTimerObjectType), F(32, ExfInterlockedInsertHeadList),
    F(33, ExfInterlockedInsertTailList), F(34, ExfInterlockedRemoveHeadList), F(35, FscGetCacheSize),
    F(36, FscInvalidateIdleBlocks), F(37, FscSetCacheSize), F(38, HalClearSoftwareInterrupt),
    F(39, HalDisableSystemInterrupt), D(40, HalDiskCachePartitionCount), D(41, HalDiskModelNumber),
    D(42, HalDiskSerialNumber), F(43, HalEnableSystemInterrupt), F(44, HalGetInterruptVector),
    F(45, HalReadSMBusValue), F(46, HalReadWritePCISpace), F(47, HalRegisterShutdownNotification),
    F(48, HalRequestSoftwareInterrupt), F(49, HalReturnToFirmware), F(50, HalWriteSMBusValue),
    F(51, InterlockedCompareExchange), F(52, InterlockedDecrement), F(53, InterlockedIncrement),
    F(54, InterlockedExchange), F(55, InterlockedExchangeAdd), F(56, InterlockedFlushSList),
    F(57, InterlockedPopEntrySList), F(58, InterlockedPushEntrySList), F(59, IoAllocateIrp),
    F(60, IoBuildAsynchronousFsdRequest), F(61, IoBuildDeviceIoControlRequest), F(62, IoBuildSynchronousFsdRequest),
    F(63, IoCheckShareAccess), D(64, IoCompletionObjectType), F(65, IoCreateDevice), F(66, IoCreateFile),
    F(67, IoCreateSymbolicLink), F(68, IoDeleteDevice), F(69, IoDeleteSymbolicLink), D(70, IoDeviceObjectType),
    D(71, IoFileObjectType), F(72, IoFreeIrp), F(73, IoInitializeIrp), F(74, IoInvalidDeviceRequest),
    F(75, IoQueryFileInformation), F(76, IoQueryVolumeInformation), F(77, IoQueueThreadIrp),
    F(78, IoRemoveShareAccess), F(79, IoSetIoCompletion), F(80, IoSetShareAccess), F(81, IoStartNextPacket),
    F(82, IoStartNextPacketByKey), F(83, IoStartPacket), F(84, IoSynchronousDeviceIoControlRequest),
    F(85, IoSynchronousFsdRequest), F(86, IofCallDriver), F(87, IofCompleteRequest), D(88, KdDebuggerEnabled),
    D(89, KdDebuggerNotPresent), F(90, IoDismountVolume), F(91, IoDismountVolumeByName), F(92, KeAlertResumeThread),
    F(93, KeAlertThread), F(94, KeBoostPriorityThread), F(95, KeBugCheck), F(96, KeBugCheckEx),
    F(97, KeCancelTimer), F(98, KeConnectInterrupt), F(99, KeDelayExecutionThread), F(100, KeDisconnectInterrupt),
    F(101, KeEnterCriticalRegion), D(102, MmGlobalData), F(103, KeGetCurrentIrql), F(104, KeGetCurrentThread),
    F(105, KeInitializeApc), F(106, KeInitializeDeviceQueue), F(107, KeInitializeDpc), F(108, KeInitializeEvent),
    F(109, KeInitializeInterrupt), F(110, KeInitializeMutant), F(111, KeInitializeQueue),
    F(112, KeInitializeSemaphore), F(113, KeInitializeTimerEx), F(114, KeInsertByKeyDeviceQueue),
    F(115, KeInsertDeviceQueue), F(116, KeInsertHeadQueue), F(117, KeInsertQueue), F(118, KeInsertQueueApc),
    F(119, KeInsertQueueDpc), D(120, KeInterruptTime), F(121, KeIsExecutingDpc), F(122, KeLeaveCriticalRegion),
    F(123, KePulseEvent), F(124, KeQueryBasePriorityThread), F(125, KeQueryInterruptTime),
    F(126, KeQueryPerformanceCounter), F(127, KeQueryPerformanceFrequency), F(128, KeQuerySystemTime),
    F(129, KeRaiseIrqlToDpcLevel), F(130, KeRaiseIrqlToSynchLevel), F(131, KeReleaseMutant),
    F(132, KeReleaseSemaphore), F(133, KeRemoveByKeyDeviceQueue), F(134, KeRemoveDeviceQueue),
    F(135, KeRemoveEntryDeviceQueue), F(136, KeRemoveQueue), F(137, KeRemoveQueueDpc), F(138, KeResetEvent),
    F(139, KeRestoreFloatingPointState), F(140, KeResumeThread), F(141, KeRundownQueue),
    F(142, KeSaveFloatingPointState), F(143, KeSetBasePriorityThread), F(144, KeSetDisableBoostThread),
    F(145, KeSetEvent), F(146, KeSetEventBoostPriority), F(147, KeSetPriorityProcess), F(148, KeSetPriorityThread),
    F(149, KeSetTimer), F(150, KeSetTimerEx), F(151, KeStallExecutionProcessor), F(152, KeSuspendThread),
    F(153, KeSynchronizeExecution), D(154, KeSystemTime), F(155, KeTestAlertThread), D(156, KeTickCount),
    D(157, KeTimeIncrement), F(158, KeWaitForMultipleObjects), F(159, KeWaitForSingleObject), F(160, KfRaiseIrql),
    F(161, KfLowerIrql), D(162, KiBugCheckData), F(163, KiUnlockDispatcherDatabase), D(164, LaunchDataPage),
    F(165, MmAllocateContiguousMemory), F(166, MmAllocateContiguousMemoryEx), F(167, MmAllocateSystemMemory),
    F(168, MmClaimGpuInstanceMemory), F(169, MmCreateKernelStack), F(170, MmDeleteKernelStack),
    F(171, MmFreeContiguousMemory), F(172, MmFreeSystemMemory), F(173, MmGetPhysicalAddress),
    F(174, MmIsAddressValid), F(175, MmLockUnlockBufferPages), F(176, MmLockUnlockPhysicalPage),
    F(177, MmMapIoSpace), F(178, MmPersistContiguousMemory), F(179, MmQueryAddressProtect),
    F(180, MmQueryAllocationSize), F(181, MmQueryStatistics), F(182, MmSetAddressProtect), F(183, MmUnmapIoSpace),
    F(184, NtAllocateVirtualMemory), F(185, NtCancelTimer), F(186, NtClearEvent), F(187, NtClose),
    F(188, NtCreateDirectoryObject), F(189, NtCreateEvent), F(190, NtCreateFile), F(191, NtCreateIoCompletion),
    F(192, NtCreateMutant), F(193, NtCreateSemaphore), F(194, NtCreateTimer), F(195, NtDeleteFile),
    F(196, NtDeviceIoControlFile), F(197, NtDuplicateObject), F(198, NtFlushBuffersFile),
    F(199, NtFreeVirtualMemory), F(200, NtFsControlFile), F(201, NtOpenDirectoryObject), F(202, NtOpenFile),
    F(203, NtOpenSymbolicLinkObject), F(204, NtProtectVirtualMemory), F(205, NtPulseEvent),
    F(206, NtQueueApcThread), F(207, NtQueryDirectoryFile), F(208, NtQueryDirectoryObject), F(209, NtQueryEvent),
    F(210, NtQueryFullAttributesFile), F(211, NtQueryInformationFile), F(212, NtQueryIoCompletion),
    F(213, NtQueryMutant), F(214, NtQuerySemaphore), F(215, NtQuerySymbolicLinkObject), F(216, NtQueryTimer),
    F(217, NtQueryVirtualMemory), F(218, NtQueryVolumeInformationFile), F(219, NtReadFile),
    F(220, NtReadFileScatter), F(221, NtReleaseMutant), F(222, NtReleaseSemaphore), F(223, NtRemoveIoCompletion),
    F(224, NtResumeThread), F(225, NtSetEvent), F(226, NtSetInformationFile), F(227, NtSetIoCompletion),
    F(228, NtSetSystemTime), F(229, NtSetTimerEx), F(230, NtSignalAndWaitForSingleObjectEx),
    F(231, NtSuspendThread), F(232, NtUserIoApcDispatcher), F(233, NtWaitForSingleObject),
    F(234, NtWaitForSingleObjectEx), F(235, NtWaitForMultipleObjectsEx), F(236, NtWriteFile),
    F(237, NtWriteFileGather), F(238, NtYieldExecution), F(239, ObCreateObject), D(240, ObDirectoryObjectType),
    F(241, ObInsertObject), F(242, ObMakeTemporaryObject), F(243, ObOpenObjectByName),
    F(244, ObOpenObjectByPointer), D(245, ObpObjectHandleTable), F(246, ObReferenceObjectByHandle),
    F(247, ObReferenceObjectByName), F(248, ObReferenceObjectByPointer), D(249, ObSymbolicLinkObjectType),
    F(250, ObfDereferenceObject), F(251, ObfReferenceObject), F(252, PhyGetLinkState), F(253, PhyInitialize),
    F(254, PsCreateSystemThread), F(255, PsCreateSystemThreadEx), F(256, PsQueryStatistics),
    F(257, PsSetCreateThreadNotifyRoutine), F(258, PsTerminateSystemThread), D(259, PsThreadObjectType),
    F(260, RtlAnsiStringToUnicodeString), F(261, RtlAppendStringToString), F(262, RtlAppendUnicodeStringToString),
    F(263, RtlAppendUnicodeToString), F(264, RtlAssert), F(265, RtlCaptureContext),
    F(266, RtlCaptureStackBackTrace), F(267, RtlCharToInteger), F(268, RtlCompareMemory),
    F(269, RtlCompareMemoryUlong), F(270, RtlCompareString), F(271, RtlCompareUnicodeString),
    F(272, RtlCopyString), F(273, RtlCopyUnicodeString), F(274, RtlCreateUnicodeString),
    F(275, RtlDowncaseUnicodeChar), F(276, RtlDowncaseUnicodeString), F(277, RtlEnterCriticalSection),
    F(278, RtlEnterCriticalSectionAndRegion), F(279, RtlEqualString), F(280, RtlEqualUnicodeString),
    F(281, RtlExtendedIntegerMultiply), F(282, RtlExtendedLargeIntegerDivide), F(283, RtlExtendedMagicDivide),
    F(284, RtlFillMemory), F(285, RtlFillMemoryUlong), F(286, RtlFreeAnsiString), F(287, RtlFreeUnicodeString),
    F(288, RtlGetCallersAddress), F(289, RtlInitAnsiString), F(290, RtlInitUnicodeString),
    F(291, RtlInitializeCriticalSection), F(292, RtlIntegerToChar), F(293, RtlIntegerToUnicodeString),
    F(294, RtlLeaveCriticalSection), F(295, RtlLeaveCriticalSectionAndRegion), F(296, RtlLowerChar),
    F(297, RtlMapGenericMask), F(298, RtlMoveMemory), F(299, RtlMultiByteToUnicodeN),
    F(300, RtlMultiByteToUnicodeSize), F(301, RtlNtStatusToDosError), F(302, RtlRaiseException),
    F(303, RtlRaiseStatus), F(304, RtlTimeFieldsToTime), F(305, RtlTimeToTimeFields),
    F(306, RtlTryEnterCriticalSection), F(307, RtlUlongByteSwap), F(308, RtlUnicodeStringToAnsiString),
    F(309, RtlUnicodeStringToInteger), F(310, RtlUnicodeToMultiByteN), F(311, RtlUnicodeToMultiByteSize),
    F(312, RtlUnwind), F(313, RtlUpcaseUnicodeChar), F(314, RtlUpcaseUnicodeString),
    F(315, RtlUpcaseUnicodeToMultiByteN), F(316, RtlUpperChar), F(317, RtlUpperString),
    F(318, RtlUshortByteSwap), F(319, RtlWalkFrameChain), F(320, RtlZeroMemory), D(321, XboxEEPROMKey),
    D(322, XboxHardwareInfo), D(323, XboxHDKey), D(324, XboxKrnlVersion), D(325, XboxSignatureKey),
    D(326, XeImageFileName), F(327, XeLoadSection), F(328, XeUnloadSection), F(329, READ_PORT_BUFFER_UCHAR),
    F(330, READ_PORT_BUFFER_USHORT), F(331, READ_PORT_BUFFER_ULONG), F(332, WRITE_PORT_BUFFER_UCHAR),
    F(333, WRITE_PORT_BUFFER_USHORT), F(334, WRITE_PORT_BUFFER_ULONG), F(335, XcSHAInit), F(336, XcSHAUpdate),
    F(337, XcSHAFinal), F(338, XcRC4Key), F(339, XcRC4Crypt), F(340, XcHMAC), F(341, XcPKEncPublic),
    F(342, XcPKDecPrivate), F(343, XcPKGetKeyLen), F(344, XcVerifyPKCS1Signature), F(345, XcModExp),
    F(346, XcDESKeyParity), F(347, XcKeyTable), F(348, XcBlockCrypt), F(349, XcBlockCryptCBC),
    F(350, XcCryptService), F(351, XcUpdateCrypto), F(352, RtlRip), D(353, XboxLANKey),
    D(354, XboxAlternateSignatureKeys), D(355, XePublicKeyData), D(356, HalBootSMCVideoMode),
    D(357, IdexChannelObject), F(358, HalIsResetOrShutdownPending), F(359, IoMarkIrpMustComplete),
    F(360, HalInitiateShutdown), F(361, RtlSnprintf), F(362, RtlSprintf), F(363, RtlVsnprintf),
    F(364, RtlVsprintf), F(365, HalEnableSecureTrayEject), F(366, HalWriteSMCScratchRegister),
};

#undef F
#undef D

static_assert(sizeof(kExports) / sizeof(kExports[0]) == kKernelOrdinalLimit - 1, "kExports must cover every ordinal");

}  // namespace

const KernelExport* find_kernel_export(uint32_t ordinal) {
    if (ordinal == 0 || ordinal >= kKernelOrdinalLimit) return nullptr;
    const KernelExport& entry = kExports[ordinal - 1];
    return entry.ordinal == ordinal ? &entry : nullptr;
}

}  // namespace mc3
