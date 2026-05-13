#!/bin/bash
# MySQL 8 安装 + TrackFlow 数据库初始化
# 在 GPU WSL 上以 xf 用户身份跑: bash scripts/setup_mysql.sh
# 幂等: 重复运行不会破坏已有数据

set -e

XF_PASS="${XF_PASS:-123456xf}"     # WSL 用户 sudo 密码
MYSQL_ROOT_PASS="${MYSQL_ROOT_PASS:-123456xf}"
APP_USER="trackflow"
APP_PASS="${TRACKFLOW_DB_PASS:-trackflow_dev_2026}"
APP_DB="trackflow"

echo "=== [1/5] apt update + install mysql-server ==="
if ! dpkg -l | grep -q '^ii  mysql-server '; then
  export DEBIAN_FRONTEND=noninteractive
  echo "$XF_PASS" | sudo -S apt-get update -y
  echo "$XF_PASS" | sudo -S apt-get install -y mysql-server mysql-client libmysqlclient-dev
else
  echo "  mysql-server 已装, 跳过"
fi

echo "=== [2/5] 启动 mysql 服务 ==="
echo "$XF_PASS" | sudo -S service mysql start || true
echo "$XF_PASS" | sudo -S service mysql status | head -3

echo "=== [3/5] 设置 root 密码 + 允许 mysql_native_password ==="
# 在 Ubuntu 22.04+ 上, root 默认 auth_socket 登录, 无需密码
echo "$XF_PASS" | sudo -S mysql -uroot <<SQL
ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY '${MYSQL_ROOT_PASS}';
FLUSH PRIVILEGES;
SQL

echo "=== [4/5] 创建数据库 + 应用账号 ==="
mysql -uroot -p"${MYSQL_ROOT_PASS}" <<SQL
CREATE DATABASE IF NOT EXISTS \`${APP_DB}\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER IF NOT EXISTS '${APP_USER}'@'localhost' IDENTIFIED BY '${APP_PASS}';
GRANT ALL PRIVILEGES ON \`${APP_DB}\`.* TO '${APP_USER}'@'localhost';
FLUSH PRIVILEGES;
SQL

echo "=== [5/5] 执行 schema 初始化 ==="
SQL_FILE="$(dirname "$0")/../sql/001_init.sql"
if [ ! -f "$SQL_FILE" ]; then
  echo "ERROR: 找不到 $SQL_FILE"
  exit 1
fi
mysql -u"${APP_USER}" -p"${APP_PASS}" "${APP_DB}" < "$SQL_FILE"

echo
echo "=== 验证 ==="
mysql -u"${APP_USER}" -p"${APP_PASS}" "${APP_DB}" -e "SHOW TABLES;"
mysql -u"${APP_USER}" -p"${APP_PASS}" "${APP_DB}" -e "SELECT * FROM scene_memory;"

echo
echo "完成. 数据库连接信息 (供 yolo_edge_server / Agent 使用):"
echo "  host=127.0.0.1  port=3306  user=${APP_USER}  password=${APP_PASS}  db=${APP_DB}"
