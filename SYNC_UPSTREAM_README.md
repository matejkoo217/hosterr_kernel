# Google GKI 上游更新同步工具

## 功能说明

这个脚本用于检查上游 Google GKI 源码的更新，按月份整理提交，并创建以日期命名的新分支进行逐月 cherry-pick。

## 使用方法

### 基本使用

```bash
./sync_upstream_gki.sh
```

### 工作流程

1. **检查上游远程仓库**
   - 自动添加或更新 `upstream` 远程仓库（指向 `https://android.googlesource.com/kernel/common`）

2. **获取上游更新**
   - 从上游分支 `common-android13-5.15` 获取最新提交
   - 如果网络连接失败，会尝试使用本地已有的引用

3. **分析新提交**
   - 查找当前分支与上游的共同祖先作为基准点
   - 列出所有新的上游提交

4. **按月份组织提交**
   - 自动按年月（YYYY-MM）分组提交
   - 显示每个月的提交列表和统计

5. **创建新分支**
   - 创建以当前日期命名的分支：`android13-5.15-YYYYMMDD`
   - 如果分支已存在，会切换到该分支

6. **逐月 Cherry-pick**
   - 按月份顺序逐个 cherry-pick 提交
   - 遇到冲突时提供交互式处理选项

## 冲突处理

当 cherry-pick 遇到冲突时，脚本会询问你的选择：

- **s** - 跳过该提交
- **a** - 中止整个操作
- **m** - 手动解决冲突后继续

## 网络问题

如果无法连接到 Google 的源码服务器，可以：

1. **配置代理**：
   ```bash
   git config --global http.proxy http://proxy.example.com:8080
   ```

2. **使用环境变量**：
   ```bash
   export http_proxy=http://proxy.example.com:8080
   export https_proxy=http://proxy.example.com:8080
   ```

3. **使用本地已有的引用**：
   如果之前已经 fetch 过上游，脚本会尝试使用本地引用

## 示例输出

```
==========================================
Google GKI 上游更新同步工具
==========================================
当前分支: serein-213
上游分支: common-android13-5.15
新分支名: android13-5.15-20251203
==========================================
[INFO] 正在获取上游更新...
[INFO] 发现 45 个新提交
[INFO] 按月份组织的提交：
[INFO] 2025-10: 12 个提交
[INFO] 2025-11: 33 个提交
...
```

## 完成后的操作

脚本完成后，会显示总结信息。然后你可以：

1. **检查新分支**：
   ```bash
   git log --oneline android13-5.15-20251203
   ```

2. **合并到主分支**（检查无误后）：
   ```bash
   git checkout serein-213
   git merge android13-5.15-20251203
   ```

3. **推送到远程**：
   ```bash
   git push serein android13-5.15-20251203
   ```

## 注意事项

- 确保当前工作目录干净（没有未提交的更改）
- 建议在操作前备份当前分支
- 大量提交的 cherry-pick 可能需要较长时间
- 如果遇到大量冲突，建议分批处理

