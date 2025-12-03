#!/bin/bash
set -e

# ==============================================================================
# Google GKI 上游更新同步脚本
# 功能：检查上游更新，按月份整理提交，创建以日期命名的新分支并逐月 cherry-pick
# ==============================================================================

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${GREEN}[INFO] $1${NC}"; }
warn() { echo -e "${YELLOW}[WARN] $1${NC}"; }
error() { echo -e "${RED}[ERROR] $1${NC}"; }
info() { echo -e "${CYAN}[INFO] $1${NC}"; }

# 配置
UPSTREAM_REMOTE="upstream"
UPSTREAM_BRANCH="common-android13-5.15"
CURRENT_BRANCH=$(git branch --show-current)
BASE_BRANCH="${CURRENT_BRANCH}"

# 获取当前日期作为新分支名
NEW_BRANCH_DATE=$(date +%Y%m%d)
NEW_BRANCH="android13-5.15-${NEW_BRANCH_DATE}"

log "=========================================="
log "Google GKI 上游更新同步工具"
log "=========================================="
log "当前分支: ${CURRENT_BRANCH}"
log "上游分支: ${UPSTREAM_BRANCH}"
log "新分支名: ${NEW_BRANCH}"
log "=========================================="

# 1. 检查并添加上游远程仓库
if ! git remote | grep -q "^${UPSTREAM_REMOTE}$"; then
    log "添加上游远程仓库..."
    git remote add ${UPSTREAM_REMOTE} https://android.googlesource.com/kernel/common
else
    log "上游远程仓库已存在: ${UPSTREAM_REMOTE}"
    git remote set-url ${UPSTREAM_REMOTE} https://android.googlesource.com/kernel/common
fi

# 2. 获取上游更新
log "正在获取上游更新（这可能需要一些时间）..."
if ! git fetch ${UPSTREAM_REMOTE} ${UPSTREAM_BRANCH} 2>&1; then
    warn "无法获取上游更新，尝试使用本地已有的引用..."
    
    # 检查是否有本地的上游引用
    if git show-ref --verify --quiet refs/remotes/${UPSTREAM_REMOTE}/${UPSTREAM_BRANCH}; then
        log "使用本地已有的上游引用"
    else
        error "无法获取上游更新，且本地没有可用的引用。"
        error "请检查网络连接或配置代理："
        error "  git config --global http.proxy http://proxy.example.com:8080"
        error "  或设置环境变量: export http_proxy=..."
        error ""
        error "或者手动指定上游提交范围："
        error "  $0 --from-commit <commit> --to-commit <commit>"
        exit 1
    fi
fi

# 3. 查找基准提交（当前分支与上游的共同祖先）
log "查找基准提交..."
BASE_COMMIT=$(git merge-base ${CURRENT_BRANCH} ${UPSTREAM_REMOTE}/${UPSTREAM_BRANCH} 2>/dev/null || echo "")
if [ -z "$BASE_COMMIT" ]; then
    warn "无法找到共同祖先，尝试查找最近的合并点..."
    # 尝试查找最近的合并提交
    MERGE_BASE=$(git log --merges --oneline -1 --grep="Merge.*upstream\|Merge.*${UPSTREAM_BRANCH}" ${CURRENT_BRANCH} 2>/dev/null | cut -d' ' -f1 || echo "")
    if [ -n "$MERGE_BASE" ]; then
        BASE_COMMIT="$MERGE_BASE"
        log "找到最近的合并点: $(git log -1 --oneline ${BASE_COMMIT})"
    else
        warn "使用当前分支的 HEAD 作为基准"
        BASE_COMMIT=$(git rev-parse ${CURRENT_BRANCH})
    fi
fi

log "基准提交: $(git log -1 --oneline ${BASE_COMMIT})"

# 4. 获取上游新提交（从基准提交到上游最新）
log "分析上游新提交..."
UPSTREAM_HEAD="${UPSTREAM_REMOTE}/${UPSTREAM_BRANCH}"

# 获取所有新提交
git log --oneline --date=short --format="%h|%ad|%s" ${BASE_COMMIT}..${UPSTREAM_HEAD} > /tmp/upstream_commits.txt

COMMIT_COUNT=$(wc -l < /tmp/upstream_commits.txt | tr -d ' ')
if [ "$COMMIT_COUNT" -eq 0 ]; then
    log "没有发现新的上游提交！"
    exit 0
fi

log "发现 ${COMMIT_COUNT} 个新提交"

# 5. 按月份组织提交
log "按月份组织提交..."
declare -A MONTHLY_COMMITS

while IFS='|' read -r hash date subject; do
    if [ -z "$hash" ] || [ -z "$date" ]; then
        continue
    fi
    
    # 提取年月 (YYYY-MM)
    year_month=$(echo "$date" | cut -d'-' -f1-2)
    
    if [ -z "$year_month" ]; then
        continue
    fi
    
    # 添加到对应月份的数组
    if [ -z "${MONTHLY_COMMITS[$year_month]}" ]; then
        MONTHLY_COMMITS[$year_month]="$hash"
    else
        MONTHLY_COMMITS[$year_month]="${MONTHLY_COMMITS[$year_month]}|$hash"
    fi
done < /tmp/upstream_commits.txt

# 6. 显示按月份组织的提交
log "=========================================="
log "按月份组织的提交："
log "=========================================="

# 按月份排序
for month in $(printf '%s\n' "${!MONTHLY_COMMITS[@]}" | sort); do
    commits="${MONTHLY_COMMITS[$month]}"
    count=$(echo "$commits" | tr '|' '\n' | wc -l | tr -d ' ')
    info "${month}: ${count} 个提交"
    
    # 显示该月的提交列表
    echo "$commits" | tr '|' '\n' | while read -r hash; do
        if [ -n "$hash" ]; then
            subject=$(git log -1 --format="%s" "$hash" 2>/dev/null || echo "N/A")
            date=$(git log -1 --format="%ad" --date=short "$hash" 2>/dev/null || echo "N/A")
            echo "  - [$date] $hash: $subject"
        fi
    done
    echo ""
done

# 7. 询问用户是否继续
echo ""
read -p "$(echo -e ${YELLOW}是否继续创建新分支并逐月 cherry-pick? [y/N]: ${NC})" -n 1 -r
echo ""
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    log "已取消操作"
    exit 0
fi

# 8. 创建新分支
log "创建新分支: ${NEW_BRANCH}"
if git show-ref --verify --quiet refs/heads/${NEW_BRANCH}; then
    warn "分支 ${NEW_BRANCH} 已存在，切换到该分支"
    git checkout ${NEW_BRANCH}
else
    git checkout -b ${NEW_BRANCH} ${CURRENT_BRANCH}
fi

# 9. 逐月 cherry-pick 提交
log "=========================================="
log "开始逐月 cherry-pick..."
log "=========================================="

FAILED_COMMITS=()
SUCCESS_COUNT=0
TOTAL_COUNT=0

for month in $(printf '%s\n' "${!MONTHLY_COMMITS[@]}" | sort); do
    log "处理 ${month} 的提交..."
    commits="${MONTHLY_COMMITS[$month]}"
    
    # 统计该月的提交数
    month_count=$(echo "$commits" | tr '|' '\n' | grep -c . || echo "0")
    log "  ${month} 共有 ${month_count} 个提交"
    
    # 使用数组而不是管道，避免子shell问题
    IFS='|' read -ra HASH_ARRAY <<< "$commits"
    
    for hash in "${HASH_ARRAY[@]}"; do
        if [ -z "$hash" ]; then
            continue
        fi
        
        TOTAL_COUNT=$((TOTAL_COUNT + 1))
        subject=$(git log -1 --format="%s" "$hash" 2>/dev/null || echo "N/A")
        info "  Cherry-picking [$TOTAL_COUNT]: $hash - $subject"
        
        if git cherry-pick "$hash" 2>&1; then
            SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
            log "    ✓ [$SUCCESS_COUNT/$TOTAL_COUNT] 成功"
        else
            error "    ✗ [$TOTAL_COUNT] 失败: $hash"
            FAILED_COMMITS+=("$hash")
            
            # 询问用户如何处理冲突
            echo ""
            warn "Cherry-pick 失败，可能遇到冲突"
            read -p "$(echo -e ${YELLOW}选择操作: [s]跳过 [a]中止 [m]手动解决后继续: ${NC})" -n 1 -r
            echo ""
            
            case $REPLY in
                [Ss]*)
                    git cherry-pick --abort 2>/dev/null || true
                    log "已跳过该提交"
                    ;;
                [Aa]*)
                    error "用户中止操作"
                    exit 1
                    ;;
                [Mm]*)
                    warn "请手动解决冲突后，运行: git cherry-pick --continue"
                    read -p "$(echo -e ${YELLOW}解决冲突后按 Enter 继续...${NC})"
                    if git cherry-pick --continue 2>&1; then
                        SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
                        log "    ✓ 手动解决后成功"
                    else
                        FAILED_COMMITS+=("$hash")
                    fi
                    ;;
                *)
                    git cherry-pick --abort 2>/dev/null || true
                    log "已跳过该提交"
                    ;;
            esac
        fi
    done
done

# 10. 显示总结
log "=========================================="
log "Cherry-pick 完成总结"
log "=========================================="
log "成功: ${SUCCESS_COUNT} 个提交"
log "失败: ${#FAILED_COMMITS[@]} 个提交"

if [ ${#FAILED_COMMITS[@]} -gt 0 ]; then
    warn "失败的提交："
    for hash in "${FAILED_COMMITS[@]}"; do
        subject=$(git log -1 --format="%s" "$hash" 2>/dev/null || echo "N/A")
        echo "  - $hash: $subject"
    done
    warn "请手动处理这些提交"
fi

log "=========================================="
log "当前分支: ${NEW_BRANCH}"
log "请检查提交后，运行以下命令合并到主分支："
log "  git checkout ${CURRENT_BRANCH}"
log "  git merge ${NEW_BRANCH}"
log "=========================================="

# 清理临时文件
rm -f /tmp/upstream_commits.txt

