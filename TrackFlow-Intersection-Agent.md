# TrackFlow-Intersection Agent 实施规划

> 本文档是 TrackFlow 上层"交通分析 Agent"的工程实施方案。
> 与之前的概念稿不同，本版本面向**逐步落地**，每个阶段都写清楚：
> 做什么、怎么验证、怎么 commit。

---

## 0. 项目主线与边界

### 0.1 一句话定位

```text
TrackFlow（C++）负责实时检测、跟踪、几何变换；
TrafficAnalyzer（C++ pipeline 末层）把轨迹转成结构化交通事件并写 MySQL；
Agent 服务（Python，独立进程）只读 MySQL + RAG，对外提供自然语言问答。
```

### 0.2 分层契约

```text
┌──────── C++ yolo_edge_server (现有, 增 TrafficAnalyzer) ─────────┐
│  ImageDecoder → Undistort? → YOLO → ByteTracker → GeoTransformer? │
│        → TrafficAnalyzer (新) → EventWriter (新, 异步)             │
└───────────────────────────────┬───────────────────────────────────┘
                                │ 写
                                ▼
                         ┌──────────────┐
                         │   MySQL      │
                         └─────┬────────┘
                               │ 只读
                               ▼
                ┌──────────────────────────────────┐
                │  Agent 服务 (Python, FastAPI)     │
                │  /chat   /report   /memory        │
                │  内部: SQL 工具 + RAG + 主循环    │
                └──────────────────────────────────┘
                               │ HTTP
                               ▼
                       前端 inference.html Agent 标签页
```

**关键约定**：

- C++ 端**只写 MySQL**，不调 Agent。
- Agent 服务**只读 MySQL**（写报告除外），不直连 C++。
- 前端调 Agent 服务的 HTTP 接口，不直接读 MySQL。
- 三层用 schema 作为契约，任意一层重写都不影响另外两层。

### 0.3 技术栈（已敲定）

| 部分 | 选型 | 备注 |
|------|------|------|
| 实时管线 | C++ (现有) | 21 FPS, batch 推理 |
| 数据库 | **MySQL 8** | 单实例, 单机部署 |
| 几何计算 | C++ 应用层 | 不用 PostGIS，ROI/point-in-polygon 内存做 |
| Agent 框架 | **手搓 ReAct 循环** | 不用 LangGraph/LangChain |
| LLM SDK | **OpenAI Python SDK** | base_url 切换适配 DeepSeek/Qwen 等 |
| 主用模型 | **DeepSeek-V3** (`deepseek-chat`) | 便宜，中文好，支持 function calling |
| 备用模型 | OpenAI GPT-4o / Anthropic Claude | 演示/对照用 |
| RAG 向量库 | Chroma (本地持久化) | SQLite 风格，零运维 |
| Embedding | bge-small-zh (本地) 或 OpenAI text-embedding-3-small | 默认本地 |
| Web 服务 | FastAPI + uvicorn | 一个 /chat 接口起步 |
| 前端集成 | inference.html 已加 Agent 标签页 | 通过 HTTP 调 Python |

### 0.4 总体里程碑

```text
M1: TrafficAnalyzer C++ 模块 (ROI + 事件) + MySQL schema 建表
M2: 转向 / 流量 / 排队 三类事件落库
M3: Python Agent 服务骨架 + 3 个 SQL 工具
M4: Scene Memory + DeepSeek 接入 + 前端 chat 联通
M5: RAG 接入 (规范速查手册) + report 生成
M6: 错误案例 + 性能 benchmark + 简历素材整理
```

每个里程碑结束做一次严格验证 + git commit + push。

---

## 1. M1: 数据库与事件总线

### 1.1 目标

- MySQL schema 建表，C++ 能写、Python 能读
- C++ 端建立 EventWriter 后台线程
- TrafficAnalyzer 骨架（先空实现，跑通管线注册）

### 1.2 步骤

#### Step 1.2.1: 部署 MySQL

```bash
sudo apt install mysql-server
sudo mysql_secure_installation
mysql -u root -p
> CREATE DATABASE trackflow CHARACTER SET utf8mb4;
> CREATE USER 'trackflow'@'%' IDENTIFIED BY '<密码>';
> GRANT ALL ON trackflow.* TO 'trackflow'@'%';
> FLUSH PRIVILEGES;
```

**验证**：本机能 `mysql -u trackflow -p trackflow` 登录。

#### Step 1.2.2: schema 初始化脚本

文件：`sql/001_init.sql`

```sql
-- 一次性建表脚本, 后续迁移用 002_*.sql 累加
CREATE TABLE IF NOT EXISTS roi_configs (
  id           VARCHAR(64)  PRIMARY KEY,
  intersection_id VARCHAR(32) NOT NULL,
  camera_id    VARCHAR(32) NOT NULL DEFAULT 'default',
  type         ENUM('approach','exit','lane','crosswalk','center','stop_line') NOT NULL,
  parent_id    VARCHAR(64) NULL,
  polygon_json JSON NOT NULL,            -- [[x,y],[x,y],...] 像素坐标
  allowed_movements JSON NULL,           -- ["straight","left","right"]
  extra        JSON NULL,
  created_at   DATETIME DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_intersection (intersection_id, camera_id)
);

CREATE TABLE IF NOT EXISTS tracks (
  id           BIGINT AUTO_INCREMENT PRIMARY KEY,
  intersection_id VARCHAR(32) NOT NULL,
  camera_id    VARCHAR(32) NOT NULL,
  track_id     INT NOT NULL,
  object_type  VARCHAR(32),
  start_time   DOUBLE,
  end_time     DOUBLE,
  start_frame  INT,
  end_frame    INT,
  avg_speed    FLOAT,
  max_speed    FLOAT,
  entry_region VARCHAR(64) NULL,
  exit_region  VARCHAR(64) NULL,
  entry_lane   VARCHAR(64) NULL,
  exit_lane    VARCHAR(64) NULL,
  movement     VARCHAR(16) NULL,
  trajectory_json JSON NULL,             -- 关键采样点, 不存逐帧
  extra        JSON NULL,
  INDEX idx_intersection_time (intersection_id, start_time),
  INDEX idx_track (intersection_id, camera_id, track_id)
);

CREATE TABLE IF NOT EXISTS vehicle_events (
  id           BIGINT AUTO_INCREMENT PRIMARY KEY,
  intersection_id VARCHAR(32) NOT NULL,
  track_id     INT NOT NULL,
  event_type   VARCHAR(32) NOT NULL,     -- 'movement' / 'queue' / 'stop' / 'violation'
  start_time   DOUBLE,
  end_time     DOUBLE,
  entry_region VARCHAR(64) NULL,
  exit_region  VARCHAR(64) NULL,
  entry_lane   VARCHAR(64) NULL,
  movement     VARCHAR(16) NULL,
  is_lane_violation TINYINT(1) DEFAULT 0,
  stop_duration FLOAT NULL,
  extra        JSON NULL,
  INDEX idx_intersection_type_time (intersection_id, event_type, start_time)
);

CREATE TABLE IF NOT EXISTS flow_stats (
  id           BIGINT AUTO_INCREMENT PRIMARY KEY,
  intersection_id VARCHAR(32) NOT NULL,
  time_bucket  DATETIME NOT NULL,        -- 5 分钟桶
  approach     VARCHAR(32),
  movement     VARCHAR(16),
  vehicle_count INT DEFAULT 0,
  pedestrian_count INT DEFAULT 0,
  UNIQUE KEY uniq_bucket (intersection_id, time_bucket, approach, movement)
);

CREATE TABLE IF NOT EXISTS queue_stats (
  id           BIGINT AUTO_INCREMENT PRIMARY KEY,
  intersection_id VARCHAR(32) NOT NULL,
  time_bucket  DATETIME NOT NULL,
  approach     VARCHAR(32),
  lane_id      VARCHAR(64) NULL,
  avg_queue_length FLOAT,
  max_queue_length FLOAT,
  avg_wait_time FLOAT,
  queue_vehicle_count INT,
  INDEX idx_bucket (intersection_id, time_bucket)
);

CREATE TABLE IF NOT EXISTS conflict_events (
  id           BIGINT AUTO_INCREMENT PRIMARY KEY,
  intersection_id VARCHAR(32) NOT NULL,
  event_type   VARCHAR(32) NOT NULL,     -- 'vehicle_vehicle' / 'vehicle_pedestrian'
  timestamp    DOUBLE NOT NULL,
  track_a      INT,
  track_b      INT,
  risk_score   FLOAT,
  min_distance FLOAT NULL,               -- 米
  pet          FLOAT NULL,               -- 秒
  speed_a      FLOAT NULL,
  speed_b      FLOAT NULL,
  description  TEXT NULL,
  video_clip_path VARCHAR(255) NULL,
  extra        JSON NULL,
  INDEX idx_intersection_time (intersection_id, timestamp)
);

CREATE TABLE IF NOT EXISTS scene_memory (
  intersection_id VARCHAR(32) PRIMARY KEY,
  default_time_window VARCHAR(32),       -- "07:30-09:00"
  focus_metrics JSON NULL,
  known_patterns JSON NULL,
  last_summary TEXT NULL,
  last_summary_at DATETIME NULL,
  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS analysis_reports (
  id           BIGINT AUTO_INCREMENT PRIMARY KEY,
  intersection_id VARCHAR(32) NOT NULL,
  title        VARCHAR(255),
  content_md   MEDIUMTEXT,
  time_window  VARCHAR(64),
  created_at   DATETIME DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_intersection_time (intersection_id, created_at)
);
```

**验证**：

```bash
mysql -u trackflow -p trackflow < sql/001_init.sql
mysql -u trackflow -p -e "SHOW TABLES FROM trackflow;"
```

应当看到 8 张表。

#### Step 1.2.3: C++ MySQL 客户端依赖

选 `mysql-connector-cpp` 或更轻的 `mariadb-connector-c`：

```bash
sudo apt install libmysqlclient-dev
```

CMakeLists.txt 加：

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED mysqlclient)
target_link_libraries(yolo_edge_server PRIVATE ${MYSQL_LIBRARIES})
target_include_directories(yolo_edge_server PRIVATE ${MYSQL_INCLUDE_DIRS})
```

**验证**：CMake 重新构建无报错。

#### Step 1.2.4: EventWriter 后台线程

新文件：`include/core/event_writer.hpp` + `src/core/event_writer.cpp`

接口：

```cpp
namespace yolo_edge {
struct VehicleEventRow { /* 对应 vehicle_events */ };
struct ConflictEventRow { /* 对应 conflict_events */ };
struct TrackRow         { /* 对应 tracks */ };

class EventWriter {
public:
  static EventWriter& instance();
  void configure(const std::string& host, int port,
                 const std::string& user, const std::string& pass,
                 const std::string& db);
  void start();
  void stop();

  void enqueue_vehicle_event(VehicleEventRow);
  void enqueue_conflict_event(ConflictEventRow);
  void enqueue_track(TrackRow);

private:
  // 单生产多消费? 不需要, 1 个写线程足够
  std::thread worker_;
  ConcurrentQueue<...> queue_;
  // ...
};
}
```

**验证**：在 main.cpp 初始化时调 `EventWriter::instance().start()`，手动塞一条假事件，看 MySQL 里能查到。

#### Step 1.2.5: TrafficAnalyzer processor 骨架

新文件：`include/processors/traffic_analyzer.hpp` + `src/processors/traffic_analyzer.cpp`

继承 `ImageProcessor`，注册到 ProcessorFactory（type = `"traffic_analyzer"`）。

第一版只做：

```cpp
void process(ProcessingContext& ctx) override {
  // 第一版啥都不做, 只确认能挂上 pipeline
  ctx.set<std::string>("traffic_analyzer", "stub");
}
```

`config.yaml` 末尾追加（先不开）：

```yaml
# pipeline 默认不含 traffic_analyzer, 需要时显式加
# pipeline: [image_decoder, undistort, yolo_detector, byte_tracker, geo_transformer, traffic_analyzer]
```

**验证**：用一个 traffic_analyzer 开启的 config 启动服务，前端推流后看 log 是否有 stub 标记。

### 1.3 M1 验收清单

- [ ] MySQL 服务起来，trackflow 数据库 8 张表都存在
- [ ] C++ 编译链通过，依赖 libmysqlclient 不报错
- [ ] EventWriter 后台线程能从队列消费并写库（用单元/集成测试）
- [ ] TrafficAnalyzer 注册成功，能通过 config 启用
- [ ] 整个推理服务 21 FPS 性能没有下降（grep FPS log 或前端 stats 卡片）

### 1.4 M1 提交

```bash
git add sql/ include/core/event_writer.hpp src/core/event_writer.cpp \
        include/processors/traffic_analyzer.hpp src/processors/traffic_analyzer.cpp \
        CMakeLists.txt config/config.yaml
git commit -m "feat(m1): MySQL schema + EventWriter + TrafficAnalyzer skeleton

- sql/001_init.sql 8 张表 (roi_configs/tracks/vehicle_events/flow_stats/
  queue_stats/conflict_events/scene_memory/analysis_reports)
- C++ 端集成 libmysqlclient, EventWriter 异步写库
- TrafficAnalyzer 注册到 pipeline, 第一版空实现"
git push
```

---

## 2. M2: ROI + 转向 / 流量 / 排队事件

### 2.1 目标

填实 TrafficAnalyzer，让它能：
1. 加载 roi_configs 表里的 ROI
2. 每帧维护 per-track 区域归属
3. track 结束时生成 movement 事件
4. 每 5 分钟聚合 flow_stats / queue_stats

### 2.2 步骤

#### Step 2.2.1: ROI 加载与运行时缓存

```cpp
class RoiRegistry {
  // intersection_id -> list of {id, type, polygon, parent, allowed_movements}
  std::unordered_map<std::string, std::vector<RoiSpec>> rois_;
public:
  void reload_from_mysql();
  std::string locate_point(const std::string& intersection_id, double x, double y);
  // 返回最具体的 ROI id (车道 > 进口 > 中心)
};
```

**验证**：写一个 unit test，手动 INSERT 几条 roi_configs，调用 locate_point 判断像素点归属。

#### Step 2.2.2: TrackStateTable

`TrafficAnalyzer` 内部维护：

```cpp
struct TrackState {
  int track_id;
  std::string entry_region;
  std::string entry_lane;
  std::string current_region;
  std::deque<std::string> region_history;     // 多数投票用
  double start_time;
  double last_seen_time;
  std::vector<cv::Point2d> trajectory_samples;  // 稀疏采样, 不存逐帧
  double speed_sum = 0;
  int speed_count = 0;
  bool stationary = false;
  double stationary_start = -1;
};
```

每帧更新：

```cpp
for (det in ctx.detections) {
  auto& st = states_[det.track_id];
  std::string roi = roi_registry_.locate_point(intersection_id_, det.cx, det.cy);
  st.region_history.push_back(roi);
  if (st.region_history.size() > 30) st.region_history.pop_front();
  // 入口未定且窗口稳定 → 锁定 entry_region/lane
  // ...
}
```

#### Step 2.2.3: 转向判断（movement）

启动时根据 ROI 几何**自动**生成 movement_rules 表：

```cpp
// 在 RoiRegistry::reload_from_mysql 后调用
std::map<std::pair<string,string>, std::string> movement_rules_;
void infer_movement_rules() {
  cv::Point2d center = intersection_center_;
  for (auto& a : approach_rois_) {
    cv::Point2d a_flow = (center - centroid(a)) / norm(...);  // 进入方向
    for (auto& e : exit_rois_) {
      cv::Point2d e_flow = (centroid(e) - center) / norm(...);  // 离开方向
      double angle = signed_angle(a_flow, e_flow);
      std::string mv;
      if (fabs(angle) < 30)       mv = "straight";
      else if (angle > 60 && angle < 120)   mv = "right";
      else if (angle < -60 && angle > -120) mv = "left";
      else if (fabs(angle) > 150) mv = "u_turn";
      else                        mv = "other";
      movement_rules_[{a.id, e.id}] = mv;
    }
  }
}
```

**核心点**：用户**不需要写 movement_rules.json**，几何自动算。

#### Step 2.2.4: track 终结 → vehicle_event

当某 track 在 N 帧（如 60 帧）未出现 → 视为终结：

```cpp
void finalize_track(int track_id) {
  auto& st = states_[track_id];
  if (st.entry_region.empty() || st.current_region.empty()) {
    states_.erase(track_id);
    return;
  }
  VehicleEventRow row{
    .intersection_id = intersection_id_,
    .track_id = track_id,
    .event_type = "movement",
    .start_time = st.start_time,
    .end_time = st.last_seen_time,
    .entry_region = st.entry_region,
    .exit_region = st.current_region,
    .entry_lane = st.entry_lane,
    .movement = movement_rules_[{st.entry_region, st.current_region}],
    .stop_duration = st.stationary_total,
    .is_lane_violation = check_lane_violation(st),
  };
  EventWriter::instance().enqueue_vehicle_event(std::move(row));
  // 同时 enqueue_track(...) 写 tracks 表
  states_.erase(track_id);
}
```

#### Step 2.2.5: 流量聚合（5 分钟桶）

后台 timer 每 5 分钟跑一次：

```cpp
void TrafficAnalyzer::flush_stats() {
  auto bucket = floor_to_5min(now());
  // 从 vehicle_events 表查上一个 5min 桶的事件 → COUNT 入 flow_stats
  // 简化做法: 直接在内存累计一个 5min 计数器
}
```

简化版：在 TrafficAnalyzer 进程内维护 `unordered_map<bucket+approach+movement, count>`，每 5min 写一行 flow_stats。

#### Step 2.2.6: 排队检测

低速判定：

```cpp
bool is_low_speed = (st.recent_avg_speed < 0.5);  // m/s
bool in_approach = (st.current_region.rfind("_in") != string::npos);
if (is_low_speed && in_approach) {
  // 该 track 当前帧"在排队"
  queue_buckets_[st.entry_lane].push_back(track_id);
}
```

每 5 分钟把 `queue_buckets_` 聚合写 queue_stats。

### 2.3 M2 验收清单

- [ ] 用一段固定视频（30 分钟样例）跑完，MySQL 里 vehicle_events 应有 >100 行
- [ ] 手动 SQL: `SELECT movement, COUNT(*) FROM vehicle_events GROUP BY movement`，分布合理（不是全 unknown）
- [ ] `SELECT * FROM flow_stats ORDER BY time_bucket DESC LIMIT 20` 有数据
- [ ] 车道违规事件（is_lane_violation=1）不超过总量 10%（防止误判）
- [ ] FPS 仍保持 ≥ 20

### 2.4 M2 提交

```bash
git add -p src/processors/traffic_analyzer.cpp include/processors/traffic_analyzer.hpp
git commit -m "feat(m2): TrafficAnalyzer 实现转向/流量/排队事件

- RoiRegistry: 加载 ROI, point-in-polygon 查询
- TrackStateTable: per-track 区域投票 + 入口锁定
- 几何自动推 movement_rules (不需要用户写规则)
- track 结束写 vehicle_events; 5min 桶聚合 flow_stats/queue_stats"
git push
```

---

## 3. M3: Python Agent 服务骨架

### 3.1 目标

建立 Python 项目，跑通：
1. FastAPI `/chat` 接收问题
2. 手搓 ReAct 循环调 DeepSeek
3. 3 个 SQL 工具：`query_flow_stats`、`query_queue_stats`、`query_vehicle_events`
4. 前端 Agent 标签页能发送/收消息

### 3.2 项目结构

```text
agent/
├── pyproject.toml          # 或 requirements.txt
├── .env.example
├── app.py                  # FastAPI 入口
├── llm_client.py           # OpenAI SDK 抽象 (DeepSeek 切换)
├── agent_core.py           # 手搓 ReAct 主循环
├── tools/
│   ├── __init__.py
│   ├── db.py               # MySQL 连接池
│   ├── flow.py             # query_flow_stats
│   ├── queue.py            # query_queue_stats
│   └── events.py           # query_vehicle_events
├── prompts/
│   └── system.txt          # 系统 prompt
└── tests/
    ├── test_tools.py       # 工具层单元测试
    └── test_agent.py       # 端到端用 mock LLM
```

### 3.3 步骤

#### Step 3.3.1: 依赖与环境

`agent/requirements.txt`：

```text
fastapi==0.115.*
uvicorn[standard]==0.32.*
openai==1.54.*
pymysql==1.1.*
python-dotenv==1.0.*
pydantic==2.9.*
pytest==8.3.*
httpx==0.27.*
```

`.env.example`：

```text
DEEPSEEK_API_KEY=
OPENAI_API_KEY=
LLM_PROVIDER=deepseek
LLM_MODEL=deepseek-chat
MYSQL_HOST=127.0.0.1
MYSQL_PORT=3306
MYSQL_USER=trackflow
MYSQL_PASSWORD=
MYSQL_DB=trackflow
```

#### Step 3.3.2: LLM 客户端封装

```python
# agent/llm_client.py
from openai import OpenAI
from dataclasses import dataclass
import os

PROVIDER_BASE_URLS = {
    "openai":   None,
    "deepseek": "https://api.deepseek.com",
    "qwen":     "https://dashscope.aliyuncs.com/compatible-mode/v1",
}

@dataclass
class LLMConfig:
    provider: str
    model:    str
    api_key:  str

def make_client(cfg: LLMConfig):
    base_url = PROVIDER_BASE_URLS.get(cfg.provider)
    return OpenAI(api_key=cfg.api_key, base_url=base_url)
```

#### Step 3.3.3: 工具层（纯函数）

```python
# agent/tools/flow.py
from typing import Optional
import pymysql

def query_flow_stats(
    intersection_id: str,
    start_time: str,        # "2026-05-13 07:30:00"
    end_time: str,
    approach: Optional[str] = None,
    movement: Optional[str] = None,
) -> list[dict]:
    """查询指定时间段的车流量统计"""
    sql = """
      SELECT time_bucket, approach, movement,
             SUM(vehicle_count) AS vehicles,
             SUM(pedestrian_count) AS pedestrians
      FROM flow_stats
      WHERE intersection_id=%s AND time_bucket BETWEEN %s AND %s
    """
    params = [intersection_id, start_time, end_time]
    if approach: sql += " AND approach=%s"; params.append(approach)
    if movement: sql += " AND movement=%s"; params.append(movement)
    sql += " GROUP BY time_bucket, approach, movement ORDER BY time_bucket"
    with get_conn() as c, c.cursor(pymysql.cursors.DictCursor) as cur:
        cur.execute(sql, params)
        return cur.fetchall()
```

每个工具都对应一个 OpenAI tool schema：

```python
TOOL_SCHEMAS = [{
  "type": "function",
  "function": {
    "name": "query_flow_stats",
    "description": "查询指定路口某段时间的车流量统计, 可按进口/转向过滤",
    "parameters": {
      "type": "object",
      "properties": {
        "intersection_id": {"type": "string"},
        "start_time": {"type": "string", "description": "格式: YYYY-MM-DD HH:MM:SS"},
        "end_time":   {"type": "string"},
        "approach":   {"type": "string", "enum": ["north_in","south_in","east_in","west_in"]},
        "movement":   {"type": "string", "enum": ["straight","left","right","u_turn"]},
      },
      "required": ["intersection_id", "start_time", "end_time"],
    }
  }
}]
```

#### Step 3.3.4: 手搓 ReAct 主循环

```python
# agent/agent_core.py
def run_agent(question: str, intersection_id: str = "A001") -> dict:
    messages = [
        {"role": "system", "content": SYSTEM_PROMPT.format(intersection_id=intersection_id)},
        {"role": "user", "content": question},
    ]
    trace = []  # 留给前端展示工具调用过程
    for _ in range(MAX_STEPS):
        resp = llm.chat.completions.create(
            model=cfg.model,
            messages=messages,
            tools=TOOL_SCHEMAS,
            tool_choice="auto",
        )
        msg = resp.choices[0].message
        messages.append(msg.model_dump(exclude_none=True))
        if not msg.tool_calls:
            return {"answer": msg.content, "trace": trace}
        for tc in msg.tool_calls:
            args = json.loads(tc.function.arguments)
            result = TOOL_REGISTRY[tc.function.name](**args)
            trace.append({"tool": tc.function.name, "args": args, "result_preview": str(result)[:200]})
            messages.append({
                "role": "tool",
                "tool_call_id": tc.id,
                "content": json.dumps(result, default=str, ensure_ascii=False),
            })
    return {"answer": "工具调用次数已达上限", "trace": trace}
```

#### Step 3.3.5: FastAPI 入口

```python
# agent/app.py
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
from agent_core import run_agent

app = FastAPI()
app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_methods=["*"], allow_headers=["*"])

class ChatReq(BaseModel):
    question: str
    intersection_id: str = "A001"

@app.post("/chat")
def chat(req: ChatReq):
    return run_agent(req.question, req.intersection_id)
```

启动：`uvicorn agent.app:app --host 0.0.0.0 --port 9100`

#### Step 3.3.6: nginx 路由

`/etc/nginx/sites-available/trackflow` 加：

```nginx
location /agent/ {
    proxy_pass http://127.0.0.1:9100/;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
}
```

前端通过 `fetch('/agent/chat', ...)` 调用。

#### Step 3.3.7: 前端 chat JS

新文件：`frontend/assets/js/inference/chat.js`

```javascript
const messages = []
const $msgs = document.getElementById('chatMessages')
const $input = document.getElementById('chatInput')
const $send  = document.getElementById('chatSendBtn')
const $status = document.getElementById('chatStatus')

async function ping() {
  try {
    const r = await fetch('/agent/health')
    if (r.ok) { $status.textContent = 'Online'; $status.classList.add('online'); enable() }
  } catch {}
}

function enable() {
  $input.disabled = false
  $send.disabled  = false
  if (messages.length === 0) $msgs.innerHTML = ''
}

function appendMsg(role, text) {
  const div = document.createElement('div')
  div.className = `chat-msg ${role}`
  div.innerHTML = `<div class="chat-bubble"></div>`
  div.querySelector('.chat-bubble').textContent = text
  $msgs.appendChild(div)
  $msgs.scrollTop = $msgs.scrollHeight
}

async function send() {
  const q = $input.value.trim()
  if (!q) return
  appendMsg('user', q)
  $input.value = ''
  const resp = await fetch('/agent/chat', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({question: q, intersection_id: 'A001'}),
  })
  const data = await resp.json()
  appendMsg('assistant', data.answer)
}

$send.onclick = send
$input.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); send() }
})

ping()
setInterval(ping, 10000)
```

`inference.html` 末尾追加：

```html
<script type="module" src="./assets/js/inference/chat.js"></script>
```

### 3.4 M3 验收清单

- [ ] Python 服务启动，`curl localhost:9100/health` 返回 ok
- [ ] 手动 `curl -X POST localhost:9100/chat -d '{"question":"今天有多少辆车?"}'` 能返回 LLM 响应（DeepSeek 计费正常）
- [ ] LLM 至少调用一次 `query_flow_stats` 工具，trace 字段能看到
- [ ] 前端 Agent 标签页：状态变 Online，输入问题后能看到 user 气泡 + assistant 气泡
- [ ] tests/test_tools.py 至少 5 个测试通过

### 3.5 M3 提交

```bash
git add agent/ frontend/assets/js/inference/chat.js frontend/inference.html
git commit -m "feat(m3): Python Agent 服务 + 3 SQL 工具 + 前端对话联通

- agent/ 目录: FastAPI + 手搓 ReAct 主循环 + DeepSeek
- 3 个 SQL 工具: query_flow_stats / queue_stats / vehicle_events
- 前端 chat.js: /agent/chat HTTP 调用, 用户/助手气泡渲染
- nginx 加 /agent/ 反代到 :9100"
git push
```

---

## 4. M4: Scene Memory + 多轮对话

### 4.1 目标

- Agent 跨会话记住路口偏好和长期摘要
- 用户问"今天怎么样"时，自动用默认时间窗 + 关注指标
- 多轮对话保留上下文

### 4.2 步骤

#### Step 4.2.1: Memory 工具

```python
# agent/tools/memory.py
def get_scene_memory(intersection_id: str) -> dict:
    """读取场景记忆"""
    sql = "SELECT * FROM scene_memory WHERE intersection_id=%s"
    ...

def update_scene_memory(intersection_id: str, **fields) -> dict:
    """UPSERT 场景记忆"""
    ...
```

加入 TOOL_REGISTRY 和 TOOL_SCHEMAS。

#### Step 4.2.2: 系统 Prompt 调整

```text
你是 TrackFlow 交叉口分析助手。每次回答前必须先调 get_scene_memory(intersection_id) 了解：
- 用户默认时间段
- 用户关注的指标
- 已知的长期问题

回答模糊时间词（"今天"、"早高峰"）时, 使用 memory 中的 default_time_window。
回答结束后, 如果用户表达了新的偏好（"我以后主要看晚高峰"）, 调 update_scene_memory 持久化。
```

#### Step 4.2.3: 会话状态

简单方案：前端在 localStorage 存最近 N 轮对话，发送时一并带上：

```javascript
fetch('/agent/chat', { body: JSON.stringify({
  question: q,
  intersection_id: 'A001',
  history: messages.slice(-6),   // 最近 3 轮
})})
```

后端把 history 拼到 messages 数组前。**不需要在后端持久化会话**——把状态留在前端，省事。

### 4.3 M4 验收清单

- [ ] 第一次问"流量怎么样"，Agent 调 get_scene_memory，发现 default_time_window 为空，反问用户
- [ ] 用户回"我主要看早高峰"，Agent 调 update_scene_memory 设置 default_time_window="07:30-09:00"
- [ ] 新会话直接问"今天怎么样"，Agent 自动用 07:30-09:00 时间窗（说明 memory 生效）
- [ ] history 字段在 2-3 轮对话内能维持上下文

### 4.4 M4 提交

```bash
git commit -m "feat(m4): Scene Memory + 多轮上下文"
```

---

## 5. M5: RAG 接入（规范速查手册）

### 5.1 目标

- 用户问"标准是什么 / 怎么改善"时，Agent 召回交通工程速查手册
- 不依赖扫描版规范 PDF，**用自己整理的精简 markdown**

### 5.2 步骤

#### Step 5.2.1: 写速查手册

新建 `agent/knowledge/`：

```text
agent/knowledge/
├── queue_thresholds.md       # 排队长度判定阈值
├── conflict_safety.md        # 冲突安全 / TTC 阈值
├── signal_design.md          # 信号配时基本原则
├── lane_function.md          # 车道功能与渠化
└── glossary.md               # 名词解释
```

每份 1-3 页，markdown 结构化（标题 + 列表）。

#### Step 5.2.2: 切片与入库

```python
# agent/rag/build_index.py
from chromadb import PersistentClient
from sentence_transformers import SentenceTransformer

embedder = SentenceTransformer("BAAI/bge-small-zh-v1.5")
client = PersistentClient(path="./agent/rag/chroma_db")
coll = client.get_or_create_collection("traffic_kb")

def chunk_markdown(text, size=400, overlap=80):
    # 按段落切, 同段落超长再按句切
    ...

for path in Path("agent/knowledge").glob("*.md"):
    text = path.read_text()
    chunks = chunk_markdown(text)
    embeddings = embedder.encode(chunks).tolist()
    coll.add(
        ids=[f"{path.stem}_{i}" for i in range(len(chunks))],
        documents=chunks,
        embeddings=embeddings,
        metadatas=[{"source": path.name}] * len(chunks),
    )
```

#### Step 5.2.3: RAG 工具

```python
def query_traffic_standards(query: str, top_k: int = 3) -> list[dict]:
    """
    检索交通工程规范和速查手册。
    适用场景: 解释指标含义、引用阈值依据、给出改进建议。
    不适用: 查询本路口的具体数据。
    """
    emb = embedder.encode([query]).tolist()
    res = coll.query(query_embeddings=emb, n_results=top_k)
    return [{"source": m["source"], "content": d}
            for m, d in zip(res["metadatas"][0], res["documents"][0])]
```

#### Step 5.2.4: 报告生成工具

```python
def generate_report(intersection_id: str, time_window: str) -> dict:
    """
    生成一份早高峰 / 晚高峰 / 自定义时段的运行分析报告。
    返回 markdown 内容并自动保存到 analysis_reports 表。
    """
    # 内部调多个 SQL 工具 + RAG, 拼成长 prompt, 让 LLM 写报告
    ...
```

### 5.3 M5 验收清单

- [ ] `agent/knowledge/` 至少 4 份手册，总计 200+ chunk 入库
- [ ] 问"排队 80 米严重吗"时, Agent 至少调一次 query_traffic_standards，answer 引用规范
- [ ] 问"生成早高峰报告"，返回 markdown，且 `analysis_reports` 表新增一行
- [ ] RAG 召回 top-3 chunk 与问题相关性人工检查 ≥ 80%

### 5.4 M5 提交

```bash
git commit -m "feat(m5): RAG 速查手册接入 + 报告生成"
```

---

## 6. M6: 验证 / 性能 / 简历素材

### 6.1 目标

- 错误案例集（10 例）
- 性能 benchmark 文档
- 答案质量评估测试集（20 条 QA）

### 6.2 步骤

#### 6.2.1: 错误案例集

新文件 `docs/error_cases.md`，至少 10 例：

- ID switch 导致同一辆车被算成两辆
- 大目标 OBB 中心偏离车道边界
- LLM 把"早高峰"理解成 06:00-09:00（实际应是 07:30-09:00）
- 等等

每例：截图 + 现象 + 原因 + 改进。

#### 6.2.2: 性能 benchmark

新文件 `docs/performance.md`：

| 配置 | FPS | 备注 |
|------|-----|------|
| 基线（仅 YOLO） | X | |
| + ByteTracker | Y | |
| + TrafficAnalyzer | Z | |
| + EventWriter | W | |

#### 6.2.3: 答案评估测试集

`agent/eval/qa_dataset.json`：

```json
[
  {
    "question": "北进口最大排队长度多少?",
    "expected_tools": ["query_queue_stats"],
    "expected_keywords": ["北进口", "米"]
  },
  ...
]
```

写一个 `agent/eval/run.py` 跑全集，输出准确率。

### 6.3 M6 验收清单

- [ ] `docs/error_cases.md` ≥ 10 例
- [ ] `docs/performance.md` 含至少 4 行 benchmark 对比
- [ ] 评估测试集运行准确率 ≥ 70%

### 6.4 M6 提交

```bash
git commit -m "docs(m6): 错误案例集 + 性能 benchmark + 评估测试集"
```

---

## 7. 验证与回归原则

### 7.1 每个 Step 完成后

- 跑相关单元测试 / 集成测试
- 手动验证关键路径（启动服务，调一次 /chat）
- 检查 FPS 没有下降

### 7.2 每个 Milestone 完成后

- 跑评估测试集
- 完整跑一遍 30 分钟视频
- 检查 MySQL 数据正确（条数级别 + 抽样人工核对）
- git add + commit + push（按 CLAUDE.md 单源仓库规则）

### 7.3 出 bug 时

按 CLAUDE.md 规则 2：把问题、原因、修复方案追加到 `debug.md`。

### 7.4 分支策略

- `main`：始终保持可演示状态
- `feature/agent`：本规划文档落地的所有改动先在这条分支上做
- 每个 Milestone 完成且通过验收 → 合并到 main
- 不在 main 上直接 commit Agent 功能代码

---

## 8. 接下来的动作

按当前位置，下一步应在 **feature/agent** 分支上启动 **M1**：

```bash
git checkout -b feature/agent
mkdir -p sql agent
# 开始 Step 1.2.1 ...
```

---

## 附录 A: 简历讲法（精简版）

### A.1 综合一段

```text
TrackFlow-Intersection Agent: 实时交通视频分析 + 自然语言查询系统

底层用 C++ 实现 21 FPS 的 YOLO OBB 检测、ByteTracker 多目标跟踪、单应矩阵
地面映射, 在 pipeline 末层加 TrafficAnalyzer 模块完成 ROI 归属判断、转向
事件提取、排队/流量统计, 异步写入 MySQL。

上层用 Python + FastAPI 实现交通分析 Agent: 手搓 ReAct 主循环 (不用
LangChain/LangGraph), 通过 OpenAI SDK 切换 DeepSeek/GPT/Claude, 工具层
包含 SQL 查询 + Chroma 向量库 RAG (规范速查手册) + Scene Memory。

强调分层数据策略: 实时检测内存缓冲不持久化, 语义事件入 MySQL, 领域知识
入向量库, 用户偏好入 Memory 表, 避免常见 Agent 项目"数据耦合"反模式。
```

### A.2 面试关键 talking points

1. **为什么不用 LangGraph**: 工作流简单, 手搓 ReAct 200 行能讲清每个细节
2. **为什么不用 PostGIS**: 几何计算放 C++ 应用层, 数据库通用性优先
3. **为什么用 DeepSeek**: 中文好, 便宜 10x, OpenAI SDK 兼容
4. **RAG 才几十 chunk 够吗**: 垂直领域 + 自整理 markdown > 几十万 PDF
5. **PET 不作为冲突主指标**: ByteTracker ID switch 会让 PET 算错
6. **21 FPS 怎么来的**: batch=6 + 异步 decode/infer + tracker 串行序号保证

---

## 附录 B: 关键文件清单

```text
sql/001_init.sql                                 ← M1
include/core/event_writer.hpp                    ← M1
src/core/event_writer.cpp                        ← M1
include/processors/traffic_analyzer.hpp          ← M1-M2
src/processors/traffic_analyzer.cpp              ← M1-M2
agent/app.py                                     ← M3
agent/llm_client.py                              ← M3
agent/agent_core.py                              ← M3-M4
agent/tools/{flow,queue,events,memory}.py        ← M3-M4
agent/rag/build_index.py                         ← M5
agent/knowledge/*.md                             ← M5
frontend/assets/js/inference/chat.js             ← M3
docs/error_cases.md                              ← M6
docs/performance.md                              ← M6
agent/eval/qa_dataset.json                       ← M6
```
