# 展示

用 `tools/showcase/record.ps1` 直接录制管理窗口和终端（[方法](/zh/reference/building#文档与展示素材)）。

## 三个无头游戏，一个管理窗口

<video controls muted playsinline preload="none" poster="/BeDD/showcase/desktop-poster.webp" src="/BeDD/showcase/desktop.mp4" width="1280" height="720" style="width:100%;height:auto"></video>

测试脚本同时驱动三个无头 Minecraft 实例。游戏没有自己的窗口：管理窗口实时显示每一个，以及它的实例、pid 和有没有输入进来。

## 脚本每一行，游戏里立刻生效

<video controls muted playsinline preload="none" poster="/BeDD/showcase/script-and-game-poster.webp" src="/BeDD/showcase/script-and-game.mp4" width="1020" height="360" style="width:100%;height:auto"></video>

`Send-BeDDKey E` 打开背包，`Send-BeDDMouse` 悬停、挪动钻石剑，`ESC` 关闭，全程不用碰那个游戏。

## 你的输入只归你

![你的输入只到你自己的应用；脚本通过各自的管道驱动三个后台游戏](/showcase/how-it-feels.svg)

鼠标键盘只给你自己的应用；脚本通过各自的管道发给每个游戏，游戏处理完才返回。

## 聊天命令，替你输入

![Send-BeDDText 在聊天栏里输入 /summon sheep 和 /summon cow](/showcase/chat-summon.webp)

`Send-BeDDText` 在聊天栏里输入 `/summon sheep` 和 `/summon cow`，游戏就像玩家亲手输入一样执行。

## 指哪儿悬停，指哪儿点

![虚拟光标在背包里触发物品提示](/showcase/inventory-hover.webp)

虚拟光标能触发物品提示、点击格子；这张图就是 `Save-BeDDScreenshot` 对一个移出屏幕的窗口截的。

## 所有游戏，一个窗口

![管理窗口里的三个无头游戏，旁边是运行脚本的终端](/showcase/desktop-poster.webp)

没有游戏窗口，没有任务栏按钮，也不抢焦点：每次测试的游戏都显示在管理窗口里，关掉它就关掉全部。每个游戏都跑在自己的实例克隆里，克隆又快又省空间。
