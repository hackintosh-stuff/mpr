# PLAN — 将 LSI/Broadcom Fusion-MPT SAS3 驱动移植到 macOS

状态：草案 v0.2（2026-11-10；依据 BootKernelExtensions.kc 反编译结果修订）
目标硬件：Broadcom Ltd. / Avago Tech (LSI) SAS3008，PCI `0x1000:0x0097`，MPI2.5 / 12 Gb/s SAS3 HBA
参考驱动：FreeBSD `mpr`、Linux `mpt3sas`
目标平台：macOS Tahoe 26（x86_64，MSI X570 测试机），基于 `MacKernelSDK` 的 IOKit kext。工作区记录的内核版本为 Darwin 25.6.0，而本次反编译的 BootKC 属于 26.7.x（KC 内 kext `DTPlatformVersion=26.7.1` / `DTSDKBuild=25G325`，分析文档标注 26.7.2 / 25G332）；以现场 `uname -a` / `kextstat` 为准，系统升级后需重取 KC。

本文件只描述计划，不含实现代码。参考源码引用以拉取到 `reference/` 的固定 commit 为准；内核侧事实来自 `C:\Users\ling\OneDrive\Documents\Agent\Hackintosh\ida\BootKernelExtensions.kc` 的 IDA 反编译/符号表（见 §4.0）。下文 `xxx.c:NNN` 形式的行号，FreeBSD 侧相对 `reference/freebsd/sys/dev/mpr/`，Linux 侧相对 `reference/linux/drivers/scsi/mpt3sas/`，SDK 侧相对 `MacKernelSDK/`。

---

## 1. 目标与范围

### 1.1 首版目标（M0–M5）

1. kext 通过 `IOPCIDevice` 匹配 SAS3008，完成 PCI 使能、BAR 映射、IOC 复位与初始化。
2. 建立 request / reply / reply-free 队列与 chain 池，能用中断或轮询收发 MPI 消息。
3. 通过固件事件完成拓扑发现，把直连及经 expander 的 SAS/SATA 磁盘暴露为**原生** `IOMedia`（`diskutil list` 可见）。
4. 支持 READ/WRITE、SYNCHRONIZE CACHE、UNMAP、auto sense、超时与 TMF 恢复、热插拔。
5. 有基本诊断通路（IORegistry 属性 + 用户态工具），可抓 IOC 状态、config page、事件日志。

### 1.2 明确不做（首版）

- IR/RAID 卷、SES 机箱管理、NVMe/PCIe enclosure、Tri-Mode、固件升级、启动盘引导。
- Apple Silicon、dext/DriverKit、多队列高 IOPS 优化、中断合并、WarpDrive。
- 兼容 SAS2（`mps`/`0x0072`）与 MPI26/GEN35/SEA 分支（SAS3416 等）。
- 接入 `IOSCSIParallelInterfaceController` / macOS SCSI 应用层栈（见 §4.2 路线 B）。

### 1.3 驱动形态

**IOKit 内核扩展（kext），不是 dext。** 依据见 §4.2。IT/IR 固件：首版按 IT 模式（纯 initiator）开发与验收，IR 模式仅保留探测与拒绝路径。

---

## 2. 基线（可复现）

| 项目 | 内容 |
|---|---|
| FreeBSD 参考 | `freebsd/freebsd-src` `main`，commit `e56090afb68b6c036d6b54c7b66d762d98bd96d0`（2026-11-10）；`sys/dev/mpr/` 13 个文件 + `mpi/` 13 个文件 |
| Linux 参考 | `torvalds/linux` `master`，commit `b3dfbde62cf1981c4638939c159dfd087b7a72e9`（2026-11-10）；`drivers/scsi/mpt3sas/` 16 个文件 + `mpi/` 10 个文件 |
| 合计 | 52 个文件，2,664,265 字节，逐文件按 GitHub Contents API 的 `size` 校验；固定 commit 的 `raw.githubusercontent.com` 下载 |
| MacKernelSDK | git 子模块 `3f750085caa17ec3a7880f11c11bf4f48cd6a164`（2026-09-23），基于 macOS 11 / Xcode 12.0 SDK，补充 `xnu-8019.80.24` 头文件 |
| 内核缓存反编译 | `ida/BootKernelExtensions.kc` 67,600,384 B（MD5 `b9da5eafcd90e439ae7c55c1ca8ddf33`）、`BootKernelExtensions.kc.i64`、survey 缓存 `.ida-mcp-survey.json`、`ionvme-symbol-analysis.md`；KC 基址 `0xffffff8000200000`，209 个 bundle / 2396 段 / 117,470 函数（111,780 已命名）/ 136,888 字符串 |
| 许可 | FreeBSD `mpr`：BSD-2-Clause（Yahoo!/LSI/Avago/Broadcom 版权，`mpr.c:1-12`）；Linux `mpt3sas`：GPL-2.0（`mpt3sas_base.c:10-12`） |
| 版本控制 | `MacKernelSDK` 是子模块；`reference/` 被 `.gitignore` 的 `/reference/` 忽略，**不入库**；`ida/` 位于工作区根目录，同样不入库、不参与构建。本节的 commit 与 MD5 是唯一复现依据 |

目录布局：

```
mpr/
  PLAN.md
  MacKernelSDK/                       (submodule, 入库)
  reference/                          (gitignored, 只读参考)
    freebsd/sys/dev/mpr/  + mpi/
    linux/drivers/scsi/mpt3sas/  + mpi/
```

复现拉取：对两个 commit 分别用 `https://api.github.com/repos/<repo>/contents/<path>?ref=<sha>` 递归列目录，再从 `https://raw.githubusercontent.com/<repo>/<sha>/<path>` 取文件；本机访问 GitHub 需走 `http://127.0.0.1:10808` 代理。不要用移动分支的 `HEAD` 作为基线。

---

## 3. 参考源码解剖：哪些能用、哪些必须重写

### 3.1 可直接（近似照搬）复用的 MPI 协议层

这部分是硬件契约，与 OS 无关，是移植的主要价值来源。以 **FreeBSD 版 `mpi/*.h` 为代码来源**（BSD 许可），Linux 版只用于交叉校验。

| 内容 | 位置 |
|---|---|
| 系统接口寄存器 | `mpi/mpi2.h`：Doorbell `0x00`、WriteSequence `0x04`、HostDiagnostic `0x08`、HostInterruptStatus/Mask `0x30/0x34`、ReplyFreeHostIndex `0x48`、ReplyPostHostIndex `0x6C`、RequestDescriptorPost `0xC0/0xC4`、Atomic `0xC8` |
| 描述符 | `mpi2.h:401-529` 8 字节 Request Descriptor（Default/HighPriority/ SCSI IO / Atomic）；`mpi2.h:536-655` Reply Descriptor（含 AddressReply、SMID=0 事件） |
| SGE / chain | `mpi2.h:894-1002` SIMPLE32/64、CHAIN、TRANSACTION；`mpi2.h:1068-1123` flags；`mpi2.h:1135/1199/1219` IEEE SGE（SAS3 必需） |
| 消息结构 | `mpi2_ioc.h`（IOC Init / IOC Facts / Port Facts）、`mpi2_init.h`（SCSI IO、TMF 类型 `440-451`）、`mpi2_cnfg.h`（config page、SAS Device Page0、IOC Page8、`SAS3008=0x0097`）、`mpi2_sas.h`、`mpi2_tool.h`、事件结构 |
| 状态机 | `mpr.c:275-353` `mpr_transition_ready` / `mpr_transition_operational`；`mpr.c:151-248` `mpr_diag_reset` 的 WriteSequence 魔数 + `MPI2_DIAG_RESET_ADAPTER` 时序；Linux `mpt3sas_base.c:8025` 同序列 |
| 队列几何 | `mpr.c:381-469` `mpr_resize_queues`（`reqframesz = IOCRequestFrameSize*4`、`chain_frame_size = IOCMaxChainSegmentSize*16`、由 `MaxChainDepth` 推 `maxio`）；Linux `mpt3sas_base.c:6293-6559` 等价推导 |
| IOC Init 参数 | `mpr.c:1231-1289` `mpr_send_iocinit`（frame size、两队列深度、低 32 位地址、`HostPageSize=4K`）；Linux `mpt3sas_base.c:7537,8203-8355` |
| SGL 构造 | `mpr.c:3388-3751` `mpr_add_chain` / `mpr_push_ieee_sge` / `mpr_add_dmaseg`；Linux `mpt3sas_base.c:2655,2716,2857` |
| 事件解码 | `mpr_sas_lsi.c:200-800` 固件事件 switch；Linux `mpt3sas_scsih.c:11943` |
| config page 请求 | `mpr.c:3926` `mpr_read_config_page` + `mpr_config.c`；Linux `mpt3sas_config.c:296` |
| 映射/缺盘算法 | `mpr_mapping.c`（DPM Page0、persistent mapping、missing count、IR/拓扑事件） |

### 3.2 初始化主链（以 FreeBSD 为主，Linux 校验）

| 阶段 | FreeBSD | Linux | 产出 |
|---|---|---|---|
| PCI 使能 | `mpr_pci.c:215-277` | `mpt3sas_base.c:3634-3685`, `_scsih_probe` `scsih.c:13356` | BAR0 MMIO、bus master、DMA mask |
| IOC 就绪 | `mpr.c:275-353`, `mpr_diag_reset` | `mpt3sas_base.c:8120,8025` | Doorbell 进入 READY |
| IOC Facts | `mpr.c:480-747,1193` | `mpt3sas_base.c:7434` | MPI 版本、`RequestCredit`、frame/queue 尺寸、`MaxMSIxVectors` |
| 内存池 | `mpr.c:1361-1600` | `mpt3sas_base.c:6293-6559` | request/reply/chain/sense/free queue/RDPQ |
| IOC Init | `mpr.c:1231,1687` | `mpt3sas_base.c:7537,8203` | OPERATIONAL、写 Host Index |
| 静态配置页 + 事件注册 | `mpr.c:2231-2252`，`mpr_config.c:462` | `mpt3sas_base.c:8351,7865` | IOC Page8 / IOUnit Page8 / Manufacturing Page11、事件掩码 |
| Port Enable + Discovery | `mpr_sas.c:3473-3545` | `mpt3sas_base.c:7754`, `scsih.c:13040` | 固件上报拓扑事件，生成设备表 |
| 中断注册 | `mpr_pci.c:286-382`（实际强制 1 个 MSI-X 向量，`mpr_pci.c:300`） | `mpt3sas_base.c:3080-3548`（多向量 + 亲和） | 首版取单向量即可 |

完成路径：`mpr.c:2482-2688` `mpr_intr_locked`（`atomic_swap_64` 取描述符、AddressReply 换算、SMID 定位、回写 `ReplyPostHostIndex`）；Linux `mpt3sas_base.c:1698-1761` `_base_process_reply_queue`。

命令路径：`mpr_sas.c:1844-2164` 构造 `Mpi2SCSIIORequest_t`（`SGLOffset0=24`、DevHandle、sense 低地址）→ `mpr.c:3765-3786` DMA 映射 → `mpr.c:1126` 写 `0xC0/0xC4`；Linux `scsih.c:5435-5590` 等价。TMF：`mpr_sas.c:1403-1613` / `scsih.c:3120-3266`。

### 3.3 两份参考的取舍

- **FreeBSD `mpr` 作为结构主参考**：CAM SIM/XPT 的「一个命令对象 + 完成回调 + 超时 + SIMQ 冻结」与 IOKit 的异步命令模型概念更接近，OS 胶水更薄；`mpr_mapping.c` 的持久化映射表可直接借鉴设计。
- **Linux `mpt3sas` 作为 SAS3 细节字典**：IEEE SGE、63 位 DMA mask（`mpt3sas_base.c:3041-3047`）、RDPQ array / combined reply queue、MSI-X 亲和与中断合并（`mpt3sas_base.c:5002`）、`_base_diag_reset` 时序、TM 超时状态机更完整。其 `mpi/` 头文件用于核对 FreeBSD 版差异。
- **不照搬的成片代码**：CAM/SIM、blk-mq tag↔SMID 绑定、libsas/transport class、`dma_pool`/`bus_dma`、workqueue/callout、sysfs/debugfs/misc ioctl。
- **注意命名差异**（避免搜索落空）：FreeBSD 无 `mpr_init`/`mpr_push_cmd`/`mpr_hard_reset`，实际是 `mpr_init_queues`+`mpr_send_iocinit`、`mpr_enqueue_request`、`mpr_diag_reset`；Linux 的 `_base_done`/`_scsih_io_done` 等静态函数名与常见文档不同，见 §2 固定 commit。

### 3.4 必须重写的 OS 依赖（摘要，完整映射见 §4.4）

CAM SIM/XPT/CCB、bus_dma/bus_space、newbus/device_t、`taskqueue`/`callout`/`msleep`、sysctl/tunables、cdev/ioctl、CAM 错误码与 auto sense、以及 FreeBSD 本身**未实现**的 suspend/resume（`mpr_pci.c:441,447` 直接返回 `EINVAL`）——电源与停机 quiesce 必须自研。

---

## 4. 平台映射与架构决策

### 4.0 内核缓存反编译给出的实证（v0.2 新增）

分析对象 `BootKernelExtensions.kc`（macOS 26.7.x Boot KC），IDA 会话 `5c67bd08`。`ionvme-symbol-analysis.md` 已确认 **KC 内地址即链接地址/运行时地址、无 slide**（IONVMeFamily `__text` 起 `0xffffff80026bd000` 与运行时 `kextstat` 一致）；`.ida-mcp-survey.json` 给出 209 个 bundle / 2396 段 / 117,470 函数（111,780 命名）/ 136,888 字符串。本次针对存储栈复核得到：

1. **系统侧 family 确实存在，且都在 BootKC 里**（survey 段名，非推测）：`com.apple.iokit.IOStorageFamily`、`com.apple.iokit.IOPCIFamily`、`com.apple.iokit.IOSCSIArchitectureModelFamily`、`com.apple.iokit.IOSCSIParallelFamily`、`com.apple.iokit.IOSCSIBlockCommandsDevice` / `IOSCSIMultimediaCommandsDevice` / `IOSCSIReducedBlockCommandsDevice`、`com.apple.iokit.SCSITaskUserClient`、`com.apple.iokit.IOAHCIBlockStorage`、`com.apple.iokit.IONVMeFamily`、`com.apple.driver.AppleVirtIO`。KC 里还有对应的 KPI bundle 标识字符串（`0xffffff800020dcc8` 起：`...IOPCIFamily`、`...IOSCSIArchitectureModelFamily`、`...IOSCSIParallelFamily`、`...IOStorageFamily`）。**因此"目标系统没有 SCSI/存储栈"是错的**；v0.1 据此否定路线 B 的理由已修正为"工具链限制"，见 §4.2。
2. **路线 A 的 ABI 基础可实证**：`__ZTV20IOBlockStorageDevice` 在 `com.apple.iokit.IOStorageFamily:__const` @ `0xffffff8002b70450`；`IOBlockStorageDevice` 的方法实体在 `IOStorageFamily:__text`（`init`、`setProperties`、`getWriteCacheState`、`setWriteCacheState`、`doLockUnlockMedia`、`reportLockability`、`reportPollRequirements`、`requestIdle`、`doDiscard`、`doUnmap`、`doSetPriority`、`doSynchronizeCache`、`doSynchronize`、`doGetProvisionStatus`，以及 `_RESERVEDIOBlockStorageDevice4..22+` 保留槽）。本次已读出该 vtable 前 256 字节的原始槽位，可与 SDK 头的虚函数顺序逐项比对。
3. **Apple 自己有多套 `IOBlockStorageDevice` 子类可作模板**：
   - `AppleVirtIOBlockStorageDevice`（`com.apple.driver.AppleVirtIO:__text`，`__ZTV29AppleVirtIOBlockStorageDevice` @ `0xffffff8001f0d1f0`）：实现全部 14 个纯虚（`doAsyncReadWrite`/`doEjectMedia`/`doFormatMedia`/`doGetFormatCapacities`/各 `get*String`/各 `report*`），以及 `doUnmap`、`doSynchronizeCache`、`doSetPriority`、`doLockUnlockMedia`、`reportPollRequirements`，并有自己的 `start`/`stop`/`willTerminate`/`queueEventHandler`。**这是与本项目最像的「自研 transport + 块设备 nub」模板，优先级高于 NVMe/AHCI**。
   - `IONVMeBlockStorageDevice`（`IONVMeFamily:__text`）：`start`、`GetDeviceProperties`、`doAsyncReadWrite`、`CompleteAsyncRequest`、`QueueCreate/Delete`、`QueueSubmitRequest`/`QueueCompleteRequest`、`PowerDownHandler`、`SetNVMeProperties`、`ValidateEntitlement`、`doUnmap`/`doUnmapNVMe`、`getSerialNumberString` 等。
   - `IOAHCIBlockStorageDevice`（`IOAHCIBlockStorage:__text`）、`IOHDIXHDDrive*`、`AppleXsanDevice` 等同形态。
4. **反编译实证（`IONVMeBlockStorageDevice::doUnmap` @ `0xffffff80026d0396`）**：先 `IOService::isInactive` 检查 → `kalloc_shared_data_external(4096,1)` → `IOMemoryDescriptor::withAddress(buf,0x1000,2)` → `md->prepare()` → 由 `this+0x90` 的 controller 走 `vtable[+2216]` 取 request（GetRequestFromPool）→ 挂 MD → 构造并提交 → 完成后归还池；临界区用 `AppleNVMeWorkLoop::Lock/SleepGate/Unlock/WakeupGate` 串行化。与 `ionvme-symbol-analysis.md` §3 归纳的同步命令流程（GetRequestFromPool → ResetForNewRequest → prepare → GenerateIOVMSegments → BuildCommand → ProcessSyncNVMeRequest → complete → ReturnRequestToPool）一致。**这是 macOS 侧「命令对象 + IOMemoryDescriptor + 同步/异步完成」的可核对模板**，也是 §4.3 `MPRCommand` 设计的现实参照。
5. **`IOSCSIParallelInterfaceController` 的实体实现确实在 `com.apple.iokit.IOSCSIParallelFamily:__text`**：`GetSCSIParallelTask`、`ExecuteParallelTask`、`CompleteAbortTask*`、`ServiceInterrupt`、`FilterInterrupt`、`EnableInterrupt`、`InitializeDeviceList`、`DestroyTargetForID` 等符号齐全。系统有实现，缺的是 **SDK 侧的伞头 / KPI 依赖声明**（§4.1）。
6. **`OSBundleLibraries` 的真实样例**：KC 的 prelink Info.plist 中 Apple kext 声明 `com.apple.iokit.IOPCIFamily = 1.1`、`com.apple.kpi.iokit/libkern/mach = 8.0/9.0` 等；这些是 Apple 内部 kext 的历史版本，只作为「依赖键长什么样」的参考，第三方 kext 的版本仍以现场 `kextstat`/KPI 为准。
7. **资料边界**：`ionvme-symbol-analysis.md` §4/§5 的 Lilu `solveSymbol` / Samsung 990 EVO 结论与本项目无关且已在别处被推翻，本计划只取其 §1–§3 的寄存器/命令/反编译事实；KC 里多数符号是 names 而非 IDA 已识别函数（`lookup_funcs` 对多数 vtable 槽返回 "Not a function"，部分 `decompile` 也因此失败），故 ABI 核对以**符号表 + vtable 原始字节**为准，不依赖 IDA 的函数识别。

### 4.1 MacKernelSDK 提供什么

存在（`MacKernelSDK/Headers/`）：`IOKit/pci/IOPCIDevice.h`（config 读写、`findPCICapability`、BAR 映射）、`IOKit/IOBufferMemoryDescriptor.h`、`IOKit/IODMACommand.h`（`gen64IOVMSegments`、`kIODMACommandOutputLittle64`）、`IOKit/IOMemoryDescriptor.h`（`IOMemoryMap`）、`IOWorkLoop`/`IOCommandGate`/`IOInterruptEventSource`/`IOFilterInterruptEventSource`/`IOTimerEventSource`/`IOCommandPool`/`IOLocks`、libkern 集合与字节序、`IOKit/storage/IOBlockStorageDevice.h` 及 `IOStorage`/`IOMedia`/`IOBlockStorageDriver`、`IOKit/storage/IOStorageProtocolCharacteristics.h`（`Physical Interconnect = "SAS"`）、以及 `IOKit/scsi/` 下的 `SCSITask.h`、`IOSCSIProtocolInterface.h`、`IOSCSIPeripheralDeviceNub.h`、各 `SCSICmds_*.h`（CDB 定义可用）。

不存在：SAS/SSP/SMP/expander 框架头、`IOSCSIParallelFamily`/`IOSCSIArchitectureModelFamily` 的伞头与 KPI 依赖声明、`IOSCSIBlockStorageDevice.h`、`SCSIControllerDriverKit`/`IOUserSCSIParallelInterfaceController`（dext 路线）、PCIDriverKit 的 `.iig` 依赖链、HBA 示例 kext、xcconfig/CI、arm64 `libkmod`、kernel collection/签名文档。SDK 中 `com.apple.iokit.IOSCSIArchitectureModelFamily` 只作为字符串常量出现（`IOKit/scsi/IOSCSIProtocolInterface.h:166`）。**注意：这里说的是「SDK 缺」，不是「系统缺」**——系统侧这些 family 的实体见 §4.0。

### 4.2 三条路线对比与决策

| 路线 | 做法 | 判定 |
|---|---|---|
| **A（采纳）** | 自研 `IOService`（provider=`IOPCIDevice`）实现 HBA；每块盘实例化一个 `IOBlockStorageDevice` 子类 nub，`registerService` 后由系统 `IOBlockStorageDriver` 创建 `IOMedia`/`IOMediaBSDClient` | **可行，且已有实证**：BootKC 中 `IOStorageFamily` 的 `__ZTV20IOBlockStorageDevice` 与 Apple 自研子类（`AppleVirtIOBlockStorageDevice`）都在（§4.0）；只依赖 `IOBlockStorageDevice` 抽象接口与 14 个纯虚方法，耦合面最小 |
| B | 继承 `IOKit/scsi/spi/IOSCSIParallelInterfaceController`，复用 macOS SCSI 应用层栈 | **当前工具链下不可行，系统侧可行**：BootKC 中 `IOSCSIArchitectureModelFamily`/`IOSCSIParallelFamily` 及控制器实体方法都在（§4.0 第 1、5 条），但 MacKernelSDK 没有对应伞头与 `OSBundleLibraries` 依赖声明，无法编译出能正确链接的子类；且该类强制 SPI 语义（Wide/Sync/QAS/DT 协商、16 槽并行总线），与 SAS3 点对点拓扑不匹配，私有 family 还有 ABI 风险。若日后从当前 Xcode SDK / KC 反编译补齐头与依赖，可作为 M7+ 的备选升级路线 |
| C | `IOService` + `IOUserClient` 诊断透传，用户态发 CDB | **只能做调试**，无法生成 `IOMedia`/`/dev/disk`，不能作为主路线；可作为 M6 的诊断通道 |

决策：**A**。SDK 的 SCSI 头文件只用来取 CDB 操作码、sense、任务状态等**定义**，不作为继承体系。

> M0 的 ABI 前置验证（v0.2 修正）：`com.apple.iokit.IOStorageFamily` / `IOPCIFamily` 的存在性已由 BootKC 段名、KPI 标识字符串与 `__ZTV20IOBlockStorageDevice` 实证，**不再是「是否存在」的风险**；剩余要验证的是 ① MacKernelSDK（macOS 11 / Xcode 12.0）头文件与 26.7 内核的 vtable 槽位/保留槽是否一一对应（不一致会加载即 panic），② `OSBundleLibraries` 的版本匹配。做法：读 `__ZTV20IOBlockStorageDevice` (@`0xffffff8002b70450`) 与 `__ZTV29AppleVirtIOBlockStorageDevice` (@`0xffffff8001f0d1f0`) 的原始槽位，与 SDK 头虚函数顺序逐项比对，再用最小 kext 在真机验证。

### 4.3 分层设计

```
MPRController (IOService)          probe/start/stop，持有 IOPCIDevice、IOMemoryMap、IOWorkLoop
 ├─ MPI/         寄存器常量与消息结构（来自 FreeBSD mpi/*.h，BSD 许可，裁剪 MPI26/NVMe 分支）
 ├─ MPRQueue     request/reply/free queue、SMID 与 chain/sense 池、doorbell 提交
 ├─ MPRDMA       IOBufferMemoryDescriptor + IODMACommand 封装，生成 IEEE SGE
 ├─ MPRIntr      IOFilterInterruptEventSource → workloop 内处理 reply descriptor
 ├─ MPRConfig    IOC Facts / Port Facts / config page / EventNotification / PortEnable
 ├─ MPRDiscovery 固件事件队列 → 目标表（devhandle、SAS address、LUN）→ nub 生命周期
 ├─ MPRBlockDev  IOBlockStorageDevice 子类：CDB 构造、IOStorageCompletion、UNMAP（模板见 §4.0 的 AppleVirtIOBlockStorageDevice）
 ├─ MPRTaskMgmt  超时、ABORT_TASK / LUN_RESET / TARGET_RESET、diag reset 恢复
 └─ MPRUserClient(后期)  寄存器 dump、config page、事件日志、MPI passthrough
```

一个 `MPRCommand` 绑定：`IOStorageCompletion` + SMID + request frame + SGL/chain + sense buffer + 超时定时器 + 目标 devhandle，对应 FreeBSD `struct mpr_command`（`mprvar.h:242-291`）与 Linux `struct scsiio_tracker`（`mpt3sas_base.h:884-892`）的职责。macOS 侧命令生命周期按 §4.0 第 4 条的 Apple 反编译实证设计：`IOMemoryDescriptor::prepare` → 自建 request 池取帧 → SGL → 提交 → 完成回调 → 归还池；临界区用 `IOWorkLoop`/gate 串行化，而不是照搬 NVMe 的 `AppleNVMeWorkLoop`。

### 4.4 OS 依赖映射表

| 概念 | FreeBSD `mpr` | Linux `mpt3sas` | macOS / MacKernelSDK |
|---|---|---|---|
| PCI 匹配与使能 | `mpr_pci.c:203-277` | `scsih.c:13356`, `base.c:3634-3685` | `IOService::probe/start`；`IOPCIPrimaryMatch` 按 `IOPCIDevice.h:324-341` 的 `<device><vendor>` 格式填 `0x00971000`（必要时带 mask），`setMemoryEnable`/`setBusMasterEnable` |
| MMIO | `bus_alloc_resource` + `bus_space` | `pci_ioremap` | `IOPCIDevice::mapDeviceMemoryWithRegister` → `IOMemoryMap::getVirtualAddress`，volatile 寄存器访问 |
| 配置空间 | `pci_read_config` | `pci_*_config_*` | `configRead32`、`findPCICapability(0x11)`（MSI-X） |
| 一致 DMA | `bus_dmamem_alloc` + tag | `dma_alloc_coherent`/`dma_pool` | `IOBufferMemoryDescriptor::withOptions`（`kIOMemoryPhysicallyContiguous` 不推荐，改用对齐 + 校验） |
| 数据 DMA 映射 | `bus_dmamap_load_ccb` | `scsi_dma_map` + sg | `IODMACommand::withSpecification` + `gen64IOVMSegments` + `prepare`/`complete`/`synchronize` |
| 4GB 边界 | `BD_BOUNDARY(4GB)`、`LOWADDR_32BIT`（`mpr.c:1386-1388,1514-1516`） | `mpt3sas_check_same_4gb_region` | 自建池并校验物理地址同属一个 4GB 区间，失败回退 32 位 SGE |
| 中断 | `bus_setup_intr(INTR_MPSAFE)` | `request_irq`/`pci_alloc_irq_vectors_affinity` | `IOFilterInterruptEventSource(..., intIndex)` + `IOWorkLoop`；filter 里只取描述符、不做阻塞操作 |
| 锁 / 原子 | `mtx`/`atomic_swap_64` | `spinlock`/`mutex`/`atomic` | `IORecursiveLock`/`IOSimpleLock`/`OSAtomic`；`IOCommandGate` 串行化（中断上下文禁止 `runAction`） |
| 异步 / 定时 | `taskqueue`/`callout`/`msleep` | `workqueue`/`timer`/`completion` | `IOWorkLoop`、`IOTimerEventSource`、`thread_call`、`IOLockSleep/Wakeup` |
| 命令与完成 | CAM CCB → `xpt_done` | `scsi_cmnd` → `scsi_done` | `MPRCommand` → `IOStorageCompletion`（Apple 侧模板见 §4.0 第 4 条） |
| 设备枚举 | `xpt_bus_register`/`xpt_rescan` | `scsi_add_host`/`sas_rphy_add` | `registerService()` + `kIOMessageMediaStateHasChanged`；设置 `device-type` 与 `Protocol Characteristics`(SAS) |
| 错误码 | `CAM_*` / `scsi_sense_data` | `SCSI` status / `resid` | `IOReturn` + sense 缓冲 + 任务状态枚举（`SCSITask.h` 仅借用定义） |
| 用户接口 | cdev ioctl (`mpr_ioctl.h`) | misc/BSG/sysfs (`mpt3sas_ctl.h`) | `IOUserClient` + IORegistry 属性 |
| 电源 / 停机 | suspend/resume 未实现 | PM ops | `IOService` PM、`setPowerState`、`systemWillShutdown`、IO 静默，需自研 |
| 日志 / 调参 | sysctl / `mpr_table.c` | module_param / debugfs | `IOLog`（默认）、IORegistry 属性、boot-args |

---

## 5. 关键实现约束

1. **DMA 几何**：控制结构（free queue 4B 项、post queue 8B 项、reply/request frame、chain）必须在低 4GB、16B/4B 对齐、chain 不跨 4GB 边界；IOC Init 的 High 地址字段写 0（`mpr.c:1267-1276`）。SAS3 数据 SGE 用 IEEE 64 位，但 Linux 对 SAS3 设 `DMA_BIT_MASK(63)`（`mpt3sas_base.c:3041-3047`），macOS 侧按 `IODMACommand` 的 `numAddressBits=63` 起。
2. **队列深度**：free/post queue 深度 `roundup2(num_replies+1, 16)` 并留一个空项（`mpr.c:1368-1405`）；初始化完成后写 `ReplyFreeHostIndex`/`ReplyPostHostIndex`，每次中断处理结束回写（`mpr.c:2683`）。
3. **SGE 与 chain**：SAS3 只走 IEEE chain，`mpr_add_chain`（`mpr.c:3388-3460`）明确拒绝 SIMPLE SGL；首帧 `SGLOffset0=24`，sense buffer 用每命令预分配的低 32 位地址。
4. **中断上下文**：filter 中只做「读描述符 / 置 `0xff` / 入队」，命令完成、TMF、nub 增删全部放到 workloop；不得在 filter 里调用 `IOCommandGate::runAction` 或任何可能睡眠的锁。
5. **SMID 与 completion 绑定**：`MPRCommand` 池与 SMID 一一对应，完成回调必须能由 SMID 反查 `IOStorageCompletion`；超时后 SMID 不能回收，直到 TMF 完成，避免固件写已复用帧。
6. **块设备契约与 ABI**：实现 `IOBlockStorageDevice.h` 的 14 个纯虚（`doAsyncReadWrite`、`reportBlockSize/MaxValidBlock/MediaState`、vendor/product/revision 等）；`doUnmap`/`doSynchronize` 有默认实现但需按 CDB 覆写；通过 `IOBlockStorageDriver` 生成 `IOMedia`（`IOBlockStorageDriver.h:1242-1397`）。**26.7 内核的实体里还有 `doDiscard`、`doSynchronizeCache`、`doSynchronize`、`doSetPriority`、`doGetProvisionStatus` 与 `_RESERVEDIOBlockStorageDevice4..22+`（§4.0 第 2 条），SDK 头把它们标为 deprecated/保留，不能按 SDK 的 `= 0` 数量推断 vtable；M0 必须先做 vtable 核对（§4.2 引用块），`AppleVirtIOBlockStorageDevice` 是最直接的实现模板。**
7. **最小 CDB 集**：INQUIRY、READ CAPACITY(16)、READ(10/16)、WRITE(10/16)、SYNCHRONIZE CACHE(10/16)、UNMAP、REQUEST SENSE、REPORT LUNS、START STOP UNIT、MODE SENSE；定义取 `MacKernelSDK/Headers/IOKit/scsi/SCSICmds_*.h`。
8. **固件事件**：首版只处理 `SAS_TOPOLOGY_CHANGE_LIST`、`SAS_DISCOVERY`、`SAS_DEVICE_DISCOVERY_ERROR`、`SAS_DEVICE_STATUS_CHANGE`，其余忽略并记录；拓扑稳定后再 `registerService`，避免盘符抖动。
9. **电源与复位**：`mpr_reinit`（`mpr.c:837-924`）的「mask irq → diag reset → PCI 配置恢复 → 重新 IOC Facts → 重新注册事件 → 释放冻结命令」需 1:1 重建；watchdog 用 1 s doorbell 检查（`mpr.c:2256`）。
10. **文件行尾与部署**：所有上传到 macOS 的 `.plist`/`.sh`/`.py`/源码必须是 LF（`.gitattributes` 已声明 `* text=auto eol=lf`；CRLF 的 `Info.plist` 会让 kext 被静默丢弃）。kext 走 `kmutil`/AuxiliaryKernelExtensions.kc 与现有 OpenCore 流程，可复用同工作区 `drivers_src/NvmeHook` 的部署验证经验；本驱动是 PCI 驱动，不依赖 Lilu。目标机串口调试默认关闭（`debug=0x100`、无 `serial=5`），长期开启 `kprintf` 会显著拖慢系统，仅在抓日志时临时开启。
11. **内核缓存反编译的使用边界**：`ida/` 下的 `.kc`/`.i64` 只用于核对类/vtable/符号/依赖键等 ABI 事实，是把 macOS 11 SDK 头「校准」到 26.7 内核的手段；不复制 Apple 代码，也不把 KC 地址、vtable 偏移或内部符号写进驱动（会随系统升级失效）。系统升级后重取 KC 并重做 §4.2 的核对。

---

## 6. 里程碑与验收

| 阶段 | 目标与交付 | 验收方式 | 退出条件 |
|---|---|---|---|
| **M0 环境** | 确认目标机 SAS3008 的 PCI ID/固件模式/BAR；Xcode 工程接入 MacKernelSDK（`KERNEL_EXTENSION_HEADER_SEARCH_PATHS`、`KERNEL_FRAMEWORK_HEADERS`）；最小 kext 能加载并匹配；**用 KC 的 `__ZTV20IOBlockStorageDevice` / `__ZTV29AppleVirtIOBlockStorageDevice` 核对 SDK 头 vtable 槽位与保留槽，并确定 `OSBundleLibraries` 版本**（§4.2） | `ioreg -l -p IODeviceTree`、`kmutil showloaded`、`kextstat`、`log show --predicate 'process == "kernel"'`；最小 kext 的 `start` 成功打印；产出 vtable 逐槽比对表 | 空 kext 稳定加载/卸载，PCI 匹配字典命中；vtable 与 SDK 头一致（或已补齐头），KPI 版本结论明确 |
| **M1 寄存器与 IOC** | MMIO dump、diag reset → READY、IOC Facts、按 Facts 计算队列几何、分配 DMA 池、IOC Init → OPERATIONAL（先轮询，不开中断） | 用户态诊断工具读 Doorbell/`IOCState`，对比参考驱动的 Facts 字段；寄存器 dump 与 `mpi2.h` 常量一致 | 无需中断即可收发一条同步 config 请求（如读 Manufacturing Page0）；IOC 保持在 OPERATIONAL |
| **M2 中断与事件** | 单 MSI/MSI-X 向量 + `IOFilterInterruptEventSource`、reply descriptor 解析、EventNotification、PortEnable、固件事件队列 | 能看到 `SAS_DISCOVERY`/拓扑事件；`mpr_user`-style config page 读取成功；连续 10 分钟无丢失 reply | reply queue 与 Host Index 回写无泄漏；事件队列不丢不重 |
| **M3 单盘只读** | 目标表 → 第一个 `IOBlockStorageDevice` nub（实现参照 §4.0 的 `AppleVirtIOBlockStorageDevice`）；INQUIRY / READ CAPACITY(16) / READ(16)；`IOMedia` 出现 | `diskutil list`、`ioreg -r -c IOMedia`、`dd if=/dev/rdiskN of=/dev/null bs=1m count=1024` 无错；只读挂载可列目录 | 1 GB 顺序读回读数据一致，无 kernel panic、无 DMA 越界 |
| **M4 读写与恢复** | WRITE(10/16)、SYNCHRONIZE CACHE、UNMAP、auto sense、超时与 TMF（ABORT/LUN RESET/TARGET RESET）、diag reset 恢复 | `dd` 写后回读校验；`diskutil` 分区/卸载；人为拔盘或超时注入后驱动能恢复；TRIM/UNMAP 对已知敏感固件先做小范围验证 | 24 h 混合读写无挂起；复位后盘能重新出现 |
| **M5 拓扑与热插拔** | topology change 事件、expander 下多盘、设备移除/新增、重启后 persistent 映射语义（可退化为每次重新发现） | 热插拔 10 轮，`diskutil list` 与 IORegistry 目标表一致；无幽灵盘 | 多盘并发 IO 下拓扑变化不丢命令 |
| **M6 加固/诊断（可选）** | watchdog、IORegistry 统计、`IOUserClient` 诊断工具、性能基线、日志开关 | 工具可导出 config page / 事件日志 / 寄存器；性能与 FreeBSD/Linux 同机对比 | 可定位 90% 现场问题而无需串口 |
| **M7 后续（不在首版）** | IR 卷、SES、固件升级、多队列/MSI-X 亲和、中断合并、启动盘、签名与发布；**或**在补齐 SCSI family 头与 KPI 依赖后评估路线 B | 单独立项 | — |

每个阶段开始前先写该阶段的失败判据与回滚方式（kext 只放测试机、保留可启动的干净 EFI 配置）。**禁止在带有重要数据的磁盘上做写测试。**

---

## 7. 合规与许可

1. FreeBSD `mpr`（BSD-2-Clause）的 MPI 头文件、寄存器时序、算法可以移植进本仓库，但**必须保留原版权声明与许可文本**，并在 `NOTICE`/`LICENSE` 中逐文件列出来源与 commit。
2. Linux `mpt3sas`（GPL-2.0）**只作为行为与协议规范的参考，不复制任何代码到驱动源码**。`reference/` 目录不参与构建、不入库；计划与文档中对它的引用属于事实描述。若未来需要 Linux 独有的实现细节，采用「读懂后独立重写 + 记录设计依据」的方式，并在 PR/提交说明中注明。
3. MPI 结构体定义以 FreeBSD `mpi/*.h` 为唯一代码来源；两份 `mpi2*.h` 的差异（Linux 版含 `mpi2_image.h`、字段更新）在 `MPI/` 目录下用注释记录，不混用。
4. `MacKernelSDK` 头文件仅用于编译，遵循其自身许可；不把 SDK 头文件拷进驱动源码树。
5. `ida/BootKernelExtensions.kc` 与 IDA 数据库属于 Apple 内核二进制的反编译资料，只用于核对 ABI / 接口契约与事实记录；不复制其代码、不把 KC 地址固化进驱动。`ionvme-symbol-analysis.md` 的 §4/§5（Lilu 符号 hook / Samsung 990 EVO）与本项目无关，不予采用。

---

## 8. 风险与未决问题

| # | 风险 | 影响 | 缓解 / 验证 |
|---|---|---|---|
| R1 | **已降级**：family 不存在/不可链接的风险已由 BootKC 实证排除（§4.0）；剩余是 MacKernelSDK（macOS 11）头与 26.7 内核 `IOBlockStorageDevice` vtable/保留槽不一致，或 `OSBundleLibraries` 版本不匹配 | 子类 vtable 错位 → 加载即 panic | M0 读 `__ZTV20IOBlockStorageDevice` (@`0xffffff8002b70450`) 与 Apple 子类 vtable 的原始槽位，与 SDK 头逐项比对；不一致则补 SDK 头 / 调整部署目标，再用最小 kext 验证 |
| R2 | SAS3008 处于 IR 固件模式或固件版本过旧，事件/能力字段不同 | 初始化失败或发现不到盘 | 先读 Manufacturing Page0/IOC Facts 记录固件版本，按 IT 模式验收；IR 模式明确拒绝 |
| R3 | MSI-X 中断源索引与 `IOPCIDevice` 的 `intIndex` 语义不一致 | 收不到中断 | M1 先轮询；M2 逐个 index 试，读 `getInterruptType` 的 `kIOInterruptTypePCIMessaged` 位；必要时回退 INTx |
| R4 | 低 4GB / 4GB boundary / 63 位 DMA 约束在 macOS 上难以保证 | 固件写错地址、静默数据损坏 | 池化分配 + 物理地址校验 + 失败回退 32 位 SGE；M3 用数据校验和验证 |
| R5 | expander/SMP 拓扑处理复杂度 | 只认直连盘 | M5 前先只支持直连；SMP passthrough 作为后续 |
| R6 | 超时/TMF 与 SMID 复用竞争 | 命令错乱、panic | SMID 冻结到 TMF 完成；参考 `mpr.c:2388-2676` 与 `scsih.c:3120-3204` 的状态机 |
| R7 | 电源管理/睡眠唤醒 | 唤醒后盘丢失或 IOC 未复位 | FreeBSD 侧无实现可抄，按 `mpr_reinit` 自研；M5 加入睡眠唤醒测试（唤醒需物理电源按钮，NanoKVM 虚拟键盘不能唤醒本机） |
| R8 | kext 签名 / SIP / AuxKC 重建 / OpenCore 配置 | 驱动加载不了 | 沿用 NvmeHook 的部署与验证流程；每次改 `Info.plist` 复查 LF 行尾与 `plutil -lint` |
| R9 | GPL 代码污染 | 许可风险 | 见 §7，`reference/` 不入构建，代码评审检查 |
| R10 | 启动盘与系统卷依赖 | 盘符/启动顺序变化 | 首版不做启动盘；测试机保留内部 SATA 系统盘 |
| R11 | UNMAP/TRIM 触发某些 SSD 固件缺陷 | 掉盘 | 已有同工作区 Optane TRIM 掉盘经验，默认先禁用 UNMAP，验证设备后再开 |
| R12 | 单中断向量性能上限 | 吞吐低 | 首版接受；M7 再按 Linux `mpt3sas_base.c:3212-3501` 做多队列 |
| R13 | 内核缓存与目标机系统版本漂移（工作区记录 Darwin 25.6.0、KC 为 26.7.x，KC 内 kext 还有 26.7.1/25G325 与 26.7.2/25G332 之别） | 按旧 vtable/符号做的设计在新内核上失效 | 以现场 `uname -a`/`kextstat` 为准；每次系统升级重取 BootKC 并重做 §4.2 的 vtable 核对；不把 KC 地址写进代码 |

需要用户决策的未决项：

1. 首版是否要求**启动盘**支持？是否要求 IR 卷？（当前按「不做」计划）
2. 目标 macOS 版本下限是否只锁 Tahoe 26 / x86_64，还是要做 12–26 的版本适配（影响 SDK 的 `__MAC_OS_X_VERSION_MIN_REQUIRED` 门控与 kmod 兼容）？
3. 是否接受首版单中断向量、无中断合并的性能水平？
4. 测试盘是否允许破坏性写入？是否需要只读验收阶段（M3）作为硬门槛？
5. 诊断用户态工具是放在本仓库，还是借用现有工具链？
6. 是否需要签名/发布级打包，还是仅测试机开发签名？
7. 目标机是否固定到与 BootKC 一致的 26.7.x 构建？（影响 §4.0 的 ABI 核对结果与 M0 结论的有效期）

---

## 9. 下一步（M0 具体动作）

1. 在目标机上确认 SAS3008 的 PCI 配置空间、BAR、当前固件模式与 `IOPCIDevice` 匹配字典（不要沿用历史磁盘/设备号），并核对 `uname -a` / `kextstat` 与 §2 的 KC 版本。
2. 建立 kext Xcode 工程与 `Info.plist`：`IOProviderClass=IOPCIDevice`、`IOPCIPrimaryMatch` 匹配 `0x1000:0x0097`、`OSBundleLibraries` 声明实际需要的 KPI/系统 family；上传前确认 LF。
3. 写空壳 `MPRController::probe/start`，只做 `setMemoryEnable`/`setBusMasterEnable` + BAR 映射 + 寄存器 dump；同时按 §4.2 用 KC vtable 核对 SDK 头，确认 `OSBundleLibraries` 版本。
4. 反编译 `AppleVirtIOBlockStorageDevice::start` / `doAsyncReadWrite`（BootKC `com.apple.driver.AppleVirtIO:__text`）作为 `MPRBlockDev` 的实现模板；从 FreeBSD `mpi/*.h` 裁剪出 SAS3008 所需的 MPI2.5 结构，落到 `MPI/`，保留版权头；建立与 `reference/` 的差异记录。
5. 按 §6 的 M1 开始实现 IOC 初始化（轮询优先），每一步都用诊断工具与参考源码的字段定义交叉核对。
