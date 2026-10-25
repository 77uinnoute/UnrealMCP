---
name: renderdoc-capture-analysis
description: "通过 RenderDoc MCP 分析真实 GPU 抓帧：确认当前捕获、定位绘制事件、核对流水线与常量、追踪 SPIR-V 数据流、读取 VB/IB 和实例缓冲、导出通道打包纹理，并将证据转换为可验证的跨引擎移植。触发：分析 rdc、原版渲染做法、shader 反汇编、draw call、资源绑定、网格提取、透明/遮挡判断或 GPU 抓帧复刻。"
---

# RenderDoc 抓帧分析与移植

目标：回答“GPU 实际画了什么、用了什么数据”，不把截图推断或局部公式冒充完整实现。本 skill 是跨工程方法；事件号、资源 ID、目标工程实体留在工程自己的 skill。

## 1. 先确认会话，再找文件

1. 先调用 `RenderDoc MCP Server.get_capture_status`，读 `loaded / api / filename`。GUI 可能已打开所需捕获；不要先扫全盘，再因没找到文件宣称缺证据。
2. 未加载才确认路径并 `RenderDoc MCP Server.open_capture`。文件没在某目录找到，只能证明该次搜索未找到。
3. 工具通过 MCP 调用；按当前工具 schema 传参。需要工具名而未激活时先检索；不要凭印象猜参数。
4. 每次分析记录 **capture 路径 + API + event ID**。换捕获后，事件号、ResourceId、缓冲布局均须重新核实。
5. 分析优先只读。shader 替换会改变重放现场；做 A/B 时记录替换内容，结束移除编辑。

## 2. 用事件与资源交叉定位，别凭邻近顺序命名

- `RenderDoc MCP Server.get_draw_calls` 按事件范围、标记、Drawcall 过滤；先少量候选，再查 `RenderDoc MCP Server.get_draw_call_details`。
- `get_pipeline_state(include_cbuffer_values=false)` 先拿轻量资源表；目标确定后再读常量。
- 用 shader ID、顶点格式、三角数、包围盒、纹理内容和对象变换交叉确认主体。
- **同 VB/IB 可以多次绘制成不同层**。草岸、覆盖、阴影、水面不能仅因同一网格或相邻事件就混为一件。
- 资源名如 `res99`、`_child8` 没有业务语义。逐项记录 descriptor set、binding、stage；工具展示槽序号与 shader binding 不应未经核对就等同。
- 纹理导出后直接看图；一张“水池附近绑定的图”可能是草岸，不一定是水面。

## 3. 材质结论必须包含流水线

至少核对：

| 项 | 能回答什么 |
|---|---|
| blend enabled / equation / factors | 是否混合、怎样混合 |
| depth test / function / write | 遮挡与深度写入 |
| cull / front winding | 单面、背面剔除与正面约定 |
| alpha-to-coverage / sample count | 覆盖率是否经 MSAA 转换 |
| shader discard / alpha test / dither | 透明裁切与覆盖处理 |
| sampler / format / sRGB / mip | 实际采样与解码语义 |

**输出 alpha=1 不足以单独证明 blend 关闭；看见后方角色也不足以证明水透明**。角色可能由独立遮挡提示 pass 再画一次。反过来，颜色相近也不等于共用材质。

`RenderDoc MCP Server.execute_python` 是工具未覆盖字段的只读补充：先从 `dir()` 或 API 文档确认成员，再访问。每次调用按独立脚本写，不依赖上次局部变量仍存在。

```python
controller.SetFrameEvent(event_id, True)
p = controller.GetPipelineState()
# 仅 Vulkan 捕获使用此状态对象；其它 API 不套用该结构。
vk = controller.GetVulkanPipelineState()
result = {
    'blend': [b.enabled for b in vk.colorBlend.blends],
    'depth_test': vk.depthStencil.depthTestEnable,
    'depth_write': vk.depthStencil.depthWriteEnable,
    'depth_function': str(vk.depthStencil.depthFunction),
    'cull': str(vk.rasterizer.cullMode),
    'alpha_to_coverage': vk.colorBlend.alphaToCoverageEnable,
}
```

## 4. 反汇编按赋值版本追踪，不能按临时变量名猜

1. 导出 VS 和 PS，保留原文；后缀 `.glsl` 不保证内容是可编译 GLSL，也可能是 RenderDoc 的 SPIR-V 文本表示。
2. 建表：输入 location → VS 输出语义；纹理 set/binding；常量块 set/binding、成员 offset、类型、真实值。
3. 从目标输出逆追，再按执行顺序正向验证。Private 临时变量常被反复覆盖；对每次写入标版本，注意 swizzle、分支和未覆盖分量。
4. 采样 UV、采样结果、另一路 UV 分开命名。不要把刚采到的 RGB 自行接成下一路坐标。
5. 还原 `exp2(log2(x)*g)`、`clamp`、多项式 smoothstep 等数学后，保留运算先后与空间单位。
6. 常量必须来自当帧 cbuffer，不从截图颜色、脚本注释或 `_child` 编号猜值。特别核对 **time×speed** 与 **time×另一 speed**，别擅自串乘。
7. RGB 通道打包纹理须按 shader 取通道；不能把黄色预览图直接当黄色表面。
8. 区分表面项、漫反射、高光、环境反射、雾和最终输出。最终颜色不能未经拆分又接到另一个引擎的 BaseColor 二次受光。

反汇编还原若交给子任务，必须核验关键数据流。模型给出的解释不是 GPU 证据。

## 5. 常量解码：反射索引不是绑定号

优先 `RenderDoc MCP Server.get_cbuffer_values`，需要原始 API 时：

```python
controller.SetFrameEvent(event_id, True)
p = controller.GetPipelineState()
stage = rd.ShaderStage.Pixel
reflection = p.GetShaderReflection(stage)
blocks = []
for index, block in enumerate(reflection.constantBlocks):
    # index 是 reflection.constantBlocks 索引，fixedBindNumber 是 shader 绑定号。
    used = p.GetConstantBlock(stage, index, 0)
    d = used.descriptor
    if not block.bufferBacked or d.resource == rd.ResourceId.Null():
        continue  # 非 buffer-backed 常量另按该 API 的来源处理。
    values = controller.GetCBufferVariableContents(
        p.GetGraphicsPipelineObject(), p.GetShader(stage), stage,
        reflection.entryPoint, index, d.resource, d.byteOffset, d.byteSize)
    blocks.append((block.name, block.fixedBindNumber, values))
```

序列化时按变量真实类型取 float/int/uint，递归保留 members、行列、数组及矩阵信息。**不要把整个 block 一律解释成 float**：整数、打包颜色、填充及未使用字段会出现无意义浮点值，不能拿来当参数。

## 6. 网格：先格式，再字节，再坐标系

需要记录：draw 的 index count、index offset、base vertex、instance offset；IB 字节偏移及 16/32 位类型；每个 attribute 的 buffer、offset、format；VB stride 和 byte offset。

- IB 起点 = 绑定字节偏移 + `indexOffset × indexStride`。
- 顶点位置 = VB 绑定字节偏移 + `(index + baseVertex) × vertexStride + attributeOffset`。
- attribute 可能来自不同 buffer；实例属性、stride=0 常量属性不能当普通逐顶点流。
- 解 `R16...FLOAT` 用 half，`UNORM` 要归一化，不能全用 float32。
- 仅读取实际引用的顶点；做 index 重映射，保留 UV seam 的重复顶点，不能按位置焊接。
- 同时保存原始坐标、法线、切线、UV、颜色、格式与索引，不仅保存“测得的轮廓”。

遍历 action tree 的通用方式：

```python
actions = {}
def walk(nodes):
    for action in nodes:
        actions[action.eventId] = action
        walk(action.children)
walk(controller.GetRootActions())
```

buffer 解码的 `struct.unpack_from` 格式必须从当前捕获格式推导。不要把某次成功的 stride、offset、顶点数当通用模板。

## 7. 实例与动态：单帧只给单帧事实

- 实例数据可能来自 storage/UAV buffer，而不在 vertex input 的 per-instance 属性里。
- 沿 VS 的 buffer 索引、stride、字段读取和点乘顺序还原矩阵；先解一例，再验证全部实例的空间分布。
- 用 `GetReadWriteResources(stage)` 等读 descriptor 的 resource/byteOffset/byteSize；buffer 被列成 UAV 不等于 shader 会写它，仍需看访问与 shader。
- 不假定 row/column-major、位置字段、颜色打包方式；按 shader 验证矩阵，不能仅看数值“像平移”。
- 某帧有 N 粒子，不代表发射上限恒为 N；单帧布局不能证明出生频率、寿命、速度或随机分布。
- 动态需多帧捕获、原版录像或粒子模拟数据补证。自行补的生命周期必须标“推断/适配”，不能标“原版恢复”。

## 8. 导出到目标引擎：数据一致不等于视觉一致

- 纹理保留原格式、sRGB、通道、alpha、寻址与 mip 记录；PNG 是查看/交接格式，不能完整保存所有 GPU 采样语义。
- 网格通过外部 DCC → FBX → 引擎正式导入工具；不在引擎里手拼网格。转轴、单位、绕序、法线、UV 的 V 方向都用导入回读验证。
- 比包围盒、三角数、槽位、实际绑定；不能只看“导入成功”。
- **装饰参与总 bbox 时，删装饰会改变归一化结果**。渲染尺度用独立 authoring frame 或水面本体范围，不依赖混合装饰的 bbox。
- 顶点色承载覆盖率等数据时，检查导入是否保留；必要时显式以独立 UV 通道承载，避免把颜色语义误作美术调色。
- 不透明、masked、alpha-to-coverage、有序抖动、时间抖动是不同手段。替代实现必须注明差异并验动态闪烁。
- WPO 改位置不自动等于法线也按同一形变重算；不要用静态法线结果宣称动态光照已一致。
- 第三方资产仅在有权分析/使用的范围内处理；原始资产提取不自动授予发布权。

## 9. 完成判据

每个结论给证据级别：**捕获实测 / 录像可见 / 数学还原 / 引擎适配 / 尚未确认**。

交付至少包含：
1. 捕获与事件资源表、VS/PS 原文、参数值及来源。
2. 几何与材质绑定、编译和流水线回读。
3. 原版与目标相同状态、尽量同视角且等主体尺度的并排图；不同机位明确标注。
4. 动态项目有连续录像；稀疏帧不证明无闪烁，编译通过不证明像原版。
5. 清理 shader 临时编辑、测试对象和 override，恢复现场。

## 10. 可执行取证脚本

`scripts/export_event_evidence.py` 是本 skill 唯一源码，跨工程不含任何游戏实体、事件常量、buffer stride 或输出绝对路径。用于 RenderDoc MCP `RenderDoc MCP Server.execute_python` 的 `rd / controller / ctx` 上下文；不是本机命令行直接打开 rdc 的工具。

导出：当前事件 VS/PS 默认目标反汇编、shader/pipeline ID、entry point、buffer-backed 常量块的 reflection index / binding / resource / byte offset / size 与递归类型值。整型按整型读取，未知类型明确标 `unsupported_type`。非 buffer-backed 常量明确跳过，不能把缺项读成零。输出 `manifest.json`、`vs_disassembly.txt`、`ps_disassembly.txt`（存在该阶段且导出成功时）。

经 `RenderDoc MCP Server.execute_python` 提交以下代码；路径均由调用方指定，输出目录必须尚不存在，避免覆盖旧证据：

```python
from pathlib import Path
script = Path('<插件根>/.claude/skills/renderdoc-capture-analysis/scripts/export_event_evidence.py')
namespace = {}
exec(compile(script.read_text(encoding='utf-8'), str(script), 'exec'), namespace)
result = namespace['export_event'](
    controller, rd,
    event_id=event_id,                  # 本次已定位的事件，不沿用别的捕获事件号
    output_dir=output_dir,             # 新目录的绝对路径
    previous_event=ctx.CurEvent(),     # 修改事件前读取；finally 恢复此事件
    capture_label=capture_path)        # 来自 get_capture_status 的真实文件名
```

`event_id / output_dir / capture_path` 在同次提交前段赋值，不依赖上次脚本变量。查看 manifest 的 `errors`、每块 `skipped`、`unsupported_type` 与 `event_restored`；遇到异常仍尝试恢复原事件并保留 manifest。脚本不改 shader、不打开/关闭捕获、不操作目标引擎场景。

**范围有限**：不导出纹理/网格、不解任意顶点格式、不自动解析 Vulkan/D3D blend/depth、push constants 或常量块 descriptor 数组（只取 array index 0）。这些按前文用对应工具另取；不能把该 manifest 叫完整流水线或完整 shader 移植。离线语法/假 controller 测试只能证明脚本逻辑，本机当前 RenderDoc 绑定的运行兼容性仍需实际调用确认。

接口缺陷、崩溃流水和版本性故障另写开发文档，不在 skill 积累；skill 不引用这些开发文档。
