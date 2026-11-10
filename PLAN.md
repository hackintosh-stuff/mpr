# PLAN — 将 LSI/Broadcom Fusion-MPT SAS3 驱动移植到 macOS

状态：草案 v0.6（2026-11-11；SAS3008 已接入并接测试盘，M0 空壳 kext 完成 ESP 部署与真机加载验证）
目标硬件：Broadcom Ltd. / Avago Tech (LSI) SAS3008，PCI `0x1000:0x0097`，MPI2.5 / 12 Gb/s SAS3 HBA（**已接入；已接 1 块 SATA SSD 测试盘，允许读写**）
参考驱动：FreeBSD `mpr`、Linux `mpt3sas`、Apple OSS `IOSCSIParallelFamily`（架构与 ABI 参考）
目标平台：macOS Tahoe 26.7.2（Build `25G332`，`Darwin 25.6.0`，x86_64，MSI X570 测试机；已由现场 `uname -a` / `sw_vers` 确认），基于 `MacKernelSDK` 的 IOKit kext。BootKC 分析文档标注同为 26.7.2 / 25G332，KC 内部分 kext 构建于 26.7.1 / 25G325 属构建批次差异，不是系统版本冲突；系统升级后需重取 KC。

本文件只描述计划，不含实现代码。参考源码引用以拉取到 `reference/` 的固定 commit 为准；内核侧事实来自 `C:\Users\ling\OneDrive\Documents\Agent\Hackintosh\ida\BootKernelExtensions.kc` 的 IDA 反编译/符号表（见 §4.0）。下文 `xxx.c:NNN` 形式的行号，FreeBSD 侧相对 `reference/freebsd/sys/dev/mpr/`，Linux 侧相对 `reference/linux/drivers/scsi/mpt3sas/`，SDK 侧相对 `MacKernelSDK/`。

---

## 1. 目标与范围

### 1.1 首版目标（M0–M5）

1. kext 通过 `IOPCIDevice` 匹配 SAS3008，完成 PCI 使能、BAR 映射、IOC 复位与初始化。
2. 建立 request / reply / reply-free 队列与 chain 池，能用中断或轮询收发 MPI 消息。
3. 通过固件事件完成拓扑发现，把经 STP/SATA 接入的磁盘暴露为**原生** `IOMedia`（`diskutil list` 可见）；目标表与 nub 生命周期覆盖 STP/SATA。SAS SSP / expander 拓扑当前无测试条件，代码保留识别路径但不列入首版验收。
4. 支持 READ/WRITE、SYNCHRONIZE CACHE、UNMAP、auto sense、超时与 TMF 恢复、热插拔。
5. 有基本诊断通路（IORegistry 属性 + 用户态工具），可抓 IOC 状态、config page、事件日志。

### 1.2 明确不做（首版）

- IR/RAID 卷、SES 机箱管理、NVMe/PCIe enclosure、Tri-Mode、固件升级、启动盘引导。
- Apple Silicon、dext/DriverKit、多队列高 IOPS 优化、中断合并、WarpDrive。
- 兼容 SAS2（`mps`/`0x0072`）与 MPI26/GEN35/SEA 分支（SAS3416 等）。
- 接入 `IOSCSIParallelInterfaceController` / macOS SCSI 应用层栈（路线 B 已复核为技术可行，但列为 M7+ 备选，见 §4.0.2、§4.2）。
- SAS SSP 直连盘、expander 下多盘与 SMP passthrough 的实机验收（当前无测试条件）；拓扑解析可保留 SSP `device_info` 分支，但不作为首版退出条件。

### 1.3 驱动形态

**IOKit 内核扩展（kext），不是 dext。** 依据见 §4.2。IT/IR 固件：首版按 IT 模式（纯 initiator）开发与验收，IR 模式仅保留探测与拒绝路径。

---

## 2. 基线（可复现）

| 项目 | 内容 |
|---|---|
| FreeBSD 参考 | `freebsd/freebsd-src` `main`，commit `e56090afb68b6c036d6b54c7b66d762d98bd96d0`（2026-11-10）；`sys/dev/mpr/` 13 个文件 + `mpi/` 13 个文件 |
| Linux 参考 | `torvalds/linux` `master`，commit `b3dfbde62cf1981c4638939c159dfd087b7a72e9`（2026-11-10）；`drivers/scsi/mpt3sas/` 16 个文件 + `mpi/` 10 个文件 |
| 合计（FreeBSD + Linux） | 52 个文件，2,664,265 字节，逐文件按 GitHub Contents API 的 `size` 校验；固定 commit 的 `raw.githubusercontent.com` 下载 |
| MacKernelSDK | git 子模块 `3f750085caa17ec3a7880f11c11bf4f48cd6a164`（2026-09-23），基于 macOS 11 / Xcode 12.0 SDK，补充 `xnu-8019.80.24` 头文件 |
| Apple OSS `IOSCSIParallelFamily` | `apple-oss-distributions/IOSCSIParallelFamily` tag `IOSCSIParallelFamily-352` / commit `560f74ab0c6f5d9aa6c99675d8e95986b202144d`（2026-04-21 推送，与 `main` HEAD 相同）；codeload tarball 261,914 B、86 个文件；落在 `reference/IOSCSIParallelFamily/`，只读参考、不参与构建 |
| 内核缓存反编译 | `ida/BootKernelExtensions.kc` 67,600,384 B（MD5 `b9da5eafcd90e439ae7c55c1ca8ddf33`）、`BootKernelExtensions.kc.i64`、survey 缓存 `.ida-mcp-survey.json`、`ionvme-symbol-analysis.md`；KC 基址 `0xffffff8000200000`，209 个 bundle / 2396 段 / 117,470 函数（111,780 已命名）/ 136,888 字符串。离线核对脚本 `ida/kc_storage_vtable_check.py`（路线 A）、`ida/kc_spi_routeb_check.py`（路线 B） |
| 许可 | FreeBSD `mpr`：BSD-2-Clause（Yahoo!/LSI/Avago/Broadcom 版权，`mpr.c:1-12`）；Linux `mpt3sas`：GPL-2.0（`mpt3sas_base.c:10-12`）；Apple OSS `IOSCSIParallelFamily`：APSL 2.0（`APPLE_LICENSE`） |
| 版本控制 | `MacKernelSDK` 是子模块；`reference/` 被 `.gitignore` 的 `/reference/` 忽略，**不入库**；`ida/` 位于工作区根目录，同样不入库、不参与构建。本节的 commit 与 MD5 是唯一复现依据 |

目录布局：

```
mpr/
  PLAN.md
  MacKernelSDK/                       (submodule, 入库)
  reference/                          (gitignored, 只读参考)
    freebsd/sys/dev/mpr/  + mpi/
    linux/drivers/scsi/mpt3sas/  + mpi/
    IOSCSIParallelFamily/           (Apple OSS, APSL 2.0)
```

复现拉取：对 FreeBSD / Linux 两个 commit 分别用 `https://api.github.com/repos/<repo>/contents/<path>?ref=<sha>` 递归列目录，再从 `https://raw.githubusercontent.com/<repo>/<sha>/<path>` 取文件；Apple OSS 仓库用 `https://codeload.github.com/apple-oss-distributions/IOSCSIParallelFamily/tar.gz/560f74ab0c6f5d9aa6c99675d8e95986b202144d` 取整树。本机访问 GitHub 需走 `http://127.0.0.1:10808` 代理，HTTPS 下载按工作区约束在 MCP 控制台执行。不要用移动分支的 `HEAD` 作为基线。

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

### 4.0 内核缓存反编译给出的实证

分析对象 `BootKernelExtensions.kc`（macOS 26.7.x Boot KC），IDA 会话 `5c67bd08`。`ionvme-symbol-analysis.md` 已确认 **KC 内地址即链接地址/运行时地址、无 slide**（IONVMeFamily `__text` 起 `0xffffff80026bd000` 与运行时 `kextstat` 一致）；`.ida-mcp-survey.json` 给出 209 个 bundle / 2396 段 / 117,470 函数（111,780 命名）/ 136,888 字符串。本次针对存储栈复核得到：

1. **系统侧 family 确实存在，且都在 BootKC 里**（survey 段名，非推测）：`com.apple.iokit.IOStorageFamily`、`com.apple.iokit.IOPCIFamily`、`com.apple.iokit.IOSCSIArchitectureModelFamily`、`com.apple.iokit.IOSCSIParallelFamily`、`com.apple.iokit.IOSCSIBlockCommandsDevice` / `IOSCSIMultimediaCommandsDevice` / `IOSCSIReducedBlockCommandsDevice`、`com.apple.iokit.SCSITaskUserClient`、`com.apple.iokit.IOAHCIBlockStorage`、`com.apple.iokit.IONVMeFamily`、`com.apple.driver.AppleVirtIO`。KC 里还有对应的 KPI bundle 标识字符串（`0xffffff800020dcc8` 起：`...IOPCIFamily`、`...IOSCSIArchitectureModelFamily`、`...IOSCSIParallelFamily`、`...IOStorageFamily`）。**因此"目标系统没有 SCSI/存储栈"是错的**；路线 B 的工具链与语义可行性见 §4.0.2、§4.2。
2. **路线 A 的 ABI 基础可实证**：`__ZTV20IOBlockStorageDevice` 在 `com.apple.iokit.IOStorageFamily:__const` @ `0xffffff8002b70450`；`IOBlockStorageDevice` 的方法实体在 `IOStorageFamily:__text`（`init`、`setProperties`、`getWriteCacheState`、`setWriteCacheState`、`doLockUnlockMedia`、`reportLockability`、`reportPollRequirements`、`requestIdle`、`doDiscard`、`doUnmap`、`doSetPriority`、`doSynchronizeCache`、`doSynchronize`、`doGetProvisionStatus`，以及 `_RESERVEDIOBlockStorageDevice4..22+` 保留槽）。本次已读出该 vtable 前 256 字节的原始槽位，可与 SDK 头的虚函数顺序逐项比对。
3. **Apple 自己有多套 `IOBlockStorageDevice` 子类可作模板**：
   - `AppleVirtIOBlockStorageDevice`（`com.apple.driver.AppleVirtIO:__text`，`__ZTV29AppleVirtIOBlockStorageDevice` @ `0xffffff8001f0d1f0`）：实现全部 14 个纯虚（`doAsyncReadWrite`/`doEjectMedia`/`doFormatMedia`/`doGetFormatCapacities`/各 `get*String`/各 `report*`），以及 `doUnmap`、`doSynchronizeCache`、`doSetPriority`、`doLockUnlockMedia`、`reportPollRequirements`，并有自己的 `start`/`stop`/`willTerminate`/`queueEventHandler`。**这是与本项目最像的「自研 transport + 块设备 nub」模板，优先级高于 NVMe/AHCI**。
   - `IONVMeBlockStorageDevice`（`IONVMeFamily:__text`）：`start`、`GetDeviceProperties`、`doAsyncReadWrite`、`CompleteAsyncRequest`、`QueueCreate/Delete`、`QueueSubmitRequest`/`QueueCompleteRequest`、`PowerDownHandler`、`SetNVMeProperties`、`ValidateEntitlement`、`doUnmap`/`doUnmapNVMe`、`getSerialNumberString` 等。
   - `IOAHCIBlockStorageDevice`（`IOAHCIBlockStorage:__text`）、`IOHDIXHDDrive*`、`AppleXsanDevice` 等同形态。
4. **反编译实证（`IONVMeBlockStorageDevice::doUnmap` @ `0xffffff80026d0396`）**：先 `IOService::isInactive` 检查 → `kalloc_shared_data_external(4096,1)` → `IOMemoryDescriptor::withAddress(buf,0x1000,2)` → `md->prepare()` → 由 `this+0x90` 的 controller 走 `vtable[+2216]` 取 request（GetRequestFromPool）→ 挂 MD → 构造并提交 → 完成后归还池；临界区用 `AppleNVMeWorkLoop::Lock/SleepGate/Unlock/WakeupGate` 串行化。与 `ionvme-symbol-analysis.md` §3 归纳的同步命令流程（GetRequestFromPool → ResetForNewRequest → prepare → GenerateIOVMSegments → BuildCommand → ProcessSyncNVMeRequest → complete → ReturnRequestToPool）一致。**这是 macOS 侧「命令对象 + IOMemoryDescriptor + 同步/异步完成」的可核对模板**，也是 §4.3 `MPRCommand` 设计的现实参照。
5. **`IOSCSIParallelInterfaceController` 的实体实现确实在 `com.apple.iokit.IOSCSIParallelFamily:__text`**：`GetSCSIParallelTask`、`ExecuteParallelTask`、`CompleteAbortTask*`、`ServiceInterrupt`、`FilterInterrupt`、`EnableInterrupt`、`InitializeDeviceList`、`DestroyTargetForID` 等符号齐全。系统有实现；SDK 侧头文件与内核 KPI 符号集均齐备（§4.0.2、§4.1）。
6. **`OSBundleLibraries` 的真实样例**：KC 的 prelink Info.plist 中 Apple kext 声明 `com.apple.iokit.IOPCIFamily = 1.1`、`com.apple.kpi.iokit/libkern/mach = 8.0/9.0` 等；这些是 Apple 内部 kext 的历史版本，只作为「依赖键长什么样」的参考，第三方 kext 的版本仍以现场 `kextstat`/KPI 为准。
7. **资料边界**：`ionvme-symbol-analysis.md` §4/§5 的 Lilu `solveSymbol` / Samsung 990 EVO 结论与本项目无关且已在别处被推翻，本计划只取其 §1–§3 的寄存器/命令/反编译事实；KC 里多数符号是 names 而非 IDA 已识别函数（`lookup_funcs` 对多数 vtable 槽返回 "Not a function"，部分 `decompile` 也因此失败），故 ABI 核对以**符号表 + vtable 原始字节**为准，不依赖 IDA 的函数识别。

#### 4.0.1 无硬件预核查结果（2026-11-10）

以下项目均在不接入 SAS3008 的条件下完成，可直接作为 M0 的输入：

| 核查项 | 方法 | 结论 |
|---|---|---|
| 目标机系统版本 | `ssh ling@192.168.50.204` → `uname -a` / `sw_vers` | `Darwin 25.6.0` / `macOS 26.7.2` / `25G332`，与 KC 分析文档一致；`Darwin 25.6.0` 与 26.7.x 是同一系统 |
| family / KPI 版本 | 现场 `kextstat` | `com.apple.iokit.IOPCIFamily 2.9`、`IOStorageFamily 2.1`、`IOSCSIArchitectureModelFamily 545.100.10`、`IOSCSIBlockCommandsDevice 545.100.10`、`SCSITaskUserClient 545.100.10`；`com.apple.kpi.{iokit,libkern,mach,bsd} 25.6.0`。`OSBundleLibraries` 填的是最低要求版本，不应照抄 KC prelink plist 里的 1.1 / 8.0 / 9.0 等历史值 |
| `IOSCSIParallelFamily` 运行时状态 | `kextstat \| grep -c IOSCSIParallelFamily` | **0，未加载**。该 family 存在于 BootKC 二进制，但本机无 SPI 硬件故不 attach；路线 B 若声明依赖，需由内核把它拉起（符号集与版本见 §4.0.2） |
| **vtable ABI 一致性（R1 核心）** | `ida/kc_storage_vtable_check.py` 离线解析 KC fileset symtab，对 vtable 槽做 chained-fixup 解码（自检：槽 2 raw `0x40000002a6c504` → `0xffffff8002b6c504`），再与 SDK 头 29 个 `virtual` 声明逐项比对 | **完全一致**。`IOBlockStorageDevice` 自有方法从槽 268 起连续排列：`doEjectMedia`268 / `doFormatMedia`269 / `doGetFormatCapacities`270 / `doLockUnlockMedia`271 / `doSynchronizeCache`272 / `getVendorString`273 … `reportWriteProtection`284 / `getWriteCacheState`285 / `setWriteCacheState`286 / **`doAsyncReadWrite`287** / `requestIdle`288 / `doDiscard`289 / `doUnmap`290 / `doSetPriority`291 / `doSynchronize`292 / `doGetProvisionStatus`293；覆写的继承方法 `init`/`getProperty`/`setProperties` 在槽 75/87/98。槽 294–321 = `_RESERVEDIOBlockStorageDevice4..31`（28 个），与 SDK 头 `OSMetaClassDeclareReservedUnused(...,4..31)` 及 `Used(...,0..3)` 完全对应。`AppleVirtIOBlockStorageDevice` 与 `IONVMeBlockStorageDevice` 两套子类在相同槽索引覆写同名方法（268/270/277/287/290 交叉验证通过）。**结论：macOS 11 SDK 的 `IOBlockStorageDevice.h` 可用于 26.7.2 内核编译，R1 的 ABI 风险关闭** |
| PCI 匹配键 | `MacKernelSDK/Headers/IOKit/pci/IOPCIDevice.h:324-356` | `IOPCIPrimaryMatch` 匹配 config 0x00 的 vendor+device，格式 `0x<device><vendor>`（示例 `0x00261011` = dev 0x0026 / vendor 0x1011），可带 `&mask`。SAS3008 用 `0x00971000` 正确 |

> 复现：`py -3 C:\Users\ling\OneDrive\Documents\Agent\Hackintosh\ida\kc_storage_vtable_check.py`（脚本位于工作区 `ida/`，与 `reference/` 一样不入库）。系统升级后重跑该脚本即可回归 ABI。

#### 4.0.2 路线 B 复核（2026-11-10，Apple OSS 源码 + BootKC）

`reference/IOSCSIParallelFamily/`（§2 的固定 commit）与同一份 BootKC 交叉核对的结论：**路线 B 在工具链与语义上都可行**；§4.2 的判定据此改为「可行但不推荐作首版」，理由改为 KPI 废弃与工程复杂度，而非可行性。

| 核查项 | 实证 | 结论 |
|---|---|---|
| SDK 侧头文件 | `MacKernelSDK/Headers/IOKit/scsi/spi/IOSCSIParallelInterfaceController.h` 存在；36 个 `virtual` 声明与 Apple OSS 头逐项同序 | 编译所需头文件齐备 |
| KPI 符号集 | 仓库 `config/IOSCSIParallelFamily.exports` 导出 117 个符号（含 `__ZTV33IOSCSIParallelInterfaceController`、`gMetaClass`/`metaClass`/`MetaClass::alloc`、`GetSCSIParallelTask`、`ExecuteParallelTask`、`CompleteParallelTask`、`CreateTargetForID`/`DestroyTargetForID`），`config/generate-symbols-plist.sh` 写入 `__LINKINFO,__symbolsets`；KC 中该 family 的 `__LINKINFO` 内 symbolsets.plist 为 `CFBundleVersion=3.0.0` / `OSBundleCompatibleVersion=1.2.0`，**117/117 导出符号都在 KC 符号表中** | 它是正式 KPI，第三方 kext 可声明依赖并解析符号 |
| 版本对应 | KC prelink plist：`/System/Library/Extensions/IOSCSIParallelFamily.kext`，3.0.0 / 1.2.0，`DTSDKName=macosx26.7.internal`、`DTPlatformVersion=26.7.1`、`LSMinimumSystemVersion=26.7`；依赖 `IOSCSIArchitectureModelFamily 1.0.0`、`IOPCIFamily 2.7`、KPI 9.0.0 | 源码与目标机 26.7.x 二进制同版本 |
| 依赖声明样例 | 仓库 `TestTools/AppleSCSIHBAEmulator/Info.plist` 是继承该类的官方 kext，声明 `com.apple.iokit.IOSCSIParallelFamily = 1.2.0`；KC 内第三方 `com.promise.driver.stex (PromiseSTEX)` 同样声明 1.2.0 且 `Physical Interconnect = SAS` | 第三方应写 **1.2.0**；不要照抄 `AppleLSIFusionMPT` 的 `1.3.7` |
| Apple 自己的 SAS 驱动 | BootKC 内 `AppleLSIFusionMPT.kext` 的 `AppleLSIFusionSAS` personality：`IONameMatch = LSILogic,sas / pci1000,50 / pci1000,56 / pci1000,58`、`Physical Interconnect = SAS`、依赖该 family | 「强制 SPI 语义与 SAS3 不匹配」不成立；但**不含 `pci1000,97`**，Apple 驱动不会匹配 SAS3008，且老 MPT 固件与 MPI2.5 命令集不同，不能靠改 Info.plist 接管 |
| 语义裁剪 | `DoesHBASupportSCSIParallelFeature()` 可对所有 feature 返回 false（官方 emulator 即如此）；`SCSIDeviceIdentifier` 为 `UInt64`（`IOKit/scsi/SCSITask.h:56`）；`DoesHBAPerformDeviceManagement()` 返回 true 时基类不做 0..max 线性扫描，由驱动按拓扑事件自行 `CreateTargetForID()`（`IOSCSIParallelInterfaceController.cpp:359-376,1710-1784`） | 可对接 SAS3 devhandle / expander，不受 16 槽并行总线限制 |
| vtable ABI | `__ZTV33IOSCSIParallelInterfaceController` @ `0xffffff8002ae6168`：槽 268–276 = 9 个纯虚，277–279 = `Used(1..3)` 的 `DoesHBAPerformAutoSense`/`ReportHBAConstraints`/`DoesHBASupportMultiPathing`，280–284 = `Unused(4..8)`，295–305 = `Used(9..12)`/`Unused(13..15)`；与 SDK 头、OSS 头声明一致（SDK 头多出的 `Unused(3)` 来自 `< 10.7` 的 `#else` 分支） | 路线 B 的 ABI 风险同样关闭 |
| 废弃状态 | OSS 头 `141` 行 `__kpi_deprecated("Use IOUserSCSIParallelInterfaceController with DriverKit, instead")`；`MacKernelSDK/Headers/sys/cdefs.h:208` 将其展开为 `__deprecated_msg`（警告，非错误）；仓库含 `SCSIControllerDriverKit/` 与 `DriverKitKernelSupport/` | 26.7.2 仍可用，但长期方向是 dext，未来系统可能移除 |

> 复现：`py -3 C:\Users\ling\OneDrive\Documents\Agent\Hackintosh\ida\kc_spi_routeb_check.py`（脚本位于工作区 `ida/`，不入库）。系统升级后与 `kc_storage_vtable_check.py` 一并重跑。

仍未实证的环节（需授权，见 §9）：① MacKernelSDK + 当前 Xcode 能否链接该 family 符号（SDK 没有它的 KPI stub/symbolset 声明，需确认 `-undefined dynamic_lookup` 或补声明）；② 真机 `kmutil` 加载最小 kext（`OSBundleLibraries` 写 `1.2.0`），确认 BootKC 中运行时未加载的 family 被依赖拉起、符号解析成功、子类 vtable 不错位。走 B 还需自研 `SCSIParallelTask` ↔ `Mpi2SCSIIORequest_t`、devhandle ↔ UInt64 target ID、TMF 与拓扑事件映射；M1/M2 工作量不变，只省掉 M3/M4 的部分 CDB 构造。

### 4.1 MacKernelSDK 提供什么

存在（`MacKernelSDK/Headers/`）：`IOKit/pci/IOPCIDevice.h`（config 读写、`findPCICapability`、BAR 映射）、`IOKit/IOBufferMemoryDescriptor.h`、`IOKit/IODMACommand.h`（`gen64IOVMSegments`、`kIODMACommandOutputLittle64`）、`IOKit/IOMemoryDescriptor.h`（`IOMemoryMap`）、`IOWorkLoop`/`IOCommandGate`/`IOInterruptEventSource`/`IOFilterInterruptEventSource`/`IOTimerEventSource`/`IOCommandPool`/`IOLocks`、libkern 集合与字节序、`IOKit/storage/IOBlockStorageDevice.h` 及 `IOStorage`/`IOMedia`/`IOBlockStorageDriver`、`IOKit/storage/IOStorageProtocolCharacteristics.h`（`Physical Interconnect = "SAS"`）、以及 `IOKit/scsi/` 下的 `SCSITask.h`、`IOSCSIProtocolInterface.h`、`IOSCSIPeripheralDeviceNub.h`、各 `SCSICmds_*.h`（CDB 定义可用），还有 `IOKit/scsi/spi/IOSCSIParallelInterfaceController.h`（路线 B 基类，36 个 `virtual` 声明与 Apple OSS 头同序，见 §4.0.2）。

`IOSCSIParallelInterfaceController` 的头文件已在 SDK 中，第三方 kext 不需要额外伞头：只需在 `Info.plist` 的 `OSBundleLibraries` 声明 `com.apple.iokit.IOSCSIParallelFamily = 1.2.0`，符号由内核 `__LINKINFO,__symbolsets` 在加载时解析。Apple OSS 仓库还提供官方 HBA 子类样例 `TestTools/AppleSCSIHBAEmulator/`（§4.0.2）。

不存在：SAS/SSP/SMP/expander 框架头、`IOSCSIArchitectureModelFamily` 的伞头、`IOSCSIBlockStorageDevice.h`、PCIDriverKit 的 `.iig` 依赖链、xcconfig/CI、arm64 `libkmod`、kernel collection/签名文档。DriverKit 侧的 `SCSIControllerDriverKit`/`IOUserSCSIParallelInterfaceController` 不在 MacKernelSDK 中，只出现在 Apple OSS 仓库。SDK 中 `com.apple.iokit.IOSCSIArchitectureModelFamily` 只作为字符串常量出现（`IOKit/scsi/IOSCSIProtocolInterface.h:166`）。**注意：这里说的是「SDK 缺」，不是「系统缺」**——系统侧这些 family 的实体与 KPI 符号集见 §4.0、§4.0.2。

### 4.2 三条路线对比与决策

| 路线 | 做法 | 判定 |
|---|---|---|
| **A（采纳）** | 自研 `IOService`（provider=`IOPCIDevice`）实现 HBA；每块盘实例化一个 `IOBlockStorageDevice` 子类 nub，`registerService` 后由系统 `IOBlockStorageDriver` 创建 `IOMedia`/`IOMediaBSDClient` | **可行，且已有实证**：BootKC 中 `IOStorageFamily` 的 `__ZTV20IOBlockStorageDevice` 与 Apple 自研子类（`AppleVirtIOBlockStorageDevice`）都在（§4.0）；只依赖 `IOBlockStorageDevice` 抽象接口与 14 个纯虚方法，耦合面最小 |
| B | 继承 `IOKit/scsi/spi/IOSCSIParallelInterfaceController`，复用 macOS SCSI 应用层栈 | **技术可行，但不作为首版**：SDK 已有该头、BootKC 有完整 KPI 符号集且 vtable ABI 一致（§4.0.2），Apple 自身 `AppleLSIFusionMPT` 与第三方 `PromiseSTEX` 都以它实现 SAS；但 KPI 已 `__kpi_deprecated` 并指向 DriverKit、依赖两个运行时未加载的私有 family、需新增 `SCSIParallelTask` ↔ MPI 映射层，而 M1/M2 工作量不变。最小 kext 链接/加载 spike 验证后列为 M7+ 备选 |
| C | `IOService` + `IOUserClient` 诊断透传，用户态发 CDB | **只能做调试**，无法生成 `IOMedia`/`/dev/disk`，不能作为主路线；可作为 M6 的诊断通道 |

决策：**A**。SDK 的 SCSI 头文件只用来取 CDB 操作码、sense、任务状态等**定义**，不作为继承体系。路线 B 不再是「不可行」，而是「可行但维护风险与复杂度更高」：KPI 已废弃、依赖私有 family 链、系统升级可能移除，且收益只覆盖 M3/M4 的部分 CDB 工作。

> M0 的 ABI 前置验证：`com.apple.iokit.IOStorageFamily` / `IOPCIFamily` 的存在性，以及 MacKernelSDK（macOS 11 / Xcode 12.0）头与 26.7 内核 `IOBlockStorageDevice` vtable 槽位/保留槽的一一对应，均已由 BootKC 离线逐槽核对完成（§4.0.1）；`OSBundleLibraries` 的最低版本按现场 `kextstat` 取值，不照抄 KC prelink plist。剩余的是在真机用最小 kext 验证加载。路线 B 若启用，另有同等级的 SPI vtable 核对（§4.0.2）与最小 kext 链接/加载 spike。

### 4.3 分层设计

```
MPRController (IOService)          probe/start/stop，持有 IOPCIDevice、IOMemoryMap、IOWorkLoop
 ├─ MPI/         寄存器常量与消息结构（来自 FreeBSD mpi/*.h，BSD 许可，裁剪 MPI26/NVMe 分支）
 ├─ MPRQueue     request/reply/free queue、SMID 与 chain/sense 池、doorbell 提交
 ├─ MPRDMA       IOBufferMemoryDescriptor + IODMACommand 封装，生成 IEEE SGE
 ├─ MPRIntr      IOFilterInterruptEventSource → workloop 内处理 reply descriptor
 ├─ MPRConfig    IOC Facts / Port Facts / config page / EventNotification / PortEnable
 ├─ MPRDiscovery 固件事件队列 → 目标表（devhandle、SAS address、LUN、协议类型）→ nub 生命周期；覆盖 STP/SATA 分支
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
6. **块设备契约与 ABI**：实现 `IOBlockStorageDevice.h` 的 14 个纯虚（`doAsyncReadWrite`、`reportBlockSize/MaxValidBlock/MediaState`、vendor/product/revision 等）；`doUnmap`/`doSynchronize` 有默认实现但需按 CDB 覆写；通过 `IOBlockStorageDriver` 生成 `IOMedia`（`IOBlockStorageDriver.h:1242-1397`）。**26.7 内核的实体里还有 `doDiscard`、`doSynchronizeCache`、`doSynchronize`、`doSetPriority`、`doGetProvisionStatus` 与 `_RESERVEDIOBlockStorageDevice4..22+`（§4.0 第 2 条），SDK 头把它们标为 deprecated/保留，不能按 SDK 的 `= 0` 数量推断 vtable；vtable 核对已按 §4.0.1 完成，`AppleVirtIOBlockStorageDevice` 是最直接的实现模板。**
7. **最小 CDB 集**：INQUIRY、READ CAPACITY(16)、READ(10/16)、WRITE(10/16)、SYNCHRONIZE CACHE(10/16)、UNMAP、REQUEST SENSE、REPORT LUNS、START STOP UNIT、MODE SENSE；定义取 `MacKernelSDK/Headers/IOKit/scsi/SCSICmds_*.h`。
8. **固件事件与 STP/SATA 目标表**：首版只处理 `SAS_TOPOLOGY_CHANGE_LIST`、`SAS_DISCOVERY`、`SAS_DEVICE_DISCOVERY_ERROR`、`SAS_DEVICE_STATUS_CHANGE`，其余忽略并记录；拓扑稳定后再 `registerService`，避免盘符抖动。目标表按 devhandle + SAS address + LUN 建键，并按 SAS Device Page0 的 `device_info` 区分 SSP / STP / SATA（`MPI2_SAS_DEVICE_INFO_SSP_TARGET 0x400`、`STP_TARGET 0x200`、`SATA_DEVICE 0x80`，`mpi2_sas.h:107-114`；分支见 `mpt3sas_scsih.c:2778-2798`）。当前测试盘是 SATA SSD 走 STP，固件给出的 `0x4433221104…` 形式 SAS address 并非设备 WWN，因此持久标识优先用 INQUIRY/VPD 的 `t10.ATA` wwid 与 devhandle，不能假定 SAS address 全局唯一或稳定；SATA 队列深度按 `max_sata_qd` 单独设定，不套用 SSP 的 wide/narrow port 深度。
9. **电源与复位**：`mpr_reinit`（`mpr.c:837-924`）的「mask irq → diag reset → PCI 配置恢复 → 重新 IOC Facts → 重新注册事件 → 释放冻结命令」需 1:1 重建；watchdog 用 1 s doorbell 检查（`mpr.c:2256`）。
10. **文件行尾与部署**：所有上传到 macOS 的 `.plist`/`.sh`/`.py`/源码必须是 LF（`.gitattributes` 已声明 `* text=auto eol=lf`；CRLF 的 `Info.plist` 会让 kext 被静默丢弃）。kext 走 `kmutil`/AuxiliaryKernelExtensions.kc 与现有 OpenCore 流程，可复用同工作区 `drivers_src/NvmeHook` 的部署验证经验；本驱动是 PCI 驱动，不依赖 Lilu。目标机串口调试默认关闭（`debug=0x100`、无 `serial=5`），长期开启 `kprintf` 会显著拖慢系统，仅在抓日志时临时开启。
11. **内核缓存反编译的使用边界**：`ida/` 下的 `.kc`/`.i64` 只用于核对类/vtable/符号/依赖键等 ABI 事实，是把 macOS 11 SDK 头「校准」到 26.7 内核的手段；不复制 Apple 代码，也不把 KC 地址、vtable 偏移或内部符号写进驱动（会随系统升级失效）。系统升级后重取 KC，并重跑 §4.0.1 / §4.0.2 的两个离线核对脚本。

---

## 6. 里程碑与验收

| 阶段 | 目标与交付 | 验收方式 | 退出条件 |
|---|---|---|---|
| **M0 环境** | 确认目标机 SAS3008 的 PCI ID/固件模式/BAR；Xcode 工程接入 MacKernelSDK（`KERNEL_EXTENSION_HEADER_SEARCH_PATHS`、`KERNEL_FRAMEWORK_HEADERS`）；最小 kext 能加载并匹配；**用 KC 的 `__ZTV20IOBlockStorageDevice` / `__ZTV29AppleVirtIOBlockStorageDevice` 核对 SDK 头 vtable 槽位与保留槽，并确定 `OSBundleLibraries` 版本**（§4.2）。**vtable / KPI / 系统版本 / PCI 匹配键核查见 §4.0.1；SAS3008 接入后已完成 PCI 探测、BAR 解码、Xcode 工程与最小 kext 真机加载（§9）** | `ioreg -l -p IODeviceTree`、`kmutil showloaded`、`kextstat`、`log show --predicate 'process == "kernel"'`；最小 kext 的 `start` 成功打印；产出 vtable 逐槽比对表 | 空 kext 稳定加载/卸载，PCI 匹配字典命中；vtable 与 SDK 头一致（已核实），KPI 版本结论明确 |
| **M1 寄存器与 IOC** | MMIO dump、diag reset → READY、IOC Facts、按 Facts 计算队列几何、分配 DMA 池、IOC Init → OPERATIONAL（先轮询，不开中断） | 用户态诊断工具读 Doorbell/`IOCState`，对比参考驱动的 Facts 字段；寄存器 dump 与 `mpi2.h` 常量一致 | 无需中断即可收发一条同步 config 请求（如读 Manufacturing Page0）；IOC 保持在 OPERATIONAL |
| **M2 中断与事件** | 单 MSI/MSI-X 向量 + `IOFilterInterruptEventSource`、reply descriptor 解析、EventNotification、PortEnable、固件事件队列 | 能看到 `SAS_DISCOVERY`/拓扑事件；`mpr_user`-style config page 读取成功；连续 10 分钟无丢失 reply | reply queue 与 Host Index 回写无泄漏；事件队列不丢不重 |
| **M3 单盘只读** | STP/SATA 目标表 → 第一个 `IOBlockStorageDevice` nub（实现参照 §4.0 的 `AppleVirtIOBlockStorageDevice`）；INQUIRY / READ CAPACITY(16) / READ(16)；`IOMedia` 出现 | `diskutil list`、`ioreg -r -c IOMedia`、`dd if=/dev/rdiskN of=/dev/null bs=1m count=1024` 无错；只读挂载可列目录 | 1 GB 顺序读回读数据一致，无 kernel panic、无 DMA 越界 |
| **M4 读写与恢复** | WRITE(10/16)、SYNCHRONIZE CACHE、UNMAP、auto sense、超时与 TMF（ABORT/LUN RESET/TARGET RESET）、diag reset 恢复 | `dd` 写后回读校验；`diskutil` 分区/卸载；人为拔盘或超时注入后驱动能恢复；TRIM/UNMAP 对已知敏感固件先做小范围验证 | 24 h 混合读写无挂起；复位后盘能重新出现 |
| **M5 拓扑与热插拔** | topology change 事件、STP/SATA 设备移除/新增、重启后 persistent 映射语义（可退化为每次重新发现）；expander 下多盘与 SSP 因无测试条件不验收 | 热插拔 10 轮（STP/SATA 单盘），`diskutil list` 与 IORegistry 目标表一致；无幽灵盘 | 拓扑变化不丢命令 |
| **M6 加固/诊断（可选）** | watchdog、IORegistry 统计、`IOUserClient` 诊断工具、性能基线、日志开关 | 工具可导出 config page / 事件日志 / 寄存器；性能与 FreeBSD/Linux 同机对比 | 可定位 90% 现场问题而无需串口 |
| **M7 后续（不在首版）** | IR 卷、SES、固件升级、多队列/MSI-X 亲和、中断合并、启动盘、签名与发布；**或**路线 B 的最小 kext 链接/加载 spike 与 `SCSIParallelTask` ↔ MPI 适配评估（头与 KPI 依赖已实证齐备，§4.0.2）；dext 方向单独立项 | 单独立项 | — |

每个阶段开始前先写该阶段的失败判据与回滚方式（kext 只放测试机、保留可启动的干净 EFI 配置）。**测试盘已确认无重要数据，M4 允许 WRITE / UNMAP / 分区等破坏性写入与回读校验；内部 SATA 系统盘与 USB 安装/救援介质仍禁止任何写入。**

---

## 7. 合规与许可

1. FreeBSD `mpr`（BSD-2-Clause）的 MPI 头文件、寄存器时序、算法可以移植进本仓库，但**必须保留原版权声明与许可文本**，并在 `NOTICE`/`LICENSE` 中逐文件列出来源与 commit。
2. Linux `mpt3sas`（GPL-2.0）**只作为行为与协议规范的参考，不复制任何代码到驱动源码**。`reference/` 目录不参与构建、不入库；计划与文档中对它的引用属于事实描述。若未来需要 Linux 独有的实现细节，采用「读懂后独立重写 + 记录设计依据」的方式，并在 PR/提交说明中注明。
3. MPI 结构体定义以 FreeBSD `mpi/*.h` 为唯一代码来源；两份 `mpi2*.h` 的差异（Linux 版含 `mpi2_image.h`、字段更新）在 `MPI/` 目录下用注释记录，不混用。
4. `MacKernelSDK` 头文件仅用于编译，遵循其自身许可；不把 SDK 头文件拷进驱动源码树。
5. `ida/BootKernelExtensions.kc` 与 IDA 数据库属于 Apple 内核二进制的反编译资料，只用于核对 ABI / 接口契约与事实记录；不复制其代码、不把 KC 地址固化进驱动。`ionvme-symbol-analysis.md` 的 §4/§5（Lilu 符号 hook / Samsung 990 EVO）与本项目无关，不予采用。
6. `reference/IOSCSIParallelFamily/` 为 Apple OSS 源码，许可 **APSL 2.0**（不同于 FreeBSD BSD-2-Clause 与 Linux GPL-2.0）；只作头文件、ABI、依赖键与行为参考，不复制实现代码进驱动源码。若将来需要引用其中常量或结构定义，按 APSL 要求保留版权与许可声明，并在 `NOTICE`/`LICENSE` 中单列来源与 commit。

---

## 8. 风险与未决问题

| # | 风险 | 影响 | 缓解 / 验证 |
|---|---|---|---|
| R1 | **已关闭**：family 不存在/不可链接（§4.0 实证排除）与 MacKernelSDK（macOS 11）头 ↔ 26.7.2 内核 `IOBlockStorageDevice` vtable/保留槽不一致，曾担心子类 vtable 错位导致加载即 panic | 曾列为路线 A 首要阻塞 | 2026-11-10 已用 KC 离线逐槽核对（§4.0.1：方法槽 268–293、保留槽 294–321、两套 Apple 子类交叉验证）确认一致；系统大版本升级后重跑 `ida/kc_storage_vtable_check.py` 回归 |
| R2 | SAS3008 处于 IR 固件模式或固件版本过旧，事件/能力字段不同 | 初始化失败或发现不到盘 | 先读 Manufacturing Page0/IOC Facts 记录固件版本，按 IT 模式验收；IR 模式明确拒绝 |
| R3 | MSI-X 中断源索引与 `IOPCIDevice` 的 `intIndex` 语义不一致 | 收不到中断 | M1 先轮询；M2 逐个 index 试，读 `getInterruptType` 的 `kIOInterruptTypePCIMessaged` 位；必要时回退 INTx |
| R4 | 低 4GB / 4GB boundary / 63 位 DMA 约束在 macOS 上难以保证 | 固件写错地址、静默数据损坏 | 池化分配 + 物理地址校验 + 失败回退 32 位 SGE；M3 用数据校验和验证 |
| R5 | expander/SMP 与 SSP 拓扑处理复杂度 | 只认直连盘 | 首版验收限定 STP/SATA 直连；SSP/expander 无测试条件，识别分支保留但不作为退出条件；SMP passthrough 作为后续 |
| R6 | 超时/TMF 与 SMID 复用竞争 | 命令错乱、panic | SMID 冻结到 TMF 完成；参考 `mpr.c:2388-2676` 与 `scsih.c:3120-3204` 的状态机 |
| R7 | 电源管理/睡眠唤醒 | 唤醒后盘丢失或 IOC 未复位 | FreeBSD 侧无实现可抄，按 `mpr_reinit` 自研；M5 加入睡眠唤醒测试（唤醒需物理电源按钮，NanoKVM 虚拟键盘不能唤醒本机） |
| R8 | kext 签名 / SIP / AuxKC 重建 / OpenCore 配置 | 驱动加载不了 | 沿用 NvmeHook 的部署与验证流程；每次改 `Info.plist` 复查 LF 行尾与 `plutil -lint` |
| R9 | GPL 代码污染 | 许可风险 | 见 §7，`reference/` 不入构建，代码评审检查 |
| R10 | 启动盘与系统卷依赖 | 盘符/启动顺序变化 | 首版不做启动盘；测试机保留内部 SATA 系统盘 |
| R11 | UNMAP/TRIM 触发某些 SSD 固件缺陷 | 掉盘 | 已有同工作区 Optane TRIM 掉盘经验；测试盘（PCCOOLER SSD T200）支持 UNMAP 且允许读写，M4 先做小范围验证，出现异常立即禁用 UNMAP 并保留只读路径 |
| R12 | 单中断向量性能上限 | 吞吐低 | 首版接受；M7 再按 Linux `mpt3sas_base.c:3212-3501` 做多队列 |
| R13 | 系统升级后内核缓存与目标机版本漂移 | 按旧 vtable/符号做的设计在新内核上失效 | 现场已确认目标机 = 26.7.2 / 25G332 / Darwin 25.6.0，与 KC 分析文档一致；KC 内 26.7.1 / 25G325 只是部分 kext 的构建批次。系统升级后重取 BootKC 并重跑 §4.0.1 / §4.0.2 的脚本；不把 KC 地址写进代码 |
| R14 | 路线 B 依赖的 `IOSCSIParallelFamily` KPI 已 `__kpi_deprecated`，Apple 引导迁移到 DriverKit，未来系统可能移除或改 ABI | 若改走 B，新系统上可能无法加载，维护成本高于路线 A | 首版不走 B；若启用，先做最小 kext 链接/加载 spike，锁定机型与系统版本，每次升级重跑 §4.0.2 的符号集与 vtable 回归；dext 方向单独立项 |

需要用户决策的未决项：

1. 首版是否要求**启动盘**支持？是否要求 IR 卷？（当前按「不做」计划）
2. 目标 macOS 版本下限是否只锁 Tahoe 26 / x86_64，还是要做 12–26 的版本适配（影响 SDK 的 `__MAC_OS_X_VERSION_MIN_REQUIRED` 门控与 kmod 兼容）？
3. 是否接受首版单中断向量、无中断合并的性能水平？
4. 诊断用户态工具是放在本仓库，还是借用现有工具链？
5. 是否需要签名/发布级打包，还是仅测试机开发签名？
6. 目标机是否锁定 26.7.2 / 25G332？（现场已确认；升级会触发 §4.0.1 / §4.0.2 的 ABI 回归）
7. 是否在 M7 用最小 kext spike 评估路线 B，或改走 DriverKit/dext 单独立项？（当前按「都不做」计划）

---

## 9. 当前进度与暂停点（2026-11-11）

**已完成**，详见 §4.0.1、§4.0.2、§6：

1. 现场确认目标机系统版本、已加载 family / KPI 版本，以及 `IOSCSIParallelFamily` 运行时未加载。
2. 用 `ida/kc_storage_vtable_check.py` 完成 `IOBlockStorageDevice` vtable 与 MacKernelSDK 头的逐槽 ABI 核对（方法槽 268–293、保留槽 294–321、两套 Apple 子类交叉验证），**R1 已关闭**。
3. 确认 `IOPCIPrimaryMatch` 格式与 `0x00971000` 写法；确认 `OSBundleLibraries` 不应照抄 KC prelink plist 的历史版本。
4. 复核 FreeBSD / Linux 参考源码基线、许可与 MPI 可复用清单（§2、§3、§7）。
5. 拉取 Apple OSS `IOSCSIParallelFamily` 到 `reference/`（§2 固定 commit），完成路线 B 静态复核（§4.0.2）：KPI 符号集、版本对应、官方/第三方 SAS 依赖先例、SPI vtable 逐槽核对；**路线 B 改判为「技术可行但不推荐作首版」**，其 KPI 废弃风险记为 R14。
6. SAS3008 已接入：IORegistry 确认 `0x1000:0x0097` / `pciclass,010700` / `pci1000,97`。BAR0 为 I/O；BAR1/BAR2 组成 **64-bit** MMIO（配置偏移 `0x14`，64 KB），BAR3/BAR4 为 64-bit MMIO（`0x1c`，256 KB）。物理地址随启动分配：macOS 侧 `0xFC440000` / `0xFC400000`，Debian 侧 `0xfc540000` / `0xfc500000`，代码不得硬编码。`IOPCIPrimaryMatch=0x00971000` 命中，系统正常启动且 macOS 侧原本无驱动接管。
7. 建立 kext 工程（`MPR/`、`MPR.xcodeproj/`、`tools/build-noxcode.sh`）与 `Info.plist`，写空壳 `MPRController::probe/start`（PCI 使能、BAR1 映射、7 个 MPI2 系统接口寄存器只读 dump 到 IORegistry）。目标机只有 Command Line Tools，构建走无 Xcode 链路：`clang++` + MacKernelSDK + `ld -kext` + ad-hoc 签名。
8. 按 §5 / `AGENTS.md` 只写内部 SATA 盘的 ESP：`EFI/OC/Kexts/MPR.kext` + `Kernel > Add` 条目；改前同目录备份 `config.plist`、`plutil -lint`、确认唯一差异为新增条目；重启后 `com.ling.MPR (0.1.0)` 加载、`ioreg -r -c MPRController` 出现且寄存器可读。**M0 的「最小 kext 加载并匹配」验收项通过**。
9. 在 Debian 救援环境完成 SAS3008 与测试盘的只读核查：PCI 配置空间 / MSI-X、固件与 IOC 队列几何、SAS 拓扑、盘身份与分区布局。固件镜像为 **IR**（`IOCCapabilities` 含 `INTEGRATED_RAID`），但 `Protocol=(Initiator)` 且未枚举 IR 卷，当前直通运行；测试盘为 **SATA SSD 走 STP**，不是 SAS SSP。

**当前状态**：空壳 kext 已在 macOS 真机加载并命中 PCI 匹配字典，BAR1 可读；它不写硬件寄存器、不发 DMA、不注册服务。SAS3008 已接 1 块测试盘，Debian 下由 `mpt3sas` 枚举为 `/dev/sdb`（`PCCOOLER SSD T200 240GB`、GPT/Windows 布局、**无重要数据**），该盘**允许破坏性写入**；macOS 侧尚未由本驱动枚举。首版验收范围限定 **STP/SATA 直连**，SAS SSP / expander 当前无测试条件、不列入退出条件。按 §6 的 M0 退出条件，空 kext 卸载验证仍未完成；ESP 上保留 `config.plist.bak_mpr-m0_20261111` 备份，回滚路径未实测。

### 9.1 部署前置条件：内置 SATA 盘被误报为 External（修复已写入，待重启验证）

**现象**：系统盘 `SanDisk SD8SNAT-128G-1006`（128.0 GB、SATA）被 `diskutil info` 报为 `Device Location: External`，不满足部署目标三要素；M1-a 的 ESP 写入与重启验证因此暂停。

**根因**：该盘挂在 AMD FCH SATA 控制器 `pci1022,7901`（`43:00.0`）的 `PRT7@7` 端口，控制器由 `AMDSata.kext`（RehabMan `SATA-unsupported` 注入器）匹配 `AppleIntelPchSeriesAHCI`。控制器位于多级 PCI 桥之后，IODeviceTree 中缺 `built-in` 属性；`AppleAHCI::start` 据此把控制器与端口的 `AHCI-Built-In` 置为 `No`，沿 `IOAHCIDevice` → `AppleAHCIDiskDriver` → `IOAHCIBlockStorageDevice` 把 `Physical Interconnect Location` 设为 `External`，最终 `IOMediaIcon=External.icns`。判据由 BootKC 中 `AppleAHCI::start` 的字符串链 `built-in | IOPCIEjectable | sata-express | AHCI-Ejectable | AHCI-Built-In` 确认。

**修复**：在目标盘 ESP 的 `config.plist` 中，对 `DeviceProperties > Add` 的 `PciRoot(0x0)/Pci(0x1,0x2)/Pci(0x0,0x0)/Pci(0x9,0x0)/Pci(0x0,0x0)` 注入 `built-in`（Data `01000000`）。路径由实时 `DeviceTreePath` 生成，目标盘按设备名 + 容量 + 总线协议动态定位，显式排除 USB 安装/救援盘的 EFI；不使用设备编号判定。

**校验**：同目录备份 `config.plist.bak_sata_internal_20261111`；`diff` 唯一差异为上述 6 行插入；`plutil -lint OK`；CR=0；SHA-256 改前 `73a1ea3de30b26ef0f225068a60224314ab656b47f9b77015b3ea6c6f74b8e7b`、改后 `71b96d2bf1415aa8e04a0b103181055f03b042dce6b0d84e7f5dd15bb857954f`。ESP 已卸载。

**待验证**：重启后须同时满足名称 `SanDisk SD8SNAT-128G-1006`、`Device Location: Internal`、`128.0 GB`，并复核 `AHCI-Built-In=Yes` / `Physical Interconnect Location=Internal`。任一不符即停止，不得部署 M1-a、不得写 ESP；回滚只需用备份恢复 `config.plist`。

**暂停点：以下步骤需要新的部署/重启授权，按用户要求在此停止。**

1. 反编译 `AppleVirtIOBlockStorageDevice::start` / `doAsyncReadWrite` 作为 `MPRBlockDev` 实现模板；从 FreeBSD `mpi/*.h` 裁剪 MPI2.5 结构落到 `MPI/`，保留版权头。
2. 进入 M1：MMIO dump、diag reset → READY、IOC Facts、队列几何、IOC Init；用 Debian 侧队列几何与固件版本作对照，必要时读 Manufacturing Page0 确认固件镜像字符串。每轮改 kext 都需要部署与重启授权，并重复 §5 的 ESP 备份 / `plutil -lint` / 唯一差异校验。
3. 按 STP/SATA 路径推进 M2–M5：目标表与 nub、只读回读、WRITE / SYNCHRONIZE CACHE / UNMAP 回读校验、热插拔与恢复；内部系统盘与 USB 介质保持只读。
4. （仅当决定评估路线 B，不需要 SAS3008）用 MacKernelSDK 编译并真机加载最小 `IOSCSIParallelInterfaceController` 子类 kext，`OSBundleLibraries` 写 `com.apple.iokit.IOSCSIParallelFamily = 1.2.0`，验证 KPI 符号解析、family 被依赖拉起与子类 vtable 不错位；仍需 kext 部署/重启授权。

**恢复条件**：用户授权后续 kext 部署与必要的重启。测试盘已接入并允许读写，不再是前置条件；在此之前不再推进。
