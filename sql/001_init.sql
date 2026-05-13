-- TrackFlow Agent 数据库初始化脚本
-- 用法: mysql -u trackflow -p trackflow < sql/001_init.sql
-- 所有改动通过新增 002_*.sql / 003_*.sql 实现, 本文件创建后不再修改

SET NAMES utf8mb4;
SET time_zone = '+08:00';

-- ============================================================
-- ROI 配置 (前端标注产物, 后端运行时只读加载)
-- ============================================================
CREATE TABLE IF NOT EXISTS roi_configs (
  id                VARCHAR(64)  NOT NULL,
  intersection_id   VARCHAR(32)  NOT NULL,
  camera_id         VARCHAR(32)  NOT NULL DEFAULT 'default',
  type              ENUM('approach','exit','lane','crosswalk','center','stop_line','conflict') NOT NULL,
  parent_id         VARCHAR(64)  NULL COMMENT '车道指向其所属进口/出口 ROI',
  polygon_json      JSON         NOT NULL COMMENT '[[x,y],[x,y],...] 像素坐标',
  allowed_movements JSON         NULL COMMENT '["straight","left","right","u_turn"]',
  extra             JSON         NULL,
  created_at        DATETIME     DEFAULT CURRENT_TIMESTAMP,
  updated_at        DATETIME     DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_intersection (intersection_id, camera_id),
  KEY idx_type (type)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- 轨迹总结 (每个 track 终结时写一行)
-- ============================================================
CREATE TABLE IF NOT EXISTS tracks (
  id              BIGINT       NOT NULL AUTO_INCREMENT,
  intersection_id VARCHAR(32)  NOT NULL,
  camera_id       VARCHAR(32)  NOT NULL DEFAULT 'default',
  track_id        INT          NOT NULL,
  object_type     VARCHAR(32)  NOT NULL,
  start_time      DOUBLE       NOT NULL COMMENT '视频/会话内秒数',
  end_time        DOUBLE       NOT NULL,
  start_frame     INT          NULL,
  end_frame       INT          NULL,
  avg_speed       FLOAT        NULL COMMENT 'm/s, 需要 geo_transform 才有意义',
  max_speed       FLOAT        NULL,
  entry_region    VARCHAR(64)  NULL,
  entry_lane      VARCHAR(64)  NULL,
  exit_region     VARCHAR(64)  NULL,
  exit_lane       VARCHAR(64)  NULL,
  movement        VARCHAR(16)  NULL COMMENT 'straight/left/right/u_turn/other',
  trajectory_json JSON         NULL COMMENT '稀疏采样点, 不存逐帧',
  extra           JSON         NULL,
  PRIMARY KEY (id),
  KEY idx_intersection_time (intersection_id, start_time),
  KEY idx_track (intersection_id, camera_id, track_id),
  KEY idx_movement (intersection_id, movement)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- 车辆事件 (转向 / 排队 / 停车 / 违规)
-- ============================================================
CREATE TABLE IF NOT EXISTS vehicle_events (
  id                BIGINT       NOT NULL AUTO_INCREMENT,
  intersection_id   VARCHAR(32)  NOT NULL,
  track_id          INT          NOT NULL,
  event_type        VARCHAR(32)  NOT NULL COMMENT 'movement / queue / stop / violation',
  start_time        DOUBLE       NOT NULL,
  end_time          DOUBLE       NOT NULL,
  entry_region      VARCHAR(64)  NULL,
  exit_region       VARCHAR(64)  NULL,
  entry_lane        VARCHAR(64)  NULL,
  movement          VARCHAR(16)  NULL,
  is_lane_violation TINYINT(1)   NOT NULL DEFAULT 0,
  stop_duration     FLOAT        NULL COMMENT '秒',
  avg_speed         FLOAT        NULL,
  extra             JSON         NULL,
  PRIMARY KEY (id),
  KEY idx_intersection_type_time (intersection_id, event_type, start_time),
  KEY idx_track (intersection_id, track_id),
  KEY idx_violation (intersection_id, is_lane_violation)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- 流量统计 (5 分钟桶)
-- ============================================================
CREATE TABLE IF NOT EXISTS flow_stats (
  id               BIGINT       NOT NULL AUTO_INCREMENT,
  intersection_id  VARCHAR(32)  NOT NULL,
  time_bucket      DATETIME     NOT NULL,
  approach         VARCHAR(32)  NOT NULL DEFAULT '',
  movement         VARCHAR(16)  NOT NULL DEFAULT '',
  vehicle_count    INT          NOT NULL DEFAULT 0,
  pedestrian_count INT          NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uniq_bucket (intersection_id, time_bucket, approach, movement),
  KEY idx_time (intersection_id, time_bucket)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- 排队统计 (5 分钟桶)
-- ============================================================
CREATE TABLE IF NOT EXISTS queue_stats (
  id                  BIGINT       NOT NULL AUTO_INCREMENT,
  intersection_id     VARCHAR(32)  NOT NULL,
  time_bucket         DATETIME     NOT NULL,
  approach            VARCHAR(32)  NOT NULL DEFAULT '',
  lane_id             VARCHAR(64)  NULL,
  avg_queue_length    FLOAT        NULL COMMENT '米',
  max_queue_length    FLOAT        NULL,
  avg_wait_time       FLOAT        NULL COMMENT '秒',
  queue_vehicle_count INT          NULL,
  PRIMARY KEY (id),
  KEY idx_bucket (intersection_id, time_bucket),
  KEY idx_approach (intersection_id, approach, time_bucket)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- 冲突事件 (右转-行人 / 车-车交叉)
-- ============================================================
CREATE TABLE IF NOT EXISTS conflict_events (
  id              BIGINT       NOT NULL AUTO_INCREMENT,
  intersection_id VARCHAR(32)  NOT NULL,
  event_type      VARCHAR(32)  NOT NULL COMMENT 'vehicle_vehicle / vehicle_pedestrian',
  timestamp       DOUBLE       NOT NULL,
  track_a         INT          NULL,
  track_b         INT          NULL,
  risk_score      FLOAT        NULL COMMENT '0~1',
  min_distance    FLOAT        NULL COMMENT '米',
  pet             FLOAT        NULL COMMENT '秒, Post-Encroachment Time',
  speed_a         FLOAT        NULL,
  speed_b         FLOAT        NULL,
  description     TEXT         NULL,
  video_clip_path VARCHAR(255) NULL,
  extra           JSON         NULL,
  PRIMARY KEY (id),
  KEY idx_intersection_time (intersection_id, timestamp),
  KEY idx_type (intersection_id, event_type, timestamp),
  KEY idx_risk (intersection_id, risk_score)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- 视频片段索引 (事件触发的回放切片)
-- ============================================================
CREATE TABLE IF NOT EXISTS video_clips (
  id               BIGINT       NOT NULL AUTO_INCREMENT,
  intersection_id  VARCHAR(32)  NOT NULL,
  clip_path        VARCHAR(255) NOT NULL,
  thumbnail_path   VARCHAR(255) NULL,
  start_time       DOUBLE       NOT NULL,
  end_time         DOUBLE       NOT NULL,
  related_event_id BIGINT       NULL,
  related_event_table VARCHAR(64) NULL COMMENT 'conflict_events / vehicle_events',
  created_at       DATETIME     DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_intersection_time (intersection_id, start_time)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- 场景记忆 (Agent 用; 每个路口一行)
-- ============================================================
CREATE TABLE IF NOT EXISTS scene_memory (
  intersection_id     VARCHAR(32)  NOT NULL,
  default_time_window VARCHAR(32)  NULL COMMENT '"07:30-09:00"',
  focus_metrics       JSON         NULL,
  known_patterns      JSON         NULL,
  last_summary        TEXT         NULL,
  last_summary_at     DATETIME     NULL,
  updated_at          DATETIME     DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (intersection_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- 分析报告 (Agent 生成的 markdown 报告)
-- ============================================================
CREATE TABLE IF NOT EXISTS analysis_reports (
  id              BIGINT       NOT NULL AUTO_INCREMENT,
  intersection_id VARCHAR(32)  NOT NULL,
  title           VARCHAR(255) NOT NULL,
  content_md      MEDIUMTEXT   NOT NULL,
  time_window     VARCHAR(64)  NULL,
  created_at      DATETIME     DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_intersection_time (intersection_id, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- 默认场景行 (避免 Agent 第一次查不到 memory 报错)
-- ============================================================
INSERT IGNORE INTO scene_memory (intersection_id, default_time_window, focus_metrics)
VALUES ('A001', '07:30-09:00', JSON_ARRAY('flow','queue','right_turn_pedestrian'));
