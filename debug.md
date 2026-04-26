# TrackFlow 调试日志

## 1. 大图导致服务器崩溃
**问题**: 处理大分辨率图片时，服务器抛出 `std::bad_alloc` 崩溃。
**原因**: 服务器尝试将原本的 Base64 图片数据序列化回 JSON 响应中。对于 4K 图片，这会产生巨大的字符串，耗尽了 `nlohmann::json` 处理所需的连续内存。
**解决**: 从响应 JSON 中移除了图片数据回显。服务器现在只返回检测结果数据。

## 2. "Processing Failed" 错误
**问题**: 前端显示 `Error: Processing failed`。
**原因**:
1.  **头文件缺失**: `image_decoder.cpp` 缺少 `<iostream>`，导致编译错误（之前被掩盖）。
2.  **JSON 格式不匹配**: C++ 后端发送的 `bbox` 是 JSON 对象 `{"x":...}`，但前端期望的是数组 `[x, y, w, h]`。
**解决**:
- 添加了 `#include <iostream>`。
- 更新 `ws_server.cpp`，确保 JSON 响应中 `obb`（8个浮点数数组）和 `bbox`（4个浮点数数组）格式分离且正确。

## 3. 连接问题 (SSH & Web)
**问题**: SSH 连接被拒绝 (端口 6000)，Web 页面无法访问 (端口 8088)。
**原因**: GPU 服务器上的 `frpc.toml` 配置文件中有拼写错误：
- `remotePort = 600` (少打了一个 '0'，应为 6000)。
- `remotePort = 80880` (多打了一个 '0'，超过了 65535 端口限制)。
**解决**: 将端口修正为 `6000` 和 `8088` 并重启 `frpc` 服务。

## 4. 检测框位置偏移
**问题**: 检测框非常小，并且挤在图片的左上角。
**原因**:
- 前端在发送前将大图（如 4000px）压缩到了 1280px。
- 服务器基于 1280px 图片进行检测，返回的坐标也是 1280px 尺度的。
- 前端直接将这些 1280px 的坐标画在了原始 4000px 的 Canvas 上，没有进行缩放。
**解决**:
- 在前端实现了 `uploadScale` (上传比例) 计算：`原始宽度 / 压缩后宽度`。
- 绘制时将所有坐标乘以 `uploadScale`，将其映射回原图尺寸。

## 5. 检测框巨大且置信度全为 100%
**问题**: 检测框位置居中但异常巨大（松散），且所有"卡车"的置信度都是 100%。
**原因**:
- 使用的 YOLO 模型是 **End-to-End** 导出版本（内置 NMS），输出格式为简化的 `(N, 7)` 张量。
- 格式为 `[x, y, w, h, score, class_id, angle]`。
- C++ 代码之前是按标准 YOLOv8 Raw 格式解析的：`[x, y, w, h, class_scores(80)..., angle]`。
- **后果**: 代码错误地将 `class_id`（卡车ID为1.0）读取为 `confidence`，所以显示 100%。同时角度解析错误导致 NMS 失效或框尺寸异常。
**解决**:
- 更新 `YoloDetector::postprocess` 以识别 7 列输出格式。
- 添加了专门的解析逻辑：`if (num_features == 7) { 按 End-to-End 格式解析 }`。

## 6. 浏览器缓存问题
**问题**: 即使修复了 `test.html`，刷新页面后仍然没有变化。
**原因**: 浏览器对 HTML 文件的缓存非常顽固。
**解决**:
- 创建了新文件 `test_v3.html` 以强制浏览器加载新内容。
- （建议后续通过 URL 参数控制缓存）

## 7. 幽灵进程 (Zombie Process)
**问题**: 即使更新了后端代码并重启，Web 端看到的依然是旧的错误现象（100%置信度），修复无效。
**原因**: 服务器上残留了一个 **18小时前** 启动的 `yolo_edge_server` 进程。由于使用了 `nohup` 且之前的关闭命令可能未生效，该进程一直占用 9002 端口。新启动的服务无法绑定端口，导致所有请求实际上还是由这个运行着旧代码的"幽灵"进程处理的。
**解决**: 使用 `kill -9` 强制杀死了所有旧进程，并确认新进程成功绑定端口。这一步最终让之前的代码修复生效。

## 8. 代码更新未生效 (Git Push/Pull 失误)
**问题**: 在本地修改了 C++ 代码逻辑（修复解析问题），但服务器运行的还是旧逻辑，没有任何变化，日志也没打出来。
**原因**: 修改文件后 **忘记了 commit 和 push**。导致远程服务器执行 `git pull` 时提示 "Already up to date"，根本没有拉取到最新的修复代码。此外，有时本地修改导致 pull 失败，需要强制覆盖。
**解决**:
- 改用 `rsync` 直接同步文件到 GPU 服务器，跳过 git push/pull 流程。
- 从 VPS 一键部署: `sshpass rsync -avz -e "ssh -p 9022" ... xf@localhost:/home/xf/TrackFlow/`

## 9. GCC 编译器崩溃 (Internal Compiler Error)
**问题**: 在远程 WSL 环境编译时，GCC 频繁报错 `internal compiler error: in purge_dead_edges` 或 `cfgrtl.cc` 错误，导致无法生成可执行文件。
**原因**:
- **Spdlog 模板元编程**: `spdlog` 库大量使用了复杂的模板元编程。
- **资源限制/环境不稳定**: WSL 环境（尤其是由于内存或系统库限制）无法处理这些复杂的模板展开，导致编译器进程 (`cc1plus`) 内部状态损坏并崩溃。
- **现象随机**: 错误位置在 `yolo_detector.cpp`, `ws_server.cpp`, 甚至 `main.cpp` 之间随机跳动，只要包含了 `<spdlog/spdlog.h>` 的文件都有可能触发。
**解决**:
- **彻底移除 spdlog**: 将整个项目的日志库从 `spdlog` 替换为原生的 `fprintf(stderr, ...)`。移除所有 `spdlog` 头文件引用。
- **安全编译标志**: 使用 `-O0` (关闭优化) 和 `-fno-var-tracking` (减少内存追踪) 降低编译器负载。
- **结果**: 编译器负载大幅降低，项目成功全量编译通过。

## 10. 模型文件丢失导致的运行时错误
**问题**: 编译通过并启动服务器后，前端显示 `Error: Processing failed`。
**原因**: 服务器日志显示 `[ERROR] YoloDetector: Failed to load model ... Load model models/yolo26.onnx failed. File doesn't exist`。
- 配置文件或代码默认指定加载 `models/yolo26.onnx`。
- 但远程服务器目录中实际并没有这个文件，或者文件名为 `yolo_obb.onnx`。
**解决**:
- 检查发现远程模型实际位于 `/home/xf/TrackFlow/models/yolo26.onnx`。
- 修改 `config/config.yaml`，使用正确的相对路径 `models/yolo26.onnx`，并确保服务器从正确的工作目录 (`/home/xf/TrackFlow`) 启动。

## 11. 检测框过大 (Loose Bounding Boxes)
**问题**: 使用 ONNX 模型后，检测框比实际车辆大一圈。
**原因**:
- 用户原图为 4K (3840×2160)，但 ONNX 模型输入尺寸为 640×640。
- 缩放比例约为 6:1，导致：
  1. **信息丢失**: 4K 中的小目标在 640 尺度下只剩几个像素
  2. **边界模糊**: 模型在低分辨率下无法精确定位边界，倾向于输出较大的框
**解决**:
- 使用 **1280×1280** 分辨率重新导出 ONNX 模型
- C++ 后端自动识别模型输入尺寸并适配预处理
- **结果**: 检测框紧贴车辆边缘，精度显著提升

## 12. WSL GPU 推理环境配置
**问题**: 尽管开启了 `use_cuda`，YOLO 推理依然在 CPU 上运行。ONNX Runtime CUDA provider 初始化失败，提示缺少系统库。
**诊断**: GPU 服务器 (RTX 4090) 的 WSL 环境中未正确安装 CUDA Toolkit 和 cuDNN 库。缺失的库包括: `libcudart`, `libcublas`, `libcublasLt`, `libcufft`, `libcurand`, `libcudnn`。
**解决**:
1. **代码启用 CUDA**: 修改 `yolo_detector.cpp`，当 `use_cuda_` 为 true 时正确添加 `OrtCUDAProviderOptions`。
2. **在 GPU 服务器 (WSL) 上安装依赖**:
   ```bash
   wget https://developer.download.nvidia.com/compute/cuda/repos/wsl-ubuntu/x86_64/cuda-keyring_1.1-1_all.deb
   sudo dpkg -i cuda-keyring_1.1-1_all.deb
   sudo apt update
   sudo apt-get install -y cuda-cudart-11-8 libcublas-11-8 libcufft-11-8 libcurand-11-8
   sudo apt-get install -y nvidia-cudnn
   ```

## 13. 前端 HTTP 服务器未运行导致页面无法访问
**问题**: `http://142.171.65.88:8088/test_v4.html` 无法打开，返回空响应。
**原因**:
- GPU 服务器上的 `python3 -m http.server 8088` 没有在运行。
- FRP 客户端虽然配置了 `trackflow-web` 代理 (remotePort: 8088)，但 GPU 本地没有对应的 HTTP 服务在监听。
- 前端页面通过 FRP 绕行 GPU 服务器不合理——静态文件应该由离用户更近的 VPS 直接提供。
**解决**:
- 改为在 VPS 上直接提供前端页面，使用 nginx 反向代理:
  - `http://142.171.65.88:8080/` → 静态文件 (`/projects/TrackFlow/`)
  - `http://142.171.65.88:8080/ws` → WebSocket 代理到 `127.0.0.1:9002` (FRP 隧道)
- 前端 WebSocket URL 从 `ws://host:9002` 改为 `ws://host:8080/ws`（同端口同源，避免跨端口被防火墙拦截）

## 14. WebSocket 跨端口连接失败
**问题**: 前端页面可以正常打开，但点击 Connect 后 WebSocket 连接失败。
**原因**:
- 前端从 VPS 端口 8080 加载，但 WebSocket 连接到端口 9002 (`ws://host:9002`)。
- 用户的网络环境可能阻止了到非标准端口 9002 的出站连接（企业/校园防火墙常见行为）。
- 或用户通过 HTTPS 代理访问页面，导致浏览器阻止 `ws://`（非加密 WebSocket）连接（混合内容安全策略）。
- 从 VPS 本地测试 `ws://142.171.65.88:9002` 正常响应 pong，排除服务端问题。
**解决**:
- 在 VPS 上部署 nginx，将前端和 WebSocket 统一到同一端口 8080:
  ```nginx
  server {
      listen 8080;
      root /projects/TrackFlow;
      location /ws {
          proxy_pass http://127.0.0.1:9002;
          proxy_http_version 1.1;
          proxy_set_header Upgrade $http_upgrade;
          proxy_set_header Connection "upgrade";
          proxy_read_timeout 86400;
      }
  }
  ```
- 前端 `connect()` 函数改为使用同源地址:
  ```javascript
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const url = proto + '://' + location.host + '/ws';
  ```
- **优势**: 前端和 WebSocket 同端口同协议，彻底消除跨端口/混合内容问题。
- **配置文件**: `/etc/nginx/sites-available/trackflow`

## 15. Connect 按钮无反应 (JS 函数缺失)
**问题**: 前端页面正常加载，但点击 Connect 按钮完全没有反应，没有任何错误提示。
**原因**:
- `connect()` 函数第一行调用了 `log('Connecting to ...')`，但 `log()` 函数在 JS 中从未定义。
- 浏览器抛出 `ReferenceError: log is not defined`，导致 `connect()` 立即终止。
- 同理，`clearLog()` 也未定义，Log 区域的 Clear 按钮也无法工作。
- 根本原因: 在重写前端页面时，HTML 中保留了 Log 区域的 DOM 结构和对 `log()`/`clearLog()` 的调用，但 `<script>` 中漏掉了这两个函数的实现。
**解决**:
- 在 `<script>` 中添加 `log(msg, type)` 和 `clearLog()` 函数定义。
- `log()` 将带时间戳的消息追加到 `#logB` 容器中，支持 success/error/warn 样式。

## 16. 跟踪器完全失效 (Track ID 每帧随机变化)
**问题**: 启用多帧并行推理后，ByteTracker 的目标 ID 每帧都不同，跟踪功能完全丧失。
**原因** (多层叠加):
1. **E2E 模型不支持 batch>1**: 原始 ONNX 模型以 `dynamic=False` 导出，输入形状硬编码为 `[1, 3, 1280, 1280]`。当 `BatchInferenceEngine` 尝试将多帧拼成 batch=2/3/4 推理时，ONNX Runtime 报错 `Got invalid dimensions for input: images index: 0 Got: 2 Expected: 1`。虽然有 fallback 逐帧重试，但引入了额外开销和不确定性。
2. **reset 消息竞态**: 前端在 `startInfer()` 中发送 `{type:'reset'}`，但此时线程池中可能已有正在处理的帧。reset 会清除 session（包括 tracker 状态），导致后续帧在新的空 tracker 上运行，所有历史 ID 丢失。
3. **并行推理 + 串行 tracker 的帧排序问题**: decode + YOLO 并行执行时，帧到达 tracker 的顺序不确定。如果帧 3 先于帧 2 到达 tracker，ByteTracker 会基于错误的时间顺序进行匹配，导致 ID 跳变。
**解决 (架构变更)**:
- **重新导出模型**: 使用 `dynamic=True, batch=4` 重新导出 ONNX，输入形状变为 `[N, 3, 1280, 1280]`，BatchInferenceEngine 可正确执行多帧并行推理。
- **帧排序机制**: 在 Session 中增加 `wait_for_turn(frame_id)` / `advance_turn()` 条件变量机制，确保 tracker 严格按帧序处理（decode+YOLO 仍然并行）。
- **修复 reset 竞态**: 前端发送 reset 后，等待服务器返回 `reset_ack` 才开始发送新帧，避免 reset 和推理帧交错。
- **异常安全**: tracker 阶段用 try/catch 包裹，确保即使 tracker 抛出异常也会调用 `advance_turn()`，防止后续帧死锁。
**关键教训**: 从串行 pipeline 改为并行架构时，必须同时解决三个问题：(1) 模型的 batch 兼容性、(2) 有状态处理器（tracker）的帧排序、(3) 全局状态操作（reset）的同步。这三者任一缺失都会导致 tracker 失效。

## 17. 自定义 Pipeline 帧序丢失
**问题**: 当前端发送自定义 pipeline（如 `["decoder","yolo","undistort","tracker","geo_transform"]`）时，ByteTracker 可能乱序处理帧，导致 track ID 不稳定。
**原因**:
- 默认 pipeline 走 `execute_default_pipeline()`，有 `wait_for_turn(frame_id)` 帧序保证。
- 自定义 pipeline 走 `session.pipeline.execute(ctx)` + `std::lock_guard`，仅保证互斥但**不保证顺序**。
- 前端 `MAX_INFLIGHT=4`，4 帧并发时 tracker 接收顺序不确定。
**解决**:
- `ProcessingPipeline` 新增 `find_index(name)` 方法，按 processor 的 `name()` 查找位置。
- `execute_default_pipeline()` 中将硬编码的 `split=2` 改为动态查找 `"ByteTracker"` 位置作为分割点。
- 移除 `is_default_pipeline` 分支，所有 pipeline 统一走 `execute_default_pipeline()`，确保 tracker 前的阶段可并行、tracker 及之后的阶段严格按帧序执行。

## 18. 双重畸变校正
**问题**: Pipeline 含 `undistort` + `geo_transform` 且都配置了 `camera_matrix` 时，检测中心点被 `cv::undistortPoints` 校正两次。
**原因**:
- `UndistortProcessor::process()` 对中心点做畸变校正。
- `GeoTransformer::process()` 内部也有 `if (has_camera_params_)` 再次校正中心点。
- 两个 processor 各自独立，无法感知对方是否已执行。
**解决**:
- `UndistortProcessor` 执行后设置 `ctx.set("undistorted", true)` 标记。
- `GeoTransformer` 检查 `ctx.get_or<bool>("undistorted", false)`，为 true 时跳过中心点的重复校正。
- 利用已有的 `ProcessingContext::extras_` 机制，零新增基础设施。

## 19. 推理中途 WebSocket 自动断连 (约 700 帧)
**问题**: 推理运行约 698 帧后，前端显示 `Disconnected`，推理停止。
**原因** (多因素叠加):
1. **inflight 死锁**: 前端 `MAX_INFLIGHT=4`，若某个响应因网络延迟/丢失未返回，对应 slot 永远不释放。4 个 slot 被耗尽后前端停止发帧。
2. **无 ping 心跳**: 前端没有周期性 ping 保活。链路经过 Cloudflare Tunnel（空闲超时 ~100s），前端停发帧后 Cloudflare 判定连接空闲并断开。
3. **服务端 idleTimeout=120**: 若 Cloudflare 未先断，服务端 120s 无数据也会主动关闭。
4. **静默丢帧**: `ws_server.cpp:170` 在 `waiting_for_image=false` 时丢弃 binary 数据且不报错，前端 inflight slot 无法释放。
**解决**:
- 前端增加 inflight 超时（15s 无响应自动清除 slot）。
- 前端增加 ping 心跳（每 30s），防止链路判定空闲。
- 前端增加自动重连机制。
- 服务端对 unexpected binary / duplicate header 返回显式错误，不再静默丢弃。

## 20. 前端轨迹渲染 bug
**问题**: 车辆轨迹显示异常——长度不受用户设置控制，且轨迹断线后无法恢复。
**原因**:
1. **硬编码缓冲区**: 旧版前端使用硬编码轨迹上限，忽略用户设置的 `trajLength` 变量。
2. **间隙绘制 bug**: 检测到帧间隙 (`gap>3`) 时执行 `moveTo` 后直接跳过本次连接，导致断线后恢复不自然。
**解决**:
- 模块化前端中统一按 `trajLength` 裁剪轨迹。
- 间隙检测后仍允许后续点继续连线，避免“断了之后永远不接回去”的问题。

## 21. 推理报错 `json.exception.type_error.302`（`class_names` 字段类型兼容）
**问题**: 前端开始推理后持续报错：`type must be array, but is string`。
**原因**:
- `YoloDetector::configure()` 会读取 `class_names`。
- 在部分配置链路中，`class_names` 可能从数组被转换成字符串（例如 `"[\"car\",\"pedestrian\"]"` 或 `car,pedestrian`）。
- 若直接按数组解析，会在 session 初始化阶段抛异常，导致每帧都返回 error。
**解决**:
- 在 `yolo_detector.cpp` 中对 `class_names` 做兼容解析：
  - 数组：按原逻辑读取；
  - 字符串：先尝试 JSON 数组字符串解析，失败再按 CSV 解析；
  - 解析失败时回退到默认类别。
- 这样可避免因配置格式差异造成推理启动失败。

## 22. 前后端 batch 调整到 6（按现网前端路径）
**问题**: 需要验证更高并发吞吐，要求前端 inflight 和后端 batch 同步提升到 6。
**处理**:
- 前端主页面已迁移到 `frontend/inference.html`，实际逻辑在 `frontend/assets/js/inference/app.js`。
- 将 `MAX_INFLIGHT` 从 4 调整为 6。
- 将后端默认 `batch_size` 从 4 调整为 6：
  - `src/main.cpp` 的 runtime defaults；
  - `config/config.yaml` 的 `yolo.batch_size`。
**备注**:
- 推理显示层已是单窗口路径（推理时隐藏 `video`，仅显示 `canvas`），避免双画面叠加。

## 23. `session_busy` 频发与 `test_v5` 页面回退
**问题**:
- 前端出现 `Error: Too many outstanding requests for session`；
- `test_v5.html` 被跳转到模块化页面后，使用体验与原先 v5 不一致。
**原因**:
- 服务端 `limits.max_requests_per_session` 仍为 4，而前端 inflight 已提升到 6，导致并发配额不足；
- `test_v5.html` 在上游变更中被改为重定向页，不再是原先完整页面。
**解决**:
- 将服务端并发配额提升到 8（`config/config.yaml`、`src/main.cpp` 默认值、`ws_server.cpp` fallback）；
- 在 `ws_server.cpp` 中统一使用同一个 `max_requests_per_session` 变量做 slot 校验；
- 将 `test_v5.html` 恢复为完整页面版本（非重定向），保持原有使用习惯。

## 24. 跟踪轨迹串线 / ID 观感失效（旧运行态残留 + 阈值过松 + 丢帧卡序）
**问题**:
- 重新点击 Start 后，右侧车辆信息与轨迹会混入上一次推理结果；
- 轨迹线出现“随机串联”，看起来像 ID 彻底失效；
- 在高并发下，偶发丢帧会让 tracker 等待缺失帧，后续帧长期阻塞。

**原因**:
1. **前端运行态未在 Start 前清空**：`vehicleTracks` 与 lane 动态统计跨轮次复用，tracker 重置后 ID 从 1 重新开始，前端却把新旧同 ID 轨迹拼接在一起。
2. **前端默认阈值被放宽过多**：`confidence=0.1 / track_thresh=0.25 / high_thresh=0.35 / min_hits=1` 引入大量低质量框，导致 ID 抖动显著。
3. **tracker 严格按序等待但无缺帧兜底**：当某帧未进入 tracker（如排队失败/早退）时，后续帧会一直等待该帧 turn。

**解决**:
- 前端新增 `resetRunTrackingState()`，每次 `startInfer()` 前重置车辆轨迹和车道动态统计（保留车道几何配置）。
- 前端恢复稳定参数：`CAPTURE_MAX_WIDTH=960`、`JPEG_QUALITY=0.5`、`yolo.confidence=0.5`、`nms=0.45`、`track_thresh=0.5`、`high_thresh=0.6`、`min_hits=3`。
- 后端 `Session::wait_for_turn()` 改为返回 `bool` 并加入超时缺帧跳过机制；`execute_default_pipeline()` 在非当前 turn 时跳过 tracker 阶段，避免全链路卡死。

## 25. 延迟暴涨（5s+）与 FPS 掉到 1：上传负载与并发积压
**问题**:
- 现场观测到 `Latency ~5731ms`，前端 FPS 约 1。

**原因**:
- 前端采集参数被调高到 `CAPTURE_MAX_WIDTH=960` + `JPEG_QUALITY=0.5`，单帧上传体积显著增大；
- 同时 `MAX_INFLIGHT=6` 会在链路/后端跟不上时持续堆积在途请求，吞吐未提升但排队时延急剧上升。

**解决**:
- 回退到低延迟档位：
  - `MAX_INFLIGHT=4`
  - `CAPTURE_MAX_WIDTH=640`
  - `JPEG_QUALITY=0.35`
- 保留上一条中的跟踪稳定性修复（运行态重置、tracker 缺帧兜底），避免“降延迟=回退 bug”。

## 26. 跟踪观感差（ID 抖动/杂点过多）进一步收敛
**问题**:
- 即便链路恢复，前端仍出现“跟踪看起来没效果”的主观观感：ID 多、轨迹杂、切换频繁。

**原因**:
1. 前端默认检测阈值偏低，保留了大量低质量框，给 tracker 引入噪声。
2. ByteTracker 匹配阶段未做类别约束，可能发生跨类匹配导致 ID 切换（例如行人/车辆相邻场景）。

**解决**:
- 前端 `test_v5.html` 收紧默认参数：
  - `yolo.confidence: 0.7`
  - `tracker.track_thresh: 0.6`
  - `tracker.high_thresh: 0.7`
  - `tracker.min_hits: 2`
- 后端 `byte_tracker.cpp` 在 IoU 匹配前增加类别一致性门控（class mismatch 直接不匹配），减少跨类 ID 污染。

## 27. 跟踪完全失效：ByteTracker 索引失效 + 丢失轨迹 ID 未赋值（2026-04-14）
**问题**:
- 经过多次优化后，部署到 GPU 服务器后跟踪功能几乎完全丧失。
- 检测框正常出现，但 track_id 要么错乱、要么始终为 -1，轨迹无法绘制。

**原因（双重 bug）**:

1. **`tracked_stracks_` 索引失效（致命）**:
   - `ByteTracker::update()` 中，`tracked_stracks_.erase(remove_if(...))` 在第 264 行移除了 Lost/Removed 状态的轨迹。
   - 但紧接其后的 detection track_id 赋值（第 272-299 行）仍使用 erase **之前** 存储的索引（`matches_high` 中的 `ti`、`matches_low` 中通过 `unmatched_tracks_high[ti]` 间接引用的索引）。
   - `erase` 会压缩 vector，导致这些旧索引：
     - 指向错误的轨迹（静默数据污染，ID 错乱）。
     - 越界访问（未定义行为 / 崩溃）。
   - **示例**: erase 前 `[S0, S1(Lost), S2, S3(Lost), S4]`，erase 后 `[S0, S2, S4]`。此时 `matches_high` 中存储的索引 2 原本指向 S2，erase 后变成 S4。索引 3、4 则直接越界。

2. **丢失轨迹重激活后 detection 未赋 track_id**:
   - 当 `lost_stracks_` 中的轨迹被重新匹配到高置信度检测框时，匹配结果仅用于更新轨迹状态和移入 `tracked_stracks_`。
   - 但对应的 `detections[orig_idx].track_id` 从未被赋值，始终保持 -1。
   - 导致前端看到这些检测框"没有被跟踪"。

**解决**:
- 将 detection track_id 赋值块整体移到 `tracked_stracks_` 的 erase 操作 **之前**，此时所有索引仍然有效。
- 在丢失轨迹匹配阶段新增 `reactivated_det_assignments` 向量，收集重激活匹配对应的 `(原始检测索引, track_id)`。
- 在赋值块中增加对 `reactivated_det_assignments` 的遍历，补全丢失轨迹重激活的 ID 赋值。
- 更新 `.gitignore`，排除视频文件、Python 缓存、临时文件等，防止 VPS 与 GPU 服务器之间因中间文件导致同步混乱。

**关键教训**: 在 STL 容器上做 `erase(remove_if(...))` 后，所有之前存储的下标立即失效。涉及下标的操作必须在 erase 之前完成，或改用不依赖下标的方式（如 ID 查找）。

## 28. 人框偏大：1024 模型实际仍按 640 输入运行（前端配置覆盖后端默认值）
**问题**:
- 切换到 `models/3class410.onnx`（训练/导出尺寸 1024）后，`person` 类检测框明显偏大；
- Python 侧推理正常，但 C++ 服务端表现异常；
- 即使前端采样宽度改到 1024，GPU 服务器日志里仍显示：
  - `Loaded model 'models/3class410.onnx' (input: 640x640, GPU)`

**原因**:
1. **前端最初采样过小**：
   - `test_v5.html` 仍用 `CAPTURE_MAX_WIDTH=640`；
   - 模块化前端 `frontend/assets/js/inference/app.js` 也只采到 `960`；
   - 对 `person` 这种小目标，先缩小再送去 1024 模型会进一步放大小框误差。

2. **真正根因在后端配置合并逻辑**：
   - WebSocket 请求头会带 `config.yolo = { confidence, nms_threshold }`；
   - `WebSocketServer::build_pipeline_config()` 之前使用：
     - `effective_config.merge_patch(overrides);`
   - 这会把服务端默认的整个 `yolo` 对象替换掉，而不仅仅覆盖前端传来的那两个字段；
   - 结果 `input_width` / `input_height` / `model_path` 等默认项被冲掉；
   - `YoloDetector` 在动态输入 ONNX 下又回退到类内默认值 `640x640`，最终出现“模型是 1024，实际预处理还是 640”的错配。

**解决**:
- 前端采样统一提升到 `1024`：
  - `test_v5.html`
  - `frontend/assets/js/inference/app.js`
- 后端显式补齐默认输入尺寸：
  - `config/config.yaml` 中新增：
    - `yolo.input_width: 1024`
    - `yolo.input_height: 1024`
  - `src/main.cpp` runtime defaults 同步新增 `1024x1024`
- 修复后端配置合并逻辑：
  - 将 `effective_config.merge_patch(overrides)` 改为“按 section 递归 merge”；
  - 当前端只传 `yolo.confidence / nms_threshold` 时，不再覆盖掉 `yolo` 默认配置的其它字段。

**结果**:
- GPU 服务器启动日志中的 `Server config defaults` 已正确保留：
  - `input_width: 1024`
  - `input_height: 1024`
- 前后端都以 1024 路径运行，`person` 类检测框恢复正常。

**关键教训**:
- 对带嵌套对象的运行时配置，不能直接对顶层对象做 `merge_patch`，否则前端传一个小字段就可能把后端默认整段配置清空。
- 排查“模型换了但效果不对”时，要优先核对**实际运行时输入尺寸**，不要只看导出脚本和模型文件名。
