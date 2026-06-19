---
name: require-approval-before-edit
description: 每次修改文件前必须经过用户同意
metadata:
  type: feedback
---

用户明确要求：**所有文件修改必须先征求同意**。给出修改理由、展示改动内容，等用户确认后才能执行 Edit/Write 操作。

**Why:** 用户希望保持对代码的完全控制，避免未经审查的自动修改。

**How to apply:** 任何涉及 Edit、Write、Bash 删除/移动文件的操作，必须先向用户说明改什么、为什么，等待用户明确同意后再执行。
