# JiuXia Kernel

基于 **hfdem** 内核，合并 Android GKI 上游源码构建的自定义 Android 内核。

- 基线分支：`android13-5.15-lts-2026-07`
- 上游：Android Common Kernel `android13-5.15-lts`
- 本仓库在上游与 hfdem 基线之上进行定制与优化合并

## 当前版本

**JiuXia-Kernel-R1-Release-20260727**

R1 基线变更：

- `LOCALVERSION`: `-hfdem-JiuXia-R1-20260727-033206`
- 移除 DRM encoder clone mask 校验
- 修复 LZ4 在 Clang 下的 MIN/MAX 宏重定义

## 构建

```bash
make O=out ARCH=arm64 LLVM=1 \
  KCFLAGS="-mllvm -regalloc-enable-advisor=default" \
  -j$(nproc)
```

刷机包使用 AnyKernel3 打包。
