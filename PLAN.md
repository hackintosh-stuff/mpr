# PLAN — 将 LSI/Broadcom Fusion-MPT SAS3 驱动移植到 macOS

状态：草案 v0.9（2026-11-11；源码版本 0.16.3 已部署到 ESP 并重启加载，命名已去除轮次编号；M4 的写后回读、UNMAP、ABORT→LUN→TARGET reset 级联与 resetAndReinitialize 已完成实机注入验证；D8–D11 已实现并做过读/写/UNMAP 回归，D12 已补 PM、watchdog 与热插拔事件/媒体状态，自动重建因事件自激暂缓，异步命令池/chain 仍未实现；reset 后分区方案重扫描已修复，24 h 混合 I/O 未做，最近部署/加载记录见 §9.1）
目标硬件：Broadcom Ltd. / Avago Tech (LSI) SAS3008，PCI `0x1000:0x0097`，MPI2.5 / 12 Gb/s SAS3 HBA；已有 1 块获准用于破坏性测试的 SATA SSD，2026-11-11 已接回 SAS3008，Debian 下为 `/dev/sdd`、macOS 下为 `disk5`，均按型号/序列号/容量现场核对（状态见 §9.1），不得沿用历史设备号推断当前身份。
参考驱动：FreeBSD `mpr`、Linux `mpt3sas`、Apple OSS `IOSCSIParallelFamily`（架构与 ABI 参考）
目标平台：macOS Tahoe 26.7.2（Build `25G332`，`Darwin 25.6.0`，x86_64，MSI X570 测试机；已由现场 `uname -a` / `sw_vers` 确认），基于 `MacKernelSDK` 的 IOKit kext。BootKC 分析文档标注同为 26.7.2 / 25G332，KC 内部分 kext 构建于 26.7.1 / 25G325 属构建批次差异，不是系统版本冲突；系统升级后需重取 KC。

本文件记录目标设计、实现差距与验收状态，不包含实现代码。§4–§6 的目标约束不代表当前代码已满足；§9 区分源码核查结果与已有实机记录，本次文档修订未重新核查目标机运行状态。参考源码引用以拉取到 `reference/` 的固定 commit 为准；内核侧事实来自 `C:\Users\ling\OneDrive\Documents\Agent\Hackintosh\ida\BootKernelExtensions.kc` 的 IDA 反编译/符号表（见 §4.0）。下文 `xxx.c:NNN` 形式的行号，FreeBSD 侧相对 `reference/freebsd/sys/dev/mpr/`，Linux 侧相对 `reference/linux/drivers/scsi/mpt3sas/`，SDK 侧相对 `MacKernelSDK/`。

---

## 1. 目标与范围

### 1.1 首版目标（M0–M5）

1. kext 通过 `IOPCIDevice` 匹配 SAS3008，完成 PCI 使能、BAR 映射、IOC 复位与初始化。
2. 建立 request / reply / reply-free 队列、命令生命周期与 SGL/chain 管理，能用中断及受控轮询收发 MPI 消息；当前原型的 64 KiB 单段暂存路径与目标架构的差距见 §4.3、§9.3。
3. 用 config page 扫描完成初始发现与复位后核对，用固件事件维护拓扑变化，把经 STP/SATA 接入的磁盘暴露为**原生** `IOMedia`（`diskutil list` 可见）。目标表、稳定设备身份与 nub 生命周期覆盖 STP/SATA；主动扫描不代替事件订阅、EVENT_ACK 和热插拔处理。SAS SSP / expander 当前无测试条件，不列入首版实机验收。
4. 支持 READ/WRITE、SYNCHRONIZE CACHE、auto sense、超时与 TMF 恢复、热插拔；UNMAP 仅对已探测支持且通过限制校验的设备启用，不支持时准确报告并保持正常块 I/O。
5. 首版通过 IORegistry 属性、内核日志及现有 `ioreg` 等工具提供基本诊断；独立 `IOUserClient` 与专用诊断程序列为 M6 可选扩展。

### 1.2 明确不做（首版）

- IR/RAID 卷、SES 机箱管理、NVMe/PCIe enclosure、Tri-Mode、固件升级、启动盘引导。
- Apple Silicon、dext/DriverKit、多队列高 IOPS 优化、中断合并、WarpDrive。
- 兼容 SAS2（`mps`/`0x0072`）与 MPI26/GEN35/SEA 分支（SAS3416 等）。
- 接入 `IOSCSIParallelInterfaceController` / macOS SCSI 应用层栈（路线 B 的静态接口与语义可行性已核查，最小子类链接/加载尚未验证；列为 M7 备选，见 §4.0.2、§4.2）。
- SAS SSP 直连盘、expander 下多盘与 SMP passthrough 的实机验收（当前无测试条件）；拓扑解析可保留 SSP `device_info` 分支，但不作为首版退出条件。

### 1.3 驱动形态

**IOKit 内核扩展（kext），不是 dext。** 依据见 §4.2。首版只暴露受支持的独立物理盘，不实现 IR/RAID 逻辑卷。现有实测固件具有 `INTEGRATED_RAID` 能力，但测试对象是未组成 IR 卷的直连 SATA 盘；不能将“带 IR 能力的固件”与“正在访问 IR 逻辑卷”混为一谈，也不能写成已有 IT 固件验收。正式实现须识别固件能力、卷及成员盘关系，排除 RAID 卷和被阵列占用的盘；无法确认独立物理盘身份时不得发布为可写介质。IT 固件与其它 IR 配置的兼容性需要分别验证。

---

## 2. 基线（可复现）

| 项目 | 内容 |
|---|---|
| FreeBSD 参考 | `freebsd/freebsd-src` `main`，commit `e56090afb68b6c036d6b54c7b66d762d98bd96d0`（2026-11-10）；`sys/dev/mpr/` 13 个文件 + `mpi/` 13 个文件 |
| Linux 参考 | `torvalds/linux` `master`，commit `b3dfbde62cf1981c4638939c159dfd087b7a72e9`（2026-11-10）；`drivers/scsi/mpt3sas/` 16 个文件 + `mpi/` 10 个文件 |
| 合计（FreeBSD + Linux） | 52 个文件，2,664,265 字节，逐文件按 GitHub Contents API 的 `size` 校验；固定 commit 的 `raw.githubusercontent.com` 下载 |
| MacKernelSDK | git 子模块 `3f750085caa17ec3a7880f11c11bf4f48cd6a164`（2026-09-23），基于 macOS 11 / Xcode 12.0 SDK，补充 `xnu-8019.80.24` 头文件 |
| Apple OSS `IOSCSIParallelFamily` | `apple-oss-distributions/IOSCSIParallelFamily` tag `IOSCSIParallelFamily-352` / commit `560f74ab0c6f5d9aa6c99675d8e95986b202144d`（2026-04-21 推送，获取时与 `main` HEAD 相同）；codeload tarball 261,914 B、86 个文件；落在 `reference/IOSCSIParallelFamily/`，只读参考、不参与构建 |
| 内核缓存反编译 | `ida/BootKernelExtensions.kc` 67,600,384 B（MD5 `b9da5eafcd90e439ae7c55c1ca8ddf33`）、`BootKernelExtensions.kc.i64`、survey 缓存 `.ida-mcp-survey.json`、`ionvme-symbol-analysis.md`；KC 基址 `0xffffff8000200000`，209 个 bundle / 2396 段 / 117,470 函数（111,780 已命名）/ 136,888 字符串。离线核对脚本 `ida/kc_storage_vtable_check.py`（路线 A）、`ida/kc_spi_routeb_check.py`（路线 B） |
| 许可 | FreeBSD `mpr`：BSD-2-Clause（Yahoo!/LSI/Avago/Broadcom 版权，`mpr.c:1-12`）；Linux `mpt3sas`：GPL-2.0（`mpt3sas_base.c:10-12`）；Apple OSS `IOSCSIParallelFamily`：APSL 2.0（`APPLE_LICENSE`） |
| 版本控制 | `MacKernelSDK` 是子模块；`reference/` 被 `.gitignore` 的 `/reference/` 忽略，**不入库**；`ida/` 位于工作区根目录，同样不入库、不参与构建。参考源码按本节固定 commit 复现；KC 的 MD5 标识已分析样本，相关本地脚本与数据库需同时保留 |

目录布局：

```
mpr/
  PLAN.md
  AGENTS.md
  MPR/                               (Controller、BlockDev、Info.plist、MPI/mpi2_min.h)
  MPR.xcodeproj/                     (工程入口)
  tools/build-noxcode.sh             (Command Line Tools 构建入口)
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
| IOC Init 参数 | `mpr.c:1231-1289` `mpr_send_iocinit`（frame size、两队列深度、DMA 基址及 reply/sense 高地址、`HostPageSize=4K`）；Linux `mpt3sas_base.c:7537,8203-8355` |
| SGL 构造 | `mpr.c:3388-3751` `mpr_add_chain` / `mpr_push_ieee_sge` / `mpr_add_dmaseg`；Linux `mpt3sas_base.c:2655,2716,2857` |
| 事件解码 | `mpr_sas_lsi.c:200-800` 固件事件 switch；Linux `mpt3sas_scsih.c:11943` |
| config page 请求 | `mpr.c:3926` `mpr_read_config_page` + `mpr_config.c`；Linux `mpt3sas_config.c:296` |
| 映射/缺盘算法 | `mpr_mapping.c`（DPM Page0、persistent mapping、missing count、IR/拓扑事件） |

### 3.2 初始化主链（以 FreeBSD 为主，Linux 校验）

| 阶段 | FreeBSD | Linux | 产出 |
|---|---|---|---|
| PCI 使能 | `mpr_pci.c:215-277` | `mpt3sas_base.c:3634-3685`, `_scsih_probe` `scsih.c:13356` | 寻找实际 memory BAR、启用 bus master、设置 DMA 约束；本机 BAR0 为 I/O，首个 MMIO 为 BAR1/BAR2（配置偏移 `0x14`），不得按“BAR0 MMIO”硬编码 |
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

分析对象 `BootKernelExtensions.kc`（macOS 26.7.x Boot KC），IDA 会话 `5c67bd08`。`ionvme-symbol-analysis.md` 记录该次采样中 IONVMeFamily `__text` 起点 `0xffffff80026bd000` 与运行时 `kextstat` 一致；这只证明该样本与该次启动的地址对应，不能据此假定所有启动均无 slide。运行地址须按当次系统重新核对，以下地址仅定位该 KC 样本；`.ida-mcp-survey.json` 给出 209 个 bundle / 2396 段 / 117,470 函数（111,780 命名）/ 136,888 字符串。本次针对存储栈复核得到：

1. **系统侧 family 确实存在，且都在 BootKC 里**（survey 段名，非推测）：`com.apple.iokit.IOStorageFamily`、`com.apple.iokit.IOPCIFamily`、`com.apple.iokit.IOSCSIArchitectureModelFamily`、`com.apple.iokit.IOSCSIParallelFamily`、`com.apple.iokit.IOSCSIBlockCommandsDevice` / `IOSCSIMultimediaCommandsDevice` / `IOSCSIReducedBlockCommandsDevice`、`com.apple.iokit.SCSITaskUserClient`、`com.apple.iokit.IOAHCIBlockStorage`、`com.apple.iokit.IONVMeFamily`、`com.apple.driver.AppleVirtIO`。KC 里还有对应的 KPI bundle 标识字符串（`0xffffff800020dcc8` 起：`...IOPCIFamily`、`...IOSCSIArchitectureModelFamily`、`...IOSCSIParallelFamily`、`...IOStorageFamily`）。**因此"目标系统没有 SCSI/存储栈"是错的**；路线 B 的工具链与语义可行性见 §4.0.2、§4.2。
2. **路线 A 的 ABI 基础可实证**：`__ZTV20IOBlockStorageDevice` 在 `com.apple.iokit.IOStorageFamily:__const` @ `0xffffff8002b70450`；`IOBlockStorageDevice` 的方法实体在 `IOStorageFamily:__text`（`init`、`setProperties`、`getWriteCacheState`、`setWriteCacheState`、`doLockUnlockMedia`、`reportLockability`、`reportPollRequirements`、`requestIdle`、`doDiscard`、`doUnmap`、`doSetPriority`、`doSynchronizeCache`、`doSynchronize`、`doGetProvisionStatus`，以及 `_RESERVEDIOBlockStorageDevice4..22+` 保留槽）。相关方法及保留槽的完整核查见 §4.0.1，不能仅凭 vtable 前 256 字节判断派生存储接口的槽位一致性。
3. **Apple 自己有多套 `IOBlockStorageDevice` 子类可作模板**：
   - `AppleVirtIOBlockStorageDevice`（`com.apple.driver.AppleVirtIO:__text`，`__ZTV29AppleVirtIOBlockStorageDevice` @ `0xffffff8001f0d1f0`）：实现全部 14 个纯虚（`doAsyncReadWrite`/`doEjectMedia`/`doFormatMedia`/`doGetFormatCapacities`/各 `get*String`/各 `report*`），以及 `doUnmap`、`doSynchronizeCache`、`doSetPriority`、`doLockUnlockMedia`、`reportPollRequirements`，并有自己的 `start`/`stop`/`willTerminate`/`queueEventHandler`。**这是与本项目最像的「自研 transport + 块设备 nub」模板，优先级高于 NVMe/AHCI**。
   - `IONVMeBlockStorageDevice`（`IONVMeFamily:__text`）：`start`、`GetDeviceProperties`、`doAsyncReadWrite`、`CompleteAsyncRequest`、`QueueCreate/Delete`、`QueueSubmitRequest`/`QueueCompleteRequest`、`PowerDownHandler`、`SetNVMeProperties`、`ValidateEntitlement`、`doUnmap`/`doUnmapNVMe`、`getSerialNumberString` 等。
   - `IOAHCIBlockStorageDevice`（`IOAHCIBlockStorage:__text`）、`IOHDIXHDDrive*`、`AppleXsanDevice` 等同形态。
4. **反编译实证（`IONVMeBlockStorageDevice::doUnmap` @ `0xffffff80026d0396`）**：先 `IOService::isInactive` 检查 → `kalloc_shared_data_external(4096,1)` → `IOMemoryDescriptor::withAddress(buf,0x1000,2)` → `md->prepare()` → 由 `this+0x90` 的 controller 走 `vtable[+2216]` 取 request（GetRequestFromPool）→ 挂 MD → 构造并提交 → 完成后归还池；临界区用 `AppleNVMeWorkLoop::Lock/SleepGate/Unlock/WakeupGate` 串行化。与 `ionvme-symbol-analysis.md` §3 归纳的同步命令流程（GetRequestFromPool → ResetForNewRequest → prepare → GenerateIOVMSegments → BuildCommand → ProcessSyncNVMeRequest → complete → ReturnRequestToPool）一致。**这是 macOS 侧「命令对象 + IOMemoryDescriptor + 同步/异步完成」的可核对模板**，也是 §4.3 `MPRCommand` 设计的现实参照。
5. **`IOSCSIParallelInterfaceController` 的实体实现确实在 `com.apple.iokit.IOSCSIParallelFamily:__text`**：`GetSCSIParallelTask`、`ExecuteParallelTask`、`CompleteAbortTask*`、`ServiceInterrupt`、`FilterInterrupt`、`EnableInterrupt`、`InitializeDeviceList`、`DestroyTargetForID` 等符号齐全。系统有实现；SDK 侧头文件与内核 KPI 符号集均齐备（§4.0.2、§4.1）。
6. **`OSBundleLibraries` 的真实样例**：KC 的 prelink Info.plist 中 Apple kext 声明 `com.apple.iokit.IOPCIFamily = 1.1`、`com.apple.kpi.iokit/libkern/mach = 8.0/9.0` 等；这些是 Apple 内部 kext 的历史版本，只作为「依赖键长什么样」的参考，第三方 kext 应依据所用 API、导出符号集及提供者兼容版本区间声明最低依赖版本；`kextstat` 只核实当前提供者版本，不能单独决定最低依赖。
7. **资料边界**：`ionvme-symbol-analysis.md` §4/§5 的 Lilu `solveSymbol` / Samsung 990 EVO 结论与本项目无关且已在别处被推翻，本计划只取其 §1–§3 的寄存器/命令/反编译事实；KC 里多数符号是 names 而非 IDA 已识别函数（`lookup_funcs` 对多数 vtable 槽返回 "Not a function"，部分 `decompile` 也因此失败），故 ABI 核对以**符号表 + vtable 原始字节**为准，不依赖 IDA 的函数识别。

#### 4.0.1 无硬件预核查结果（2026-11-10）

以下项目均在不接入 SAS3008 的条件下完成，可直接作为 M0 的输入：

| 核查项 | 方法 | 结论 |
|---|---|---|
| 目标机系统版本 | `ssh ling@192.168.50.204` → `uname -a` / `sw_vers` | `Darwin 25.6.0` / `macOS 26.7.2` / `25G332`，与 KC 分析文档一致；`Darwin 25.6.0` 与 26.7.x 是同一系统 |
| family / KPI 版本 | 现场 `kextstat` | `com.apple.iokit.IOPCIFamily 2.9`、`IOStorageFamily 2.1`、`IOSCSIArchitectureModelFamily 545.100.10`、`IOSCSIBlockCommandsDevice 545.100.10`、`SCSITaskUserClient 545.100.10`；`com.apple.kpi.{iokit,libkern,mach,bsd} 25.6.0`。`OSBundleLibraries` 按所用 API 与实际兼容区间选择最低要求版本，不能直接照抄当前运行版本，也不能仅因样例数字较旧就否定它；现有路线 A 声明 `IOStorageFamily = 1.0` |
| `IOSCSIParallelFamily` 运行时状态 | `kextstat \| grep -c IOSCSIParallelFamily` | **该次查询结果为 0**。该 family 的实现存在于 BootKC；这不构成“系统缺少 family”的证据。路线 B 的依赖解析与运行可用性仍须通过最小子类加载验证（§4.0.2） |
| **vtable ABI 一致性（R1 核心）** | `ida/kc_storage_vtable_check.py` 离线解析 KC fileset symtab，对 vtable 槽做 chained-fixup 解码（自检：槽 2 raw `0x40000002a6c504` → `0xffffff8002b6c504`），再与 SDK 头 29 个 `virtual` 声明逐项比对 | **完全一致**。`IOBlockStorageDevice` 自有方法从槽 268 起连续排列：`doEjectMedia`268 / `doFormatMedia`269 / `doGetFormatCapacities`270 / `doLockUnlockMedia`271 / `doSynchronizeCache`272 / `getVendorString`273 … `reportWriteProtection`284 / `getWriteCacheState`285 / `setWriteCacheState`286 / **`doAsyncReadWrite`287** / `requestIdle`288 / `doDiscard`289 / `doUnmap`290 / `doSetPriority`291 / `doSynchronize`292 / `doGetProvisionStatus`293；覆写的继承方法 `init`/`getProperty`/`setProperties` 在槽 75/87/98。槽 294–321 = `_RESERVEDIOBlockStorageDevice4..31`（28 个），与 SDK 头 `OSMetaClassDeclareReservedUnused(...,4..31)` 及 `Used(...,0..3)` 完全对应。`AppleVirtIOBlockStorageDevice` 与 `IONVMeBlockStorageDevice` 两套子类在相同槽索引覆写同名方法（268/270/277/287/290 交叉验证通过）。**结论：这组 SDK 头与已分析 KC 的相关 vtable 槽位静态一致；后续已有路线 A 加载并生成 `IOMedia` 的实机记录（§9.2）。此结论不外推到其它系统版本，也不代替路线 B 的加载验证** |
| PCI 匹配键 | `MacKernelSDK/Headers/IOKit/pci/IOPCIDevice.h:324-356` | `IOPCIPrimaryMatch` 匹配 config 0x00 的 vendor+device，格式 `0x<device><vendor>`（示例 `0x00261011` = dev 0x0026 / vendor 0x1011），可带 `&mask`。SAS3008 用 `0x00971000` 正确 |

> 复现：`py -3 C:\Users\ling\OneDrive\Documents\Agent\Hackintosh\ida\kc_storage_vtable_check.py`（脚本位于工作区 `ida/`，与 `reference/` 一样不入库）。系统升级后先重取对应 KC，再重跑脚本核查，并完成相关加载/运行回归。

#### 4.0.2 路线 B 复核（2026-11-10，Apple OSS 源码 + BootKC）

`reference/IOSCSIParallelFamily/`（§2 的固定 commit）与同一份 BootKC 交叉核对的结论：**路线 B 的头文件、导出符号、vtable 与 SAS 适配语义具备静态依据**；最小子类的链接、依赖解析与实机加载仍未验证。§4.2 将其作为后续备选，首版继续路线 A。

| 核查项 | 实证 | 结论 |
|---|---|---|
| SDK 侧头文件 | `MacKernelSDK/Headers/IOKit/scsi/spi/IOSCSIParallelInterfaceController.h` 存在；36 个 `virtual` 声明与 Apple OSS 头逐项同序 | 编译所需头文件齐备 |
| KPI 符号集 | 仓库 `config/IOSCSIParallelFamily.exports` 导出 117 个符号（含 `__ZTV33IOSCSIParallelInterfaceController`、`gMetaClass`/`metaClass`/`MetaClass::alloc`、`GetSCSIParallelTask`、`ExecuteParallelTask`、`CompleteParallelTask`、`CreateTargetForID`/`DestroyTargetForID`），`config/generate-symbols-plist.sh` 写入 `__LINKINFO,__symbolsets`；KC 中该 family 的 `__LINKINFO` 内 symbolsets.plist 为 `CFBundleVersion=3.0.0` / `OSBundleCompatibleVersion=1.2.0`，**117/117 导出符号都在 KC 符号表中** | 它是正式 KPI，第三方 kext 可声明依赖并解析符号 |
| 版本对应 | KC prelink plist：`/System/Library/Extensions/IOSCSIParallelFamily.kext`，3.0.0 / 1.2.0，`DTSDKName=macosx26.7.internal`、`DTPlatformVersion=26.7.1`、`LSMinimumSystemVersion=26.7`；依赖 `IOSCSIArchitectureModelFamily 1.0.0`、`IOPCIFamily 2.7`、KPI 9.0.0 | 记录了目标 KC 中的 family 版本与构建标签；接口对应需结合导出符号/vtable 核查，版本标签本身不证明来自同一源码构建 |
| 依赖声明样例 | 仓库 `TestTools/AppleSCSIHBAEmulator/Info.plist` 是继承该类的官方 kext，声明 `com.apple.iokit.IOSCSIParallelFamily = 1.2.0`；KC 内第三方 `com.promise.driver.stex (PromiseSTEX)` 同样声明 1.2.0 且 `Physical Interconnect = SAS` | 已有样例使用 **1.2.0**；本项目按所需 API 和兼容区间验证，不能仅按其它驱动的 `1.3.7` 复制依赖 |
| Apple 自己的 SAS 驱动 | BootKC 内 `AppleLSIFusionMPT.kext` 的 `AppleLSIFusionSAS` personality：`IONameMatch = LSILogic,sas / pci1000,50 / pci1000,56 / pci1000,58`、`Physical Interconnect = SAS`、依赖该 family | 「强制 SPI 语义与 SAS3 不匹配」不成立；但**不含 `pci1000,97`**，Apple 驱动不会匹配 SAS3008，且老 MPT 固件与 MPI2.5 命令集不同，不能靠改 Info.plist 接管 |
| 语义裁剪 | `DoesHBASupportSCSIParallelFeature()` 可对所有 feature 返回 false（官方 emulator 即如此）；`SCSIDeviceIdentifier` 为 `UInt64`（`IOKit/scsi/SCSITask.h:56`）；`DoesHBAPerformDeviceManagement()` 返回 true 时基类不做 0..max 线性扫描，由驱动按拓扑事件自行 `CreateTargetForID()`（`IOSCSIParallelInterfaceController.cpp:359-376,1710-1784`） | 可对接 SAS3 devhandle / expander，不受 16 槽并行总线限制 |
| vtable ABI | `__ZTV33IOSCSIParallelInterfaceController` @ `0xffffff8002ae6168`：槽 268–276 = 9 个纯虚，277–279 = `Used(1..3)` 的 `DoesHBAPerformAutoSense`/`ReportHBAConstraints`/`DoesHBASupportMultiPathing`，280–284 = `Unused(4..8)`，295–305 = `Used(9..12)`/`Unused(13..15)`；与 SDK 头、OSS 头声明一致（SDK 头多出的 `Unused(3)` 来自 `< 10.7` 的 `#else` 分支） | 所核查槽位静态一致；最小子类链接/加载仍待验证，不能标记整条路线的 ABI 风险已关闭 |
| 废弃状态 | OSS 头 `141` 行 `__kpi_deprecated("Use IOUserSCSIParallelInterfaceController with DriverKit, instead")`；`MacKernelSDK/Headers/sys/cdefs.h:208` 将其展开为 `__deprecated_msg`（警告，非错误）；仓库含 `SCSIControllerDriverKit/` 与 `DriverKitKernelSupport/` | 26.7.2 仍可用，但长期方向是 dext，未来系统可能移除 |

> 复现：`py -3 C:\Users\ling\OneDrive\Documents\Agent\Hackintosh\ida\kc_spi_routeb_check.py`（脚本位于工作区 `ida/`，不入库）。系统升级后先重取对应 KC，再与 `kc_storage_vtable_check.py` 一并重跑，并完成相关加载/运行回归。

仍未实证的环节（仅在选择评估路线 B 后开展）：① 用现有 Command Line Tools + MacKernelSDK 的 `ld -kext` 链路构建最小子类，检查驱动内部符号已定义及预期未解析的内核 KPI；链接成功不等于内核能解析全部依赖。② 依照本仓库 ESP-only 约束，经内部 SATA ESP 的 OpenCore `Kernel > Add` 注入并在已授权范围内重启验证（`OSBundleLibraries` 以 `1.2.0` 为待验证最低依赖），确认相关 family 依赖解析、子类加载与 vtable 行为。不得改走系统卷安装或 AuxKC 重建；`kmutil showloaded` 可用于只读查询。走 B 还需自研 `SCSIParallelTask` ↔ `Mpi2SCSIIORequest_t`、devhandle ↔ UInt64 target ID、TMF 与拓扑事件映射；M1/M2 的 IOC、队列、DMA 和事件管理仍需实现；可复用的 SCSI 上层能力与适配成本在路线 B 评估时单独核算。

### 4.1 MacKernelSDK 提供什么

存在（`MacKernelSDK/Headers/`）：`IOKit/pci/IOPCIDevice.h`（config 读写、`findPCICapability`、BAR 映射）、`IOKit/IOBufferMemoryDescriptor.h`、`IOKit/IODMACommand.h`（`gen64IOVMSegments`、`kIODMACommandOutputLittle64`）、`IOKit/IOMemoryDescriptor.h`（`IOMemoryMap`）、`IOWorkLoop`/`IOCommandGate`/`IOInterruptEventSource`/`IOFilterInterruptEventSource`/`IOTimerEventSource`/`IOCommandPool`/`IOLocks`、libkern 集合与字节序、`IOKit/storage/IOBlockStorageDevice.h` 及 `IOStorage`/`IOMedia`/`IOBlockStorageDriver`、`IOKit/storage/IOStorageProtocolCharacteristics.h`（`Physical Interconnect = "SAS"`）、以及 `IOKit/scsi/` 下的 `SCSITask.h`、`IOSCSIProtocolInterface.h`、`IOSCSIPeripheralDeviceNub.h`、各 `SCSICmds_*.h`（CDB 定义可用），还有 `IOKit/scsi/spi/IOSCSIParallelInterfaceController.h`（路线 B 基类，36 个 `virtual` 声明与 Apple OSS 头同序，见 §4.0.2）。

`IOSCSIParallelInterfaceController` 的头文件已在 SDK 中，不需要另造伞头。路线 B 的 `Info.plist` 可参考官方示例声明 `com.apple.iokit.IOSCSIParallelFamily = 1.2.0`，并核对实际使用的其它依赖；内核按导出符号集解析依赖，是否成功仍须用最小子类加载验证。Apple OSS 仓库还提供官方 HBA 子类样例 `TestTools/AppleSCSIHBAEmulator/`（§4.0.2）。

不存在：SAS/SSP/SMP/expander 框架头、`IOSCSIArchitectureModelFamily` 的伞头、`IOSCSIBlockStorageDevice.h`、PCIDriverKit 的 `.iig` 依赖链、xcconfig/CI、arm64 `libkmod`、kernel collection/签名文档。DriverKit 侧的 `SCSIControllerDriverKit`/`IOUserSCSIParallelInterfaceController` 不在 MacKernelSDK 中，只出现在 Apple OSS 仓库。SDK 中 `com.apple.iokit.IOSCSIArchitectureModelFamily` 只作为字符串常量出现（`IOKit/scsi/IOSCSIProtocolInterface.h:166`）。**注意：这里说的是「SDK 缺」，不是「系统缺」**——系统侧这些 family 的实体与 KPI 符号集见 §4.0、§4.0.2。

### 4.2 三条路线对比与决策

| 路线 | 做法 | 判定 |
|---|---|---|
| **A（采纳）** | 自研 `IOService`（provider=`IOPCIDevice`）实现 HBA；每块盘实例化 `IOBlockStorageDevice` 子类 nub，由系统 `IOBlockStorageDriver` 创建 `IOMedia`/`IOMediaBSDClient` | 静态 ABI 核查与单盘只读实机链路已有证据（§4.0、§9.2）；块设备状态、错误传播与恢复仍由本驱动负责。当前选用此路线，不把 14 个纯虚方法实现完等同于存储契约完整 |
| B | 继承 `IOKit/scsi/spi/IOSCSIParallelInterfaceController`，复用 macOS SCSI 应用层栈 | 静态可行性已有依据，最小子类链接/加载未验证（§4.0.2）。它依赖正式但已废弃的 SPI KPI，并增加 `SCSIParallelTask` ↔ MPI 适配；现有记录中 `IOSCSIArchitectureModelFamily` 已加载，只有 `IOSCSIParallelFamily` 未加载，不能称为“两个未加载的私有 family”。M1/M2 仍需自研，列为 M7 备选 |
| C | `IOService` + `IOUserClient` 诊断透传，用户态发 CDB | **只能做调试**，无法生成 `IOMedia`/`/dev/disk`，不能作为主路线；可作为 M6 的诊断通道 |

决策：**A**。SDK 的 SCSI 头文件用于 CDB、sense 与任务状态等定义，不作为当前继承体系。B 可能复用更多 SCSI 上层处理，但其额外适配、已废弃 KPI 与尚未完成的加载验证应在单独评估中比较；不以“私有 family”或“无法实现 SAS”作为否定理由。

> 路线 A 的 family 存在性、指定 SDK/KC 的 vtable 核查以及最小 kext/单盘 nub 加载已有记录（§4.0.1、§9.2）。`OSBundleLibraries` 依实际 API 与提供者兼容区间选择；现有 `Info.plist` 的 `IOStorageFamily = 1.0` 与运行时 2.1 并不矛盾。系统升级后重新核查。路线 B 的静态核查不替代其独立的链接/加载验证。

### 4.3 目标分层与当前实现

下表是目标职责划分，不要求每项立即拆成独立文件。当前仓库主要由 `MPRController`、`MPRBlockDev` 与 `MPR/MPI/mpi2_min.h` 组成；不能把计划中的组件名当成已实现的类。

| 职责 | 目标设计 | 当前落点 / 差距 |
|---|---|---|
| Controller / MPI | PCI、MMIO、IOC 状态与消息结构 | `MPRController`、`MPI/mpi2_min.h` 已有原型 |
| Queue / Command | request/reply/free 队列、按 SMID 绑定的命令状态、保留管理 credit、回复帧回收 | 目前固定队列和共享完成槽；回收与生命周期缺口见 §9.3 |
| DMA / SGL | 准备与解除 DMA 映射、地址/边界校验、IEEE SGL/chain | 目前低 4 GiB 物理连续池与 64 KiB 单段暂存；`IODMACommand` 数据直映和 chain 池尚未实现 |
| Interrupt / Completion | filter 做有界状态判定，单一串行上下文消费 reply，按命令保存完成结果 | 已有 interrupt event source；轮询与 IRQ 消费仍缺统一同步 |
| Config / Discovery | Facts/config page、正确事件订阅与 ACK、稳定身份目标表、nub 增删 | 已能读取单盘 config；掩码极性错误、ACK 和完整拓扑生命周期待实现 |
| BlockDev | 块设备契约、CDB、完成回调、写缓存和 UNMAP 能力报告 | 已有 `IOBlockStorageDevice` 子类；当前同步串行 I/O，返回前在锁外调用 completion |
| Recovery / PM | 超时、TMF、复位、安全停止、重新关联同一设备、睡眠/唤醒 | 0.14.3 的 D1/D2/D3/D4/D5/D6/D7 修改已随加载生效，写与 UNMAP 成功路径过一次实机；`mpr_tmf_fail=1/2/3` 分别验证了 `LUN_RESET`、`TARGET_RESET` 与 `resetAndReinitialize`，reset 后盘重新枚举为 `disk6` 且读写正常；安全停止与 PM 未验收 |
| UserClient（M6） | 受限诊断、config/事件/寄存器导出 | 尚未实现，首版先用 IORegistry 与现有工具 |

目标中的 `MPRCommand` 绑定 completion、活动 SMID、请求类型、目标身份/拓扑代次、request/SGL/sense、DMA 映射、超时及完成状态。参考 FreeBSD `struct mpr_command`（`mprvar.h:242-291`）、Linux `struct scsiio_tracker`（`mpt3sas_base.h:884-892`）的职责及 §4.0 的 macOS 命令模型。正常生命周期为准备映射 → 提交 → 确认完成 → 解除映射 → 释放资源；超时不能跳过硬件停止访问的确认。即使首版保持单条数据 I/O，原请求、管理命令、事件和迟到完成也需要独立状态，不能共用一个“最后回复”槽。

### 4.4 OS 依赖映射表

| 概念 | FreeBSD `mpr` | Linux `mpt3sas` | macOS / MacKernelSDK |
|---|---|---|---|
| PCI 匹配与使能 | `mpr_pci.c:203-277` | `scsih.c:13356`, `base.c:3634-3685` | `IOService::probe/start`；`IOPCIPrimaryMatch` 按 `IOPCIDevice.h:324-341` 的 `<device><vendor>` 格式填 `0x00971000`（必要时带 mask），`setMemoryEnable`/`setBusMasterEnable` |
| MMIO | `bus_alloc_resource` + `bus_space` | `pci_ioremap` | `IOPCIDevice::mapDeviceMemoryWithRegister` → `IOMemoryMap::getVirtualAddress`，volatile 寄存器访问 |
| 配置空间 | `pci_read_config` | `pci_*_config_*` | `configRead32`、`findPCICapability(0x11)`（MSI-X） |
| 一致 DMA | `bus_dmamem_alloc` + tag | `dma_alloc_coherent`/`dma_pool` | 当前用 `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` 的低地址连续池，并校验覆盖范围、对齐和设备可见地址；仅“对齐”不保证物理或 IOVA 连续。采用 IOMMU 映射时必须使用映射后的设备地址 |
| 数据 DMA 映射 | `bus_dmamap_load_ccb` | `scsi_dma_map` + sg | `IODMACommand::withSpecification` + `gen64IOVMSegments` + `prepare`/`complete`/`synchronize` |
| 4 GiB 边界 | `BD_BOUNDARY(4GB)`、`LOWADDR_32BIT`（`mpr.c:1386-1388,1514-1516`） | `mpt3sas_check_same_4gb_region` | 根据字段宽度与池约束校验起止地址；低地址分配失败则有界重试、使用满足约束的暂存池或拒绝初始化，不能用“改成 32 位 SGE”修复超界地址 |
| 中断 | `bus_setup_intr(INTR_MPSAFE)` | `request_irq`/`pci_alloc_irq_vectors_affinity` | `IOFilterInterruptEventSource(..., intIndex)` + `IOWorkLoop`；filter 只做有界状态判定/通知，reply 的消费与回收在统一串行上下文进行 |
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

以下是实施要求；当前不满足项逐项列于 §9.3。不能以“编译通过”代替这些契约的实现与验收。

1. **DMA 地址与生命周期**：当前低 4 GiB 池是简化策略，不是 MPI 所有地址字段只能表示 32 位。`IOC Init` 的 request/free/post 基址是 64 位；reply/sense 的低地址与共享高 32 位字段必须相容（`mpi2_ioc.h:248-252`）。当前高字段写 0 时，相应缓冲必须全落在低 4 GiB。按各结构要求校验对齐、完整长度、边界与设备可访问性；后续 IEEE 数据 SGE 的地址位数参考 Linux SAS3 的 63 位约束并由映射结果验证。地址不合格时不能截断或改成 32 位 SGE 继续运行。所有映射和缓冲保留到硬件不再可能访问后才解除/释放。
2. **队列与回复帧回收**：深度受 IOC Facts 限制，按协议取整并为环形队列留空位；当前 free/post 深度 144、初始交给 IOC 的 reply frame 为 128，不能混作同一数量。验证 AddressReply 的类型、SMID、frame 地址、对齐及消息长度后复制所需数据，恰好一次归还对应 frame 到 Reply Free Queue，推进并回写 `ReplyFreeHostIndex`；消费 post descriptor 后更新 `ReplyPostHostIndex`。成功描述符没有 reply frame，不能伪造归还。验证应覆盖多轮绕环及超过初始池容量的 config、事件、错误和 TMF 回复。
3. **SGE 与 chain**：SCSI IO 使用 IEEE simple/chain；当前 CONFIG 采用协议允许的 legacy 64 位 Simple SGE。不能把“SAS3 的 SCSI 数据 SGL 用 IEEE”扩大成所有 MPI 消息都只允许 IEEE。SCSI `SGLOffset0=24`，sense 空间与命令生命周期绑定；当前 64 KiB 暂存不等于已实现任意散布聚集或 chain。目标 chain 池必须受 Facts 的 frame size、chain length/depth 等约束，并在资源不足时限流。
4. **中断与并发**：filter 只判定相关状态并触发后续处理，必要屏蔽按寄存器定义执行，不做阻塞工作。不能把整个 `HostInterruptStatus` 概括为 W1C；参考驱动通过排空 reply queue、更新 post index 完成回复队列确认，doorbell 状态另按其语义处理。IRQ action 与超时轮询必须共用同一队列消费者或同一同步机制，不得同时推进索引。管理命令、恢复和 nub 操作不能阻塞负责自身回复的 workloop；同步等待应释放所需 gate 或放入独立工作上下文。
5. **SMID 与完成绑定**：为数据、config、事件订阅/ACK、TMF 分别保留活动命令状态与资源，按 SMID、请求类型及活动生命周期分派，完成恰好一次。SMID 0 不作为普通请求；TMF 使用协议要求的 high-priority request descriptor（`0x06`）及可用 high-priority credit，不能复用 default descriptor（`0x08`）冒充。轮换 16 个编号不能防止迟到回复；软件代次也不能凭空给硬件回复添加代次。超时请求的 frame、数据、sense 与 SMID 在终止/复位及旧回复隔离得到确认前均不得复用；恢复失败必须保留故障状态并阻止后续提交。
6. **块设备完成与错误契约**：实现 `IOBlockStorageDevice` 纯虚接口只是起点。区分未接受请求的同步返回与已接受请求的 completion，锁外调用回调，并按实际已传输字节报告 `actualByteCount`。AddressReply 必须核对 IOCStatus、SCSIStatus、SCSIState 与 TransferCount；短传输不能假报整块成功。sense 仅在有效位及有效长度满足要求时解析，支持固定/描述符格式，并把结果传到上层完成状态。重试按错误类型和命令语义有界进行；介质错误、设备移除、恢复失败和结果未知的写不能统一无条件重试。
7. **容量与 CDB 范围**：首版数据路径覆盖 INQUIRY、TUR、READ CAPACITY(10/16)、READ/WRITE(10/16)、SYNCHRONIZE CACHE，必要时 REQUEST SENSE；先按已验证的直连盘 LUN 0 工作，REPORT LUNS/多 LUN、MODE SENSE 和 START STOP UNIT 按对应能力与 PM 需求补齐，不冒充现成功能。READ CAPACITY(16) 的 allocation length 在 CDB bytes 10–13，32 字节应写入 byte 13，byte 15 为 control；RC10 返回 `0xffffffff` 是需 RC16 的容量哨兵。容量、LBA、块计数、字节乘法均检查溢出；10 字节命令不能表示的范围必须走正确的 16 字节命令，未实现前拒绝暴露/处理该范围，不得截断。
8. **写入持久化与 UNMAP**：普通 WRITE 成功不代表介质已持久化。`IOStorageAttributes` 的 FUA 要由 CDB FUA 或有序同步缓存满足；`doSynchronize` 的范围、options/barrier 不能静默忽略，必要时可用更强的全盘同步满足契约。UNMAP 需先探测支持性与 Block Limits/Logical Block Provisioning 等限制，校验容量边界、粒度、描述符数及 32 位块计数字段，按限制拆分或明确拒绝。未证实能力时不启用。禁用 UNMAP 与切为只读是不同状态：命令不支持可保留普通读写；掉盘或恢复失败须使目标离线/按既定策略保护。只有设备承诺释放后读零时才以读零验收。
9. **事件与发现**：MPI `EventMasks` 中 **1=屏蔽、0=订阅**；先全置 1，再清除需要的事件位（FreeBSD `mpr.c:2769-2791`）。等订阅命令确认成功后再推进 PortEnable，分别核对对应 SMID/Function/IOCStatus。初始扫描与事件增量更新结合；需要 ACK 的事件按其 Event/EventContext 确认，未知但要求 ACK 的事件也不能只丢弃。先复制有界事件数据、归还 reply frame，再异步处理与 ACK，保留管理资源，避免耗尽或阻塞完成队列。事件计数或“已见位图”不证明不丢不重。
10. **目标身份与固件模式**：SAS Device Page0 的 `device_info` 用于区分 SSP/STP/SATA，flags 用于选择普通或 fast-path 请求；不能对全部目标硬编码 FAST_PATH。首版 IR/物理盘过滤遵循 §1.3。devhandle 仅是当前 IOC 拓扑的寻址句柄，SATA 派生 SAS address 也不必跨端口稳定；持久关联用经验证的 VPD 标识/序列号，必要时结合端口与人工确认。复位或热插拔后重新核对身份、容量、块大小及能力；原盘消失或身份改变时撤销/重建 nub，不能把旧 nub 自动绑定到第一个非零 handle。
11. **恢复、停止与电源**：先冻结新提交、同步在途命令及回调，再按协议停用/复位 IOC；在确认设备已停止 DMA、旧回复被隔离之后，才回收旧资源或重建队列。仅拆除中断事件源不等于停止 DMA；不得采用 `freeQueueMemory → diagReset`。重初始化必须检查每步返回值，重新发现同一目标并核对能力后才开放 I/O。无法证明 DMA 停止时，保留其仍可能访问的资源、维持故障状态并走受控停止路径。安全 `stop`、初始化失败清理和停机 quiesce 为首版必需；定时 IOC 健康检查纳入 M4，睡眠/唤醒策略及验证纳入 M5，不能作为可选日志功能略过。
12. **构建、行尾与部署**：当前构建入口是 macOS Command Line Tools 执行 `tools/build-noxcode.sh`，无需安装完整 Xcode；构建仅产生 bundle，不部署。上传文本须为 LF，遵守工作区上传前后检查及 `plutil -lint`；`.gitattributes` 已声明 `* text=auto eol=lf`，仍应检查实际传输文件。本驱动只安装到**经现场重新核对的内部 SATA 盘 ESP** 的 `EFI/OC/Kexts/MPR.kext`，由 OpenCore `Kernel > Add` 注入；不得写入 `/Library/Extensions`、其它系统卷或 USB EFI，也不把 AuxKC 重建当作本项目部署步骤。改 config 前在同目录备份，改后 `plutil -lint`/`ocvalidate` 并核对只有预期差异；若仅替换 kext，config 应保持不变。用 `kmutil showloaded`/`kextstat` 及 IORegistry 只读确认实际加载版本，不把 ESP 文件版本当运行版本。
13. **操作范围**：实机写入、部署、重启按会话中已授权的具体范围执行，已有授权不因历史段落重复索要；本文不授予新的实机操作权限。写测试仅针对重新核对身份的已授权测试盘；内部系统/数据卷、USB 安装/救援介质保持只读，内部 ESP 仅允许授权的驱动部署操作。预计超过 1 GB 的备份按工作区规则单独说明范围、大小与位置并取得许可；验收可流式计算校验和，无需自动制作大备份。串口调试默认关闭（`debug=0x100`、无 `serial=5`），遵守工作区按需启停要求。
14. **反编译资料边界**：`ida/` 的 KC/IDA 资料只用于核对接口、vtable、符号和依赖，不复制 Apple 实现，也不将 KC 地址或内部偏移写入驱动。系统升级后重取对应 KC 并重跑 §4.0.1 / §4.0.2 的离线核查，随后做相关加载/运行回归；静态脚本结果不替代运行验证。

---

## 6. 里程碑与验收

本节定义最终退出条件，进度以 §9 为准。早期功能跑通不豁免后来发现的基础缺口；M2 的队列与事件修正必须先于 M4 恢复/长时写入验收。

| 阶段 | 目标与交付 | 验收方式 | 退出条件 |
|---|---|---|---|
| **M0 环境** | 核对 PCI/固件/BAR、SDK/KC、依赖版本；Command Line Tools 构建；内部 ESP 注入最小 kext | 以 `ioreg`、`kmutil showloaded`/`kextstat` 与日志确认加载和匹配；保存可回退的内部 ESP 配置 | 正确构建、加载、匹配且回退步骤可执行；不以强行在线卸载代替回滚。完整驱动的在线 stop/卸载须等安全生命周期完成后另验 |
| **M1 寄存器与 IOC** | READY、IOC Facts、受 Facts 约束的 DMA 池与 IOC Init → OPERATIONAL | 先轮询收发 Manufacturing Page0，与参考字段核对；检查几何与失败清理 | 同步 config 请求成功、IOC 保持 OPERATIONAL；初始化失败不遗留活动 DMA 对已释放内存的访问 |
| **M2 中断、回复与事件** | 单中断源、可核对的实际中断模式、完整回复帧回收、按命令分派、正确 EventMasks/ACK、PortEnable | 持续至少 10 分钟，并产生远超初始 128 个 frame 的 AddressReply（config/事件/错误/管理回复），验证绕环与资源稳定；核对订阅位、ACK 和命令应答 | 无回复帧泄漏、误配完成和竞争消费；已订阅事件能够被处理。未观察到某事件时记录触发条件，不把掩码错误解释为固件特性 |
| **M3 单盘只读** | 识别独立 STP/SATA 盘、身份与容量验证、只读 nub、READ(10/16) 范围控制 | `diskutil list`/IORegistry 核对；同一有效 LBA 范围至少两次读取 **1 GiB** 并流式比较校验和，另做只读挂载；核对错误和短传输回传 | 两次完整读取与基准/彼此一致，无 panic、越界 DMA 或错误容量报告。`dd ... of=/dev/null bs=1m count=1024` 只证明是否报错，不能单独证明数据一致 |
| **M4 读写与恢复** | WRITE(10/16)、缓存同步/FUA、按能力启用 UNMAP、有效 sense、超时/TMF、IOC 故障检测与安全 reset | 在已授权测试盘做小范围写后回读/同步缓存，再扩至 24 h 混合读写；注入迟到回复、短传输、TMF 成败、复位失败；UNMAP 在专用测试范围内验证限制与设备声明语义 | 无错误成功、挂起、重复完成和跨设备续写；恢复成功只重新开放原目标，失败明确离线并保留必要资源；不支持 UNMAP 时普通 I/O 行为正确 |
| **M5 拓扑、热插拔与 PM** | 目标表与 nub 增删、handle 复用隔离、安全 stop/停机、明确的睡眠/唤醒策略；SSP/expander 不纳入首版实机覆盖 | STP/SATA 热插拔至少 10 轮，覆盖在途 I/O/换盘/复位；核对媒体变化通知。按目标机已授权操作流程验证睡眠/唤醒；在策略完成前不宣称支持系统睡眠 | 无幽灵盘、命令丢失、资源释放后访问或旧身份复用；明确失败路径及恢复结果。目标机物理电源唤醒依工作区规则执行 |
| **M6 诊断/性能（可选）** | 独立 `IOUserClient`/工具、统计导出、日志开关与性能基线 | 能导出关联同一次故障的 config、事件、命令与 IOC 状态；同机同盘比较性能 | 输出可支持定位具体失败阶段；基础生命周期、故障检测及停机安全不放入可选范围 |
| **M7 后续（不在首版）** | IR 卷、SES、固件升级、多队列、中断合并、启动盘、签名发布；或路线 B 最小子类验证、dext 评估 | 扩大范围时单独定义需求和验证矩阵 | 不阻塞既定首版 |

每个实机阶段先确定失败判据、测试盘身份、测试范围和回退方式。已有测试盘破坏性读写授权不代表可以对其它盘操作；文档修订不等于授权重启或执行破坏性测试。写后回读验证应包含确切字节数与失败状态，避免只看校验和而漏掉管道上游读取失败。睡眠/热拔等测试在基础 DMA/恢复缺口修复之后进行。

---

## 7. 合规与许可

1. FreeBSD `mpr` 的复用遵循对应文件许可，保留原版权声明与许可文本，并在 `NOTICE`/`LICENSE` 中列明来源。当前 `MPR/MPI/mpi2_min.h` 已保留 BSD 版权/许可头，但仓库尚无独立 `NOTICE`/`LICENSE`，发布前应补齐来源与许可清单；固定参考 commit 统一见 §2。
2. Linux `mpt3sas`（GPL-2.0）**只作为行为与协议规范的参考，不复制任何代码到驱动源码**。`reference/` 目录不参与构建、不入库；计划与文档中对它的引用属于事实描述。协议和可观察行为的设计依据应记录清楚；若将来拟引入 Linux 的具体实现，先核实相应许可要求，不能把改写措辞或重新录入代码当作消除许可义务的方法。
3. MPI 结构体定义以 FreeBSD `mpi/*.h` 为唯一代码来源；两份 `mpi2*.h` 的差异（Linux 版含 `mpi2_image.h`、字段更新）在 `MPI/` 目录下用注释记录，不混用。
4. `MacKernelSDK` 头文件仅用于编译，遵循其自身许可；不把 SDK 头文件拷进驱动源码树。
5. `ida/BootKernelExtensions.kc` 与 IDA 数据库属于 Apple 内核二进制的反编译资料，只用于核对 ABI / 接口契约与事实记录；不复制其代码、不把 KC 地址固化进驱动。`ionvme-symbol-analysis.md` 的 §4/§5（Lilu 符号 hook / Samsung 990 EVO）与本项目无关，不予采用。
6. `reference/IOSCSIParallelFamily/` 为 Apple OSS 源码，许可 **APSL 2.0**（不同于 FreeBSD BSD-2-Clause 与 Linux GPL-2.0）；只作头文件、ABI、依赖键与行为参考，不复制实现代码进驱动源码。若将来需要引用其中常量或结构定义，按 APSL 要求保留版权与许可声明，并在 `NOTICE`/`LICENSE` 中单列来源与 commit。

---

## 8. 风险与后续范围

| # | 风险 / 当前结论 | 影响 | 缓解 / 验证 |
|---|---|---|---|
| R1 | 路线 A 的指定 SDK/KC 静态 ABI 及单盘只读加载已有证据；不等于跨系统兼容保证 | 系统升级可能改变接口或加载行为 | 依据 §4.0.1 重取并核对 KC，再做运行回归；路线 B 加载仍待验证 |
| R2 | 当前固件带 IR 能力；独立物理盘、IR 卷和成员盘过滤尚未完整实现 | 误暴露卷/成员盘或错误宣称 IT 兼容 | 按 §1.3 明确对象类型与能力，不一概拒绝 IR 固件，也不把单盘结果推广到全部 IR 配置 |
| R3 | `intIndex=0`、edge 类型或 `HostMSIxVectors=1` 均不足以单独证明已用 MSI-X | 中断模式误判或丢中断 | 核对 provider interrupt specifier、消息中断标志和 PCI 能力实际 enable 状态；受控轮询与 IRQ 必须串行消费 |
| R4 | DMA 地址约束、映射方式及释放顺序尚不完备 | 错误 DMA、内存破坏 | 校验设备可见地址、完整范围和几何；修正先 free 后 reset/无 quiesce 的路径（§9.3），不靠地址截断回退 |
| R5 | SSP/expander 缺少实机覆盖 | 首版不能承诺其稳定性 | 首版明确为 STP/SATA 直连，保留识别分支不等于支持验收 |
| R6 | SMID/完成槽/迟到回复及 TMF 资源隔离缺口 | 误完成、重试已失控请求、复用活动资源 | 按 §5 第 5 条 建活动命令状态；恢复失败不解冻，先修复再注入故障 |
| R7 | PM、停机和 stop 生命周期未完成 | 睡眠、卸载或停机时丢盘/访问已释放资源 | M4/M5 完成 quiesce/恢复与 PM 策略；未验收前限定测试运行条件，不宣称睡眠支持 |
| R8 | kext 签名、依赖和 OpenCore 注入配置 | ESP 有文件但实际未加载或仍运行旧版 | ESP-only 部署、LF/plist/签名/配置检查，重启后独立查实际加载版本 |
| R9 | 第三方源码许可与来源清单不完整 | 发布合规问题 | 按 §7 保留文件许可并补清单；参考资料不参与构建 |
| R10 | 测试盘、内部系统盘与救援介质身份混淆 | 误写非目标设备 | 现场按型号/序列号/容量/分区身份重新定位；不复用历史 `diskN`，内部 ESP 仅承载授权部署 |
| R11 | UNMAP 未探测能力就默认开启，失败仅禁用 UNMAP | 设备异常或错误宣称已转只读 | 先探测并校验限制，再小范围验证；掉盘/恢复失败另走目标状态机；不预设本测试盘支持性已验证 |
| R12 | 同步串行、64 KiB 暂存与单中断源限制 | 超时可长时间占用调用线程，性能不代表目标架构 | 保留有界等待并修正完成路径；先验收正确性，再评估直映/chain/并发性能 |
| R13 | 目标系统和已分析 KC 漂移 | 旧符号/vtable 推断失效 | 现有记录为 26.7.2 / 25G332 / Darwin 25.6.0；升级后重新采样，不沿用静态地址 |
| R14 | 路线 B 的 SPI KPI 已废弃，且最小子类尚未加载验证 | 后续维护与兼容性不确定 | 首版维持 A；选择 B 时单独完成构建、依赖解析和运行验证 |
| R15 | EventMasks 极性错误、EVENT_ACK 缺失、AddressReply 不回收 | 预期事件缺失、回复池耗尽 | 属已确认源码缺口，不再解释为固件偶发特性；M2 修复并做绕环/ACK 验证 |
| R16 | 容量 CDB、sense/短传输、写入持久化与恢复后身份检查未闭合 | 错容量、错误成功或写错对象 | 按 §9.3 分项修复；代码存在和编译成功均不能代替协议/数据完整性验收 |

**已定首版范围**：x86_64 测试机、已记录 Tahoe 系统版本、路线 A、直连 STP/SATA 物理盘、单中断源；不做启动盘或 IR 卷。以下仅在扩大范围时再确定，不作为本次文档修正或既定首版推进的阻塞：跨 macOS 版本适配、正式签名发布、独立诊断工具交付形式、额外拓扑/硬件矩阵，以及路线 B 或 dext 项目。

---

## 9. 当前实现、证据边界与下一步（2026-11-11）

### 9.1 版本、设备与部署状态

本节是状态唯一入口。2026-11-11 的核实更新了接线、设备身份、只读复测、受控读写压力测试，以及 0.14.0 的部署/加载与写后回读、UNMAP 初步验证（见测试盘行及 §9.2）；ESP UUID、SAS3008 固件与其它状态沿用原有记录，执行前仍须现场确认。

| 对象 | 可确认的状态 | 边界 |
|---|---|---|
| 仓库源码 | `MPR/Info.plist` 与构建脚本标记 0.16.3；属性名、方法名、成员名与宏名已去除里程碑轮次编号，包含读写、UNMAP、sense、TMF/复位、按 SMID 完成槽、传输长度与有效 sense 贯通、fast-path 能力选择、目标身份过滤、UNMAP 能力探测、FUA/barrier、PM 睡眠/唤醒、watchdog、SAS 设备/拓扑事件记录、复位后重建 nub 与分区重扫描，以及仅在 `mpr_timeout_test=1` 时启用的一次性超时注入 | §9.3 的 D1–D11 已有源码修改并随 0.16.3 加载；D12 已补 `reportMediaState`、PM、watchdog 与热插拔事件/媒体状态，异步命令池/chain 仍是同步暂存原型，设备变化后的自动重建待事件句柄比对，M4 仍不能标记“代码完成” |
| 最近编译/部署记录 | 0.14.0 至 0.16.3 均经 `tools/build-noxcode.sh` 在目标 macOS 26.7.2 使用 Apple clang 21 构建、链接及 ad-hoc 签名；`plutil -lint`、bundle 类型和内部符号校验通过。0.16.3 已按名称/容量/ESP UUID 动态定位替换内部 SanDisk ESP 的 MPR.kext（新增备份 `bak_d8_20261111` 至 `bak_d12_20261111`、`bak_d9_reset_fix_20261111`、`bak_partition_rescan*_20261111`、`bak_watchdog_20261112`、`bak_hotplug_*_20261112`、`bak_watchdog_arm_20261112`）；注入用 `config.plist` 已恢复，SHA-256 回到 `71b96d2b…` | 部署记录本身不证明加载后的行为；0.16.3 的加载结果见下一行 |
| 最近实际加载记录 | 2026-11-11 重启后 `com.ling.MPR (0.16.3)`；`MPRStage=read-write`、`MPRBlockDeviceState=ok`、`MPRMediaBlocks=468862128`、`MPRMediaBlockSize=512`、`MPRFastPathEnabled=1`、`MPRUnmapEnabled=1`、`MPRWatchdogState=ok`，`MPRBlockDev` 生成可写 `IOMedia`，读 32 MiB 写 16 MiB 回读正常 | 只验证了当前 0.16.3 的一次加载；D12 的异步池/chain 与 24 h 不在本次验证范围；热插拔自动重建尚未在真实拔插下验证 |
| SAS3008 / 固件 | PCI `0x1000:0x0097`；MPI 2.5；FW `09.00.00.00`；`IOCCapabilities=0x6985c` 包含 `INTEGRATED_RAID=0x1000`，此前未枚举 IR 卷 | 已验证对象是带 IR 能力固件下的独立 SATA 盘；尚非完整 IT/IR 兼容矩阵 |
| 测试盘 | PCCOOLER SSD T200 240GB，468,862,128 × 512 B = 240,057,409,536 B；Windows USB 桥下已格式化为 exFAT（卷标 `testdisk`）。2026-11-11 重新直连 SAS3008，Debian 下现场核对为 `/dev/sdd`（SN `AA000000000000000458`，`phy-1:5` 6.0 Gbit，`sdd1` 16 MiB + `sdd2` exFAT） | 只读复测通过：整盘前 1 GiB 顺序读 1024/1024 完成、两次 SHA-256 一致；旧坏 LBA 1255072–1255074 用 `O_DIRECT` 单扇区读成功；内核 exFAT 只读挂载成功。Debian 内核 exFAT rw 挂载下又完成受控压力测试：3 轮 × 1 GiB 写后回读（清页缓存前后 SHA-256 一致）、100 个 4 KiB 小文件元数据压力、卸载后只读重挂的持久化校验，全部一致，内核日志错误计数 0，测试文件已清理。macOS 侧 0.14.0 加载后完成 1 GiB 顺序读、256 MiB 随机写后回读（两次 SHA-256 一致）、64 MiB 零写与源哈希一致、16 MiB UNMAP（偏移 8 GiB）且卷可重新挂载；0.14.1 注入一次超时后 `ABORT_TASK` 成功，恢复阶段出现一次 `UNIT ATTENTION`（ASC 0x29），重试后 45/64/256 MiB 读均成功；0.14.2 以 `mpr_tmf_fail=1/2/3` 逐级验证 `LUN_RESET`、`TARGET_RESET` 与 `resetAndReinitialize`，reset 后盘重新枚举为 `disk6` 且读写正常；0.14.3 的按 SMID 完成槽又通过 64 MiB 读写、16 MiB UNMAP 与一次 `ABORT_TASK` 注入回归。**仍非 24 h / 满盘 / 并发压测，§9.3 的 D8–D12 缺口未验证**；历史 GPT/四分区信息不能用于推断当前分区布局，破坏性测试仅限重新核对身份后的这块获准测试盘 |
| 内部启动盘 / ESP | 原记录中 SanDisk SD8SNAT-128G-1006（128 GB、SATA）已由 `diskutil` 确认为 Internal；MPR 仅注入其 ESP | 每次部署重新核对整盘位置、型号/容量和 ESP UUID，不沿用 `disk0s1` 等编号；系统/数据卷与 USB EFI 不用于部署 |

原有回退资料包括内部 ESP 同目录的 `config.plist.bak_mpr-m0_20261111`、`config.plist.bak_sata_internal_20261111` 和 `MPR.kext.bak_m4_20261111`、`MPR.kext.bak_namefix_20261111`，实际存在性与对应内容应在执行前确认。最新部署记录为仅替换 kext、config 校验和不变；不得为了“重新完成 M0”覆盖现有配置或清理历史备份。

### 9.2 已有实机证据与未完成验收

| 能力 | 已有记录 | 正确结论 / 仍缺内容 |
|---|---|---|
| M0：匹配、MMIO、构建/加载 | `IOPCIPrimaryMatch=0x00971000` 命中；BAR0 是 I/O，BAR1/BAR2 为 64 位 64 KiB MMIO（配置偏移 `0x14`），BAR3/BAR4 为 64 位 256 KiB MMIO（`0x1c`）。最小 kext 已加载；后续单盘 nub 也已加载 | 物理 BAR 地址随启动分配，不硬编码。目标机只有 Command Line Tools，已有无 Xcode 构建链路；完整驱动安全 stop/卸载和回滚演练不能因此标记完成 |
| 内部 SATA 位置判定 | 此前 AMD FCH SATA `pci1022,7901` 下系统盘被报 External；在实际 DeviceTree 对应的 `PciRoot(0x0)/Pci(0x1,0x2)/Pci(0x0,0x0)/Pci(0x9,0x0)/Pci(0x0,0x0)` 注入 `built-in`（Data `01000000`）后，后续记录已确认为 Internal | 已不再是“待重启确认”的当前阻塞。保留现有配置，只在新的部署前重新检查身份与位置 |
| M1：READY、Facts、IOC Init、Manufacturing Page0 | 记录显示 READY → OPERATIONAL（`0x20000000`）；request/reply frame 各 32 dwords=128 B、RequestCredit=3072、MaxMSIxVectors=96、MaxChainDepth=128。原型取 128 个 request frame、144 深度的 free/post 队列、初始 128 个可用 reply frame。Manufacturing Page0 读取成功：LSISAS3008、Quanta Mezz、35S2BMA0000 | 基本收发链路已跑通；单条 config 成功不证明 reply frame 回收、失败清理和任意 Facts 几何已正确实现 |
| M2：中断与主动 config 发现 | 曾记录 `MPRIntType=1`、`MPRIntCount=1`、`MPRReplyViaIrq=1`；PortEnable 返回成功。SAS IO Unit Page0 为 336 B、8 phys，phy 4 关联 handle 9；SAS Device Page0 为 56 B、`DeviceInfo=0x0881`（END_DEVICE/SATA_DEVICE/DIRECT_ATTACH）、Parent=1、Phy=4、Slot=4、Enclosure=1、派生 SAS address `0x4433221104000000` | 单次 IRQ 完成和主动读页有效。实际 MSI/MSI-X 类型、完整多目标遍历、事件订阅/ACK、reply 回收及热插拔均未完成验收；主动读页不关闭这些缺口 |
| M2：事件观测 | 旧代码掩码为 `0x10C08004` / `0x00200002`，曾见 EVENT_CHANGE `0x0A`、SAS_ENCL_DEVICE_STATUS_CHANGE `0x1D`，未见期望的 discovery/topology 事件 | 已找到订阅掩码极性错误，不能再推断为“固件不自发 discovery”。修正及确认订阅回复后重新观察（§9.3） |
| M3：SCSI 与容量 | TUR/INQUIRY 成功，vendor=ATA、product=PCCOOLER SSD T20、revision=5B；RC10 得到 last LBA 468862127、块大小 512。使用过 fast-path 请求并接受普通/fast-path success reply | 证明该配置的命令通路及 RC10 容量。RC16 的 CDB 编码有错，旧 CHECK CONDITION 不能证明 SAT/SATA 不支持 RC16；fast-path 也必须按目标能力选择 |
| M3：块读取与只读 nub | READ(10) 从 LBA0 读 4096 B，曾见 MBR `0xAA55`、保护分区 `0xEE`、LBA1 `EFI PART`；0.12.2 的 `MPRBlockDev` 经系统 `IOBlockStorageDriver` 生成只读 `IOMedia`，600 MiB 两次顺序读 SHA-256 相同 | 只读链路已部分实测。尚未完成 §6 的 1 GiB 完整顺序读与一致性退出条件；这些记录没有验证 WRITE、flush、UNMAP 或恢复 |
| M4：实现与初步验证 | 0.14.0 已加载并完成写/UNMAP 成功路径；0.14.2 逐级验证 `ABORT_TASK`、`LUN_RESET`、`TARGET_RESET` 与 `resetAndReinitialize`；0.14.3 用 `fReplySlots[128]` 按 SMID 保存完成结果，`IOSimpleLock` 串行化 `processReplyQueue`，读写/UNMAP/TMF 注入回归通过 | 只覆盖成功路径；错误注入与 24 h 混合 I/O 未验收；D8–D12 缺口仍成立 |

**测试盘健康与归因边界**：原记录中，macOS 与 Debian 13 上游 `mpt3sas` 对同一盘都出现介质读取错误；固定 LBA 1255072–1255074 在 Debian `O_DIRECT` 单扇区读取中也失败（记录为 9/9），其它位置正常。该交叉结果支持先调查测试盘/设备路径，不能把该处错误直接归为本驱动独有缺陷，也不能据此豁免驱动的错误传播、超时和恢复问题。随后 Windows 的 RTL9210B-CG USB 桥下格式化并做 1 GiB、4 GiB 写后回读均无错，只说明那次路径/状态下的结果；重映射或链路差异只是可能解释。2026-11-11 接回 SAS3008 后按现场身份完成只读复测：前 1 GiB 顺序读两次 SHA-256 均为 `7ce1c096…c183e`、旧坏 LBA 可读、无新介质错误，故此前阻碍 1 GiB 验收的样本异常在该路径下未复现。随后按现场身份在 Debian 内核 exFAT 下完成受控读写压力测试：3 轮 × 1 GiB 写后回读 SHA-256 一致、100 个 4 KiB 小文件元数据压力、跨卸载重挂持久化一致，内核日志 0 错误，测试文件已清理。这仍不等于 24 h / 满盘 / 并发稳定性已验证。macOS 侧 0.14.0 加载后另行完成 256 MiB 随机写后回读、64 MiB 零写与 16 MiB UNMAP，均无错误，事件掩码自检与订阅应答也通过；这不替代故障注入与长期稳定性。M4 缺口按 §9.3 处理，进一步扩大写入须先按 §9.4 完成核对并取得授权。

### 9.3 已确认的实现缺口与修正要求

以下依据当前 `MPR/MPRController.cpp`、`MPR/MPRBlockDev.cpp`、`MPR/MPI/mpi2_min.h` 及 §2 固定参考源码。表中按函数定位，不将易漂移的本仓库行号作为唯一依据。**当前状态**：D1–D11 已在源码中做了对应修改并随 0.15.6 加载；D2 的事件掩码自检与订阅应答已在实机通过，D3 的按 SMID 槽与串行消费已通过读写/UNMAP/TMF 回归，D6 的三级 TMF/reset 已逐级实机触发，D8 的短传输/sense 有效位检查、D9 的 SAS DevicePage0 过滤与 fast-path 选择、D10 的 VPD 0xB0 探测与超范围 UNMAP 拒绝、D11 的 FUA/barrier 已编译并在正常读写中验证无回归。D12 已补 `reportMediaState`、PM、watchdog 与 SAS 设备/拓扑事件记录：`fWatchdogTimer` 每秒检查并发布 `MPRWatchdogState`（`ok`/`topology-change`/`offline`/`frozen`），热插拔事件置 `fTopologyChanged`、`stop()` 释放定时器并拆除 nub。**自动重建暂缓**：启动后的 `DEVICE_STATUS_CHANGE` 会让 `transferBlocks` 自激 reset（实测 `MPRResetCount=8~13`、`readSasIOUnitPage0` 失败、媒体消失），因此已从 I/O 路径移除自动 reset，等事件携带可核对的 handle/SAS 地址后再启用。**异步命令池/chain 仍未实现**，当前仍是单 staging 缓冲、`fIOLock` 串行的同步暂存原型。**复位后分区重扫描已在 0.15.10 修复**：`resetAndReinitialize()` 重建 nub，`publishBlockDevice()` 重新分配成对常驻缓冲，让 `IOBlockStorageDriver` 重新 probe LBA1；注入验证中 `Content` 恢复为 `GUID_partition_scheme`、`disk6s2` 自动出现并挂载、读 64 MiB 正常。表中「必须完成的修正与验收」仍是完整要求，不能因部分改动而标记完成。

| # | 当前实现 / 问题 | 必须完成的修正与验收 |
|---|---|---|
| D1 | `processReplyQueue()` 消费 AddressReply 后只回写 post index，未归还 reply frame；初始只有 128 个 frame 交给 IOC | 实现有地址/长度/对齐校验且恰好一次的 frame 归还和 free index 更新。config、错误、事件与 TMF 回复总量显著超过 128 并多轮绕环后仍有稳定资源；成功快路径读不能代替这项验收 |
| D2 | `sendEventNotification()` 清零结构后对目标事件做 `\|=`，实际屏蔽所需事件；未等待对应订阅应答、没有 EVENT_ACK；事件只存最后一份数据和位图 | 先将四个 mask 置 `0xffffffff` 再清所需位。保留现有七个事件时前两 word 应为 `0xEF3F7FFB`、`0xFFDFFFFD`，后两 word 仍全 1；以实际事件集为准。按对应请求确认成功，实现有界事件队列、AckRequired/EventContext 处理及管理 credit，修正后重新验证 discovery/topology |
| D3 | 所有命令共用 `fLastReply`/SMID；同一批回复互相覆盖。IRQ action 与 `waitForSmid()`/`waitForReply()` 的回退轮询可并发消费队列；filter 将整个状态寄存器当 W1C 的解释没有参考依据 | 统一串行消费，按活动命令保存结果并核对 Function/SMID/消息长度；按寄存器定义处理状态，不能用未经核实的写 1 操作作为通用清中断。覆盖原命令迟到回复与 TMF 应答同时到达的情况 |
| D4 | `resetAndReinitialize()` 先 `freeQueueMemory()` 再 `diagReset()`；`stop()` 直接释放 DMA、MMIO 与锁，缺少冻结入口、在途 I/O/回调同步及 DMA 停止确认 | 按 §5 第 11 条 实现 quiesce → 确认硬件停止访问 → 回收/重建；覆盖初始化失败、复位失败和有在途 I/O 的停止，不释放 IOC 仍可能访问的资源 |
| D5 | `recoverTimedOutCommand()` 无论结果如何都解除 `fFrozenSmid`；数据路径忽略恢复结果并用同一 SMID 最多尝试 3 次，正常 `scsiReadWrite()` 也没有该冻结检查 | 恢复失败保持故障/离线，停止新请求及资源复用；完整跟踪原命令和管理命令的终止与迟到完成。按命令和错误类别决定重试，写结果未知不能当普通可重试错误；每个请求最终只完成一次 |
| D6 | `sendTaskMgmt()` 通过 `postDefaultRequest()` 发 `0x08` default descriptor，未采用参考 TMF 的 high-priority 路径；检查管理回复不等于已排空原命令 | 依据 `mpi2.h` 的 high-priority `0x06` 与 FreeBSD TMF 提交链建立管理保留资源，限制为 IOC Facts 的 HighPriorityCredit；ABORT/LUN RESET/TARGET RESET 与 IOC reset 逐级验收，并证明旧请求已隔离 |
| D7 | `scsiReadCapacity16()` 把 `0x20` 写在 CDB byte 15，bytes 10–13 allocation length 实际为 0；RC10 没处理 `lastLba=0xffffffff`。数据路径只有 READ/WRITE(10)，`scsiSynchronizeCache()` 也只编码 32 位 LBA | 修正 RC16 allocation length 与 control，检查有效返回长度后再解析容量；RC10 哨兵要求 RC16。补齐 16 字节命令或在能力边界拒绝不可表示容量/请求，不得悄悄截断。先修编码再用有效 sense 判断设备能力 |
| D8 | `scsiReadWrite()` 只凭成功状态判断整条数据请求成功，未检查 TransferCount；`decodeSense()` 的结果仅写 IORegistry，块读写最终仍返回通用 DeviceError；有效位、SenseCount 与旧 sense 隔离不足 | 把有效 sense、IOC/SCSI 状态和实际传输长度贯通到 completion；按返回格式/长度解析并清理本命令 sense。覆盖短读/短写、介质错误、无效或截断 sense，不能把未传输数据作为成功数据复制 |
| D9 | `postScsiIo()` 和各请求硬编码 FAST_PATH；目前仅挑选首个非零 handle，缺完整物理盘/阵列成员过滤。reset 后只更新 handle，不核对稳定身份、容量或能力 | 按 SAS Device Page0 的 FAST_PATH_CAPABLE/ENABLED_FAST_PATH 等信息选通路；按 §1.3 过滤对象。恢复后重新 INQUIRY/VPD、容量和能力核对，身份改变就撤销/重建 nub；不得延用旧盘容量对新盘写入 |
| D10 | `fUnmapEnabled` 默认 true，未做支持性/Block Limits 探测；extent 的 64 位 blockCount 编码时只保留低 32 位；当前只接受最多 32 个 extent | 先探测后启用，按设备/协议限制拆分 extent 与批次或准确返回不支持/参数错误，防止截断。失败禁用 UNMAP 时普通读写仍开启，不能把现状写成“自动转只读”；掉盘和恢复失败另外处理 |
| D11 | `MPRBlockDev::doAsyncReadWrite()` 忽略 `IOStorageAttributes`，`doSynchronize()` 忽略 options；写保护无条件报告 false | 明确 FUA、同步范围和 barrier 的实现；目标身份/容量/状态未确认前不发布可写介质。普通 WRITE 完成、flush 完成与 UNMAP 禁用必须区分，不虚报持久化或写保护能力 |
| D12 | 当前无完整异步命令池/chain，`reportMediaState()` 始终报告 changed=false，安全 PM、watchdog 与热插拔生命周期未闭合 | 当前同步暂存实现按受限原型说明；完成必要的命令状态与故障检测后，再推进 nub 增删、换盘、stop、停机及睡眠/唤醒。职责分层可渐进实施，不能靠新增类名替代契约验收 |

协议校核依据：事件极性见 FreeBSD `mpr.c:2769-2791`；request descriptor 类型及布局见 `mpi/mpi2.h`；TMF 参考 `mpr_sas.c` 的 task management 构造与提交；SCSI CDB/IOStorage 请求属性参考 `MacKernelSDK/Headers/IOKit/scsi/SCSICmds_*.h` 与 `IOKit/storage/IOStorage.h`。固定参考行号只用于该版本；当前实现以表中函数名定位。

### 9.4 推进顺序与验收入口

1. **先修基础所有权与完成路径**：D1、D3、D4、D5、D6 共同决定回复、SMID、DMA 和恢复能否安全闭合；必须覆盖失败分支，不能仅修成功 reset 顺序。同步修正 D2 的事件订阅/ACK，避免带着错误订阅继续推断固件行为。
2. **再闭合设备与块 I/O 契约**：修正 D7–D11 的容量、状态/长度、身份、UNMAP 和持久化语义。M3 已有读证据不代表这些边界已经正确；READ(16)/WRITE(16)、真实支持范围与回调状态需明确。
3. **完成构建与部署核对后做受控加载**：确认所有上传文本行尾、结构布局、内部符号、plist 和签名。实际部署/重启依据届时会话授权，经已核对内部 ESP 执行；重启后检查实际加载版本、IOC 和只读媒体，不直接以“ESP 已放 0.14.0”认定验收。
4. **接回并重新确认测试盘，先恢复 M2/M3 验收**：核对身份、当前分区及健康状态；先做回复池绕环、事件/ACK、config 和只读回读。达成 1 GiB 一致性条件后，才扩大写入测试；样本异常与驱动缺陷分别定位。
5. **按 M4 → M5 扩展**：小范围写后回读与持久化、按能力验证 UNMAP、错误/超时/TMF/复位故障注入，随后 24 h 混合 I/O，再做热插拔、换盘和 PM。失败时按可执行回退方案停止，不把失败后的自动续写作为恢复成功。

§9.1 的部署记录、既有测试盘许可和本文的实施顺序不等于已完成全部 M4 验收。2026-11-11 的补充更新了远程核实结果（接线、身份、只读复测、Debian 侧受控读写压力测试），以及 0.14.0–0.16.3 的构建/部署/加载、macOS 侧写后回读、UNMAP、超时注入、LUN/TARGET/reset 级联、按 SMID 完成槽回归、D10 超范围 UNMAP 拒绝、D11 同步路径、复位后分区重扫描与 watchdog；D8–D11 已实现并做正常路径验证，D12 的异步池/chain、热插拔自动重建与 24 h 仍按 §9.3/§9.4 处理。
