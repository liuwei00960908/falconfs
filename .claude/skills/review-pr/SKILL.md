---
name: review-pr
description: This skill should be used when the user asks to "review PR", "review pull request", "/review-pr", mentions "PR review" combined with "FalconFS", or needs to analyze GitHub pull requests for the FalconFS project. The skill automates code review by fetching PR data, analyzing diffs, and checking against FalconFS project conventions.
version: 1.2.0
---

# GitHub PR 自动 Review Skill

## 概述

此 skill 用于自动化 review FalconFS 项目的 GitHub Pull Request。它通过分析 PR 代码变更，根据项目规范提供 review 意见，并可选择将结果自动提交到 GitHub。

**支持两种评论模式：**
- **整体评论模式** - 在 PR 顶部提交汇总意见（默认）
- **行级评论模式** - 将问题直接关联到具体代码行（使用 `--line-comments` 参数）

## 执行步骤

当用户请求 PR review 时，按以下步骤执行：

### 1. 解析参数

支持的参数格式：
- `review-pr <PR编号>` - 只在终端显示 review 结果
- `review-pr <PR编号> --submit` - 提交整体评论到 GitHub
- `review-pr <PR编号> --approve` - 提交批准 review
- `review-pr <PR编号> --request-changes` - 提交请求修改 review
- `review-pr <PR编号> --line-comments` - **提交行级评论**（问题直接关联到代码行）
- `review-pr <PR编号> --line-comments --approve` - 行级评论 + 批准
- `review-pr --help` - 显示帮助信息

### 2. 解析 PR 标识

用户可能提供以下格式：
- PR 编号: `123`
- PR URL: `https://github.com/falcon-infra/falconfs/pull/123`
- 仓库+编号: `falcon-infra/falconfs#123`

### 3. 获取 PR 信息

使用以下 Bash 命令获取 PR 详情：

```bash
gh pr view <PR_NUMBER> --repo falcon-infra/falconfs \
  --json title,body,author,state,headRefName,baseRefName,additions,deletions,changedFiles,labels,commits,createdAt,updatedAt
```

### 4. 获取 PR Diff

```bash
gh pr diff <PR_NUMBER> --repo falcon-infra/falconfs
```

### 5. 执行自动分析（可选）

对于完整的自动分析，执行 Python 脚本：

```bash
python3 .claude/skills/review-pr/review-pr.py <PR_NUMBER>
```

该脚本会检查：
- **错误码处理**: 直接返回内部错误码应使用 `ErrorCodeToErrno()` 转换
- **日志输出**: 建议使用 glog 而非 printf/cout
- **内存管理**: 建议使用智能指针而非裸 new
- **Protobuf 变更**: 提醒需要重新生成 .pb.cc/.h 文件
- **PostgreSQL 插件**: 提醒 falcon/ 目录文件会被复制到 third_party
- **CMake 变更**: 提醒新依赖需要 Find*.cmake 脚本

### 6. 提供 Review 意见

基于分析结果，提供包含以下部分的 review：

- **PR 概述** - 标题、作者、状态、变更统计
- **代码质量分析** - 代码风格、项目规范符合度
- **具体改进建议** - 按文件和行号指出问题
- **潜在风险** - 安全、性能、测试覆盖等

### 7. 提交 Review 到 GitHub（可选）

如果用户指定了 `--submit`、`--approve` 或 `--request-changes` 参数：

#### 模式 A：整体评论模式（默认）

1. 将 review 内容格式化为 Markdown
2. 使用 `gh pr review` 命令提交：

```bash
REVIEW_TYPE=""                    # COMMENTED (默认)
REVIEW_TYPE="--approve"           # APPROVED
REVIEW_TYPE="--request-changes"   # CHANGES_REQUESTED

gh pr review <PR_NUMBER> --repo falcon-infra/falconfs $REVIEW_TYPE --body "<review内容>"
```

#### 模式 B：行级评论模式（使用 --line-comments）

1. 从 diff 中提取每个问题的具体文件和行号
2. 获取 PR 的 head commit SHA：

```bash
HEAD_SHA=$(gh pr view <PR_NUMBER> --repo falcon-infra/falconfs --json headRefOid --jq '.headRefOid')
```

3. 构建行级评论 JSON 数组，格式：

```json
{
  "body": "整体 review 总结",
  "event": "COMMENTED",
  "comments": [
    {
      "path": "falcon/hcom_comm_adapter/falcon_meta_service.cpp",
      "line": 86,
      "body": "🔴 Init() 函数体为空，需要补充实现"
    },
    {
      "path": "falcon/hcom_comm_adapter/falcon_meta_service.cpp",
      "line": 1028,
      "body": "⚠️ 建议使用 LOG(WARNING) 替代 fprintf"
    }
  ]
}
```

4. 使用 GitHub API 提交行级评论：

```bash
gh api \
  --method POST \
  -H "Accept: application/vnd.github+json" \
  repos/falcon-infra/falconfs/pulls/<PR_NUMBER>/reviews \
  -f commit_id="$HEAD_SHA" \
  -f body="$OVERALL_COMMENT" \
  -f event="$EVENT_TYPE" \
  -f comments="$COMMENTS_JSON"
```

5. 验证提交成功并显示结果

**行级评论格式要求：**
- `path`: 文件路径（相对于仓库根目录）
- `line`: 问题所在行号（使用 diff 中显示的行号）
- `body`: 具体问题描述
- `event`: `COMMENTED` | `APPROVED` | `CHANGES_REQUESTED`

### 8. 显示 Review 结果

无论是否提交到 GitHub，都在终端显示完整的 review 结果。

## FalconFS 项目规范参考

### 错误处理
```cpp
// 错误示例: 直接返回内部错误码
return -1;

// 正确做法
return ErrorCodeToErrno(internal_error_code);
```

### 日志输出
```cpp
// 避免
printf("Error: %s\n", msg);
std::cout << "Error: " << msg;

// 推荐使用 glog
LOG(INFO) << "Message: " << msg;
LOG(ERROR) << "Error: " << msg;
```

### 内存管理
```cpp
// 避免
auto* obj = new MyClass();

// 推荐使用智能指针
auto obj = std::make_unique<MyClass>();
```

## 依赖要求

- Python 3
- gh (GitHub CLI) - 用户需要先运行 `gh auth login` 认证
- git
