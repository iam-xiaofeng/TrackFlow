# TrackFlow 协作规则

以下规则为本项目固定执行规范：

1. 每次对话开始前，先阅读 `HANDOVER.md` 熟悉当前约定与部署流程。

2. 每次修复 bug 后，必须将问题、原因、解决方案追加记录到 `debug.md`。

3. **仓库单一权威源 (single source of truth)**
   - 唯一权威仓库：`https://github.com/iam-xiaofeng/TrackFlow`
   - 本地 VPS (`/projects/TrackFlow`) 与 GPU 服务器 WSL (`/home/xf/TrackFlow`)
     任一时刻都必须能通过 `git pull` 同步到对方状态，**不允许长期保留未提交修改**。
   - 无论在哪台机器改代码，都必须先 `git commit` + `git push` 把改动落到
     GitHub，再去另一台机器 `git pull`。**严禁用 `rsync` 等绕过 git 的方式
     同步代码**——历史上这样做过（debug.md #8），导致 GPU 的"生产态"和本地
     仓库长期分叉，最终需要一次性 force-push 收敛（参见 tag
     `prod-baseline-2026-05-12` 和 `archive/pre-gpu-sync`）。
   - 每次会话开始前，若两台机器有任一处存在 uncommitted 改动，**必须先
     commit + push 收敛**再开始新工作。可以用以下命令快速检查：
     ```bash
     # 本地
     git -C /projects/TrackFlow status --short
     # GPU
     ssh -p 9022 xf@localhost 'cd /home/xf/TrackFlow && git status --short'
     ```

4. 每次代码更新后，必须执行完整发布链路：
   - 改动产生的机器：`git commit` + `git push`
   - 另一台机器：`git pull`
   - **仅当后端 C++ 源码有变更时**才需要：杀掉旧进程 (`yolo_edge_server`)
     → 重新编译 → 重新启动服务。纯前端 HTML / 文档 / 配置变更不需要重启后端。

5. 路径与 GitHub remote 的事实修正
   - GPU 服务器实际 git 仓库在 `/home/xf/TrackFlow`（HANDOVER.md 之前提到的
     `/projects/TrackFlow` 是本地 VPS 路径，已过时，待 HANDOVER.md 同步修正）。
   - GitHub remote 历史曾从 `AbeginnerFLM/TrackFlow` 改名为 `iam-xiaofeng/TrackFlow`，
     两个 URL 现在都会被 GitHub 自动重定向到同一个仓库；新提交统一推到
     `iam-xiaofeng/TrackFlow`。
