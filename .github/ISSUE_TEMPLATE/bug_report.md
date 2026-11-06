---
name: Bug report
about: 出问题了？请按下面的模板把信息填全，否则很难定位
title: '[Bug] '
labels: bug
---

## 现象

<!-- 一句话说明：UI 也被滤镜影响了 / 没有 injected / 游戏启动不了 / 崩溃 ... -->

## 环境

| 项 | 内容 |
|---|---|
| PreUI 版本 | <!-- 例如 0.7.0，见 PreUI.log 第一行 --> |
| ReShade 版本 | <!-- 例如 6.8.0 --> |
| 游戏 / 引擎 | <!-- 例如 XXX / UE4.27 --> |
| 分辨率 / 是否 HDR | |
| 显卡 / 驱动版本 | |
| DLSS 设置 | <!-- 质量档位；帧生成是否开启 --> |
| 系统 | <!-- Windows 版本 --> |

## 日志

`PreUI.log` 全文粘贴（`PreUI.ini` 里 `Trace=1`）：

```
把 PreUI.log 内容贴在这里
```

`ReShade.log` 最后 30 行：

```
把 ReShade.log 尾部贴在这里
```

## 已经试过

- [ ] `PreUI.ini` 里 `Enabled=0`（能否启动）
- [ ] 移除其它 ReShade add-on（尤其是 hook NGX 的，如 RenoDX）
- [ ] 关闭 DLSS 帧生成
- [ ] 调大 `DelayFrames`

## 补充

<!-- 截图 / 录屏 / 你的猜测 -->
