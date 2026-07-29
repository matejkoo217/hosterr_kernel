# JiuXia Kernel

基于 **hfdem** 内核、合并 Android GKI 上游源码构建的自定义 Android 内核。

- 基线分支：`android13-5.15-lts-2026-07`
- 上游：Android Common Kernel `android13-5.15-lts`
- 当前基线版本：Linux 5.15.211（Android 13 GKI）
- 本仓库在上游与 hfdem 基线之上进行定制与优化合并

## 当前版本

**JiuXia-Kernel-R2-20260729**

R2 在 R1 基础上将四项酷安 5.15 通用优化模块移植为内核内建，去除对预编译 `.ko` 的依赖。

### R2 内建优化

1. **mi_sw_sync** — 内建 `mi_sw_sync` misc 设备
   - 提供 `SW_SYNC_IOC_CREATE_FENCE` / `SW_SYNC_IOC_INC` ioctl
   - 解除 `SW_SYNC` 对 `DEBUG_FS` 的依赖
   - 源码：`drivers/dma-buf/sw_sync.c`、`sync_debug.{c,h}`

2. **moon_binder** — Binder UI 调度优化
   - 注册三个 Android vendor hook：`binder_trans` / `binder_set_priority` / `binder_proc_transaction_finish`
   - 针对 SurfaceFlinger 事务及 `com.miui.home` 等关键 UI 线程提升 FIFO/RT 调度优先级
   - 暴露 `/proc/binder_sched_opt_status` 状态节点
   - 源码：`drivers/android/binder_sched_opt.c`

3. **moon_kshrink_slabd** — 异步 Slab 回收
   - 通过 `shrink_slab_bypass` vendor hook 接管 Slab 回收路径
   - 249 jiffies（`0xf9`）节流间隔，排除 `com.miui.home`
   - 使用可冻结内核线程异步执行回收，降低前台路径负担
   - 源码：`mm/slabd.{c,h}`

4. **moon_kshrink_lruvecd** — 异步 LRUvec 回收
   - 定义 `KSHRINK_SKIP_TRYLOCK` / `KSHRINK_TRYLOCK_DELAY` 标志
   - 5 个 page trylock 相关 vendor hook
   - 4096 高水位限制迁移队列，对 rmap 锁竞争页面做异步回收
   - 源码：`mm/kshrink_lruvecd.c`、`include/linux/kshrink_lruvecd.h`

### R1 基线变更（R2 全部继承）

- **显示黑屏修复**：移除 `drivers/gpu/drm/drm_atomic_helper.c` 中的 `drm_atomic_check_valid_clones()`
- **LZ4 编译兼容性**：`lib/lz4/lz4hc.c` 中 `#undef MIN` / `#undef MAX` 后再定义，修复 Clang 宏冲突
- **版本标识**：`CONFIG_LOCALVERSION = -hfdem-JiuXia-R1-20260727-033206`

### 构建调整

- MLGO 寄存器分配 advisor：`release` → `default`（兼容 Debian clang 19）

## 构建

```bash
make O=out ARCH=arm64 LLVM=1 \
  KCFLAGS="-mllvm -regalloc-enable-advisor=default \
           -Wno-error=unused-but-set-variable \
           -Wno-error=unused-const-variable \
           -Wno-error=unused-variable" \
  -j$(nproc)
```

工具链：Android Clang `clang-r547379`。

刷机包使用 AnyKernel3 打包，仅替换 boot 分区 `Image.gz`，不含 DTB/DTBO/vendor_boot/init_boot/ramdisk。

## 致谢

- **@hfdem** — 本项目的基线内核作者，提供稳定的 Android 13 GKI 基线与大量底层修复
- **@Amktiao（酷安）** — 四项 5.15 通用优化模块（mi_sw_sync / moon_binder / moon_kshrink_slabd / moon_kshrink_lruvecd）的原作者，本仓库在此基础上完成内建移植
- Android Common Kernel 上游与各子系统维护者
