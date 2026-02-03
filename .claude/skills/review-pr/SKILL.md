---
name: review-pr
description: This skill should be used when the user asks to "review PR", "review pull request", "/review-pr", mentions "PR review" combined with "FalconFS", or needs to analyze GitHub pull requests for the FalconFS project. The skill automates code review by fetching PR data, analyzing diffs, and checking against FalconFS project conventions.
version: 1.3.0
---

# GitHub PR 自动 Review Skill

## 概述

此 skill 用于自动化 review FalconFS 项目的 GitHub Pull Request。它通过分析 PR 代码变更，根据项目规范提供 review 意见，并可选择将结果自动提交到 GitHub。

**支持两种评论模式：**
- **整体评论模式** - 在 PR 顶部提交汇总意见
- **行级评论模式** - 将问题直接关联到具体代码行（推荐）

---

## 使用方法

### 基本语法

```
/review-pr <PR编号> [选项]
```

### 参数说明

**PR 标识格式：**
- PR 编号: `74`
- PR URL: `https://github.com/falcon-infra/falconfs/pull/74`
- 仓库+编号: `falcon-infra/falconfs#74`

### 选项说明

| 选项 | Review 类型 | 说明 |
|------|-------------|------|
| 无选项 | - | 只在终端显示，不提交 |
| `--submit` | COMMENT | 提交整体评论到 PR 顶部 |
| `--approve` | APPROVE | 批准 PR |
| `--request-changes` | CHANGES_REQUESTED | 请求修改 |
| `--line-comments` | COMMENT | 提交行级评论（关联代码行）|
| `--line-comments --approve` | APPROVE | 行级评论 + 批准 |
| `--line-comments --request-changes` | CHANGES_REQUESTED | 行级评论 + 请求修改 |
| `--help` | - | 显示帮助信息 |

### 使用示例

```bash
# 只在终端显示 review
/review-pr 74

# 提交整体评论
/review-pr 74 --submit

# 批准 PR
/review-pr 74 --approve

# 提交行级评论（推荐）
/review-pr 74 --line-comments

# 行级评论 + 批准
/review-pr 74 --line-comments --approve

# 请求修改
/review-pr 74 --line-comments --request-changes
```

---

## 执行步骤

### 步骤 1：解析参数

识别用户输入的 PR 编号和选项。

### 步骤 2：获取 PR 信息

```bash
gh pr view <PR_NUMBER> --repo falcon-infra/falconfs \
  --json title,body,author,state,headRefName,baseRefName,additions,deletions,changedFiles,labels,commits,createdAt,updatedAt,headRefOid
```

### 步骤 3：获取 PR Diff

```bash
gh pr diff <PR_NUMBER> --repo falcon-infra/falconfs
```

### 步骤 4：分析代码变更

检查项目规范符合度：
- **错误码处理**: 直接返回内部错误码应使用 `ErrorCodeToErrno()` 转换
- **日志输出**: 建议使用 glog 而非 printf/cout/fprintf
- **内存管理**: 建议使用智能指针而非裸 new
- **Protobuf 变更**: 提醒需要重新生成 .pb.cc/.h 文件
- **PostgreSQL 插件**: 提醒 falcon/ 目录文件会被复制到 third_party
- **CMake 变更**: 提醒新依赖需要 Find*.cmake 脚本

### 步骤 5：生成 Review 意见

包含以下部分：
- **PR 概述** - 标题、作者、状态、变更统计
- **代码质量分析** - 代码风格、项目规范符合度
- **具体改进建议** - 按文件和行号指出问题
- **潜在风险** - 安全、性能、测试覆盖等

### 步骤 6：提交 Review（可选）

#### 模式 A：整体评论模式

使用 `gh pr review` 命令：

```bash
# 根据 event 类型选择参数
gh pr review <PR_NUMBER> --repo falcon-infra/falconfs \
  --approve           # APPROVED
  # 或
  --request-changes   # CHANGES_REQUESTED
  # 或（默认）
  # (无参数)           # COMMENTED
  --body "<review内容>"
```

#### 模式 B：行级评论模式（推荐）

1. 获取 PR 的 head commit SHA：
```bash
HEAD_SHA=$(gh pr view <PR_NUMBER> --repo falcon-infra/falconfs --json headRefOid --jq '.headRefOid')
```

2. 解析 diff，提取问题文件和行号

3. 构建行级评论 JSON 数组：

```json
{
  "commit_id": "<HEAD_SHA>",
  "body": "整体 review 总结",
  "event": "COMMENT",
  "comments": [
    {
      "path": "falcon/hcom_comm_adapter/falcon_meta_service.cpp",
      "position": 22,
      "body": "🔴 函数体为空，需要补充实现"
    },
    {
      "path": "falcon/hcom_comm_adapter/falcon_meta_service.cpp",
      "position": 29,
      "body": "⚠️ 建议使用 LOG(WARNING) 替代 fprintf"
    }
  ]
}
```

**重要格式说明：**
- `commit_id`: PR 的 head commit SHA
- `event`: `COMMENT` | `APPROVE` | `REQUEST_CHANGES`
- `path`: 文件路径（相对于仓库根目录）
- `position`: 在 diff hunk 中的位置（从 1 开始），不是文件的绝对行号
- `body`: 具体问题描述

4. 使用 GitHub API 提交：

```bash
echo "$REQUEST_JSON" | gh api \
  --method POST \
  -H "Accept: application/vnd.github+json" \
  repos/falcon-infra/falconfs/pulls/<PR_NUMBER>/reviews \
  --input -
```

### 步骤 7：显示结果

无论是否提交，都在终端显示完整的 review 结果。

---

## 评论模式对比

| 特性 | 整体评论 | 行级评论 |
|------|----------|----------|
| 命令 | `--submit` | `--line-comments` |
| 显示位置 | PR 顶部 | 具体代码行旁边 |
| 问题关联 | ❌ 无 | ✅ 直接关联 |
| 开发者体验 | 需手动查找 | ✅ 直接看到位置 |
| 适用场景 | 总结性意见 | 具体代码问题 |

---

## Review 输出格式

### 终端输出格式

```
## PR #<编号> Review: <标题>

### 📊 概述
| 项目 | 信息 |
|------|------|
| **状态** | 🔵 OPEN / ✅ MERGED |
| **作者** | @username |
| **变更** | +xxx / -yy 行 |

### 🔍 发现的问题

#### 🔴 高优先级
| 文件 | 行号 | 问题 |
|------|------|------|
| xxx.cpp | 123 | ... |

#### ⚠️ 中优先级
| 文件 | 行号 | 问题 |
|------|------|------|
| xxx.cpp | 456 | ... |

### 📝 总结
- **评价**: LGTM / LGTM with suggestions / Changes requested
```

### 行级评论标签

- 🔴 **高优先级** - 必须修复的问题
- ⚠️ **中优先级** - 建议改进的问题
- ℹ️ **低优先级** - 代码风格问题

---

## FalconFS 项目规范

### 错误处理
```cpp
// ❌ 错误：直接返回内部错误码
return -1;

// ✅ 正确：转换为 errno
return ErrorCodeToErrno(internal_error_code);
```

### 日志输出
```cpp
// ❌ 避免
printf("Error: %s\n", msg);
std::cout << "Error: " << msg;
fprintf(stderr, "Warning: %s\n", msg);

// ✅ 推荐
LOG(INFO) << "Message: " << msg;
LOG(WARNING) << "Warning: " << msg;
LOG(ERROR) << "Error: " << msg;
```

### 内存管理
```cpp
// ❌ 避免
auto* obj = new MyClass();

// ✅ 推荐
auto obj = std::make_unique<MyClass>();
```

---

## 依赖要求

- **Python 3** - 用于运行分析脚本
- **gh (GitHub CLI)** - 用于获取 PR 信息和提交 review
- **git** - 用于版本控制操作
- **jq** - 用于 JSON 处理

### 前置配置

使用前需要认证 GitHub CLI：

```bash
gh auth login
```

选择：
- `GitHub.com`
- `HTTPS`
- `Login with a web browser`

---

## 常见问题

### Q: 行级评论的 line 字段为什么是 null？
A: GitHub API 对已提交的 commit 需要 `position` 参数（diff 中的位置）而不是 `line` 参数（文件的绝对行号）。

### Q: 如何删除已提交的 review？
A: 只能删除 PENDING 状态的 review：
```bash
gh api --method DELETE repos/<owner>/<repo>/pulls/<pr>/reviews/<review_id>
```

### Q: event: COMMENT 和 COMMENTED 有什么区别？
A:
- `COMMENT` - 用于创建新 review
- `COMMENTED` - 已创建 review 的状态（只读）

---

## 版本历史

- **v1.3.0** - 更新行级评论实现方式，修正 event 类型
- **v1.2.0** - 新增行级评论模式
- **v1.1.0** - 新增自动提交功能
- **v1.0.0** - 初始版本
