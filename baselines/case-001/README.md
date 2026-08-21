# CASE-001 Baseline

## 目录

- [结论](#结论)
- [内容](#内容)
- [校验](#校验)
- [边界](#边界)

## 结论

本目录是 `CASE-001@1` 的机器可读事实基线，固定 Source/Reference identity、来源状态、使用矩阵和平台无关场景；`quickapp-code-test1` 保持只读。

## 内容

| 文件 | 所属任务 | 内容 |
|---|---|---|
| `source-inventory.json` | T01 | Source 文件集合、bytes、SHA-256、snapshot digest 与排除规则 |
| `provenance.json` | T02 | 已知本地来源事实和 `[待验证]` 上游字段 |
| `reference-inventory.json` | T03 | build/debug/release 目录与 RPK/RPKS 身份、成员、构建 metadata |
| `usage-matrix.json` | T04 | DSL、组件、Binding、Event、Style、Module、Page Control 使用事实 |
| `scenarios.json` | T05 | S1-S5 的语义操作、可见结果、Lifecycle/Trace 和跨平台规则 |
| `verify.mjs` | T01-T05 | 零依赖、只读的确定性校验器 |

## 校验

从 `quickapp-examples` 目录运行：

```bash
node baselines/case-001/verify.mjs
```

校验器验证 Source 精确文件集合、全部文件/目录/归档 identity、ZIP 成员和基线数据的最小结构；不构建产物、不启动 Runtime、不访问网络。

## 边界

- 联盟 RPK/RPKS 仅是 Reference Fact，不是 QuickApp Kit Runtime 输入。
- 未知上游 URL、commit/tag、许可证和获取时间保持 `[待验证]`。
- 本目录不执行 T06-T11，不归属 Toolkit、Platform 或 Benchmark 工作。
