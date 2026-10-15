# 发布检查（2026-10-15）

## 当前结论

项目整体采用 **GPL-3.0-only**。当前源码适合作为明确标注限制的开发预览公开，尚不能宣称正式固件发布检查全部通过。架构拆分可以逐步完成，无需为首次公开重写整个项目。

当前仓库已整理为一个新的初始提交。旧 Git 历史、SD 私人备份和本地应用配置已移到项目外私有目录，不属于当前公开历史。旧历史含来源未核实的图片，不应重新导入公开仓库。设备 SD 备份、appdata、私信、坐标、WiFi 和 API 配置不得进入公开包。

## 已完成的来源整理

- MeshCore 和 MeshtasticLite 以固定版本源码纳入仓库，包含各自许可及本地修改说明；ed25519 不再引用相邻工程。
- 根目录包含完整 GPLv3 文本；README、第三方声明及原创 DeepSeek 图标声明已统一为 GPL-3.0-only。
- 硬件参考代码的匹配实现明确按 GPLv3 保留，参考许可证、文件及 SHA-256 见 `licenses/hardware-reference` 和 `HARDWARE_PROVENANCE.json`。本地参考树没有 Git 元数据，不虚构上游提交。
- audio-driver 的 GPLv3 声明保留；采用 GPLv3 后不再作为原先 MIT 方案的冲突项。
- 字体、二维码及可获取的依赖许可已保留。未核实来源的旧应用图片和 ZIP 已从当前源码删除，见 `ASSET_PROVENANCE.md`。
- 源码包含构建说明、固定依赖、应用校验、主机测试及 GitHub Actions。远端 CI 尚未执行。

## 正式固件发布前仍须处理

1. **HTTPS 证书**：`PocketJsRuntime::jsHttpGet` 使用 `setInsecure()`，DeepSeek API Key 经该路径发送。代理地图请求也使用 `MBEDTLS_SSL_VERIFY_NONE`。需要可信 CA、主机名和证书有效期校验，并验证错误证书拒绝及首次授时策略。
2. **地图服务许可**：离线区域下载硬编码 `tile.openstreetmap.org`。目前没有完成该用途的服务授权核对；需要明确允许离线下载的供应商/用户自建源、配置方式、署名及使用限制。软件 GPL 不授权外部地图内容或服务使用。
3. **二进制分发材料**：需对应版本的完整源码、构建依赖与工具链获取说明及适用的第三方声明；本轮提供源码开发快照，不把固件二进制当作正式 Release。

上述限制应随源码公开展示，不应隐藏在安装流程中。当前不建议把 DeepSeek 的联网密钥功能作为安全性已验证的正式功能。

## 架构建议

`CommunicationService` 管理通信后端，`ChatStore` 管理持久化，`SystemClock` 独立授时，PocketJS 为应用提供边界，足够支持开源协作。后续优先将 AppUI 页面、USB 诊断、Web Files 和地图服务拆成独立模块，保持公共 JS SDK 稳定。

`/appdata` 隔离应用代码与数据，但不加密 SD 卡；设备备份留在忽略的本地 outputs 下。历史的清卡、数据恢复和消息互通验证不等于完整安全审计。

## 本轮验证

- 实现提交 `d5b6b5e` 在独立目录 `/tmp/wio-gpl-clean-20261015` 通过 `git clone --no-local` 获取，工作树干净，未复制原工程源码、依赖目录或编译对象；使用本机 PlatformIO 工具链和下载缓存。`pio run -e wio_tracker_l2` 成功，RAM 149008/327680，Flash 3818709/6553600 字节。不宣称构建产物逐字节可复现。
- 干净克隆的全部四个 JavaScript 测试文件及应用 manifest/JS 校验通过。原工作树 Node 运行器报告 5 项通过。不同 Node 版本对文件/子测试统计不同。
- CommunicationService、ChatStore、ChatReadState 主机测试通过；Meshtastic golden AES、PKI 认证、边界及解析测试使用 OpenSSL 通过。本机默认 macOS 27 SDK 与安装的链接器不兼容，改用已安装的 macOS 12.1 SDK 完成主机测试；未启用 sanitizer，Linux CI 仍保留该检查。
- 固件已通过 USB 上传。真机截图确认 Chats 键盘弹出；连续 8 次“打开 Chats → 弹出键盘 → 底部上滑 → Home”均成功，所有触摸/滑动请求被接受，每轮收到 PONG，未捕获 Guru Meditation、断言或重启标记。首轮过快操作有滑动被拒绝，未计入这 8 次有效验证。这不能保证穷尽全部偶发场景。
- 修复将底部导航移出 LVGL 输入回调，并在隐藏/释放键盘时解除 textarea 关联。独立 Mesh Test 从源码移除，通信后端及 Radio 保留。
- 旧 outputs 下 4 个已跟踪固件二进制从源码索引移除（本地文件保留）；当前跟踪文件的常见私钥、API Key/token 格式扫描无命中。该扫描不覆盖任意通用密码或所有凭据形式，源码快照也不包含旧 Git 历史。

- SD `/apps` 枚举确认 19 个应用，无 Mesh Test；临时 I²C QA 应用验证后已删除。
- I²C 应用与原生扫描均返回 0x14、0x18、0x21、0x22、0x2C、0x34、0x48、0x5D。0x34 是 LP5814 广播地址；0x5D 读取到 GT911 的 ASCII 911 标识。0x14 身份未确认，保持 unknown，不凭地址猜测器件。详细证据见 [I²C 核对](I2C_VERIFICATION.md)。

## Public baseline cleanup

The English README uses the working name MeshDesk. The public history starts
with one root commit; earlier verification commit IDs above refer to the archived
private history. Firmware source is unchanged by this cleanup. Device identifiers
in public examples are synthetic. Generated Python bytecode, local application
configuration, SD backups, and old firmware output files are excluded. Commit
metadata uses a project identity rather than a personal email address.
