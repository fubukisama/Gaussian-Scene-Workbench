# 3DGS Training Kit

这套脚本用于在本机重复训练新的 3D Gaussian Splatting 模型。

## 一次性准备

如果已经按本次流程配置好 `gaussian-splatting` 环境，可以直接跳到“训练新场景”。

如果重新 clone 了官方仓库，先运行：

```bat
training_kit\apply_local_fixes.bat
training_kit\create_env.bat
```

这些修复包含：

- Windows + 新版 COLMAP 参数兼容
- VS2022 新工具集 + CUDA 11.8 编译兼容
- 从已有 `point_cloud` 继续训练的 `--load_iteration` 参数
- Pillow 缺失 `libdeflate.dll` 的本机修复

## 训练新场景

1. 创建场景目录：

```bat
training_kit\new_scene.bat scene_name
```

2. 把照片放到：

```text
datasets\scene_name\images
```

3. 转换照片，生成 COLMAP 相机位姿：

```bat
training_kit\convert_scene.bat scene_name
```

4. 快速试训到 7000 步：

```bat
training_kit\train_quick_7000.bat scene_name
```

5. 继续优化到 30000 步：

```bat
training_kit\continue_train.bat scene_name 7000 30000
```

最终模型会在：

```text
output\scene_name\point_cloud\iteration_30000\point_cloud.ply
```

## 从零直接训练 30000 步

```bat
training_kit\train_full_30000.bat scene_name
```

## 低显存建议

这台电脑是 4GB 显存，脚本默认使用：

```text
-r 8 --data_device cpu
```

如果以后换更大显存 GPU，可以把脚本里的 `-r 8` 改成 `-r 4` 或 `-r 2` 提高质量。

