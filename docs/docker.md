# Docker 部署

本文说明如何构建、运行和升级 ais2api 容器镜像。镜像以 Debian 13 为运行时，内置 Debian Chromium，不包含 CloakBrowser，提供 `linux/amd64` 与 `linux/arm64` 两种架构。

## 构建

在仓库根目录执行：

```sh
docker build -t ais2api:local .
```

构建分为依赖层和源码层。依赖层按 `vcpkg.json` 的固定 baseline 安装；源码层编译程序并嵌入 Web 控制台。修改源码不会重新编译 vcpkg 依赖。构建脚本按目标架构选择 vcpkg triplet；镜像构建不重复运行 CTest，测试由现有 CI 覆盖。

默认按当前主机架构构建。发布流程使用 GitHub 的 amd64 与 arm64 原生 runner；在 x86_64 主机上交叉构建 arm64 需要 QEMU binfmt，编译在模拟下运行，耗时明显更长。

内存较小的主机可以限制并行编译：

```sh
docker build --build-arg BUILD_JOBS=2 -t ais2api:local .
```

在 x86_64 主机上构建 arm64 镜像时，先注册 QEMU，并使用 `docker-container` 驱动：

```sh
docker run --privileged --rm tonistiigi/binfmt --install arm64
docker buildx create --name ais2api-builder --driver docker-container --use
docker buildx build --platform linux/arm64 -t ais2api:arm64 --load .
```

原生 arm64 主机不需要 QEMU 或单独的 builder，直接执行 `docker build` 即可。

## 运行

```sh
docker run -d --name ais2api \
  -p 18451:18451 \
  -v "$PWD/data:/data" \
  ais2api:local
```

首次启动会在数据目录生成 `config.json`，并在容器日志中打印随机生成的管理密码：

```sh
docker logs ais2api 2>&1 | grep 'generated management password'
```

这个密码同时保存在 `config.json` 的 `admin_key` 中，用它登录 Web 控制台。打开 `data/config.json`，设置 API Key：

```json
{
  "api_key": "换成你自己的长随机字符串"
}
```

重启容器：

```sh
docker restart ais2api
```

浏览器访问 `http://127.0.0.1:18451/`。接口地址：

| 用途 | 地址 |
|---|---|
| Web 控制台 | `http://127.0.0.1:18451/` |
| Gemini 原生接口 | `http://127.0.0.1:18451/v1beta` |
| OpenAI 兼容接口 | `http://127.0.0.1:18451/v1` |

也可以使用仓库根目录的 `compose.yaml`：

```sh
docker compose up -d --build
```

### 管理密码

容器不使用原生安装的默认管理密码 `admin`。程序只允许本机和局域网来源使用默认密码，但在 Docker Desktop、rootless Docker 或经 `docker-proxy` 转发（例如 IPv6 客户端访问 IPv4 网桥）时，远端请求会以私有地址进入容器，这项限制无法区分来源。因此入口脚本在首次创建配置时：

- 未设置 `AIS2API_ADMIN_KEY`：生成 48 位十六进制随机密码写入 `admin_key`，并打印到容器日志；
- 已设置 `AIS2API_ADMIN_KEY`：使用该值。设为空字符串表示关闭 HTTP 管理。

已有的 `config.json` 不受影响。用旧镜像创建、仍是 `admin` 的配置，请在控制台修改密码或直接编辑 `admin_key`。

`-p 18451:18451` 会在宿主机所有网卡上开放端口。只在本机使用时，改为 `-p 127.0.0.1:18451:18451`（`compose.yaml` 中同理）。

## 数据目录

| 路径 | 内容 |
|---|---|
| `/data/config.json` | 配置，包含明文密钥 |
| `/data/state.sqlite3` | 账号、Cookie、冷却状态与旧版统计 |
| `/data/usage.sqlite3` | 请求记录、累计用量、价格缓存与已保存的模型清单 |
| `/data/management.log` | 管理日志 |
| `/data/logs/` | 浏览器日志 |
| `/data/runtime/` | 浏览器资料目录与下载缓存 |

`serve` 的请求日志写到标准输出，用 `docker logs ais2api` 查看。挂载 `/data` 后，删除并重建容器不会丢失账号和配置。

容器不读取原 Python 项目的数据目录，也没有自动迁移。旧账号需要转换成 `accounts import` 的凭据包后重新导入；旧的 `stats.json`、`rotator_state.json` 和 `config.yaml` 不会迁移。

## 配置

镜像设置 `AIS2API_BIND=0.0.0.0` 和 `AIS2API_DATA_DIR=/data`。这些值会覆盖配置文件中的同名字段；需要按配置文件监听时，移除对应环境变量。

| 变量 | 作用 |
|---|---|
| `AIS2API_BIND` | 监听地址，镜像默认为 `0.0.0.0` |
| `AIS2API_PORT` | 监听端口，默认 `18451` |
| `AIS2API_ADMIN_KEY` | 管理控制台密码；首次启动未设置时随机生成，见[管理密码](#管理密码) |
| `AIS2API_BROWSER_EXECUTABLE` | 浏览器可执行文件 |
| `AIS2API_PROXY` | 上游代理 |
| `AIS2API_HEADLESS` | 是否无界面运行，默认 `true` |
| `AIS2API_CONFIG_FILE` | 容器入口使用的配置文件路径，默认 `$AIS2API_DATA_DIR/config.json`，程序本身没有这个变量 |

`api_key` 和 `cookie_import_key` 只从配置文件读取。修改后重启容器，或在服务运行时执行：

```sh
docker exec ais2api /app/bin/ais2api config reload --config /data/config.json
```

健康检查请求 `/health`。通过 `AIS2API_PORT` 修改容器内端口时，健康检查会跟随；只修改配置文件中的端口时，需要同时覆盖镜像的健康检查。

## 浏览器

镜像内的浏览器是 Debian Chromium，路径 `/usr/bin/chromium`。程序支持其他 Chromium 构建，但 CloakBrowser 的指纹参数会被其他构建忽略；需要固定账号指纹时使用 CloakBrowser。挂载的 CloakBrowser 必须与容器架构一致。

CloakBrowser 的二进制许可不允许再分发，公开镜像不能包含它。已有 CloakBrowser 时，把目录只读挂载进容器并指定可执行文件：

```sh
docker run -d --name ais2api \
  -p 18451:18451 \
  -v "$PWD/data:/data" \
  -v /opt/cloakbrowser:/opt/cloakbrowser:ro \
  -e AIS2API_BROWSER_EXECUTABLE=/opt/cloakbrowser/chrome \
  ais2api:local
```

浏览器必须从 CloakHQ 官方渠道获取。不要把自己构建的含 CloakBrowser 镜像上传到公共仓库。

## 发布镜像

`.github/workflows/docker.yml` 在 amd64 与 arm64 原生 runner 上分别构建并运行冒烟测试，PR 只构建不推送。推送到 `main`、标签或手动触发时，先把各架构镜像推送到 `ghcr.io/mananekoha114/ais2api`，再合并为 multi-arch manifest。

首次推送后，在 GitHub 的 Packages 设置中把镜像可见性改为 public，否则匿名用户需要先 `docker login ghcr.io`。

## 升级

使用已发布镜像：

```sh
docker compose pull
docker compose up -d
```

使用本地构建：

```sh
docker build -t ais2api:local .
docker rm -f ais2api
docker run -d --name ais2api \
  -p 18451:18451 \
  -v "$PWD/data:/data" \
  ais2api:local
```

`/data` 保留配置、账号与统计。升级前先停止旧容器，让浏览器子进程正常退出。

## 限制

- 提供 `linux/amd64` 与 `linux/arm64` 镜像；原生 `scripts/build.sh linux` 仍只支持 x86_64。
- 容器内以 root 运行，浏览器以 `--no-sandbox` 启动；容器内无法提供 Chromium 沙箱。
- 容器没有系统 D-Bus，浏览器日志会出现 `Failed to connect to the bus`；它不影响 CDP 和生成。
- 生成请求串行处理，超过队列上限返回 `429 queue_full`。
- 构建和冒烟测试只验证程序启动、配置生成、随机管理密码、嵌入式 Web 控制台和 `/health`，不验证 Google 登录、账号探测或真实生成。
