# Wearable Fitness Band

椭圆手环（194x368）运动健康快应用 Showcase。

## 功能

- Home：竖向卡片流，展示步数、心率、卡路里、距离
- Goals：今日目标列表，含 keyed for、if 条件渲染、滚动
- Detail：目标详情，含 router.push/back 导航

## 构建

```bash
node scripts/build-band.mjs
```

## 运行

```bash
SDL_VIDEODRIVER=dummy ./build-m1-s2/quickapp_case001_lvgl \
  --rpk showcases/wearable-fitness-band/dist/wearable-fitness-band.rpk
```
