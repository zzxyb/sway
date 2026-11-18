# Workspace image capture：Sway 与 wlroots 实现方案

状态：设计提案，未实现。本文的新增 C API 均为示意，需要结合 wlroots 的接口风格评审。

协议依据：`/home/ut000903@uos/Documents/Code/Gitlab/wayland-protocols/staging/ext-workspace-image-capture-source/ext-workspace-image-capture-source-v1.xml`。

## 1. 设计结论

采用“独立组合 workspace 节点，离屏合成到 buffer”的方案：

- Sway 决定捕获画面的内容、布局、可见性和堆叠顺序。
- wlroots 提供独立的 scene composition：引用多个节点或子树，为每个引用设置捕获局部坐标、裁剪与顺序，不修改原始桌面场景树。
- wlroots 将 composition 离屏渲染到 swapchain buffer，再通过现有 image capture source / image copy capture 管线交付。
- workspace 协议 manager 只负责 handle 解析和 source 绑定，参照现有 foreign toplevel 实现。

不向 wlroots 传入 `sway_workspace`、`sway_container` 或 `list_t`。通用封装的输入是场景内容及明确的组合参数。

“独立”指组合状态与输出独立；底层 surface buffer 可以共享，不需要为每个窗口先截图再拼接。各节点直接参与同一次合成，从而保留遮挡、透明混合和裁剪关系。

## 2. 协议约束

捕获必须表现为工作区在所属 workspace group 中处于活动状态，即使它当前不可见。创建 source 和捕获都不得切换工作区或改变真实显示内容。

画面是工作区的合成结果，窗口之间存在遮挡。协议允许按策略省略受保护内容、panel、dock、background 和 overlay。光标由使用 source 的捕获协议管理。

本提案中以下行为属于 Sway 策略，并非 XML 已规定的行为：画布大小、scale、空工作区底色、无输出工作区处理、global fullscreen 和 sticky 内容归属。

## 3. 当前源码基础与限制

### wlroots

`types/ext_image_capture_source_v1/foreign_toplevel.c` 已提供如下模式：

1. 接收 create_source，解析 handle。
2. 向合成器发出 capture_request。
3. 合成器选择 source 并调用 request_accept。
4. 使用已有 source resource 和 image-copy 协议实现捕获。

`types/ext_image_capture_source_v1/scene.c` 已提供 `wlr_ext_image_capture_source_v1_create_with_scene_node()`，内部通过专用 wlr_output、scene_output 和 swapchain 合成到 buffer。

但该实现通过 `scene_node_get_root(node)` 创建 scene_output，传入 node 主要用于计算 extents。从当前代码看，它没有提供严格的任意子树隔离渲染；同一根 scene 中位于捕获范围内的其他节点可能参与合成。独立 composition 必须保证内容集合隔离，不能只按矩形截取真实桌面。

此外，workspace 的尺寸不能使用内容 extents：空工作区、越界浮动窗口和窗口移动都不应该使录制尺寸随内容变化。

### Sway

`include/sway/tree/workspace.h` 中的 floating / tiling 是布局模型；对应的 scene 节点并不全部位于一个 workspace 子树中。

`transaction.c` 中，浮动节点被放到全局 floating 层，fullscreen 有独立层。非活动工作区执行 disable_workspace，并未执行完整的活动工作区场景布局。因此，仅引用现有 workspace tiling 节点并忽略 enabled 标志仍不足以保证正确画面。

当前 toplevel capture 已维护独立的 image_capture_scene 和 surface/popup 表示，可作为独立内容表示的实现参考。

## 4. 分层与数据流

```text
ext_workspace_handle_v1
        │ create_source
        ▼
wlroots workspace source manager
        │ capture_request(handle, client)
        ▼
Sway workspace capture controller
        │ 已提交布局 → 捕获布局 → 内容引用、坐标、裁剪、顺序
        ▼
wlroots scene composition
        │ 构建隔离的 render list
        ▼
离屏 render pass → swapchain buffer
        │ damage、尺寸约束、时间戳
        ▼
image capture source → image copy capture → 客户端 buffer
```

workspace controller 与 source 按工作区共享，不为每个客户端重复合成。客户端协议资源可以引用同一个 source；各 capture session 的目标 buffer 和损伤历史仍由 image-copy 层管理。

## 5. wlroots 的通用 composition 封装

### 5.1 组合对象

建议新增通用的 `wlr_scene_composition` 概念。它不是第二个真实输出，不加入 output layout，不向客户端公布 wl_output，不承担输入命中测试。

composition 包含：

- 固定逻辑 viewport 和输出像素 scale。
- 有序的内容 entry，包含节点或子树引用、局部位置和裁剪。
- 独立的捕获可见性、损伤与渲染状态。
- 内容销毁监听和显式更新接口。

首版可以限制平移、裁剪和统一 scale，不必支持任意仿射变换。

### 5.2 引用语义必须明确

现有 scene node 只有一个 parent，不能把同一个 node reparent 到两个场景。entry 应是非拥有引用或独立代理，绝不移动原 node。

对一个被引用子树，建议定义：

- 不继承原 node 的祖先位置、祖先 enabled 状态和祖先裁剪。
- entry 的位置替代被引用根节点相对于原 parent 的位置。
- 子树内部的相对位置和 enabled 状态默认保留。
- composition 自己定义外部裁剪和 entry 堆叠顺序。
- 引用销毁时自动失效，移除 entry 并损伤旧范围。

不能提供简单的“忽略全部 enabled”开关来解决隐藏 workspace：它会错误显示 tabbed/stacked 中本应隐藏的窗口。

如果 Sway 需要的内部布局与原子树不同，应生成独立的捕获代理子树，或提交更细粒度的 entry。通用引用不会自动重建“假如活动”的布局。

### 5.3 建议 API 形态

以下仅表达职责，不是可直接编译的头文件：

```c
struct wlr_scene_composition;
struct wlr_scene_composition_entry;

struct wlr_scene_composition *wlr_scene_composition_create(
    struct wlr_renderer *renderer, struct wlr_allocator *allocator);

bool wlr_scene_composition_configure(
    struct wlr_scene_composition *composition,
    const struct wlr_box *viewport, float scale);

struct wlr_scene_composition_entry *wlr_scene_composition_add_node(
    struct wlr_scene_composition *composition, struct wlr_scene_node *node);

/* entry 的位置、裁剪、enabled 和顺序通过专门 setter 更新。 */
/* 提供 begin/end_update，使一次事务的修改原子生效。 */

struct wlr_ext_image_capture_source_v1 *
wlr_ext_image_capture_source_v1_create_with_scene_composition(
    struct wlr_scene_composition *composition,
    struct wl_event_loop *event_loop);
```

直接渲染到调用方提供的 `wlr_buffer` 也可以作为底层接口；前提是明确 buffer 尺寸、格式、设备兼容性、同步和渲染失败语义。首版由 composition/source 管理 swapchain 更容易统一约束和损伤历史。

### 5.4 离屏渲染实现

建议复用 scene 渲染内部的纹理导入、裁剪、遮挡、透明混合和 render pass 代码，抽出可接受 composition render list 的内部路径，避免另写一套渲染器。

每帧步骤：

1. 固定一次已提交的组合状态。
2. 获取尺寸匹配的 swapchain buffer，并计算该 buffer 的 repaint damage。
3. 按捕获 viewport 构建有序 render list，仅包含 composition 内容。
4. 清理需要重绘的底色区域，直接混合 rect 和 surface buffer。
5. submit render pass，按现有同步约定保证消费安全。
6. 发出 source frame 事件，由 image-copy 层向客户端交付。

帧事件中的逻辑内容 damage 与 swapchain buffer 因历史内容需要重绘的区域应区分。首版可以整帧合成和整帧 damage，后续再做增量优化。

内部暂时复用当前 scene source 的专用 wlr_output 机制也可行，但应把它视为实现细节，检查 output enter/leave、preferred scale、frame callback 和 presentation 行为。长期 API 不应要求合成器创建可见的虚拟显示器。

## 6. Sway 的工作区捕获控制器

### 6.1 从 committed state 构建

以 workspace/container 的 `current` 状态构建捕获布局，在事务应用后原子更新 composition，避免 pending 几何、旧 buffer 和新可见性混用。

建议将现有布局过程逐步拆为：

1. 计算节点几何、可见性、裁剪和顺序。
2. 将结果应用到真实桌面场景。
3. 将结果应用到捕获代理或 composition entries。

真实桌面使用“当前输出激活哪个 workspace”的可见性；捕获使用“假设目标 workspace 活动”的可见性。二者共用布局规则，使用不同的激活上下文。

首版可先在 Sway 中维护独立的捕获代理树，借鉴 toplevel 的 surface/popup 镜像。只有在子树内部布局相同的情况下，才直接引用现有子树。不能假设 floating/tiling 列表中的节点都已经具有正确捕获布局。

### 6.2 合成策略

| 内容 | 首版建议 |
|---|---|
| 画布 | 所属输出完整逻辑范围；局部原点为 (0, 0) |
| 像素尺寸 | 根据逻辑尺寸和输出 scale 计算，统一定义取整规则 |
| tiling | 使用 usable area、gaps 和已提交布局 |
| floating | 按工作区浮动堆叠顺序组合，裁剪到画布 |
| tabbed/stacked | 使用保存的活动子节点和现有装饰规则 |
| workspace fullscreen | 应用 fullscreen 可见性及 transient 规则 |
| global fullscreen | 不让其他工作区的 global fullscreen 遮盖目标工作区；目标工作区自身的行为单独定义并测试 |
| popup/subsurface | 跟随所属窗口，以捕获坐标布局并保留正确堆叠 |
| decoration | 纳入 Sway 自绘标题栏与边框 |
| background/panel/overlay | 首版省略，使用固定不透明底色 |
| scratchpad/sticky | 使用“目标工作区被激活时会出现哪些窗口”的规则；不得直接复制全局 floating 层 |
| cursor | 单独对接捕获协议，不默认把真实输出光标画入 |

移到其他输出、输出 scale/尺寸变化或 usable area 变化时，重新布局并更新 source constraints；不得仅缩放旧截图。

没有关联输出的工作区，首版建议使 source 不可捕获并按 image-copy 协议发送对应的停止/失败结果。具体状态映射必须检查现有协议实现，不能发明新的 wire error。

## 7. workspace 协议接入

wlroots 增加 workspace source manager，结构参照 foreign_toplevel.c：

- 解析 ext_workspace_handle_v1。
- 发出包含 workspace_handle、client 和 new_id 的 capture_request。
- Sway 查询关联的 sway_workspace，应用捕获策略并取得共享 source。
- request_accept 将 source 绑定到客户端的新 resource。
- 失效 handle、拒绝和分配失败均必须完成 new_id 的处理，避免留下未创建的协议对象；具体采用 inert source 或 no-memory 等已有机制。

manager resource 的 destroy 只销毁该 manager resource，不应销毁已经创建的 source。source 的有效期由 workspace 和共享捕获对象控制。

现有 Sway global filter / security-context 限制也应覆盖新增 capture global；授权策略由合成器决定。

## 8. 帧调度、损伤与生命周期

### 帧调度

source 的存在不等于正在录制。仅在有活跃 session 时驱动捕获；多个客户端共享一次工作区渲染，按需求合并请求并限制最大帧率。

非活动工作区的可见 surface 需要得到适当 frame callback 才能持续更新。必须与真实输出回调协调：不能让捕获重复驱动同一 surface，也不能伪造物理输出 presentation。复制内容的代理节点尤其需要集中处理回调。

### 损伤

buffer 更新、节点移动/隐藏/删除、顺序和裁剪变化都应产生 damage。几何变更至少覆盖旧范围与新范围。隐藏到显示、viewport/scale 变化和策略变化触发整帧损伤。

### 生命周期

- workspace 销毁：终止 source，解除引用和监听，确保存量客户端获得现有协议规定的停止行为。
- view/node 销毁：在释放前使 entry 失效，不能持有悬空指针。
- workspace 迁移：保持工作区 source 身份，原子更新布局与 constraints。
- renderer 重建：丢弃旧纹理、swapchain 和相关设备约束，重建或终止 source。
- 最后一个 session 停止：停止驱动隐藏 surface，释放或按明确策略保留离屏资源。

## 9. 实施顺序

1. 明确 capture viewport、scale、光标、无输出和 fullscreen 策略。
2. 在 wlroots 增加 workspace manager 请求/接受流程。
3. 实现最小 composition：多个引用、平移、裁剪、顺序、固定 viewport、整帧离屏合成。
4. Sway 实现普通 tiling/floating 捕获代理和事务后更新。
5. 补齐 tabbed/stacked、decoration、popup、fullscreen/transient 和迁移。
6. 处理 frame callback、多客户端、停止及销毁路径。
7. 正确性稳定后优化代理共享、局部损伤和遮挡。

## 10. 验证标准

- 对相同工作区，活动与非活动捕获的布局和遮挡一致，真实桌面不切换、不闪烁。
- 输入焦点、workspace active 状态及真实场景父子关系不因捕获改变。
- 重叠窗口透明混合正确，其他工作区和全局层内容不会泄漏到捕获。
- 空工作区、越界浮动和移动窗口不改变画布尺寸。
- tabbed/stacked 隐藏窗口不出现，fullscreen/transient/popup 顺序正确。
- 非活动应用持续更新；停止后不再因捕获持续消耗渲染资源。
- 多客户端共享渲染，单个客户端退出不影响其他 session。
- 输出移除、scale 变化、窗口/工作区销毁和 renderer 重建没有悬空引用。
- 受保护内容按明确策略处理，隐藏或替换后不残留旧 buffer 内容。

## 11. 推荐边界

wlroots 封装“独立内容组合 → 离屏 buffer → capture source”；Sway 实现“workspace 状态 → 组合内容”。

这个边界支持 workspace，也能复用于选择多个窗口或自定义区域的合成捕获。关键是让组合引用具有独立的坐标、裁剪和生命周期，同时承认：非活动 workspace 的布局重建仍是合成器职责，通用 node 引用本身无法推导这些语义。
