# PLAN — 将 LSI/Broadcom Fusion-MPT SAS3 驱动移植到 macOS

状态：源码 0.20.9（2026-11-13）。已在内部 SATA 盘的 ESP 部署并经 OpenCore 注入加载，单盘读写、热插拔、超时/TMF/复位恢复与睡眠/唤醒均已实机验证；多目标场景下两块盘各自发布、按 DevHandle 路由、写入策略隔离，以及两块盘分别物理拔插后的逐目标移除与重新发布，均已实机验证。当前源码行为与诊断见 [DEBUG.md](DEBUG.md)，对外能力与验证边界见 [README.md](README.md)。

## 1. 目标与范围

### 1.1 目标硬件与平台

- HBA：Broadcom/Avago SAS3008，PCI `0x1000:0x0097`，MPI 2.5，12 Gb/s SAS3；固件 `09.00.00.00`，`IOCCapabilities` 含 `INTEGRATED_RAID`。
- 测试盘：PCCOOLER SSD T200 240 GB（468,862,128 × 512 B），exFAT 卷 `testdisk`；扩容器验证用 Adaptec AEC-82885T 加单背板。多目标验证时同一背板上另有 TOSHIBA MG04SCA60EE 6 TB（11,721,045,168 × 512 B，SSP，属于 ZFS 阵列 `main_data`），只读、不写。
- 平台：macOS Tahoe 26.7.2（`25G332`、`Darwin 25.6.0`），x86_64，MSI X570 测试机；IOKit kext，基于 MacKernelSDK 构建。
- 硬件事实：BAR0 是 I/O，MMIO 在 BAR1/BAR2（配置偏移 `0x14`）；地址随启动重新分配，不硬编码。

实机操作的设备身份一律现场核对（型号、序列号、容量、当前设备号），不沿用历史 `diskN`。

### 1.2 范围

1. 自研 `IOService`（provider = `IOPCIDevice`）实现 HBA：PCI 使能、BAR 映射、IOC 复位与初始化、队列、中断、事件订阅与 ACK。
2. 把每个合格独立盘（STP/SATA 或 SSP）经系统 `IOBlockStorageDriver` 发布为原生整盘 `IOMedia`，并维护每个目标的身份、几何与媒体生命周期；写入权限按目标、按显式规则授予以外一律只读。
3. 块 I/O：READ/WRITE(10)/(16)、SYNCHRONIZE CACHE、FUA、auto sense、超时与 TMF 恢复、热插拔；UNMAP 仅在探测到支持且通过限制校验后启用。
4. 电源与停机：睡眠/唤醒、`systemWillShutdown` 的静默路径。
5. 诊断：IORegistry 属性与可选故障注入 boot-args。

### 1.3 不做

- IR/RAID 逻辑卷及其成员管理、SES 机箱管理、SMP passthrough、固件升级、启动盘引导。
- 多 LUN、多路径、SAS/SSP 盘的写路径。
- NVMe/PCIe/Tri-Mode、SAS2（`mps`/`0x0072`）、GEN35/SEA 分支。
- Apple Silicon、dext/DriverKit、多队列与性能调优、FreeBSD user ioctl 兼容层。
- 继承 `IOSCSIParallelInterfaceController`（§3 路线 B），仅保留为后续评估。

## 2. 基线（可复现）

| 项目 | 内容 |
|---|---|
| FreeBSD 参考 | `freebsd/freebsd-src` `main`，commit `e56090afb68b6c036d6b54c7b66d762d98bd96d0`（2026-11-10），`sys/dev/mpr/` 与 `mpi/` 各 13 个文件；BSD-2-Clause |
| Linux 参考 | `torvalds/linux` `master`，commit `b3dfbde62cf1981c4638939c159dfd087b7a72e9`（2026-11-10），`drivers/scsi/mpt3sas/` 16 个文件 + `mpi/` 10 个；GPL-2.0，仅作行为参考 |
| MacKernelSDK | git 子模块 `3f750085caa17ec3a7880f11c11bf4f48cd6a164`（2026-09-23），基于 macOS 11 / Xcode 12.0 SDK 并补充 `xnu-8019.80.24` 头文件 |
| Apple OSS | `apple-oss-distributions/IOSCSIParallelFamily` commit `560f74ab0c6f5d9aa6c99675d8e95986b202144d`，APSL 2.0，只读参考、不参与构建 |
| 反编译资料 | `ida/BootKernelExtensions.kc`（67,600,384 B，MD5 `b9da5eafcd90e439ae7c55c1ca8ddf33`）与对应 IDA 数据库、离线核查脚本；仅用于核对 ABI、vtable 与依赖 |
| 版本控制 | `MacKernelSDK` 是子模块；`reference/`、`ida/` 不入库、不参与构建；参考源码按上表固定 commit 复现 |

目录布局：

```
mpr/
  README.md  DEBUG.md  PLAN.md  AGENTS.md  NOTICE
  MPR/                               (Controller、BlockDev、Async、Target、MPI/mpi2_min.h、Info.plist)
  MPR.xcodeproj/                     (工程入口)
  tools/build-noxcode.sh             (Command Line Tools 构建入口)
  tools/test_protocol.cpp            (协议层主机侧测试)
  tools/validate-build-inputs.py     (行尾/版本/工程成员校验)
  MacKernelSDK/                      (submodule, 入库)
  reference/                         (gitignored, 只读参考)
    freebsd/sys/dev/mpr/  + mpi/
    linux/drivers/scsi/mpt3sas/  + mpi/
    IOSCSIParallelFamily/            (Apple OSS, APSL 2.0)
```

复现：对固定 commit 分别用 GitHub Contents API 递归列目录，从 `raw.githubusercontent.com` 取文件；Apple OSS 用 `codeload` tarball 取整树。本机访问 GitHub 需走 `http://127.0.0.1:10808` 代理，HTTPS 下载按工作区约束在 MCP 控制台执行。不要用移动分支的 `HEAD` 作基线。

## 3. 架构决策

| 路线 | 做法 | 判定 |
|---|---|---|
| **A（采纳）** | 自研 `IOService` 实现 HBA；每块盘实例化 `IOBlockStorageDevice` 子类 nub，由系统 `IOBlockStorageDriver` 生成 `IOMedia`/`IOMediaBSDClient` | 目标系统 vtable 槽位与 SDK 头离线核对一致，且已实机加载并发布 `IOMedia`；块设备状态、错误传播与恢复由本驱动负责 |
| B | 继承 `IOSCSIParallelInterfaceController`，复用 macOS SCSI 应用层栈 | 静态依据齐备（SDK 头、117 个导出符号、官方 HBA 样例），但 KPI 已标注废弃，且需自研 `SCSIParallelTask` ↔ MPI 适配；最小子类链接/加载未验证 |
| C | `IOService` + `IOUserClient` 透传 CDB | 只能调试，无法生成 `/dev/disk`，不作主路线 |

补充事实：目标系统的存储 family（`IOStorageFamily`、`IOSCSIArchitectureModelFamily`、`IOSCSIParallelFamily`）都在 BootKC 内，"系统没有 SCSI 栈"不成立；`OSBundleLibraries` 按实际使用的 API 与提供者兼容区间选择，现有 `IOStorageFamily = 1.0` 与运行时 2.1 不矛盾。FreeBSD 的 suspend/resume 直接返回 `EINVAL`，电源路径必须自研。

## 4. 能力与边界

| 能力 | 状态 | 边界 |
|---|---|---|
| IOC 初始化、Facts 约束队列、中断回复、事件订阅/ACK | 已验证 | 单中断源；回复由单一上下文消费 |
| 异步请求调度（4 个共享命令窗口） | 已验证 | 吞吐受窗口数与单一回复消费者限制 |
| SGL/chain 与常驻暂存回退 | 已验证 | 客户端数据经常驻窗口拷贝，无零拷贝直映 |
| 块 I/O 与缓存命令 | 已验证 | READ/WRITE(10)/(16)、RC10/16、TUR、INQUIRY、SYNC(10)/(16)、FUA 回退 |
| UNMAP | 已验证 | 探测 B0/B2 后启用，按上报限制拆分，不外扩调用者范围 |
| 发现与身份 | 已验证（双目标） | 每个合格目标进入目标表并独立 probe/nub；handle 只作固件地址，复用按 SAS 地址与稳定身份判定 |
| IR 成员过滤 | 已实现，未在阵列上验证 | 拒绝卷、成员盘、热备与扩容成员；当前固件虽带 IR 能力但无卷样本 |
| 移除与恢复 | 已验证（双目标；含在途读/写在途拔盘） | 逐目标 TARGET_RESET → 命令排空 → REMOVE_DEVICE 实机触发：被拔盘立即离线、另一目标继续服务、在途请求以设备错误返回、不重放；TMF 级联与有界 IOC 复位另有注入验证 |
| 热插拔与媒体生命周期 | 已验证（双目标，背板） | 拔盘即离线、另一目标不受影响；插回需 PortEnable 重发现（8 s/45 s 两次有界复位，期间另一盘短暂下线），按 SAS 地址复用槽位、盘号不变 |
| 睡眠/唤醒、停机静默 | 已验证（双目标） | 睡眠前逐可写目标 flush；唤醒后两盘在 25 秒内恢复、盘号不变、`MPRPowerState=on`；未就绪目标按 1 秒节奏重试而不计入失败或触发探测复位；每次唤醒仍有一次有界控制器重建（配置页管理超时引发），`MPRResetCount` 增加 1 |
| 多 LUN、多路径、SES/SMP、NVMe/Tri-Mode | 未实现 | 见 §1.3 |

## 5. 里程碑状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| M0 环境 | Command Line Tools 构建、内部 ESP 注入、匹配与加载 | 完成 |
| M1 寄存器与 IOC | READY → OPERATIONAL、IOC Facts、DMA 池、Manufacturing Page0 | 完成 |
| M2 中断、回复与事件 | 中断回复、回复帧回收、按命令分派、EventMasks/ACK、PortEnable | 完成（回复帧回收随长时间运行覆盖，未单独统计绕环次数） |
| M3 单盘只读 | 独立盘识别、身份与容量、只读 nub、大范围顺序读一致性 | 完成 |
| M4 读写与恢复 | WRITE、缓存同步/FUA、UNMAP、sense、超时/TMF/复位 | 完成；注入长跑只完成多轮短程 |
| M5 拓扑、热插拔与 PM | nub 增删、handle 复用隔离、安全 stop、睡眠/唤醒 | 完成（直连与双目标背板，含在途 I/O 拔盘） |
| M6 诊断工具（可选） | 独立 `IOUserClient`/工具、统计导出 | 未实施；现用 IORegistry 属性与 boot-args |
| M7 后续 | IR 卷、SES、多队列、启动盘、签名发布；路线 B / dext 评估 | 不在当前范围 |

## 6. 尚未完成

- 唤醒后配置页管理超时仍需要一次有界 IOC 复位：`MPRStickyFaultCode=3` / `MPRStickyFaultSmid=4` 表明 fabric 稳定前 SMID 4 的配置页读取超时。IOC 可能仍持有该请求帧，`finishManagementRequest()` 会隔离它并置协议故障，只能经复位恢复；「一次唤醒零复位」尚未做到，也不属于探测分类能解决的范围。
- 复位末段失败的诊断与重试：PortEnable/初始化一类失败目前只记为笼统的 `MPRReinitState=failed`，靠协议故障兜底再复位才恢复，缺少逐阶段的失败点与就地重试。
- 故障注入长跑未跑满：需夹具在媒体重建后按新 BSD 号重开设备，并把预期内复位与短时错误纳入判据。
- 性能未调优：仍为常驻窗口拷贝、4 个命令窗口、单一回复消费者。
- 换盘到不同容量设备、跨 macOS 版本回归、正式签名与发布流程。
- 路线 B 的最小子类链接/加载验证（仅在决定改走该路线时开展）。

## 7. 验证方式

- 主机侧：`tools/test_protocol.cpp` 用独立基准字节与独立 chain 解码器验证协议层（136,593 条断言、2,000 个确定性 SGL 用例，含故意拒绝用例），启用 ASan/UBSan；`tools/validate-build-inputs.py` 校验 LF 行尾、bundle 与运行版本一致、所有编译单元均纳入 Xcode 工程。
- 构建/加载：`tools/build-noxcode.sh` 产出 `MPR.kext`，校验 kmod 入口、bundle 类型、驱动内部符号与 plist 后 ad hoc 签名；注入内部 ESP 后以 `kmutil showloaded` 与 `MPRBuild` 三处一致确认。交叉编译的 Mach-O 目标不等于链接、签名、可加载。
- 实机：单盘 1 GiB 级顺序读两次一致性、写后回读逐字节一致、UNMAP 后卷可重挂、物理拔插与在途拔盘、注入超时后的 TMF/复位级联、睡眠/唤醒；扩容器下同样验证有界读写与 UNMAP。双目标时另验证两盘 LBA0 与高 LBA 读一致性、可写盘写后回读、以及 `MPRTargetNWriteCount` 只落在可写目标。睡眠/唤醒必须有旧版与当前版的 `MPRResetCount` 前后对照，并同时核对 `MPRResetInitiator`、`MPRStickyFaultCode`、`MPRProbeRetries` 与两盘 online 时间；单看「online」或单看一次复位都不能判定唤醒路径是否改善。
- 长跑：以命令数预算代替墙钟时间，关注按操作次数累积的驱动问题（回复帧、SMID、事件 ACK、属性字典、恢复循环退化），不是介质耐久性。串行命令原型曾以约 345 万条命令、90 分钟通过（p99 无漂移、`MPRResetCount` 不增长、属性数量稳定、参考区哈希不变）；当前异步调度版本尚未重跑。
- 破坏性写仅限现场重新核对身份的测试盘，写窗口事后逐字节还原；`IORegistry` 的"在线"属性不能替代字节数与回读内容。

## 8. 风险

| # | 风险 | 缓解 |
|---|---|---|
| R1 | 系统升级后 vtable/符号/加载行为漂移 | 升级前重取 KC 并重跑离线核查，再做加载与只读回归；不外推旧结论 |
| R2 | 多目标仅验证到"各自发布 + 只读隔离 + 读一致性"，移除/热插拔/PM 未在双目标下复测 | 逐目标路径已实现并记录未验证项；不以单目标结果推断多目标移除行为 |
| R3 | 固件带 IR 能力，过滤规则覆盖有限 | 无法确认独立盘身份时不发布；不把单盘结果推广到全部 IR 配置 |
| R4 | expander 覆盖为同一背板上的两块终端盘（一块 SATA、一块 SSP），拔插与 PM 未复测 | 明确记录未复测项，不外推为通用支持 |
| R5 | 性能原型的窗口拷贝与单一回复消费者 | 先保证正确性；调优时再评估直映与多队列 |
| R6 | 路线 B 的 SPI KPI 已废弃 | 维持路线 A；改走 B 前单独完成构建与加载验证 |
| R7 | ESP 有文件但实际未加载或仍运行旧版 | ESP-only 部署；重启后独立核对运行版本；config 变更前备份并校验 |

## 9. 许可

1. FreeBSD `mpr`（BSD-2-Clause）：`MPR/MPI/mpi2_min.h` 保留 Broadcom 版权与三条款许可；来源与许可清单见仓库 `NOTICE`。
2. Linux `mpt3sas`（GPL-2.0）只作行为与协议参考，不复制代码到驱动源码；`reference/` 不入库、不参与构建。
3. MPI 结构体定义以 FreeBSD `mpi/*.h` 为唯一代码来源；与 Linux 版的差异只在 `MPI/` 内注释记录，不混用。
4. `MacKernelSDK` 头文件仅用于编译，遵循其自身许可，不拷入驱动源码树。
5. `ida/` 的 KC 与 IDA 数据库只用于核对接口、vtable、符号与依赖，不复制 Apple 实现，不把 KC 地址写入驱动。
6. `reference/IOSCSIParallelFamily/`（APSL 2.0）只作头文件、ABI 与依赖键参考；若将来引用其常量或结构定义，按 APSL 保留声明并在 `NOTICE` 单列来源与 commit。

## 10. 操作约束

- 驱动只安装到经现场核对的**内部 SATA 盘 ESP** 的 `EFI/OC/Kexts/MPR.kext`，由 OpenCore `Kernel > Add` 注入；不写 `/Library/Extensions`、其它系统卷或 USB EFI，不把 AuxKC 重建当作部署步骤。
- 改 `config.plist` 前在同目录备份，改后 `plutil -lint`/`ocvalidate` 并核对只有预期差异；仅替换 kext 时 config 应保持不变。
- 上传的文本文件必须是 LF；改后重新检查，不沿用旧副本的检查结论。
- 串口调试默认关闭（`debug=0x100`、无 `serial=5`），仅在需要抓内核日志时临时开启并随即关闭。
- 预计超过 1 GB 的备份须先说明范围、大小与位置并取得针对该备份的许可；验收用流式校验和，不自动制作整盘镜像。
- 实机写入、部署、重启按会话中已授权的具体范围执行；写测试仅限重新核对身份后的测试盘。
