# QuickApp Examples

[QuickApp Kit](https://github.com/quickapp-kit) 的示例应用和集成演示。

## 目录说明

| 目录 | 说明 |
|------|------|
| `composition/` | C++ 组合示例 — 包加载、JS 执行、LVGL 渲染 |
| `binding-001/` | 数据绑定示例（JS ↔ Native） |
| `baselines/` | 正确性验证基线用例 |
| `alliance-hap-case001/` | 联盟 HAP 范例（UX 源码，只读基线） |
| `quickapp-code-test2/` | 另一个快应用示例工程 |

## 构建（C++ 示例）

需要兄弟仓库：`quickapp-runtime-core`、`quickapp-runtime-js`、`quickapp-runtime-lvgl`。

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

### 运行 LVGL 模拟器

```bash
./build/quickapp_lvgl_simulator
```

## 快应用示例项目

```bash
cd alliance-hap-case001
npm install
# 使用 quickapp-toolkit CLI 构建/运行
```

## 相关仓库

- [quickapp-runtime-core](https://github.com/quickapp-kit/quickapp-runtime-core)
- [quickapp-runtime-js](https://github.com/quickapp-kit/quickapp-runtime-js)
- [quickapp-runtime-lvgl](https://github.com/quickapp-kit/quickapp-runtime-lvgl)
- [quickapp-toolkit](https://github.com/quickapp-kit/quickapp-toolkit)

## 许可证

[MIT](LICENSE)
