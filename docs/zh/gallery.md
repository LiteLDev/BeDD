# 展示

用 `tools/showcase/record.ps1` 录制真实屏幕（[方法](/zh/reference/building#文档与展示素材)）。

## 三个游戏，一张桌面，电脑照样归你用

<video controls muted playsinline preload="none" poster="/BeDD/showcase/desktop-poster.webp" src="/BeDD/showcase/desktop.mp4" width="1280" height="720" style="width:100%;height:auto"></video>

测试脚本同时驱动三个 Minecraft 实例，前面的终端始终保持焦点。录的是真实屏幕。

## 脚本每一行，游戏里立刻生效

<video controls muted playsinline preload="none" poster="/BeDD/showcase/script-and-game-poster.webp" src="/BeDD/showcase/script-and-game.mp4" width="1280" height="360" style="width:100%;height:auto"></video>

`Send-BeDDKey E` 打开背包，`Send-BeDDMouse` 悬停、挪动钻石剑，`ESC` 关闭，全程不用点那个窗口。

## 你的输入只归你

![你的输入只到你自己的应用；脚本通过各自的管道驱动三个后台游戏](/showcase/how-it-feels.svg)

鼠标键盘只给你自己的应用；脚本通过各自的管道发给每个游戏，游戏处理完才返回。

## 聊天命令，替你输入

![Send-BeDDText 在聊天栏里输入 /summon sheep 和 /summon cow](/showcase/chat-summon.webp)

`Send-BeDDText` 在聊天栏里输入 `/summon sheep` 和 `/summon cow`，游戏就像玩家亲手输入一样执行。

## 指哪儿悬停，指哪儿点

![虚拟光标在背包里触发物品提示](/showcase/inventory-hover.webp)

虚拟光标能触发物品提示、点击格子；这张图就是 `Save-BeDDScreenshot` 对一个移出屏幕的窗口截的。

## 游戏从不抢焦点

![三个游戏和一个终端；只有终端的标题栏是激活的](/showcase/desktop-poster.webp)

看标题栏：只有终端是激活的。每个游戏都跑在自己的实例克隆里，克隆又快又省空间。
