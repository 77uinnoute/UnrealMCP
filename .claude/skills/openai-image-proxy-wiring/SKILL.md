---
name: openai-image-proxy-wiring
description: "把「OpenAI 兼容的本地代理」接进图像生成工具链（img-gen MCP：`<home>/.codemaker/mcp-servers/img-gen-mcp/server.py`）可复用的做法与判据：接线的两个独立旋钮（`OPENAI_BASE_URL` 换 base URL vs 代理进程级的「驱动模型」env）、为什么换驱动模型必须重启代理、为什么 env 改了必须重启 MCP server、用上游 `/v1/models`（`chatgpt-bridge models`）挑模型而不是信代理文档默认值、免重启试候选模型的手法（直接 POST 代理 `/v1/responses` 自带 model + image_generation tool，从 SSE 取 base64）、就绪判据（探端口 + `/health` 自证，不看日志偏移）、逐级验收链（health → doctor → check_setup → generate_image → read_file 看图）与反面证据读法（`insufficient_quota` / `credit_balance_exhausted` / `not supported when using Codex with a ChatGPT account` / 返回尺寸与请求不符）、以及凭证与安全纪律（先备份 `~/.codex/auth.json`、登录必须用户本人跑、用前 npm view + npm pack 审计第三方包、代理监听期间本机任何进程可白嫖订阅额度、订阅换 API 属 ToS 灰度必须先告知用户）、`auth.json` 只有 API key 而没 `tokens` 块时代理不回落该 key 的 OAuth-only 约束。触发场景：准备调用 img-gen 的 `generate_image` / `check_setup`（尤其在 openai provider 上）之前；以及任何涉及「OpenAI API 没余额/`insufficient_quota` 但要出图、想用 ChatGPT 订阅额度跑自动化出图、接第三方 OpenAI 兼容代理或本地 OpenAI 兼容 HTTP 服务、给某个 provider 加 base_url 覆写、`127.0.0.1:10531` connection refused、代理 healthy 但出图报模型不支持、代理 `serve` 重启后仍用旧模型或旧 env、改了 provider env 后工具行为没变、要判断某条出图链路是真通还是只通了鉴权」的任务。"
metadata:
  version: "1.0.0"
  upstream: ~
  downstream: ~
---

# OpenAI 兼容本地代理 × 图像生成链路接线 Skill

适用：本机 CodeMaker MCP 里的 `img-gen`（`~/.codemaker/mcp-servers/img-gen-mcp/server.py`，配置在 `~/.codemaker/mcps.json` 的 `mcpServers.img-gen`），
以及任何以「本地 OpenAI 兼容 HTTP 服务」替代 `api.openai.com` 的接线场景（本文以 `chatgpt-bridge` 为实测对象）。

> 本文只写**可复用的做法与判据**；不收录具体配方，也不写第三方工具的版本专属缺陷。

---

## 〇、先选路（判据）

| 你要什么 | 用 | 不要 |
|---|---|---|
| 稳定、可预期、按张付费 | 官方 `https://api.openai.com`（需余额） | 把它当"备用"塞进代理链路 —— 不会自动回落 |
| 用已有 ChatGPT 订阅额度、自己用、接受灰度 | 本地代理（OAuth 换额度） | 拿它做多人共享/生产（ToS 与限速都不允许） |
| 零配置、免费、能忍水印 | pollinations 免费层 | 期望它听 `model`/尺寸参数 |

**纪律：换路是显式动作，不是碰运气。** provider 级 base URL 是**全局改道**：指向本地代理后，代理进程不在就是 `connection refused`，
**不会回落到官方 API**。要切回官方必须改 env + 重启 MCP server（见 §三）。

---

## 一、接线的两个独立旋钮（结构性事实）

看图工具链一旦走「代理 → 上游 Responses API」这条路，就有**两个互不相干**的模型旋钮：

1. **出图模型**：请求体里的 `model`（走 `/v1/images/generations` 的标准字段）。
2. **驱动模型**：由代理进程持有的「用哪个文本模型去调 `image_generation` 工具」的 env（如 chatgpt-bridge 的 `CHATGPT_BRIDGE_IMAGE_MODEL`）。

**判据：先问"我换的是哪个旋钮"。** 报错说"模型不支持"时，**大概率是驱动模型**而不是出图模型 —— 两者报错形态不同：
出图模型不受支持会在探测阶段就报模型名，驱动模型不受支持则是**上游转发**回来的 400（见 §四）。

推论（都实测过）：

- 驱动模型只在**代理启动时**读 ⇒ 改它**必须重启代理进程**。
- base URL 由 img-gen 侧 env 覆写 ⇒ 改它**必须重启 MCP server**（MCP server 的 `env` 只在进程启动读一次）。
- 代理没起时 img-gen 只会在**调用的那一刻**失败，`check_setup` / `usable` 一律正常 ⇒ 见 §四，别拿 `usable` 当出图判据。

---

## 二、挑模型的判据：以**上游返回的模型目录**为准

**不要**信代理文档给的默认值，也别信"官方 API 有这个模型所以这边也有"。

- 账号类型会限制可用模型集合（订阅型账号与按量账号的能力集不同）。
- 取目录：`npx -y <代理包>@<版本> models`（或直接 `GET <proxy>/v1/models`）。
- 目录里给的 id 才是**上游认得**的 id。

**免重启试候选模型的手法**（省掉"改 env → 重启 → 试 → 再改"的循环）：

直接 POST 代理的 `/v1/responses`，body 里自带 `model` + `tools:[{type:"image_generation"}]` + `stream:true`，
从 SSE 事件里取 `item.type=="image_generation_call"` 的 `item.result`（base64）。命中即用，然后才把选中的 id 写进代理 env 并重启正式服务。

要点：

- 这是**绕开代理自身驱动模型配置**的唯一口子：那条路径的 `model` 完全由请求体决定。
- 探针要**逐个串行**试，不要并发（怕触发限速、也让日志可读）。
- 纪律：探针脚本 + 产物用完**删掉**，别留在机器上。

---

## 三、启动、env 传递与就绪判据

- 启动：`npx -y <代理包>@<版本> serve`（长驻，前台窗口别关；要后台跑就走各自的进程管理）。
- **默认就用 gpt（不需要先问）**：图像生成优先 `provider="openai"`（gpt-image 家族），因为它在**提示词遵循**与**透明底**上都比免费档可靠；`provider="auto"` 在 openai 已配置时就是它（img-gen 侧 `PREFERRED_PROVIDER`/`_auto_provider()`）。
  ⇒ **代理没跑就自己起**：探到 `127.0.0.1:10531` 拒连（`ConnectionRefusedError` / `WinError 10061`）就执行启动命令（带驱动模型 env），再探端口确认，**不要**因此退回 pollinations/minimax（那是静默降级，出的东西不一样）。
- **就绪 = 探监听端口**（chatgpt-bridge 默认 `127.0.0.1:10531`）。
  不要用"日志出现某行"或"睡够 N 秒"当就绪判据 —— 日志会轮转、睡眠时长必然漂移。
- 就绪后再要一份**自证**：`GET /health` → `auth.loaded=true` + `source_path` + `expires_in_seconds` + `rate_limit.hourly_used`。
- **env 传递**：在同一条命令里 `set VAR=...&& <启动命令>`。
  不要依赖"先 `setx` 过、新开的 shell 一定继承" —— 进程环境是否继承取决于调用方，最稳的是显式带上。
  要跨会话持久化，再补 `setx`（用户级）。

---

## 四、验收链（逐级，不许跳）

| 级 | 判据 | 暴露什么 |
|---|---|---|
| 1 | 探端口通 | 代理进程活着 |
| 2 | `GET /health` → `auth.loaded=true` | 鉴权装载 + 剩余额度 |
| 3 | `npx -y <代理包>@<版本> doctor` → `status=healthy` | 上游可达（含 `upstream HTTP 200`） |
| 4 | img-gen `check_setup` → `providers.openai.base_url` 指向代理 | MCP 侧重启后 env 真生效 |
| 5 | img-gen `generate_image(provider="openai")` → 回读 `provider/model/size/bytes` 行 | 全链路出图 |
| 6 | **把图片路径交 `read_file` 亲眼看内容** | 出的是不是你要的东西 |

**纪律：HTTP 200 / 端口开 / `doctor healthy` / `usable=true` 都不算出图成功。**
只认「真实生成 + 肉眼确认内容」。第 6 级是判"效果"的，不能省；前 5 级是判"链路"的，也不能跳。

**反面证据读法**（原文引自实测）：

- `{"code":"insufficient_quota"}` / `"credit_balance_exhausted"` / `You have no credits remaining` → **官方 API 侧没余额**，与代理无关，别去改接线。
- `Upstream 400: {"detail":"The 'X' model is not supported when using Codex with a ChatGPT account."}` → **驱动模型**不被该账号类型接受 ⇒ 回 §二换模型。
- **返回尺寸 ≠ 请求尺寸** → 代理链路里 `size`/`quality` 这类参数可能被上游改写（或忽略）。
  ⇒ 断言**返回的** size/bytes，不要假设请求值生效；要精确尺寸就自己后处理。

---

## 四b、参考图（文字 + 图）接线

「给一张参考图 + 文字」在代理链路里**不是标准字段**，接之前先确认三件事：

1. **谁支持**：代理通常把它做成**扩展字段**（例：`/v1/images/generations` 上的 `reference_images[]`，数组、可多张），
   而 `api.openai.com` 本身拒收该字段 ⇒ 同一份代码切回官方 API 会失败。**接线时要显式记住这条路只对代理成立。**
2. **本地路径受"允许根"限制**：代理为了防路径穿越，只接受**它自己进程工作目录之下**的本地文件，否则直接拒：
   `Attachment path is outside allowed root (<cwd>)`。它限制的是**代理进程**的 cwd，不是你的 cwd。
   ⇒ **最稳的做法是把文件内联成 data URI**（`data:image/png;base64,…`）再发，绕开这条约束；
   否则就得把参考图拷到代理 cwd 下（脆弱：取决于代理怎么启动的）。
3. **语义要问清**：不同实现差异很大 —— 有的当「主体一致」（复现主体，不是逐像素重绘），有的当「构图/风格基底」。
   判据只能看结果：**构图保持 + 属性被改** 说明它吃到了参考图；只换了颜色而构图全变，说明它只当提示词用。

**验收**：不要只看 `http 200`。取回包里的 `revised_prompt`（若有）看它怎么理解参考图，再**肉眼看图**比"主体/构图/属性"三项。

---

## 五、凭证与安全纪律

- 代理读写你 Codex 的 `~/.codex/auth.json`（OAuth tokens）。**改它之前先备份**。
- `auth.json` 里**只有 API key、没有 `tokens{access_token,id_token,refresh_token}` 块时，代理不会回落用那个 key**
  （OAuth-only 约束）⇒ 症状是 `doctor` 报 `Access token missing after refresh attempt`。
- **登录由用户本人跑**（交互式浏览器 OAuth，涉及用户凭据）——不要代跑，也不要代读 token 内容。
- **用前先审计第三方包**：`npm view <pkg>@<ver> --json`（看有无 `preinstall`/`postinstall`）+ `npm pack` 解开，
  扫出站主机、`child_process`/`spawn`、`writeFile` 目标。只看"有没有 install 脚本 + 往哪发数据"这两件事就能过滤掉大部分风险。
- **代理监听期间，本机任何进程都能白嫖你的订阅额度**（它不校验鉴权头）。不用就停进程。
- 订阅额度换程序化调用属 **ToS 灰度**：事实要先讲给用户（含"账号可能被标记"），由用户决定是否继续，不代做决定。
- 探针/日志**只打印长度或脱敏字段**，绝不回显 key / token 原文。

---

## 六、落盘分工（本 skill 的边界）

- 本 skill 只写**长期成立**的东西：旋钮分工、判据、验收链、凭证纪律。
- 版本相关的失效（某版本默认值不兼容、某参数被忽略）随版本变化，属另一类内容，不进本 skill。
