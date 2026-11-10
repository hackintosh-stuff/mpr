# PLAN — 将 LSI/Broadcom Fusion-MPT SAS3 驱动移植到 macOS

状态：草案 v0.1（2026-11-10）
目标硬件：Broadcom Ltd. / Avago Tech (LSI) SAS3008，PCI `0x1000:0x0097`，MPI2.5 / 12 Gb/s SAS3 HBA
参考驱动：FreeBSD `mpr`、Linux `mpt3sas`
目标平台：macOS Tahoe 26（Darwin 25.6.0，x86_64，MSI X570 测试机），基于 `MacKernelSDK` 的 IOKit kext

本文件只描述计划，不含实现代码。所有对参考源码的引用都是本次实际拉取到 `reference/` 的版本；下文 `xxx.c:NNN` 形式的行号，FreeBSD 侧相对 `reference/freebsd/sys/dev/mpr/`，Linux 侧相对 `reference/linux/drivers/scsi/mpt3sas/`，SDK 侧相对 `MacKernelSDK/`。

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
| 许可 | FreeBSD `mpr`：BSD-2-Clause（Yahoo!/LSI/Avago/Broadcom 版权，`mpr.c:1-12`）；Linux `mpt3sas`：GPL-2.0（`mpt3sas_base.c:10-12`） |
| 版本控制 | `MacKernelSDK` 是子模块；`reference/` 被 `.gitignore` 的 `/reference/` 忽略，**不入库**，故本节的 commit 是唯一复现依据 |

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

### 4.1 MacKernelSDK 提供什么

存在（`MacKernelSDK/Headers/`）：`IOKit/pci/IOPCIDevice.h`（config 读写、`findPCICapability`、BAR 映射）、`IOKit/IOBufferMemoryDescriptor.h`、`IOKit/IODMACommand.h`（`gen64IOVMSegments`、`kIODMACommandOutputLittle64`）、`IOKit/IOMemoryDescriptor.h`（`IOMemoryMap`）、`IOWorkLoop`/`IOCommandGate`/`IOInterruptEventSource`/`IOFilterInterruptEventSource`/`IOTimerEventSource`/`IOCommandPool`/`IOLocks`、libkern 集合与字节序、`IOKit/storage/IOBlockStorageDevice.h` 及 `IOStorage`/`IOMedia`/`IOBlockStorageDriver`、`IOKit/storage/IOStorageProtocolCharacteristics.h`（`Physical Interconnect = "SAS"`）、以及 `IOKit/scsi/` 下的 `SCSITask.h`、`IOSCSIProtocolInterface.h`、`IOSCSIPeripheralDeviceNub.h`、各 `SCSICmds_*.h`（CDB 定义可用）。

不存在：SAS/SSP/SMP/expander 框架头、`IOSCSIParallelFamily` 伞头与实体实现、`IOSCSIBlockStorageDevice.h`、`SCSIControllerDriverKit`/`IOUserSCSIParallelInterfaceController`（dext 路线）、PCIDriverKit 的 `.iig` 依赖链、HBA 示例 kext、xcconfig/CI、arm64 `libkmod`、kernel collection/签名文档。SDK 中 `com.apple.iokit.IOSCSIArchitectureModelFamily` 只作为字符串常量出现（`IOKit/scsi/IOSCSIProtocolInterface.h:166`），没有对应的可链接 KPI 声明。

### 4.2 三条路线对比与决策

| 路线 | 做法 | 判定 |
|---|---|---|
| **A（采纳）** | 自研 `IOService`（provider=`IOPCIDevice`）实现 HBA；每块盘实例化一个 `IOBlockStorageDevice` 子类 nub，`registerService` 后由系统 `IOBlockStorageDriver` 创建 `IOMedia`/`IOMediaBSDClient` | **可行**。只依赖 `IOBlockStorageDevice` 抽象接口与 `doAsyncReadWrite` 等 14 个纯虚方法；耦合面最小，SSP/SCSI 命令由我们自己封装成 MPI 请求 |
| B | 继承 `IOKit/scsi/spi/IOSCSIParallelInterfaceController`，复用 macOS SCSI 应用层栈 | **本 SDK 下不可行**：超类实体（`start`/`stop`、任务池、`CompleteParallelTask`、`CreateTargetForID`）在 `IOSCSIArchitectureModelFamily`/`IOSCSIParallelFamily`，SDK 只有声明、无伞头、无 KPI；且该类强制 SPI 语义（Wide/Sync/QAS/DT 协商、16 槽并行总线），与 SAS3 点对点拓扑不匹配 |
| C | `IOService` + `IOUserClient` 诊断透传，用户态发 CDB | **只能做调试**，无法生成 `IOMedia`/`/dev/disk`，不能作为主路线；可作为 M6 的诊断通道 |

决策：**A**。SDK 的 SCSI 头文件只用来取 CDB 操作码、sense、任务状态等**定义**，不作为继承体系。

> M0 必须验证的前置假设：目标系统上 `com.apple.iokit.IOStorageFamily` 与 `com.apple.iokit.IOPCIFamily` KPI 版本可链接、`IOBlockStorageDevice` 的 OSMetaClass/vtable 符号可用。SDK 自身没有这些 bundle 的 KPI stub（其示例 kext 的 `OSBundleLibraries` 只有 `com.apple.kpi.iokit`/`com.apple.kpi.libkern`），必须在真机上用最小 kext 验证，否则 A 也要改方案。

### 4.3 分层设计

```
MPRController (IOService)          probe/start/stop，持有 IOPCIDevice、IOMemoryMap、IOWorkLoop
 ├─ MPI/         寄存器常量与消息结构（来自 FreeBSD mpi/*.h，BSD 许可，裁剪 MPI26/NVMe 分支）
 ├─ MPRQueue     request/reply/free queue、SMID 与 chain/sense 池、doorbell 提交
 ├─ MPRDMA       IOBufferMemoryDescriptor + IODMACommand 封装，生成 IEEE SGE
 ├─ MPRIntr      IOFilterInterruptEventSource → workloop 内处理 reply descriptor
 ├─ MPRConfig    IOC Facts / Port Facts / config page / EventNotification / PortEnable
 ├─ MPRDiscovery 固件事件队列 → 目标表（devhandle、SAS address、LUN）→ nub 生命周期
 ├─ MPRBlockDev  IOBlockStorageDevice 子类：CDB 构造、IOStorageCompletion、UNMAP
 ├─ MPRTaskMgmt  超时、ABORT_TASK / LUN_RESET / TARGET_RESET、diag reset 恢复
 └─ MPRUserClient(后期)  寄存器 dump、config page、事件日志、MPI passthrough
```

一个 `MPRCommand` 绑定：`IOStorageCompletion` + SMID + request frame + SGL/chain + sense buffer + 超时定时器 + 目标 devhandle，对应 FreeBSD `struct mpr_command`（`mprvar.h:242-291`）与 Linux `struct scsiio_tracker`（`mpt3sas_base.h:884-892`）的职责。

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
| 命令与完成 | CAM CCB → `xpt_done` | `scsi_cmnd` → `scsi_done` | `MPRCommand` → `IOStorageCompletion` |
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
6. **块设备契约**：实现 `IOBlockStorageDevice.h` 的 14 个纯虚（`doAsyncReadWrite`、`reportBlockSize/MaxValidBlock/MediaState`、vendor/product/revision 等）；`doUnmap`/`doSynchronize` 有默认实现但需按 CDB 覆写；通过 `IOBlockStorageDriver` 生成 `IOMedia`（`IOBlockStorageDriver.h:1242-1397`）。
7. **最小 CDB 集**：INQUIRY、READ CAPACITY(16)、READ(10/16)、WRITE(10/16)、SYNCHRONIZE CACHE(10/16)、UNMAP、REQUEST SENSE、REPORT LUNS、START STOP UNIT、MODE SENSE；定义取 `MacKernelSDK/Headers/IOKit/scsi/SCSICmds_*.h`。
8. **固件事件**：首版只处理 `SAS_TOPOLOGY_CHANGE_LIST`、`SAS_DISCOVERY`、`SAS_DEVICE_DISCOVERY_ERROR`、`SAS_DEVICE_STATUS_CHANGE`，其余忽略并记录；拓扑稳定后再 `registerService`，避免盘符抖动。
9. **电源与复位**：`mpr_reinit`（`mpr.c:837-924`）的「mask irq → diag reset → PCI 配置恢复 → 重新 IOC Facts → 重新注册事件 → 释放冻结命令」需 1:1 重建；watchdog 用 1 s doorbell 检查（`mpr.c:2256`）。
10. **文件行尾与部署**：所有上传到 macOS 的 `.plist`/`.sh`/`.py`/源码必须是 LF（`.gitattributes` 已声明 `* text=auto eol=lf`；CRLF 的 `Info.plist` 会让 kext 被静默丢弃）。kext 走 `kmutil`/AuxiliaryKernelExtensions.kc 与现有 OpenCore 流程，可复用同工作区 `drivers_src/NvmeHook` 的部署验证经验；本驱动是 PCI 驱动，不依赖 Lilu。目标机串口调试默认关闭（`debug=0x100`、无 `serial=5`），长期开启 `kprintf` 会显著拖慢系统，仅在抓日志时临时开启。

---

## 6. 里程碑与验收

| 阶段 | 目标与交付 | 验收方式 | 退出条件 |
|---|---|---|---|
| **M0 环境** | 确认目标机 SAS3008 的 PCI ID/固件模式/BAR；Xcode 工程接入 MacKernelSDK（`KERNEL_EXTENSION_HEADER_SEARCH_PATHS`、`KERNEL_FRAMEWORK_HEADERS`）；最小 kext 能加载并匹配；验证 `com.apple.iokit.IOStorageFamily`/`IOPCIFamily` KPI 可链接 | `ioreg -l -p IODeviceTree`、`kmutil showloaded`、`log show --predicate 'process == "kernel"'`；最小 kext 的 `start` 成功打印 | 空 kext 稳定加载/卸载，PCI 匹配字典命中；KPI 问题有明确结论 |
| **M1 寄存器与 IOC** | MMIO dump、diag reset → READY、IOC Facts、按 Facts 计算队列几何、分配 DMA 池、IOC Init → OPERATIONAL（先轮询，不开中断） | 用户态诊断工具读 Doorbell/`IOCState`，对比参考驱动的 Facts 字段；寄存器 dump 与 `mpi2.h` 常量一致 | 无需中断即可收发一条同步 config 请求（如读 Manufacturing Page0）；IOC 保持在 OPERATIONAL |
| **M2 中断与事件** | 单 MSI/MSI-X 向量 + `IOFilterInterruptEventSource`、reply descriptor 解析、EventNotification、PortEnable、固件事件队列 | 能看到 `SAS_DISCOVERY`/拓扑事件；`mpr_user`-style config page 读取成功；连续 10 分钟无丢失 reply | reply queue 与 Host Index 回写无泄漏；事件队列不丢不重 |
| **M3 单盘只读** | 目标表 → 第一个 `IOBlockStorageDevice` nub；INQUIRY / READ CAPACITY(16) / READ(16)；`IOMedia` 出现 | `diskutil list`、`ioreg -r -c IOMedia`、`dd if=/dev/rdiskN of=/dev/null bs=1m count=1024` 无错；只读挂载可列目录 | 1 GB 顺序读回读数据一致，无 kernel panic、无 DMA 越界 |
| **M4 读写与恢复** | WRITE(10/16)、SYNCHRONIZE CACHE、UNMAP、auto sense、超时与 TMF（ABORT/LUN RESET/TARGET RESET）、diag reset 恢复 | `dd` 写后回读校验；`diskutil` 分区/卸载；人为拔盘或超时注入后驱动能恢复；TRIM/UNMAP 对已知敏感固件先做小范围验证 | 24 h 混合读写无挂起；复位后盘能重新出现 |
| **M5 拓扑与热插拔** | topology change 事件、expander 下多盘、设备移除/新增、重启后 persistent 映射语义（可退化为每次重新发现） | 热插拔 10 轮，`diskutil list` 与 IORegistry 目标表一致；无幽灵盘 | 多盘并发 IO 下拓扑变化不丢命令 |
| **M6 加固/诊断（可选）** | watchdog、IORegistry 统计、`IOUserClient` 诊断工具、性能基线、日志开关 | 工具可导出 config page / 事件日志 / 寄存器；性能与 FreeBSD/Linux 同机对比 | 可定位 90% 现场问题而无需串口 |
| **M7 后续（不在首版）** | IR 卷、SES、固件升级、多队列/MSI-X 亲和、中断合并、启动盘、签名与发布 | 单独立项 | — |

每个阶段开始前先写该阶段的失败判据与回滚方式（kext 只放测试机、保留可启动的干净 EFI 配置）。**禁止在带有重要数据的磁盘上做写测试。**

---

## 7. 合规与许可

1. FreeBSD `mpr`（BSD-2-Clause）的 MPI 头文件、寄存器时序、算法可以移植进本仓库，但**必须保留原版权声明与许可文本**，并在 `NOTICE`/`LICENSE` 中逐文件列出来源与 commit。
2. Linux `mpt3sas`（GPL-2.0）**只作为行为与协议规范的参考，不复制任何代码到驱动源码**。`reference/` 目录不参与构建、不入库；计划与文档中对它的引用属于事实描述。若未来需要 Linux 独有的实现细节，采用「读懂后独立重写 + 记录设计依据」的方式，并在 PR/提交说明中注明。
3. MPI 结构体定义以 FreeBSD `mpi/*.h` 为唯一代码来源；两份 `mpi2*.h` 的差异（Linux 版含 `mpi2_image.h`、字段更新）在 `MPI/` 目录下用注释记录，不混用。
4. `MacKernelSDK` 头文件仅用于编译，遵循其自身许可；不把 SDK 头文件拷进驱动源码树。

---

## 8. 风险与未决问题

| # | 风险 | 影响 | 缓解 / 验证 |
|---|---|---|---|
| R1 | `com.apple.iokit.IOStorageFamily` / `IOPCIFamily` KPI 在目标 macOS 26 上不可用或版本不匹配 | 路线 A 无法链接 | M0 用最小 kext 验证；先尝试调整 `OSBundleLibraries` 中系统 family 的版本声明，仍不可用则暂停 M3 并重新评估路线（可退化为 IOUserClient 诊断，但不产出磁盘） |
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

需要用户决策的未决项：

1. 首版是否要求**启动盘**支持？是否要求 IR 卷？（当前按「不做」计划）
2. 目标 macOS 版本下限是否只锁 Tahoe 26 / x86_64，还是要做 12–26 的版本适配（影响 SDK 的 `__MAC_OS_X_VERSION_MIN_REQUIRED` 门控与 kmod 兼容）？
3. 是否接受首版单中断向量、无中断合并的性能水平？
4. 测试盘是否允许破坏性写入？是否需要只读验收阶段（M3）作为硬门槛？
5. 诊断用户态工具是放在本仓库，还是借用现有工具链？
6. 是否需要签名/发布级打包，还是仅测试机开发签名？

---

## 9. 下一步（M0 具体动作）

1. 在目标机上确认 SAS3008 的 PCI 配置空间、BAR、当前固件模式与 `IOPCIDevice` 匹配字典（不要沿用历史磁盘/设备号）。
2. 建立 kext Xcode 工程与 `Info.plist`：`IOProviderClass=IOPCIDevice`、`IOPCIPrimaryMatch` 匹配 `0x1000:0x0097`、`OSBundleLibraries` 声明实际需要的 KPI/系统 family；上传前确认 LF。
3. 写空壳 `MPRController::probe/start`，只做 `setMemoryEnable`/`setBusMasterEnable` + BAR 映射 + 寄存器 dump，验证 M0 的 KPI 假设。
4. 从 FreeBSD `mpi/*.h` 裁剪出 SAS3008 所需的 MPI2.5 结构，落到 `MPI/`，保留版权头；建立与 `reference/` 的差异记录。
5. 按 §6 的 M1 开始实现 IOC 初始化（轮询优先），每一步都用诊断工具与参考源码的字段定义交叉核对。
