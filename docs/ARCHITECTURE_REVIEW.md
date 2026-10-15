# 架构与开源准备检查

## 当前结构

- `src/main.cpp`：初始化硬件、WiFi、LVGL、通信服务，并在主循环调度服务。
- `CommunicationService`：协议后端生命周期与收发入口；`SystemClock`：独立系统时钟。
- `Hardware`：屏幕、触摸、电源、GNSS、音频与板级外设。
- `MeshService`：MeshCore 射频、身份、频道、联系人和消息。
- `AppUI`：系统桌面、原生页面、导航、应用生命周期及文件浏览器。
- `PocketJsRuntime`：QuickJS 沙箱与 Pocket API，运行 SD 卡中的 JavaScript 应用。
- `ConnectivityService`：WiFi、时间同步、网页文件管理、串口安装、SD 卡和地图下载。
- `SdrService`：WiFi 射频扫描。
- `apps/`：可独立部署的应用；`scripts/`：构建辅助及部署工具；`tests/`：部分应用的 Node 测试。

界面主要在主循环运行；地图下载及格式化使用 FreeRTOS 后台任务。LVGL 和 JavaScript 数据优先放 PSRAM，显示 DMA 缓冲使用内部 RAM。TLS 分配已改为优先 PSRAM。

## 初审历史（下列部分问题已解决，当前状态见 RELEASE_READINESS.md）

1. **构建可复现**：`platformio.ini` 含作者机器上的 MeshCore 绝对路径，`lib/MeshCore` 是指向相邻 checkout 的符号链接。改为明确固定版本/提交的依赖，并验证全新目录可以构建。
2. **许可证**：仓库根目录没有项目许可证。需根据 MeshCore、QuickJS、ed25519、字体、图标和应用资产的实际许可确定项目许可并提供第三方声明；不能直接假定所有内容都适用同一许可。
3. **README 更新**：当前文档仍描述早期五页界面和 SDK v2，实际已有 PocketJS SDK v3、文件管理、地图及更多应用。补齐硬件版本、依赖、安装、地区射频配置与已知限制。
4. **地图真实性**：当前下载将 PNG 保存到 `/maps/osm/<z>/<x>/<y>.png`，地图页面仍有演示矢量街道层。需验证并明确真实瓦片的显示路径，不能把文件下载成功等同于地图可用。
5. **地图供应商要求**：发布前核实地图提供方对离线缓存、署名和下载行为的要求，选择允许目标用途的服务或由用户配置供应商。
6. **凭据与历史**：忽略规则已经排除部分应用配置；仍需检查所有跟踪文件与 Git 历史中的密钥、WiFi 配置、个人位置和运行数据。当前检查未构成完整凭据审计。
7. **HTTPS 校验**：多个请求路径关闭证书验证。应建立证书/时间同步策略，并说明代理与外部服务的配置方式。

## 建议的后续拆分

- 将 `AppUI.cpp` 的页面构建拆成独立模块，保留统一导航和生命周期控制。
- 将 `ConnectivityService` 拆为网络配置、文件服务器、SD 存储和地图服务，减少相互耦合。
- 后台任务用明确的结果结构与同步机制发布状态；避免依赖 `volatile` 代替跨核同步。
- 地图文件采用临时文件、完整性校验与原子提交，防止中断后把半张图片当作缓存命中。
- 地图服务增加可取消任务、具体错误提示和总时限；测试断网、SD 满、无 GPS、无效坐标及页面退出。
- CI 至少覆盖固件构建、应用脚本语法、manifest 校验与已有 Node 测试。

## 本次设备证据

2026-10-11，通过独占 USB 串口向设备请求纬度 50、经度 178、14 级地图。原先不存在的九张瓦片依次被请求，任务约 18 秒后报告 `Storage job: complete done=9/9`；结束时内部 RAM 约 59 KB，PSRAM 约 6.2 MB。该测试证明后台下载新瓦片成功，尚未证明用户界面入口及真实地图显示均正确。

## 2026-10-13 整理进展

已添加项目 MIT LICENSE、第三方声明、ed25519 许可、应用校验脚本与 GitHub Actions。MeshCore 改为仓库内源码快照，去除绝对路径和外部符号链接。三个 Node 测试及应用校验通过。上述旧检查项为初审历史；字体/图片来源审计、历史凭据审计、完整文档更新、地图可视验证及通信后端拆分仍未完成。此处为当时状态；后续 Meshtastic 实现见下方更新。

## 本轮更新

- 真机武汉 Z13 已显示 SD 卡 9/9 瓦片，可见道路、街道标签与河流；保存的经度从 14.3056 纠正为 114.3055。
- 第一阶段通信接口曾支持 Off 与 MeshCore；当前已扩展至 Meshtastic 和统一联系人/事件接口，见下方更新。
- Radio 页增加协议选择及带 MHz 的地区预设；Off 状态修改后重启仍能保留设置。快捷菜单改为 Reboot 和 Settings。
- 中文字库来源已根据生成注释核对为 LVGL 内的 SourceHanSansSC，并保存其许可；Montserrat、Font Awesome 许可也已补齐。应用图片、历史凭据和参考工程实现来源的审查仍待完成。

## Meshtastic integration update

Radio ownership now resides in the communication facade with one shared SX1262
instance. Native UI and PocketJS peers/messages use a portable model. MeshCore
and Meshtastic configurations persist separately; system clock has independent
ownership. Meshtastic is a CLIENT_MUTE implementation using a pinned BSD library.
The next useful split is portable message/peer storage out of AppUI, then Radio
configuration views out of AppUI. Device connectivity and USB diagnostics remain
in ConnectivityService and should eventually be separated. These are incremental
refactors; the two-device interoperability gate and third-party provenance review
remain before a public release.

## 2026-10-15 更新

项目整体许可改为 GPL-3.0-only，保留参考硬件实现及 audio-driver 的 GPLv3 声明。独立 Mesh Test 已移除，Radio 与通信 API 保留。当前公开范围及未解决的发布限制以 [发布检查](RELEASE_READINESS.md) 为准；此前 MIT 结论不再适用于当前版本。
