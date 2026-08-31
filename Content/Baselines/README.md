# 图像回归基线

两份签名,都是末帧的 32×18 平均池化,由 `Scripts/verify.ps1` 的"图像回归"
步骤逐次比对:

- `demo-scene.sig` —— 演示场景(一盏方向光,几个物体)。
- `showcase-scene.sig` —— 综合场景(三种光源类型都投影,四类材质,全套后处理)。

两份都要,因为它们的覆盖面不同。演示场景轻,能抓住基础着色的回归;综合场景把
三条阴影路径、分簇、GPU 驱动、TAA、GTAO、泛光同时跑起来,子系统之间的互相
影响只有它抓得住。

## 它能抓住什么

着色路径上那些**数值合法但结果变了**的回归:UBO 字段整体错位、材质参数读错、
某盏光源丢失、色调映射改动、阴影跑位。这些在其它自检里全部是绿的 —— 速度矢量
照样为零,法线照样朝向相机,能量照样守恒。

## 它抓不住什么

比的是 32×18 的格子均值,每格覆盖 40×40 像素。单个物体轮廓移动几个像素、
细小高光的位置变化,都会被平均掉。它是"整体是否还对"的哨兵,不是像素级 diff。

## 什么时候需要重新生成

- **换了显卡或驱动**。基线里的数字来自具体硬件。
- **有意改变了画面**(新的色调映射、改了默认光照、换了演示场景)。

重新生成:

```
.\Binaries\Development\Win64\LimxLaunch.exe --frames 20 --warmup 5 --screenshot shot.ppm
pwsh Scripts/image-signature.ps1 -Ppm shot.ppm -Write Content/Baselines/demo-scene.sig
```

综合场景(注意选项必须与 verify.ps1 里那一步逐字一致 —— 少一个 `--gtao-half`
画面就不同):

```
.\Binaries\Development\Win64\LimxLaunch.exe --showcase --gpu-driven --gtao --gtao-half --taa --bloom --clustered --frames 20 --warmup 5 --screenshot shot.ppm
pwsh Scripts/image-signature.ps1 -Ppm shot.ppm -Write Content/Baselines/showcase-scene.sig
```

**生成之前必须先看一眼 `shot.ppm` 确认画面确实正确。** 出现不一致时直接重新生成
基线,等于把这个检查永久关掉 —— 而且关掉之后没有任何迹象。
