import * as THREE from "three";
import { TrackballControls } from "./vendor/controls/TrackballControls.js?v=20260707_app_pan";
import * as GaussianSplats3D from "gaussian-splats-3d";
import {
  isEditableShortcutTarget,
  selectionModeStatus,
  shouldStartSelectionDrag,
  temporaryNavigationStatus,
} from "./interaction.mjs";
import { parsePlyMesh } from "./mesh-loader.mjs";
import { parseObjTexturedMesh } from "./textured-mesh-loader.mjs";
import { textureOptionsFromQuality } from "./texture-options.mjs?v=20260603_colmap_defaults";
import { configureOpenMvsTexture } from "./texture-rendering.mjs?v=20260604_colmap_defaults";
import {
  compactMeshByDeletedFaces,
  facesTouchingVertices,
  meshToAsciiPly,
  selectableVertices,
} from "./mesh-edit.mjs";
import { parseMeshChunk } from "./mesh-chunk-loader.mjs";
import { shouldKeepUnsavedMeshTrim } from "./mesh-state.mjs?v=20260616_mesh_reload_guard";
import { createTrainingFileStore } from "./training-files.mjs";
import { makePanelDraggable } from "./floating-panels.mjs";
import { createFpsMeter } from "./fps-meter.mjs";
import { createGpuRenderTimer } from "./gpu-render-timer.mjs";
import { existingTrainingOutputSceneName, trainingOutputSceneName } from "./training-targets.mjs?v=20260625_aligned_source";
import {
  meshActionsForMode,
  meshModeMatchesBackend,
  meshModeSupportsTextureBake,
  meshModeUses3dgs,
  meshModeUsesGs2Mesh,
  meshModeUsesSugar,
} from "./mesh-mode-capabilities.mjs?v=20260629_gs2mesh_texture";
import { sugarOptionsFromPreset } from "./sugar-options.mjs?v=20260625_sugar_quality_ui";
import {
  collectPickingIds,
  deviceReadRect,
  encodePickingId,
  pickingRenderKey,
  pickingBoundsForBrush,
  pickingBoundsForPolygon,
  pickingBoundsForRect,
  pointInPickingPolygon,
  shouldSampleBrushPicking,
} from "./selection-picking.mjs";

const MESH_PREVIEW_SIZE_THRESHOLD = 80 * 1024 * 1024;
const TEXTURED_OBJ_INLINE_LOAD_LIMIT = 256 * 1024 * 1024;

const canvas = document.getElementById("three");
const overlay = document.getElementById("overlay");
const toolbar = document.getElementById("toolbar");
const ctx = overlay.getContext("2d");
const statusEl = document.getElementById("status");
const fpsMeterEl = document.getElementById("fpsMeter");
const sceneSelect = document.getElementById("scene");
const languageSelect = document.getElementById("languageSelect");
const uiScaleSelect = document.getElementById("uiScaleSelect");
const serverPortEl = document.getElementById("serverPort");
const appVersionEl = document.getElementById("appVersion");
const iterationInput = document.getElementById("iteration");
const outputInput = document.getElementById("outputScene");
const visibleSelectionToggle = document.getElementById("visibleSelection");
const splatImportFileInput = document.getElementById("splatImportFile");
const openAssetManagerButton = document.getElementById("openAssetManager");
const openExperimentManagerButton = document.getElementById("openExperimentManager");
const downloadSpzButton = document.getElementById("downloadSpz");
const downloadSogButton = document.getElementById("downloadSog");
const trainSceneInput = document.getElementById("trainScene");
const alignedSourceSelect = document.getElementById("alignedSource");
const trainFilesInput = document.getElementById("trainFiles");
const trainFolderInput = document.getElementById("trainFolder");
const trainMasksInput = document.getElementById("trainMasks");
const trainBackendSelect = document.getElementById("trainBackend");
const trainQualitySelect = document.getElementById("trainQuality");
const trainIterationsInput = document.getElementById("trainIterations");
const trainResolutionInput = document.getElementById("trainResolution");
const trainOptimizerSelect = document.getElementById("trainOptimizer");
const trainDepthRatioInput = document.getElementById("trainDepthRatio");
const trainDensifyGradInput = document.getElementById("trainDensifyGrad");
const trainDensifyIntervalInput = document.getElementById("trainDensifyInterval");
const trainDensifyUntilInput = document.getElementById("trainDensifyUntil");
const trainAntialiasingToggle = document.getElementById("trainAntialiasing");
const trainExposureToggle = document.getElementById("trainExposure");
const colmapPresetSelect = document.getElementById("colmapPreset");
const colmapMatchingSelect = document.getElementById("colmapMatching");
const colmapCameraSelect = document.getElementById("colmapCamera");
const colmapMaxImageSizeInput = document.getElementById("colmapMaxImageSize");
const colmapMaxFeaturesInput = document.getElementById("colmapMaxFeatures");
const colmapBaIterationsInput = document.getElementById("colmapBaIterations");
const colmapBaFreqInput = document.getElementById("colmapBaFreq");
const colmapMinMatchesInput = document.getElementById("colmapMinMatches");
const colmapOverlapInput = document.getElementById("colmapOverlap");
const colmapResetToggle = document.getElementById("colmapReset");
const colmapGpuToggle = document.getElementById("colmapGpu");
const colmapGuidedToggle = document.getElementById("colmapGuided");
const videoFpsInput = document.getElementById("videoFps");
const trainPanel = document.getElementById("trainPanel");
const trainLog = document.getElementById("trainLog");
const mediaPanel = document.getElementById("mediaPanel");
const mediaSummary = document.getElementById("mediaSummary");
const mediaPreview = document.getElementById("mediaPreview");
const importProgressPanel = document.getElementById("importProgressPanel");
const importProgressStage = document.getElementById("importProgressStage");
const importProgressPercent = document.getElementById("importProgressPercent");
const importProgressFill = document.getElementById("importProgressFill");
const importProgressDetails = document.getElementById("importProgressDetails");
const importProgressRecent = document.getElementById("importProgressRecent");
const jobCenterPanel = document.getElementById("jobCenterPanel");
const jobCenterList = document.getElementById("jobCenterList");
const openJobCenterButton = document.getElementById("openJobCenter");
const assetManagerPanel = document.getElementById("assetManagerPanel");
const assetManagerSummary = document.getElementById("assetManagerSummary");
const assetManagerList = document.getElementById("assetManagerList");
const experimentManagerPanel = document.getElementById("experimentManagerPanel");
const experimentManagerSummary = document.getElementById("experimentManagerSummary");
const experimentManagerList = document.getElementById("experimentManagerList");
const importOnlyButton = document.getElementById("importOnly");
const startTrainButton = document.getElementById("startTrain");
const trainExistingButton = document.getElementById("trainExisting");
const runColmapButton = document.getElementById("runColmap");
const cancelTrainButton = document.getElementById("cancelTrain");
const clearTrainInputButton = document.getElementById("clearTrainInput");
const meshModeSelect = document.getElementById("meshMode");
const meshResInput = document.getElementById("meshRes");
const meshTextureResInput = document.getElementById("meshTextureRes");
const meshTextureQualitySelect = document.getElementById("meshTextureQuality");
const meshTextureBackendSelect = document.getElementById("meshTextureBackend");
const sugarQualitySelect = document.getElementById("sugarQuality");
const sugarPolySelect = document.getElementById("sugarPoly");
const sugarRefinementSelect = document.getElementById("sugarRefinement");
const sugarRegularizationSelect = document.getElementById("sugarRegularization");
const sugarSurfaceLevelInput = document.getElementById("sugarSurfaceLevel");
const sugarSquareSizeInput = document.getElementById("sugarSquareSize");
const sugarMaxImagesInput = document.getElementById("sugarMaxImages");
const sugarMaxImageSizeInput = document.getElementById("sugarMaxImageSize");
const sugarPostprocessToggle = document.getElementById("sugarPostprocess");
const gs2meshDownsampleInput = document.getElementById("gs2meshDownsample");
const gs2meshBaselineInput = document.getElementById("gs2meshBaseline");
const gs2meshTsdfVoxelInput = document.getElementById("gs2meshTsdfVoxel");
const gs2meshTsdfMinInput = document.getElementById("gs2meshTsdfMin");
const gs2meshTsdfMaxInput = document.getElementById("gs2meshTsdfMax");
const gs2meshScene360Toggle = document.getElementById("gs2meshScene360");
const exportMeshButton = document.getElementById("exportMesh");
const loadMeshButton = document.getElementById("loadMesh");
const bakeTextureButton = document.getElementById("bakeTexture");
const loadTextureButton = document.getElementById("loadTexture");
const showMeshToggle = document.getElementById("showMesh");
const meshTextureToggle = document.getElementById("meshTexture");
const meshTextureLabel = document.getElementById("meshTextureLabel");
const meshTrimToggle = document.getElementById("meshTrim");
const downloadMeshButton = document.getElementById("downloadMesh");
const downloadTextureButton = document.getElementById("downloadTexture");
const downloadGlbButton = document.getElementById("downloadGlb");
const psnrBackendSelect = document.getElementById("psnrBackend");
const psnrCountInput = document.getElementById("psnrCount");
const psnrEvalWidthInput = document.getElementById("psnrEvalWidth");
const runPsnrButton = document.getElementById("runPsnr");
const openPsnrOutputButton = document.getElementById("openPsnrOutput");
const IMAGE_EXTS = new Set(["jpg", "jpeg", "png", "bmp", "tif", "tiff", "webp"]);
const VIDEO_EXTS = new Set(["mp4", "mov", "avi", "mkv", "webm", "m4v"]);
const AXIS_GIZMO_AXES = [
  { label: "X", color: "#ff4d4d", vector: [1, 0, 0] },
  { label: "Y", color: "#6ee27d", vector: [0, 1, 0] },
  { label: "Z", color: "#6d8cff", vector: [0, 0, 1] }
];
const CENTER_GIZMO_SCREEN_RADIUS_PX = 92;
const I18N = {
  en: {
    "app.subtitle": "Gaussian scene research toolkit",
    "section.scene": "Scene",
    "section.view": "View",
    "section.language": "Language",
    "section.selection": "Selection",
    "section.output": "Output",
    "section.mesh": "Mesh",
    "section.analysis": "Analysis",
    "section.train": "Train",
    "section.jobs": "Jobs",
    "label.scene": "Scene",
    "label.iter": "Iter",
    "label.size": "Size",
    "label.language": "UI",
    "label.uiScale": "Scale",
    "label.port": "Port",
    "label.version": "Version",
    "label.brush": "Brush",
    "label.output": "Output",
    "label.meshMode": "Mode",
    "label.res": "Res",
    "label.tex": "Tex",
    "label.texQuality": "Tex Quality",
    "label.bake": "Bake",
    "label.sugarQuality": "SuGaR Quality",
    "label.sugarPoly": "Poly",
    "label.sugarRefine": "Refine",
    "label.sugarReg": "Reg",
    "label.sugarLevel": "Level",
    "label.sugarUv": "UV",
    "label.sugarImages": "Images",
    "label.sugarImgSize": "Img Max",
    "label.gs2meshDownsample": "Down",
    "label.gs2meshViews": "Views",
    "label.gs2meshBaseline": "Base %",
    "label.gs2meshVoxel": "Voxel",
    "label.gs2meshDepth": "Depth",
    "label.name": "Name",
    "label.alignedSource": "Aligned",
    "label.backend": "Backend",
    "label.mode": "Mode",
    "label.trainIter": "Iter",
    "label.trainRes": "Train r",
    "label.optimizer": "Opt",
    "label.depthRatio": "Depth",
    "label.densifyGrad": "Dens Grad",
    "label.densifyInterval": "Dens Int",
    "label.densifyUntil": "Dens Until",
    "label.colmapPreset": "COLMAP",
    "label.matching": "Match",
    "label.camera": "Camera",
    "label.maxImg": "Max Img",
    "label.features": "Features",
    "label.baIter": "BA Iter",
    "label.baFreq": "BA Freq",
    "label.minMatches": "Min Match",
    "label.overlap": "Overlap",
    "label.fps": "FPS",
    "label.psnrBackend": "Backend",
    "label.psnrCount": "Views",
    "label.psnrEvalWidth": "Eval W",
    "toggle.points": "Points",
    "toggle.soft": "Soft",
    "toggle.real": "Real",
    "toggle.cameras": "Cameras",
    "toggle.pivot": "Pivot",
    "toggle.resetColmap": "Reset COLMAP",
    "toggle.colmapGpu": "COLMAP GPU",
    "toggle.guided": "Guided",
    "toggle.visibleOnly": "Visible",
    "toggle.aa": "AA",
    "toggle.exposure": "Exposure",
    "toggle.overwrite": "Overwrite",
    "toggle.showMesh": "Show Mesh",
    "toggle.texture": "Texture",
    "toggle.vertexColor": "Vertex Color",
    "toggle.photoTexture": "Photo Texture",
    "toggle.meshTrim": "Mesh Trim",
    "toggle.sugarPost": "Postprocess",
    "toggle.gs2mesh360": "360 Scene",
    "button.load": "Load",
    "button.reset": "Reset",
    "button.previewReal": "Preview Real",
    "button.navigate": "Navigate",
    "button.rect": "Rect",
    "button.lasso": "Lasso",
    "button.brush": "Brush",
    "button.clear": "Clear",
    "button.invert": "Invert",
    "button.delete": "Delete",
    "button.undo": "Undo",
    "button.save": "Save",
    "button.importSplat": "Import Splat",
    "button.assetManager": "Assets",
    "button.experimentManager": "Experiments",
    "button.cloneExperiment": "Clone",
    "button.resumeCheckpoint": "Resume",
    "button.compareExperiment": "Compare",
    "button.markBest": "Best",
    "button.pinCheckpoint": "Pin",
    "button.unpinCheckpoint": "Unpin",
    "button.deleteCheckpoint": "Delete CKPT",
    "button.downloadSpz": "Download SPZ",
    "button.downloadSog": "Download SOG",
    "button.exportAsset": "Export",
    "button.openAsset": "Open",
    "button.downloadJson": "JSON",
    "button.downloadCsv": "CSV",
    "button.jobCenter": "Job Center",
    "button.cancelJob": "Cancel",
    "button.retryJob": "Retry",
    "button.downloadJob": "Download",
    "button.openOutput": "Open Output",
    "button.openLog": "Log",
    "button.exportMesh": "Export Mesh",
    "button.loadMesh": "Load Mesh",
    "button.bakeTexture": "Bake Photo Texture",
    "button.loadTexture": "Load Photo Texture",
    "button.downloadMesh": "Download Mesh",
    "button.downloadTexture": "Download Photo Texture",
    "button.downloadGlb": "Download GLB",
    "button.runPsnr": "Render PSNR",
    "button.openPsnrOutput": "Open PSNR Output",
    "button.photosVideo": "Photos/Video",
    "button.folder": "Folder",
    "button.masks": "Masks",
    "button.clearInput": "Clear Input",
    "button.importOnly": "Import Only",
    "button.importTrain": "Import + Train",
    "button.trainExisting": "Train Existing",
    "button.runColmap": "Run COLMAP",
    "button.cancel": "Cancel",
    "button.refresh": "Refresh",
    "button.close": "Close",
    "button.checkEnv": "Check Env",
    "option.bounded": "Bounded",
    "option.unbounded": "Unbounded",
    "option.sugar": "SuGaR",
    "option.gs2mesh": "GS2Mesh",
    "option.sugarPreview": "Preview",
    "option.sugarBalanced": "Balanced",
    "option.sugarHigh": "High",
    "option.sugarUltra": "Ultra",
    "option.lowPoly": "Low",
    "option.highPoly": "High",
    "option.refineShort": "Short",
    "option.refineMedium": "Medium",
    "option.refineLong": "Long",
    "option.regDn": "DN consistency",
    "option.regDensity": "Density",
    "option.regSdf": "SDF",
    "option.fast": "Fast 30k",
    "option.balanced": "Balanced 100k",
    "option.smooth": "Smooth 8192",
    "option.original": "Original",
    "option.ultra": "Original 8192",
    "option.quick": "Quick 7000",
    "option.full": "Full 30000",
    "option.quality": "Quality r4 30000",
    "option.maxQuality": "Max r2 30000",
    "option.colmapDefault": "Default",
    "option.colmapRobust": "Robust",
    "option.colmapSequential": "Sequential",
    "option.exhaustive": "Exhaustive",
    "option.sequential": "Sequential",
    "option.noAlignedSource": "No aligned source",
    "option.uiScaleAuto": "Auto",
    "summary.meshAdvanced": "Mesh Parameters",
    "summary.trainAdvanced": "Training Parameters",
    "panel.inputPreview": "Input Preview",
    "panel.trainingLog": "Training Log",
    "panel.importProgress": "Import Progress",
    "panel.jobCenter": "Job Center",
    "panel.assetManager": "Asset Manager",
    "panel.experimentManager": "Experiment Manager",
    "status.loadingScenes": "Loading scenes...",
    "status.noInput": "No input selected",
    "status.inputSelected": "input selected",
    "status.selectScene": "Select a scene and press Load.",
    "status.noJobs": "No jobs yet.",
    "status.noAssets": "No assets found.",
    "status.noCheckpoints": "No checkpoints found.",
    "status.noDiff": "No parameter differences.",
    "status.noModels": "No trained models found under output/."
  },
  zh: {
    "app.subtitle": "高斯场景研究工具链",
    "section.scene": "场景",
    "section.view": "视图",
    "section.language": "语言",
    "section.selection": "选择",
    "section.output": "输出",
    "section.mesh": "网格",
    "section.analysis": "分析",
    "section.train": "训练",
    "section.jobs": "任务",
    "label.scene": "场景",
    "label.iter": "迭代",
    "label.size": "大小",
    "label.language": "界面",
    "label.uiScale": "缩放",
    "label.port": "端口",
    "label.version": "版本",
    "label.brush": "笔刷",
    "label.output": "输出",
    "label.meshMode": "模式",
    "label.res": "分辨率",
    "label.tex": "纹理",
    "label.texQuality": "纹理质量",
    "label.bake": "烘焙",
    "label.sugarQuality": "SuGaR质量",
    "label.sugarPoly": "面数",
    "label.sugarRefine": "细化",
    "label.sugarReg": "正则",
    "label.sugarLevel": "等值",
    "label.sugarUv": "UV",
    "label.sugarImages": "图片数",
    "label.sugarImgSize": "图像边长",
    "label.gs2meshDownsample": "降采样",
    "label.gs2meshViews": "视图数",
    "label.gs2meshBaseline": "基线%",
    "label.gs2meshVoxel": "体素",
    "label.gs2meshDepth": "深度",
    "label.name": "名称",
    "label.alignedSource": "对齐源",
    "label.backend": "后端",
    "label.mode": "模式",
    "label.trainIter": "迭代",
    "label.trainRes": "训练 r",
    "label.optimizer": "优化器",
    "label.depthRatio": "深度",
    "label.densifyGrad": "增密梯度",
    "label.densifyInterval": "增密间隔",
    "label.densifyUntil": "增密到",
    "label.colmapPreset": "COLMAP",
    "label.matching": "匹配",
    "label.camera": "相机",
    "label.maxImg": "最大图像",
    "label.features": "特征数",
    "label.baIter": "BA迭代",
    "label.baFreq": "BA频率",
    "label.minMatches": "最小匹配",
    "label.overlap": "重叠",
    "label.fps": "帧率",
    "label.psnrBackend": "后端",
    "label.psnrCount": "视图数",
    "label.psnrEvalWidth": "评估宽",
    "toggle.points": "点云",
    "toggle.soft": "软渲染",
    "toggle.real": "真实渲染",
    "toggle.cameras": "相机",
    "toggle.pivot": "中心球",
    "toggle.resetColmap": "重置COLMAP",
    "toggle.colmapGpu": "COLMAP GPU",
    "toggle.guided": "引导匹配",
    "toggle.visibleOnly": "仅可见",
    "toggle.aa": "抗锯齿",
    "toggle.exposure": "曝光",
    "toggle.overwrite": "覆盖",
    "toggle.showMesh": "显示网格",
    "toggle.texture": "纹理",
    "toggle.vertexColor": "顶点颜色",
    "toggle.photoTexture": "照片纹理",
    "toggle.meshTrim": "网格修剪",
    "toggle.sugarPost": "后处理",
    "toggle.gs2mesh360": "360场景",
    "button.load": "加载",
    "button.reset": "重置",
    "button.previewReal": "预览真实渲染",
    "button.navigate": "导航",
    "button.rect": "矩形",
    "button.lasso": "套索",
    "button.brush": "笔刷",
    "button.clear": "清除",
    "button.invert": "反选",
    "button.delete": "删除",
    "button.undo": "撤销",
    "button.save": "保存",
    "button.importSplat": "导入 Splat",
    "button.assetManager": "资产",
    "button.experimentManager": "实验",
    "button.cloneExperiment": "复制实验",
    "button.resumeCheckpoint": "继续训练",
    "button.compareExperiment": "对比",
    "button.markBest": "设为最佳",
    "button.pinCheckpoint": "固定",
    "button.unpinCheckpoint": "取消固定",
    "button.deleteCheckpoint": "删除CKPT",
    "button.downloadSpz": "下载 SPZ",
    "button.downloadSog": "下载 SOG",
    "button.exportAsset": "导出",
    "button.openAsset": "打开",
    "button.downloadJson": "JSON",
    "button.downloadCsv": "CSV",
    "button.jobCenter": "任务中心",
    "button.cancelJob": "终止",
    "button.retryJob": "重试",
    "button.downloadJob": "下载",
    "button.openOutput": "打开目录",
    "button.openLog": "日志",
    "button.exportMesh": "导出网格",
    "button.loadMesh": "加载网格",
    "button.bakeTexture": "照片烘焙纹理",
    "button.loadTexture": "加载照片纹理",
    "button.downloadMesh": "下载网格",
    "button.downloadTexture": "下载照片纹理",
    "button.downloadGlb": "下载 GLB",
    "button.runPsnr": "渲染 PSNR",
    "button.openPsnrOutput": "打开 PSNR 结果",
    "button.photosVideo": "照片/视频",
    "button.folder": "文件夹",
    "button.masks": "Mask",
    "button.clearInput": "清空输入",
    "button.importOnly": "仅导入",
    "button.importTrain": "导入并训练",
    "button.trainExisting": "训练已有数据",
    "button.runColmap": "运行 COLMAP",
    "button.cancel": "取消",
    "button.refresh": "刷新",
    "button.close": "关闭",
    "button.checkEnv": "检查环境",
    "option.bounded": "有界",
    "option.unbounded": "无界",
    "option.sugar": "SuGaR",
    "option.gs2mesh": "GS2Mesh",
    "option.sugarPreview": "预览",
    "option.sugarBalanced": "均衡",
    "option.sugarHigh": "高质量",
    "option.sugarUltra": "最高",
    "option.lowPoly": "低",
    "option.highPoly": "高",
    "option.refineShort": "短",
    "option.refineMedium": "中",
    "option.refineLong": "长",
    "option.regDn": "DN一致性",
    "option.regDensity": "密度",
    "option.regSdf": "SDF",
    "option.fast": "快速 30k",
    "option.balanced": "均衡 100k",
    "option.smooth": "平滑 8192",
    "option.original": "原始",
    "option.ultra": "原始 8192",
    "option.quick": "快速 7000",
    "option.full": "完整 30000",
    "option.quality": "高质量 r4 30000",
    "option.maxQuality": "最高 r2 30000",
    "option.colmapDefault": "默认",
    "option.colmapRobust": "稳健",
    "option.colmapSequential": "顺序",
    "option.exhaustive": "全量匹配",
    "option.sequential": "顺序匹配",
    "option.noAlignedSource": "无对齐源",
    "option.uiScaleAuto": "自动",
    "summary.meshAdvanced": "网格参数",
    "summary.trainAdvanced": "训练参数",
    "panel.inputPreview": "输入预览",
    "panel.trainingLog": "训练日志",
    "panel.importProgress": "导入/抽帧进度",
    "panel.jobCenter": "任务中心",
    "panel.assetManager": "资产管理",
    "panel.experimentManager": "实验管理",
    "status.loadingScenes": "正在加载场景...",
    "status.noInput": "未选择输入",
    "status.inputSelected": "个输入已选择",
    "status.selectScene": "选择场景后点击加载。",
    "status.noJobs": "暂无任务。",
    "status.noAssets": "没有找到资产。",
    "status.noCheckpoints": "没有找到 checkpoint。",
    "status.noDiff": "参数没有差异。",
    "status.noModels": "output/ 下没有找到训练模型。"
  },
  ja: {
    "app.subtitle": "ガウスシーン研究ツール",
    "section.scene": "シーン",
    "section.view": "表示",
    "section.language": "言語",
    "section.selection": "選択",
    "section.output": "出力",
    "section.mesh": "メッシュ",
    "section.analysis": "分析",
    "section.train": "学習",
    "section.jobs": "ジョブ",
    "label.scene": "シーン",
    "label.iter": "反復",
    "label.size": "サイズ",
    "label.language": "UI",
    "label.uiScale": "拡大率",
    "label.port": "ポート",
    "label.version": "版",
    "label.brush": "ブラシ",
    "label.output": "出力",
    "label.meshMode": "モード",
    "label.res": "解像度",
    "label.tex": "テクスチャ",
    "label.texQuality": "品質",
    "label.bake": "ベイク",
    "label.sugarQuality": "SuGaR品質",
    "label.sugarPoly": "面数",
    "label.sugarRefine": "精緻化",
    "label.sugarReg": "正則化",
    "label.sugarLevel": "レベル",
    "label.sugarUv": "UV",
    "label.sugarImages": "画像数",
    "label.sugarImgSize": "画像上限",
    "label.gs2meshDownsample": "縮小",
    "label.gs2meshViews": "ビュー数",
    "label.gs2meshBaseline": "基線%",
    "label.gs2meshVoxel": "ボクセル",
    "label.gs2meshDepth": "深度",
    "label.name": "名前",
    "label.alignedSource": "整列済み",
    "label.backend": "バックエンド",
    "label.mode": "モード",
    "label.trainIter": "反復",
    "label.trainRes": "学習 r",
    "label.optimizer": "最適化",
    "label.depthRatio": "深度",
    "label.densifyGrad": "密化勾配",
    "label.densifyInterval": "密化間隔",
    "label.densifyUntil": "密化終了",
    "label.colmapPreset": "COLMAP",
    "label.matching": "照合",
    "label.camera": "カメラ",
    "label.maxImg": "最大画像",
    "label.features": "特徴数",
    "label.baIter": "BA回数",
    "label.baFreq": "BA頻度",
    "label.minMatches": "最小照合",
    "label.overlap": "重複",
    "label.fps": "FPS",
    "label.psnrBackend": "バックエンド",
    "label.psnrCount": "ビュー数",
    "label.psnrEvalWidth": "評価幅",
    "toggle.points": "点群",
    "toggle.soft": "ソフト",
    "toggle.real": "実レンダー",
    "toggle.cameras": "カメラ",
    "toggle.pivot": "中心球",
    "toggle.resetColmap": "COLMAP初期化",
    "toggle.colmapGpu": "COLMAP GPU",
    "toggle.guided": "ガイド照合",
    "toggle.visibleOnly": "可視のみ",
    "toggle.aa": "AA",
    "toggle.exposure": "露出",
    "toggle.overwrite": "上書き",
    "toggle.showMesh": "メッシュ表示",
    "toggle.texture": "テクスチャ",
    "toggle.vertexColor": "頂点カラー",
    "toggle.photoTexture": "写真テクスチャ",
    "toggle.meshTrim": "メッシュ編集",
    "toggle.sugarPost": "後処理",
    "toggle.gs2mesh360": "360シーン",
    "button.load": "読込",
    "button.reset": "リセット",
    "button.previewReal": "実レンダー確認",
    "button.navigate": "移動",
    "button.rect": "矩形",
    "button.lasso": "投げ縄",
    "button.brush": "ブラシ",
    "button.clear": "クリア",
    "button.invert": "反転",
    "button.delete": "削除",
    "button.undo": "元に戻す",
    "button.save": "保存",
    "button.importSplat": "Splat 読込",
    "button.assetManager": "アセット",
    "button.experimentManager": "実験",
    "button.cloneExperiment": "複製",
    "button.resumeCheckpoint": "再開",
    "button.compareExperiment": "比較",
    "button.markBest": "Best",
    "button.pinCheckpoint": "固定",
    "button.unpinCheckpoint": "固定解除",
    "button.deleteCheckpoint": "CKPT削除",
    "button.downloadSpz": "SPZ 保存",
    "button.downloadSog": "SOG 保存",
    "button.exportAsset": "出力",
    "button.openAsset": "開く",
    "button.downloadJson": "JSON",
    "button.downloadCsv": "CSV",
    "button.jobCenter": "ジョブ",
    "button.cancelJob": "中止",
    "button.retryJob": "再実行",
    "button.downloadJob": "保存",
    "button.openOutput": "出力を開く",
    "button.openLog": "ログ",
    "button.exportMesh": "メッシュ出力",
    "button.loadMesh": "メッシュ読込",
    "button.bakeTexture": "写真テクスチャ",
    "button.loadTexture": "写真テクスチャ読込",
    "button.downloadMesh": "メッシュ保存",
    "button.downloadTexture": "写真テクスチャ保存",
    "button.downloadGlb": "GLB 保存",
    "button.runPsnr": "PSNRレンダー",
    "button.openPsnrOutput": "PSNR出力を開く",
    "button.photosVideo": "写真/動画",
    "button.folder": "フォルダ",
    "button.masks": "マスク",
    "button.clearInput": "入力クリア",
    "button.importOnly": "インポートのみ",
    "button.importTrain": "インポート+学習",
    "button.trainExisting": "既存データ学習",
    "button.runColmap": "COLMAP実行",
    "button.cancel": "中止",
    "button.refresh": "更新",
    "button.close": "閉じる",
    "button.checkEnv": "環境確認",
    "option.bounded": "有界",
    "option.unbounded": "無界",
    "option.sugar": "SuGaR",
    "option.gs2mesh": "GS2Mesh",
    "option.sugarPreview": "プレビュー",
    "option.sugarBalanced": "標準",
    "option.sugarHigh": "高品質",
    "option.sugarUltra": "最高",
    "option.lowPoly": "低",
    "option.highPoly": "高",
    "option.refineShort": "短",
    "option.refineMedium": "中",
    "option.refineLong": "長",
    "option.regDn": "DN一貫性",
    "option.regDensity": "密度",
    "option.regSdf": "SDF",
    "option.fast": "高速 30k",
    "option.balanced": "標準 100k",
    "option.smooth": "平滑 8192",
    "option.original": "オリジナル",
    "option.ultra": "オリジナル 8192",
    "option.quick": "高速 7000",
    "option.full": "完全 30000",
    "option.quality": "高品質 r4 30000",
    "option.maxQuality": "最高 r2 30000",
    "option.colmapDefault": "標準",
    "option.colmapRobust": "堅牢",
    "option.colmapSequential": "連続",
    "option.exhaustive": "総当たり",
    "option.sequential": "連続照合",
    "option.noAlignedSource": "整列済みデータなし",
    "option.uiScaleAuto": "自動",
    "summary.meshAdvanced": "メッシュ設定",
    "summary.trainAdvanced": "学習設定",
    "panel.inputPreview": "入力プレビュー",
    "panel.trainingLog": "学習ログ",
    "panel.importProgress": "インポート進捗",
    "panel.jobCenter": "ジョブセンター",
    "panel.assetManager": "アセット管理",
    "panel.experimentManager": "実験管理",
    "status.loadingScenes": "シーンを読み込み中...",
    "status.noInput": "入力未選択",
    "status.inputSelected": "件の入力を選択済み",
    "status.selectScene": "シーンを選択して読込を押してください。",
    "status.noJobs": "ジョブはありません。",
    "status.noAssets": "アセットがありません。",
    "status.noCheckpoints": "checkpoint がありません。",
    "status.noDiff": "パラメータ差分はありません。",
    "status.noModels": "output/ に学習済みモデルが見つかりません。"
  }
};

const COLMAP_UI_PRESETS = {
  default: {
    matching: "exhaustive",
    camera: "OPENCV",
    maxImageSize: -1,
    maxFeatures: 8192,
    baIterations: 50,
    baFreq: 500,
    minMatches: 15,
    overlap: 10,
    guided: false,
    gpu: true,
    reset: true,
  },
  robust: {
    matching: "exhaustive",
    camera: "OPENCV",
    maxImageSize: 2400,
    maxFeatures: 12000,
    baIterations: 15,
    baFreq: 100,
    minMatches: 10,
    overlap: 10,
    guided: true,
    gpu: true,
    reset: true,
  },
  sequential: {
    matching: "sequential",
    camera: "OPENCV",
    maxImageSize: 2400,
    maxFeatures: 12000,
    baIterations: 15,
    baFreq: 100,
    minMatches: 10,
    overlap: 20,
    guided: true,
    gpu: true,
    reset: true,
  },
};

const UI_SCALE_STORAGE_KEY = "cropEditorUiScale";
const UI_SCALE_CHOICES = new Set(["auto", "0.8", "0.9", "1", "1.1", "1.25"]);
const AUTO_UI_SCALE_MIN = 0.72;
const AUTO_UI_SCALE_MAX = 1;

let currentLanguage = localStorage.getItem("cropEditorLanguage") || "en";
let uiScaleMode = normalizeUiScaleMode(localStorage.getItem(UI_SCALE_STORAGE_KEY) || "auto");
let currentUiScale = 1;

let renderer, scene, centerGizmoScene, pickingScene, camera, controls, pointCloud, splatCloud, pickingCloud, pickingTarget, cameraGroup, selectionPoints, meshObject, meshSelectionPoints, centerGizmo;
let gpuRenderTimer = null;
let focusRaycaster = null;
let realSplatViewer = null;
let realSplatSceneIndex = null;
let realSplatLoading = false;
let realSplatReady = false;
let realPreviewKey = null;
let realMask = null;
let realIndexMaskInstalled = false;
let lastRealPointCount = 0;
let lastRealSplatCount = 0;
let positions, colors, opacities, splatScales, indexMap, deletedOriginal = new Set(), selected = new Set();
let mode = "navigate";
let dragStart = null;
let dragCurrent = null;
let brushSubtract = false;
let viewPanDrag = null;
let pivotPickMode = false;
let autoPickPivotAtCenter = false;
let lasso = [];
let lastBrushBuild = 0;
let undoStack = [];
let isSpaceDown = false;
let currentScene = null;
let currentIteration = null;
let currentBackend = "3dgs";
const outputBackendByName = new Map();
let alignedDatasets = [];
let currentBounds = null;
let hasUnsavedEdits = false;
let editRevision = 0;
let pickingRevision = -1;
let pickingTargetKey = "";
let pickingRenderCacheKey = "";
let lastBrushPickingSample = null;
const pickingStats = { renders: 0, cacheHits: 0, readbacks: 0, brushSkips: 0 };
let trainPollTimer = null;
let jobCenterPollTimer = null;
let trainMaskFiles = [];
let activeTrainJobId = null;
let importPollTimer = null;
let activeImportJobId = null;
let lastImportUploadPercent = 0;
let meshPollTimer = null;
let activeMeshJobId = null;
let psnrPollTimer = null;
let activePsnrJobId = null;
let lastPsnrJobId = null;
let lastMeshDownloadUrl = null;
let lastTextureDownloadUrl = null;
let lastGlbDownloadUrl = null;
let currentMeshUrl = null;
let currentMeshTexture = null;
let meshData = null;
let meshSelected = new Set();
let meshDeletedFaces = new Set();
let meshUndoStack = [];
let meshDirty = false;
let mediaObjectUrls = [];
const trainingFiles = createTrainingFileStore();
let threeReady = false;
let centerGizmoRadius = 1;
let lastToolbarHeight = 0;
let activeRibbonPanel = "";
const fpsMeter = createFpsMeter({ smoothingFrames: 60, minSamples: 5 });
let displayedFpsText = "";

function dot3(a, b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function finiteVector3(value, fallback) {
  if (!Array.isArray(value) || value.length !== 3 || value.some((item) => !Number.isFinite(item))) {
    return fallback;
  }
  return value;
}

function axisGizmoSegments(options) {
  options = options || {};
  const width = options.width;
  const height = options.height;
  const length = options.length === undefined ? 42 : options.length;
  const margin = options.margin === undefined ? 24 : options.margin;
  const cameraRight = options.cameraRight || [1, 0, 0];
  const cameraUp = options.cameraUp || [0, 0, 1];
  const w = Number.isFinite(width) ? width : 0;
  const h = Number.isFinite(height) ? height : 0;
  const axisLength = Number.isFinite(length) ? length : 42;
  const inset = Number.isFinite(margin) ? margin : 24;
  const origin = {
    x: Math.round(w - inset - axisLength),
    y: Math.round(h - inset - axisLength)
  };
  const right = finiteVector3(cameraRight, [1, 0, 0]);
  const up = finiteVector3(cameraUp, [0, 0, 1]);
  return AXIS_GIZMO_AXES.map((axis) => {
    return {
      label: axis.label,
      color: axis.color,
      vector: axis.vector,
      start: origin,
      end: {
        x: Math.round(origin.x + dot3(axis.vector, right) * axisLength),
        y: Math.round(origin.y - dot3(axis.vector, up) * axisLength)
      }
    };
  });
}

const TRAINING_UI_PRESETS = {
  "3dgs": {
    quick: {
      iterations: 7000,
      resolution: 8,
      optimizer: "default",
      antialiasing: false,
      exposure: false,
      densifyGrad: 0.0002,
      densifyInterval: 100,
      densifyUntil: 7000
    },
    full: {
      iterations: 30000,
      resolution: 8,
      optimizer: "default",
      antialiasing: false,
      exposure: false,
      densifyGrad: 0.0002,
      densifyInterval: 100,
      densifyUntil: 15000
    },
    quality: {
      iterations: 30000,
      resolution: 4,
      optimizer: "sparse_adam",
      antialiasing: true,
      exposure: false,
      densifyGrad: 0.00016,
      densifyInterval: 100,
      densifyUntil: 18000
    },
    max_quality: {
      iterations: 30000,
      resolution: 2,
      optimizer: "sparse_adam",
      antialiasing: true,
      exposure: true,
      densifyGrad: 0.00012,
      densifyInterval: 80,
      densifyUntil: 22000
    }
  },
  "2dgs": {
    quick: { iterations: 7000, resolution: 8, depthRatio: 0 },
    full: { iterations: 30000, resolution: 8, depthRatio: 0 },
    quality: { iterations: 30000, resolution: 4, depthRatio: 0 },
    max_quality: { iterations: 30000, resolution: 2, depthRatio: 0 }
  }
};

function centerGizmoMetrics(bounds) {
  if (!bounds || !Array.isArray(bounds.min) || !Array.isArray(bounds.max)) {
    return { visible: false, center: [0, 0, 0], radius: 1 };
  }
  const min = finiteVector3(bounds.min, null);
  const max = finiteVector3(bounds.max, null);
  if (!min || !max) {
    return { visible: false, center: [0, 0, 0], radius: 1 };
  }
  const size = [
    Math.max(0, max[0] - min[0]),
    Math.max(0, max[1] - min[1]),
    Math.max(0, max[2] - min[2])
  ];
  const diagonal = Math.hypot(size[0], size[1], size[2]);
  const longest = Math.max(size[0], size[1], size[2], 1);
  return {
    visible: true,
    center: [
      (min[0] + max[0]) * 0.5,
      (min[1] + max[1]) * 0.5,
      (min[2] + max[2]) * 0.5
    ],
    radius: Math.max(diagonal * 0.075, longest * 0.045, 0.05)
  };
}

function screenStableGizmoScale(options) {
  options = options || {};
  const distance = options.distance;
  const fovDeg = options.fovDeg;
  const viewportHeight = options.viewportHeight;
  const pixelRadius = options.pixelRadius === undefined ? CENTER_GIZMO_SCREEN_RADIUS_PX : options.pixelRadius;
  const fallback = options.fallback === undefined ? 1 : options.fallback;
  if (
    !Number.isFinite(distance) ||
    !Number.isFinite(fovDeg) ||
    !Number.isFinite(viewportHeight) ||
    !Number.isFinite(pixelRadius) ||
    distance <= 0 ||
    viewportHeight <= 0 ||
    pixelRadius <= 0
  ) {
    return fallback;
  }
  const visibleWorldHeight = 2 * Math.tan(THREE.MathUtils.degToRad(fovDeg) * 0.5) * distance;
  return Math.max((visibleWorldHeight * pixelRadius) / viewportHeight, 0.0001);
}

initUi();
loadScenes();
try {
  initThree();
  threeReady = true;
  setTimeout(resize, 0);
} catch (err) {
  console.error(err);
  setStatus(`3D viewport failed to start, but import/training is still available: ${err.message}`);
}

window.addEventListener("error", (event) => {
  console.error(event.error || event.message);
  setStatus(`UI error: ${event.message}`);
});

window.addEventListener("unhandledrejection", (event) => {
  console.error(event.reason);
  setStatus(`UI error: ${event.reason?.message || event.reason}`);
});

function apiPath(path) {
  if (window.location.protocol === "file:") {
    throw new Error("Do not open index.html directly. Start training_kit\\open_crop_editor.bat, then open http://127.0.0.1:7860");
  }
  return path;
}

function setStatus(text) {
  statusEl.textContent = text;
}

function updateServerPortDisplay() {
  if (!serverPortEl) return;
  serverPortEl.textContent = window.location.port || (window.location.protocol === "https:" ? "443" : "80");
}

async function updateAppVersionDisplay() {
  if (!appVersionEl) return;
  try {
    const response = await fetch(apiPath("/api/app/health"), { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const health = await response.json();
    const version = health.version || {};
    const sourceVersion = version.source_version || "";
    const packageVersion = version.package_version || "";
    appVersionEl.textContent = sourceVersion || packageVersion || "--";
    const details = [
      packageVersion ? `package ${packageVersion}` : "",
      version.updated_at ? `updated ${version.updated_at}` : "",
      version.git_commit ? `commit ${version.git_commit}` : ""
    ].filter(Boolean);
    appVersionEl.parentElement.title = details.join(" | ") || "Build version";
  } catch (err) {
    appVersionEl.textContent = "--";
    appVersionEl.parentElement.title = `Build version unavailable: ${err.message}`;
  }
}

function t(key) {
  return I18N[currentLanguage]?.[key] || I18N.en[key] || key;
}

function meshTextureDisplayKey() {
  return currentMeshTexture || meshData?.hasTextureMap ? "toggle.photoTexture" : "toggle.vertexColor";
}

function updateMeshTextureToggleLabel() {
  if (!meshTextureLabel) return;
  const key = meshTextureDisplayKey();
  meshTextureLabel.dataset.i18n = key;
  meshTextureLabel.textContent = t(key);
}

function clampNumber(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function normalizeUiScaleMode(value) {
  const normalized = String(value || "auto");
  return UI_SCALE_CHOICES.has(normalized) ? normalized : "auto";
}

function computeAutoUiScale() {
  const dpr = Number.isFinite(window.devicePixelRatio) ? Math.max(window.devicePixelRatio, 1) : 1;
  const width = Number.isFinite(window.innerWidth) ? window.innerWidth : 1600;
  const height = Number.isFinite(window.innerHeight) ? window.innerHeight : 900;
  let scale = 1;
  if (dpr > 1.05) {
    scale = Math.min(scale, 1 / Math.min(dpr, 1.38));
  }
  if (height < 900) scale = Math.min(scale, 0.9);
  if (height < 760) scale = Math.min(scale, 0.82);
  if (width < 1300) scale = Math.min(scale, 0.88);
  return clampNumber(scale, AUTO_UI_SCALE_MIN, AUTO_UI_SCALE_MAX);
}

function resolveUiScale(mode = uiScaleMode) {
  if (mode === "auto") return computeAutoUiScale();
  const parsed = Number(mode);
  return Number.isFinite(parsed) ? clampNumber(parsed, 0.72, 1.25) : 1;
}

function applyUiScale(mode = uiScaleMode, options = {}) {
  const persist = options.persist !== false;
  const scheduleResize = options.scheduleResize !== false;
  uiScaleMode = normalizeUiScaleMode(mode);
  if (uiScaleSelect) uiScaleSelect.value = uiScaleMode;
  if (persist) localStorage.setItem(UI_SCALE_STORAGE_KEY, uiScaleMode);
  const nextScale = resolveUiScale(uiScaleMode);
  if (Math.abs(nextScale - currentUiScale) > 0.005) {
    currentUiScale = nextScale;
    document.documentElement.style.setProperty("--ui-scale", currentUiScale.toFixed(3));
    lastToolbarHeight = 0;
    if (scheduleResize) requestAnimationFrame(resize);
  }
}

function applyLanguage(lang = currentLanguage) {
  currentLanguage = I18N[lang] ? lang : "en";
  document.documentElement.lang = currentLanguage === "zh" ? "zh-CN" : currentLanguage;
  if (languageSelect) languageSelect.value = currentLanguage;
  localStorage.setItem("cropEditorLanguage", currentLanguage);
  if (window.gsEditor && typeof window.gsEditor.setLanguage === "function") {
    window.gsEditor.setLanguage(currentLanguage);
  }
  for (const el of document.querySelectorAll("[data-i18n]")) {
    el.textContent = t(el.dataset.i18n);
  }
  for (const el of document.querySelectorAll("[data-i18n-placeholder]")) {
    el.placeholder = t(el.dataset.i18nPlaceholder);
  }
  if (statusEl.textContent === "Loading scenes..." || !statusEl.textContent) {
    setStatus(t("status.loadingScenes"));
  }
  updateMeshTextureToggleLabel();
  updateTrainFileStatus();
  requestAnimationFrame(resize);
}

function syncToolbarHeight() {
  if (!toolbar) return;
  const next = Math.ceil(toolbar.getBoundingClientRect().height);
  if (next > 0 && Math.abs(next - lastToolbarHeight) > 1) {
    lastToolbarHeight = next;
    document.documentElement.style.setProperty("--workspace-top", `${next}px`);
  }
}

function setRibbonPanel(panel, persist = true) {
  activeRibbonPanel = panel || "";
  for (const tab of document.querySelectorAll(".ribbon-tab")) {
    tab.classList.toggle("is-active", tab.dataset.panelTarget === activeRibbonPanel);
    tab.setAttribute("aria-expanded", tab.dataset.panelTarget === activeRibbonPanel ? "true" : "false");
  }
  for (const item of document.querySelectorAll(".ribbon-panel")) {
    item.classList.toggle("is-active", item.dataset.panel === activeRibbonPanel);
  }
  if (persist) {
    if (activeRibbonPanel) localStorage.setItem("cropEditorActivePanel", activeRibbonPanel);
    else localStorage.removeItem("cropEditorActivePanel");
  }
  requestAnimationFrame(resize);
}

function initThree() {
  renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
  gpuRenderTimer = createGpuRenderTimer(renderer.getContext());
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  scene = new THREE.Scene();
  centerGizmoScene = new THREE.Scene();
  pickingScene = new THREE.Scene();
  scene.background = new THREE.Color(0x111111);
  camera = new THREE.PerspectiveCamera(60, 1, 0.01, 10000);
  camera.position.set(0, -4, 2);
  controls = new TrackballControls(camera, renderer.domElement);
  controls.rotateSpeed = 3.0;
  controls.zoomSpeed = 1.2;
  controls.panSpeed = 0.8;
  controls.middleButtonPan = true;
  controls.noRoll = false;
  controls.staticMoving = true;
  controls.dynamicDampingFactor = 0.15;
  focusRaycaster = new THREE.Raycaster();
  const light = new THREE.AmbientLight(0xffffff, 0.28);
  scene.add(light);
  const keyLight = new THREE.DirectionalLight(0xffffff, 2.15);
  keyLight.position.set(3.5, -4.5, 6.0);
  scene.add(keyLight);
  const fillLight = new THREE.DirectionalLight(0xffffff, 0.18);
  fillLight.position.set(-3.0, 2.0, 2.5);
  scene.add(fillLight);
  const rimLight = new THREE.DirectionalLight(0xffffff, 0.75);
  rimLight.position.set(-4.0, 4.0, 3.0);
  scene.add(rimLight);
  centerGizmoScene.add(new THREE.AmbientLight(0xffffff, 0.45));
  const centerGizmoKeyLight = new THREE.DirectionalLight(0xffffff, 1.45);
  centerGizmoKeyLight.position.set(2.0, -3.0, 4.0);
  centerGizmoScene.add(centerGizmoKeyLight);
  centerGizmo = createCenterGizmo();
  centerGizmoScene.add(centerGizmo);
  window.addEventListener("resize", resize);
  resize();
  animate();
}

function initUi() {
  updateServerPortDisplay();
  updateAppVersionDisplay();
  applyUiScale(uiScaleMode, { persist: false });
  if (languageSelect) {
    languageSelect.value = currentLanguage;
    languageSelect.onchange = () => applyLanguage(languageSelect.value);
  }
  if (uiScaleSelect) {
    uiScaleSelect.value = uiScaleMode;
    uiScaleSelect.onchange = () => applyUiScale(uiScaleSelect.value);
  }
  if (openJobCenterButton) openJobCenterButton.onclick = showJobCenter;
  for (const tab of document.querySelectorAll(".ribbon-tab")) {
    tab.onclick = () => {
      const next = activeRibbonPanel === tab.dataset.panelTarget ? "" : tab.dataset.panelTarget;
      setRibbonPanel(next);
    };
  }
  document.getElementById("load").onclick = loadCurrent;
  document.getElementById("resetView").onclick = () => currentBounds && frameBounds(currentBounds);
  document.getElementById("clear").onclick = clearSelection;
  document.getElementById("invert").onclick = invertSelection;
  document.getElementById("delete").onclick = deleteSelected;
  document.getElementById("undo").onclick = undo;
  document.getElementById("save").onclick = saveModel;
  splatImportFileInput.onchange = importSplatFile;
  openAssetManagerButton.onclick = showAssetManager;
  if (openExperimentManagerButton) openExperimentManagerButton.onclick = showExperimentManager;
  downloadSpzButton.onclick = () => downloadSplatFormat("spz");
  downloadSogButton.onclick = () => downloadSplatFormat("sog");
  meshModeSelect.onchange = () => {
    setSugarControlsVisible(selectedMeshModeUsesSugar());
    setGs2MeshControlsVisible(selectedMeshModeUsesGs2Mesh());
    setMeshBusy(false);
    const status = meshModeUses3dgs(selectedMeshMode())
      ? `${selectedMeshModeUsesGs2Mesh() ? "GS2Mesh" : "SuGaR"} mesh mode targets loaded 3DGS scenes.`
      : "2DGS mesh mode targets loaded 2DGS scenes.";
    setStatus(status);
  };
  if (sugarQualitySelect) sugarQualitySelect.onchange = applySugarPreset;
  applySugarPreset();
  setSugarControlsVisible(selectedMeshModeUsesSugar());
  setGs2MeshControlsVisible(selectedMeshModeUsesGs2Mesh());
  document.getElementById("showPoints").onchange = updateLayerVisibility;
  document.getElementById("showSplats").onchange = updateLayerVisibility;
  document.getElementById("showRealSplats").onchange = onRealSplatsToggle;
  document.getElementById("previewReal").onclick = previewRealNow;
  document.getElementById("showCameras").onchange = updateLayerVisibility;
  document.getElementById("showPivot").onchange = updateLayerVisibility;
  document.getElementById("pointSize").oninput = updatePointSize;
  importOnlyButton.onclick = importOnly;
  startTrainButton.onclick = () => importAndStartTraining(true);
  trainExistingButton.onclick = () => importAndStartTraining(false);
  if (runColmapButton) runColmapButton.onclick = startColmapAlignment;
  cancelTrainButton.onclick = cancelActiveTraining;
  exportMeshButton.onclick = startMeshExport;
  loadMeshButton.onclick = loadCurrentMesh;
  bakeTextureButton.onclick = startTextureBake;
  loadTextureButton.onclick = loadCurrentTexture;
  if (runPsnrButton) runPsnrButton.onclick = startPsnrAnalysis;
  if (openPsnrOutputButton) openPsnrOutputButton.onclick = openLastPsnrOutput;
  showMeshToggle.onchange = updateLayerVisibility;
  meshTextureToggle.onchange = () => {
    rebuildMeshObject();
    const label = t(meshTextureDisplayKey());
    setStatus(meshTextureEnabled() ? `${label} enabled.` : `${label} disabled. Showing gray shaded mesh.`);
  };
  meshTrimToggle.onchange = () => {
    meshSelected.clear();
    buildMeshSelectionCloud();
    if (meshTrimToggle.checked && meshData?.isChunked) {
      meshTrimToggle.checked = false;
      setStatus("Large mesh is loaded in progressive chunks. Accurate trim needs server-side full-mesh trimming and is not enabled for chunked view yet.");
      return;
    }
    if (meshTrimToggle.checked && meshData?.isPreview) {
      setStatus("Loading full-resolution mesh for accurate trim...");
      loadCurrentMesh({ forceOriginal: true });
      return;
    }
    setStatus(meshTrimEnabled() ? "Mesh Trim mode: selection tools edit mesh triangles; Delete removes touched faces." : "Mesh Trim disabled. Selection tools edit Gaussians.");
  };
  downloadMeshButton.onclick = downloadLastMesh;
  downloadTextureButton.onclick = downloadLastTexture;
  downloadGlbButton.onclick = downloadLastGlb;
  document.getElementById("checkTrainEnv").onclick = checkTrainingEnvironment;
  document.getElementById("closeTrainLog").onclick = () => {
    trainPanel.hidden = true;
  };
  document.getElementById("closeMediaPreview").onclick = () => {
    mediaPanel.hidden = true;
  };
  document.getElementById("closeImportProgress").onclick = () => {
    importProgressPanel.hidden = true;
  };
  document.getElementById("closeJobCenter").onclick = () => {
    jobCenterPanel.hidden = true;
  };
  document.getElementById("refreshJobCenter").onclick = refreshJobCenter;
  document.getElementById("closeAssetManager").onclick = () => {
    assetManagerPanel.hidden = true;
  };
  document.getElementById("refreshAssetManager").onclick = refreshAssetManager;
  if (document.getElementById("closeExperimentManager")) {
    document.getElementById("closeExperimentManager").onclick = () => {
      experimentManagerPanel.hidden = true;
    };
  }
  if (document.getElementById("refreshExperimentManager")) {
    document.getElementById("refreshExperimentManager").onclick = refreshExperimentManager;
  }
  makePanelDraggable(mediaPanel, document.getElementById("mediaPanelHeader"), { storageKey: "cropEditor.mediaPanel.position" });
  makePanelDraggable(trainPanel, document.getElementById("trainPanelHeader"), { storageKey: "cropEditor.trainPanel.position" });
  makePanelDraggable(jobCenterPanel, document.getElementById("jobCenterHeader"), { storageKey: "cropEditor.jobCenterPanel.position" });
  makePanelDraggable(assetManagerPanel, document.getElementById("assetManagerHeader"), { storageKey: "cropEditor.assetManagerPanel.position" });
  if (experimentManagerPanel) {
    makePanelDraggable(experimentManagerPanel, document.getElementById("experimentManagerHeader"), { storageKey: "cropEditor.experimentManagerPanel.position" });
  }
  makePanelDraggable(importProgressPanel, document.getElementById("importProgressHeader"), { storageKey: "cropEditor.importProgressPanel.position" });
  trainFilesInput.onchange = () => appendTrainingInputFiles(trainFilesInput);
  trainFolderInput.onchange = () => appendTrainingInputFiles(trainFolderInput);
  trainMasksInput.onchange = () => {
    trainMaskFiles = Array.from(trainMasksInput.files || []);
    updateTrainFileStatus();
    setStatus(`${trainMaskFiles.length.toLocaleString()} mask file(s) selected.`);
  };
  clearTrainInputButton.onclick = clearTrainingInputFiles;
  if (alignedSourceSelect) alignedSourceSelect.onchange = syncAlignedSourceOutputName;
  trainBackendSelect.onchange = applyTrainingPreset;
  trainQualitySelect.onchange = applyTrainingPreset;
  colmapPresetSelect.onchange = () => applyColmapPreset(colmapPresetSelect.value);
  videoFpsInput.oninput = () => updateTrainFileStatus();
  applyTrainingPreset();
  for (const id of ["navigate", "rect", "lasso", "brush"]) {
    document.getElementById(id).onclick = () => setMode(id);
  }
  canvas.addEventListener("pointerdown", viewPanPointerDown, true);
  canvas.addEventListener("pointermove", viewPanPointerMove, true);
  canvas.addEventListener("pointerup", viewPanPointerUp, true);
  canvas.addEventListener("pointercancel", viewPanPointerCancel, true);
  canvas.addEventListener("pointerdown", pointerDown, true);
  canvas.addEventListener("pointermove", pointerMove, true);
  canvas.addEventListener("pointerup", pointerUp, true);
  canvas.addEventListener("pointercancel", pointerCancel, true);
  canvas.addEventListener("dblclick", focusOnDoubleClick, true);
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());
  window.addEventListener("keydown", handleGlobalKeyDown, true);
  window.addEventListener("keyup", handleGlobalKeyUp, true);
  document.addEventListener("keydown", handleGlobalKeyDown, true);
  document.addEventListener("keyup", handleGlobalKeyUp, true);
  window.addEventListener("blur", resetInteractionState);
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) resetInteractionState();
  });
  if (toolbar && "ResizeObserver" in window) {
    new ResizeObserver(() => requestAnimationFrame(resize)).observe(toolbar);
  }
  updateInteractionCursor();
  setMeshBusy(false);
  setPsnrBusy(false);
  applyColmapPreset(colmapPresetSelect.value || "default");
  setRibbonPanel(activeRibbonPanel, false);
  applyLanguage(currentLanguage);
  refreshJobCenter();
}

async function updateTrainFileStatus() {
  const count = selectedTrainingFiles().length;
  const maskCount = trainMaskFiles.length;
  const label = document.getElementById("trainSelection");
  if (label) {
    label.textContent = count
      ? `${count.toLocaleString()} ${t("status.inputSelected")}${maskCount ? `, ${maskCount.toLocaleString()} mask(s)` : ""}`
      : (maskCount ? `${maskCount.toLocaleString()} mask(s)` : t("status.noInput"));
  }
  if (clearTrainInputButton) clearTrainInputButton.disabled = (count === 0 && maskCount === 0) || Boolean(activeTrainJobId);
  if (count || maskCount) setStatus(`${count.toLocaleString()} input file(s), ${maskCount.toLocaleString()} mask file(s) selected for training.`);
  await renderMediaPreview();
}

async function appendTrainingInputFiles(input) {
  const added = trainingFiles.add(input?.files || []);
  if (input) input.value = "";
  await updateTrainFileStatus();
  if (added > 0) {
    setStatus(`${added.toLocaleString()} file(s) added. ${selectedTrainingFiles().length.toLocaleString()} total input file(s).`);
  }
}

async function clearTrainingInputFiles() {
  trainingFiles.clear();
  trainMaskFiles = [];
  if (trainFilesInput) trainFilesInput.value = "";
  if (trainFolderInput) trainFolderInput.value = "";
  if (trainMasksInput) trainMasksInput.value = "";
  await updateTrainFileStatus();
  setStatus(t("status.noInput"));
}

function applyColmapPreset(name) {
  const preset = COLMAP_UI_PRESETS[name] || COLMAP_UI_PRESETS.default;
  if (colmapPresetSelect) colmapPresetSelect.value = name in COLMAP_UI_PRESETS ? name : "default";
  colmapMatchingSelect.value = preset.matching;
  colmapCameraSelect.value = preset.camera;
  colmapMaxImageSizeInput.value = preset.maxImageSize;
  colmapMaxFeaturesInput.value = preset.maxFeatures;
  colmapBaIterationsInput.value = preset.baIterations;
  colmapBaFreqInput.value = preset.baFreq;
  colmapMinMatchesInput.value = preset.minMatches;
  colmapOverlapInput.value = preset.overlap;
  colmapGuidedToggle.checked = preset.guided;
  colmapGpuToggle.checked = preset.gpu;
  colmapResetToggle.checked = preset.reset;
}

function applyTrainingPreset() {
  const backend = trainBackendSelect?.value || "3dgs";
  const quality = trainQualitySelect?.value || "quick";
  const preset = (TRAINING_UI_PRESETS[backend] || TRAINING_UI_PRESETS["3dgs"])[quality] || TRAINING_UI_PRESETS[backend].quick;
  trainIterationsInput.value = preset.iterations;
  trainResolutionInput.value = preset.resolution;
  if (backend === "2dgs") {
    trainDepthRatioInput.value = preset.depthRatio;
  } else {
    trainOptimizerSelect.value = preset.optimizer;
    trainAntialiasingToggle.checked = preset.antialiasing;
    trainExposureToggle.checked = preset.exposure;
    trainDensifyGradInput.value = preset.densifyGrad;
    trainDensifyIntervalInput.value = preset.densifyInterval;
    trainDensifyUntilInput.value = preset.densifyUntil;
  }
  updateTrainingControlsForBackend();
}

function updateTrainingControlsForBackend() {
  const backend = trainBackendSelect?.value || "3dgs";
  for (const el of document.querySelectorAll(".train-3dgs")) {
    el.hidden = backend !== "3dgs";
  }
  for (const el of document.querySelectorAll(".train-2dgs")) {
    el.hidden = backend !== "2dgs";
  }
}

function trainingOptions() {
  const backend = trainBackendSelect?.value || "3dgs";
  const options = {
    iterations: Number(trainIterationsInput.value || 7000),
    resolution: Number(trainResolutionInput.value || 8),
  };
  if (backend === "2dgs") {
    options.depth_ratio = Number(trainDepthRatioInput.value || 0);
    return options;
  }
  options.optimizer_type = trainOptimizerSelect.value || "default";
  options.antialiasing = Boolean(trainAntialiasingToggle.checked);
  options.exposure_compensation = Boolean(trainExposureToggle.checked);
  options.densify_grad_threshold = Number(trainDensifyGradInput.value || 0.0002);
  options.densification_interval = Number(trainDensifyIntervalInput.value || 100);
  options.densify_until_iter = Number(trainDensifyUntilInput.value || Math.min(options.iterations, 15000));
  return options;
}

function colmapTrainingOptions() {
  return {
    preset: colmapPresetSelect.value || "default",
    matching: colmapMatchingSelect.value || "exhaustive",
    camera_model: colmapCameraSelect.value || "OPENCV",
    feature_max_image_size: Number(colmapMaxImageSizeInput.value || -1),
    feature_max_num_features: Number(colmapMaxFeaturesInput.value || 8192),
    mapper_ba_global_max_num_iterations: Number(colmapBaIterationsInput.value || 50),
    mapper_ba_global_frames_freq: Number(colmapBaFreqInput.value || 500),
    mapper_min_num_matches: Number(colmapMinMatchesInput.value || 15),
    sequential_overlap: Number(colmapOverlapInput.value || 10),
    matcher_guided: Boolean(colmapGuidedToggle.checked),
    use_gpu: Boolean(colmapGpuToggle.checked),
    reset: Boolean(colmapResetToggle.checked),
  };
}

function handleGlobalKeyDown(e) {
  if (isEscapeKey(e)) {
    if (clearMeshTrimSelectionOnEscape()) {
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (pivotPickMode) {
      setPivotPickMode(false);
      setStatus("Pick rotation center cancelled.");
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (dragStart) {
      cancelSelectionDrag();
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (meshTrimEnabled()) {
      if (meshUndoStack.length) {
        undo();
      } else {
        meshTrimToggle.checked = false;
        buildMeshSelectionCloud();
        setStatus("Mesh Trim disabled.");
      }
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (selected.size) {
      clearSelection();
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (mode !== "navigate") {
      setMode("navigate");
      setStatus("Navigate mode.");
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (document.fullscreenElement) {
      document.exitFullscreen().catch(() => {});
      e.preventDefault();
      e.stopPropagation();
    }
    return;
  }
  if (e.code === "Space" && !isEditableShortcutTarget(e.target)) {
    const wasSpaceDown = isSpaceDown;
    isSpaceDown = true;
    updateInteractionCursor();
    if (!wasSpaceDown && mode !== "navigate") setStatus(temporaryNavigationStatus(mode));
    e.preventDefault();
    e.stopPropagation();
  }
  if (e.key?.toLowerCase() === "i" && !e.ctrlKey && !e.metaKey && !e.altKey && !isEditableShortcutTarget(e.target)) {
    invertSelection();
    e.preventDefault();
  }
  if (e.key?.toLowerCase() === "p" && !e.ctrlKey && !e.metaKey && !e.altKey && !isEditableShortcutTarget(e.target)) {
    if (e.shiftKey) {
      autoPickPivotAtCenter = !autoPickPivotAtCenter;
      setStatus(`Auto-pick rotation center ${autoPickPivotAtCenter ? "enabled" : "disabled"}.`);
    } else {
      setPivotPickMode(!pivotPickMode);
    }
    e.preventDefault();
    e.stopPropagation();
  }
}

function handleGlobalKeyUp(e) {
  if (e.code === "Space" && !isEditableShortcutTarget(e.target)) {
    isSpaceDown = false;
    updateInteractionCursor();
    if (mode !== "navigate") setStatus(selectionModeStatus(mode));
    e.preventDefault();
    e.stopPropagation();
  }
}

function isEscapeKey(e) {
  return e.key === "Escape" || e.key === "Esc" || e.code === "Escape";
}

function clearMeshTrimSelectionOnEscape() {
  if (!meshTrimEnabled() || !meshSelected.size) return false;
  meshSelected.clear();
  buildMeshSelectionCloud();
  setStatus("Mesh trim selection cleared.");
  return true;
}

function setMode(next) {
  mode = next;
  if (pivotPickMode) setPivotPickMode(false, false);
  if (controls) controls.enabled = true;
  for (const id of ["navigate", "rect", "lasso", "brush"]) {
    document.getElementById(id).classList.toggle("active", id === mode);
  }
  clearOverlay();
  updateInteractionCursor();
  if (mode === "rect" || mode === "lasso" || mode === "brush") {
    setStatus(selectionModeStatus(mode));
  }
}

function updateInteractionCursor() {
  document.body.classList.toggle("select-tool", mode === "rect" || mode === "lasso" || mode === "brush");
  document.body.classList.toggle("pivot-pick", pivotPickMode);
  document.body.classList.toggle("nav-override", isSpaceDown);
  document.body.classList.toggle("selecting", Boolean(dragStart));
}

function setPivotPickMode(enabled, announce = true) {
  pivotPickMode = Boolean(enabled);
  if (pivotPickMode) {
    if (mode !== "navigate") mode = "navigate";
    for (const id of ["navigate", "rect", "lasso", "brush"]) {
      document.getElementById(id).classList.toggle("active", id === "navigate");
    }
    if (controls) controls.enabled = true;
    if (announce) setStatus("Pick rotation center: click a mesh or point-cloud point. Esc cancels.");
  } else if (announce) {
    setStatus("Pick rotation center disabled.");
  }
  updateInteractionCursor();
}

function resetInteractionState() {
  isSpaceDown = false;
  if (viewPanDrag) finishViewPan(viewPanDrag.pointerId);
  if (dragStart) cancelSelectionDrag();
  if (controls) controls.enabled = true;
  updateInteractionCursor();
}

async function loadScenes() {
  let data;
  try {
    const res = await fetch(apiPath("/api/scenes"));
    data = await res.json();
  } catch (err) {
    setStatus(err.message);
    return;
  }
  sceneSelect.innerHTML = "";
  outputBackendByName.clear();
  for (const item of data.scenes) {
    outputBackendByName.set(item.name, item.backend || "3dgs");
    const opt = document.createElement("option");
    opt.value = item.name;
    opt.textContent = item.backend ? `${item.name} [${item.backend.toUpperCase()}]` : item.name;
    opt.title = item.path || "";
    opt.dataset.path = item.path || "";
    opt.dataset.iteration = item.latest_iteration;
    opt.dataset.backend = item.backend || "3dgs";
    sceneSelect.appendChild(opt);
  }
  if (data.scenes.length) {
    iterationInput.value = data.scenes[0].latest_iteration;
    outputInput.value = `${data.scenes[0].name}_crop`;
    if (trainSceneInput && !trainSceneInput.value.trim()) trainSceneInput.value = `scene_${new Date().toISOString().slice(0, 10).replaceAll("-", "")}`;
    sceneSelect.onchange = () => {
      const opt = sceneSelect.selectedOptions[0];
      iterationInput.value = opt.dataset.iteration;
      outputInput.value = `${opt.value}_crop`;
    };
    setStatus(t("status.selectScene"));
  } else {
    if (trainSceneInput && !trainSceneInput.value.trim()) trainSceneInput.value = `scene_${new Date().toISOString().slice(0, 10).replaceAll("-", "")}`;
    setStatus(t("status.noModels"));
  }
  await loadAlignedSources();
}

async function loadAlignedSources() {
  if (!alignedSourceSelect) return;
  let datasets = [];
  try {
    const res = await fetch(apiPath("/api/datasets"));
    const data = await res.json();
    datasets = Array.isArray(data.datasets) ? data.datasets : [];
  } catch (_) {
    datasets = [];
  }
  alignedDatasets = datasets;
  const previous = alignedSourceSelect.value;
  alignedSourceSelect.innerHTML = "";
  const empty = document.createElement("option");
  empty.value = "";
  empty.textContent = t("option.noAlignedSource");
  empty.dataset.i18n = "option.noAlignedSource";
  alignedSourceSelect.appendChild(empty);
  for (const item of datasets) {
    const opt = document.createElement("option");
    opt.value = item.name;
    opt.dataset.hasAlignment = item.has_alignment ? "true" : "false";
    const count = Number(item.image_count || 0).toLocaleString();
    const suffix = item.has_alignment ? "aligned" : "images only";
    opt.textContent = `${item.name} (${count}, ${suffix})`;
    alignedSourceSelect.appendChild(opt);
  }
  if (previous && Array.from(alignedSourceSelect.options).some((opt) => opt.value === previous)) {
    alignedSourceSelect.value = previous;
  }
}

function selectedSceneBackend() {
  return sceneSelect.selectedOptions[0]?.dataset.backend || "3dgs";
}

function activeRendererLabel() {
  return currentBackend === "2dgs" ? "2DGS" : "3DGS";
}

function meshTrimEnabled() {
  return Boolean(meshTrimToggle?.checked && meshData && !meshData.isChunked);
}

function meshTextureEnabled() {
  return Boolean(meshTextureToggle?.checked && (meshData?.hasVertexColors || currentMeshTexture));
}

function selectedMeshMode() {
  return meshModeSelect?.value || "bounded";
}

function selectedMeshModeUsesSugar() {
  return meshModeUsesSugar(selectedMeshMode());
}

function selectedMeshModeUsesGs2Mesh() {
  return meshModeUsesGs2Mesh(selectedMeshMode());
}

function setSugarControlsVisible(visible) {
  for (const control of document.querySelectorAll(".sugar-control")) {
    control.hidden = !visible;
  }
}

function setGs2MeshControlsVisible(visible) {
  for (const control of document.querySelectorAll(".gs2mesh-control")) {
    control.hidden = !visible;
  }
}

function applySugarPreset() {
  const options = sugarOptionsFromPreset(sugarQualitySelect?.value || "preview");
  if (sugarPolySelect) sugarPolySelect.value = options.sugar_quality;
  if (sugarRefinementSelect) sugarRefinementSelect.value = options.sugar_refinement_time;
  if (sugarRegularizationSelect) sugarRegularizationSelect.value = options.sugar_regularization;
  if (sugarSurfaceLevelInput) sugarSurfaceLevelInput.value = String(options.sugar_surface_level);
  if (sugarSquareSizeInput) sugarSquareSizeInput.value = String(options.sugar_square_size);
  if (sugarMaxImagesInput) sugarMaxImagesInput.value = String(options.sugar_max_images);
  if (sugarMaxImageSizeInput) sugarMaxImageSizeInput.value = String(options.sugar_max_image_size);
  if (sugarPostprocessToggle) sugarPostprocessToggle.checked = Boolean(options.sugar_postprocess);
}

function numberOrFallback(value, fallback) {
  const number = Number(value);
  return Number.isFinite(number) ? number : fallback;
}

function sugarMeshOptions() {
  const preset = sugarOptionsFromPreset(sugarQualitySelect?.value || "preview");
  return {
    sugar_quality: sugarPolySelect?.value || preset.sugar_quality,
    sugar_refinement_time: sugarRefinementSelect?.value || preset.sugar_refinement_time,
    sugar_regularization: sugarRegularizationSelect?.value || preset.sugar_regularization,
    sugar_surface_level: numberOrFallback(sugarSurfaceLevelInput?.value, preset.sugar_surface_level),
    sugar_square_size: Math.round(numberOrFallback(sugarSquareSizeInput?.value, preset.sugar_square_size)),
    sugar_max_images: Math.round(numberOrFallback(sugarMaxImagesInput?.value, preset.sugar_max_images)),
    sugar_max_image_size: Math.round(numberOrFallback(sugarMaxImageSizeInput?.value, preset.sugar_max_image_size)),
    sugar_postprocess: Boolean(sugarPostprocessToggle?.checked),
  };
}

function gs2meshMeshOptions() {
  return {
    gs2mesh_downsample: Math.round(numberOrFallback(gs2meshDownsampleInput?.value, 2)),
    gs2mesh_baseline_percentage: numberOrFallback(gs2meshBaselineInput?.value, 7),
    gs2mesh_tsdf_voxel: Math.round(numberOrFallback(gs2meshTsdfVoxelInput?.value, 2)),
    gs2mesh_tsdf_min_depth_baselines: Math.round(numberOrFallback(gs2meshTsdfMinInput?.value, 4)),
    gs2mesh_tsdf_max_depth_baselines: Math.round(numberOrFallback(gs2meshTsdfMaxInput?.value, 20)),
    gs2mesh_scene_360: Boolean(gs2meshScene360Toggle?.checked),
  };
}

function selectedMeshModeMatchesBackend() {
  return meshModeMatchesBackend(selectedMeshMode(), currentBackend);
}

async function refreshSceneList(selectedName = null) {
  const previous = selectedName || sceneSelect.value;
  await loadScenes();
  if (previous) {
    const found = Array.from(sceneSelect.options).find((opt) => opt.value === previous);
    if (found) {
      sceneSelect.value = previous;
      iterationInput.value = found.dataset.iteration;
    }
  }
}

function selectedTrainingFiles() {
  return trainingFiles.list();
}

function alignedDatasetInfo(sceneName) {
  return alignedDatasets.find((item) => item.name === sceneName) || null;
}

function selectedAlignedSourceName(fallbackName = "") {
  const selected = (alignedSourceSelect?.value || "").trim();
  return selected || fallbackName;
}

function looksLikeAutoTrainName(value) {
  return /^scene_\d{8}$/i.test(String(value || "").trim());
}

function syncAlignedSourceOutputName() {
  if (!alignedSourceSelect || !trainSceneInput) return;
  const source = alignedSourceSelect.value.trim();
  if (!source) return;
  const current = trainSceneInput.value.trim();
  if (current && !looksLikeAutoTrainName(current)) return;
  const backend = trainBackendSelect?.value || "3dgs";
  trainSceneInput.value = existingTrainingOutputSceneName(source, source, backend, outputBackendByName);
}

function fileExtension(file) {
  return (file.name.split(".").pop() || "").toLowerCase();
}

function isImageFile(file) {
  return file.type.startsWith("image/") || IMAGE_EXTS.has(fileExtension(file));
}

function isVideoFile(file) {
  return file.type.startsWith("video/") || VIDEO_EXTS.has(fileExtension(file));
}

function clearMediaPreviewUrls() {
  for (const url of mediaObjectUrls) URL.revokeObjectURL(url);
  mediaObjectUrls = [];
}

function videoMetadata(file) {
  return new Promise((resolve) => {
    const url = URL.createObjectURL(file);
    mediaObjectUrls.push(url);
    const video = document.createElement("video");
    video.preload = "metadata";
    video.onloadedmetadata = () => resolve({ url, duration: Number.isFinite(video.duration) ? video.duration : 0 });
    video.onerror = () => resolve({ url, duration: 0 });
    video.src = url;
  });
}

function makeMediaItem(file, url, metaText, isVideo) {
  const item = document.createElement("div");
  item.className = "media-item";
  const media = document.createElement(isVideo ? "video" : "img");
  media.className = "media-thumb";
  media.src = url;
  if (isVideo) {
    media.muted = true;
    media.controls = false;
  }
  const meta = document.createElement("div");
  meta.className = "media-meta";
  meta.title = file.webkitRelativePath || file.name;
  meta.textContent = metaText;
  item.append(media, meta);
  return item;
}

async function renderMediaPreview() {
  const files = selectedTrainingFiles();
  clearMediaPreviewUrls();
  mediaPreview.textContent = "";
  if (!files.length) {
    mediaPanel.hidden = true;
    mediaSummary.textContent = "";
    return;
  }
  mediaPanel.hidden = false;
  const fps = Math.max(Number(videoFpsInput.value || 2), 0.1);
  let imageCount = 0;
  let videoCount = 0;
  let estimatedFrames = 0;
  const previewFiles = files.slice(0, 16);
  for (const file of files) {
    if (isImageFile(file)) imageCount++;
    else if (isVideoFile(file)) videoCount++;
  }
  for (const file of previewFiles) {
    if (isImageFile(file)) {
      const url = URL.createObjectURL(file);
      mediaObjectUrls.push(url);
      mediaPreview.appendChild(makeMediaItem(file, url, file.name, false));
    } else if (isVideoFile(file)) {
      const info = await videoMetadata(file);
      const frames = Math.max(1, Math.ceil(info.duration * fps));
      estimatedFrames += frames;
      mediaPreview.appendChild(makeMediaItem(file, info.url, `${file.name} ~${frames} frames`, true));
    }
  }
  if (files.length > previewFiles.length) {
    const more = document.createElement("div");
    more.className = "media-item";
    more.innerHTML = `<div class="media-thumb"></div><div class="media-meta">+${files.length - previewFiles.length} more</div>`;
    mediaPreview.appendChild(more);
  }
  const frameText = videoCount
    ? `estimated ${estimatedFrames.toLocaleString()} extracted frames from previewed videos at ${fps} FPS`
    : "no video frames to extract";
  mediaSummary.textContent = `${files.length.toLocaleString()} files selected: ${imageCount.toLocaleString()} photos, ${videoCount.toLocaleString()} videos, ${frameText}.`;
}

function validateSceneName(name) {
  if (!/^[A-Za-z0-9_.-]+$/.test(name)) {
    throw new Error("Scene name can only use letters, numbers, underscore, dot, and dash.");
  }
}

function showTrainingLog() {
  trainPanel.hidden = false;
}

function writeTrainingLog(job) {
  showTrainingLog();
  const lines = [
    `Job: ${job.id}`,
    `Backend: ${(job.backend || "3dgs").toUpperCase()}`,
    `Scene: ${job.scene} -> output/${job.output_scene}`,
    `Status: ${job.status} / ${job.stage}`,
    job.error ? `Error: ${job.error}` : "",
    "",
    ...(job.log || []),
  ].filter((line) => line !== "");
  trainLog.textContent = lines.join("\n");
  trainLog.scrollTop = trainLog.scrollHeight;
}

function writeMeshLog(job) {
  showTrainingLog();
  const isTexture = job.kind === "texture";
  const isGlb = job.kind === "glb";
  const isColmap = job.kind === "colmap";
  const lines = [
    `${isColmap ? "COLMAP Job" : (isGlb ? "GLB Job" : (isTexture ? "Texture Job" : "Mesh Job"))}: ${job.id}`,
    `Scene: output/${job.scene}`,
    `Iteration: ${job.iteration}`,
    `Mode: ${job.mode}`,
    `Status: ${job.status} / ${job.stage}`,
    job.output_mesh ? `Mesh: ${job.output_mesh}` : "",
    job.texture?.zip ? `Texture ZIP: ${job.texture.zip}` : "",
    job.texture?.glb ? `Texture GLB: ${job.texture.glb}` : "",
    job.error ? `Error: ${job.error}` : "",
    "",
    ...(job.log || []),
  ].filter((line) => line !== "");
  trainLog.textContent = lines.join("\n");
  trainLog.scrollTop = trainLog.scrollHeight;
}

function appendTrainingLog(text) {
  showTrainingLog();
  trainLog.textContent = `${trainLog.textContent ? `${trainLog.textContent}\n` : ""}${text}`;
  trainLog.scrollTop = trainLog.scrollHeight;
}

function showJobCenter() {
  if (!jobCenterPanel) return;
  jobCenterPanel.hidden = false;
  refreshJobCenter();
}

function jobIsActive(job) {
  return ["queued", "running", "cancelling"].includes(job?.status);
}

function jobKindLabel(job) {
  if (job.kind === "training") return `${(job.backend || "3dgs").toUpperCase()} training`;
  if (job.kind === "texture") return "Texture bake";
  if (job.kind === "glb") return "GLB export";
  if (job.kind === "colmap") return "COLMAP alignment";
  if (job.kind === "experiment_clone") return "Experiment clone";
  if (job.kind === "mesh") return `${job.mode || "mesh"} mesh export`;
  if (job.kind === "splat_export") return `${(job.format || "splat").toUpperCase()} export`;
  return job.title || job.kind || "Job";
}

function jobSceneLabel(job) {
  if (job.kind === "training") return `${job.scene} -> ${job.output_scene}`;
  if (job.kind === "experiment_clone") return `${job.scene} -> ${job.output_scene}`;
  const iteration = Number.isFinite(Number(job.iteration)) ? ` @ ${job.iteration}` : "";
  return `${job.scene || "scene"}${iteration}`;
}

function formatJobTime(value) {
  const timestamp = Number(value || 0);
  if (!Number.isFinite(timestamp) || timestamp <= 0) return "";
  return new Date(timestamp * 1000).toLocaleString();
}

function jobProgressText(job) {
  if (job.kind !== "experiment_clone" || !job.total_files) return "";
  const files = `${Number(job.copied_files || 0).toLocaleString()}/${Number(job.total_files || 0).toLocaleString()} files`;
  if (job.total_bytes) {
    return `${files}, ${formatByteSize(job.copied_bytes || 0)}/${formatByteSize(job.total_bytes)}`;
  }
  return files;
}

function writeUnifiedJobLog(job) {
  showTrainingLog();
  const lines = [
    `${jobKindLabel(job)}: ${job.id}`,
    `Scene: ${jobSceneLabel(job)}`,
    `Status: ${job.status} / ${job.stage}`,
    jobProgressText(job) ? `Progress: ${jobProgressText(job)}` : "",
    job.output_dir ? `Output: ${job.output_dir}` : "",
    job.output_path ? `File: ${job.output_path}` : "",
    job.output_mesh ? `Mesh: ${job.output_mesh}` : "",
    job.texture?.zip ? `Texture ZIP: ${job.texture.zip}` : "",
    job.texture?.glb ? `Texture GLB: ${job.texture.glb}` : "",
    job.error ? `Error: ${job.error}` : "",
    "",
    ...(job.log || []),
  ].filter((line) => line !== "");
  trainLog.textContent = lines.join("\n");
  trainLog.scrollTop = trainLog.scrollHeight;
}

function makeJobButton(labelKey, handler, disabled = false) {
  const button = document.createElement("button");
  button.type = "button";
  button.textContent = t(labelKey);
  button.disabled = disabled;
  button.onclick = handler;
  return button;
}

function renderJobCenterJobs(jobs) {
  if (!jobCenterList) return;
  jobCenterList.innerHTML = "";
  if (!jobs.length) {
    const empty = document.createElement("div");
    empty.className = "job-empty";
    empty.textContent = t("status.noJobs");
    jobCenterList.appendChild(empty);
    return;
  }
  for (const job of jobs) {
    const card = document.createElement("div");
    card.className = `job-card ${job.status || ""}`;

    const main = document.createElement("div");
    main.className = "job-main";
    const titleWrap = document.createElement("div");
    const title = document.createElement("div");
    title.className = "job-title";
    title.textContent = jobKindLabel(job);
    const meta = document.createElement("div");
    meta.className = "job-meta";
    meta.textContent = [jobSceneLabel(job), jobProgressText(job), formatJobTime(job.updated_at || job.created_at)].filter(Boolean).join(" | ");
    titleWrap.append(title, meta);
    const status = document.createElement("div");
    status.className = "job-status";
    status.textContent = `${job.status || "unknown"} / ${job.stage || "-"}`;
    main.append(titleWrap, status);
    card.appendChild(main);

    if (job.output_dir || job.output_path || job.output_mesh) {
      const path = document.createElement("div");
      path.className = "job-path";
      path.textContent = job.output_dir || job.output_path || job.output_mesh;
      card.appendChild(path);
    }
    if (job.error) {
      const error = document.createElement("div");
      error.className = "job-error";
      error.textContent = job.error;
      card.appendChild(error);
    }

    const actions = document.createElement("div");
    actions.className = "job-actions";
    actions.appendChild(makeJobButton("button.openLog", () => writeUnifiedJobLog(job)));
    actions.appendChild(makeJobButton("button.openOutput", () => openJobOutput(job.id)));
    if (job.download_url) {
      actions.appendChild(makeJobButton("button.downloadJob", () => {
        window.location.href = apiPath(job.download_url);
      }));
    }
    if (jobIsActive(job)) {
      actions.appendChild(makeJobButton("button.cancelJob", () => cancelJobCenterJob(job.id)));
    }
    if (job.can_retry) {
      actions.appendChild(makeJobButton("button.retryJob", () => retryJobCenterJob(job.id)));
    }
    card.appendChild(actions);
    jobCenterList.appendChild(card);
  }
}

async function refreshJobCenter() {
  if (!jobCenterList) return [];
  try {
    const res = await fetch(apiPath("/api/jobs?limit=50"));
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Could not list jobs");
    const jobs = Array.isArray(data.jobs) ? data.jobs : [];
    renderJobCenterJobs(jobs);
    updateJobCenterPolling(jobs.some(jobIsActive));
    return jobs;
  } catch (err) {
    jobCenterList.innerHTML = "";
    const error = document.createElement("div");
    error.className = "job-empty";
    error.textContent = err.message;
    jobCenterList.appendChild(error);
    updateJobCenterPolling(false);
    return [];
  }
}

function updateJobCenterPolling(shouldPoll) {
  if (shouldPoll && !jobCenterPollTimer) {
    jobCenterPollTimer = setInterval(refreshJobCenter, 1500);
  } else if (!shouldPoll && jobCenterPollTimer) {
    clearInterval(jobCenterPollTimer);
    jobCenterPollTimer = null;
  }
}

async function cancelJobCenterJob(jobId) {
  try {
    const res = await fetch(apiPath("/api/jobs/cancel"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id: jobId }),
    });
    const job = await res.json();
    if (!res.ok) throw new Error(job.error || "Cancel failed");
    writeUnifiedJobLog(job);
    await refreshJobCenter();
  } catch (err) {
    setStatus(`Cancel failed: ${err.message}`);
  }
}

async function retryJobCenterJob(jobId) {
  try {
    const res = await fetch(apiPath("/api/jobs/retry"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id: jobId }),
    });
    const job = await res.json();
    if (!res.ok) throw new Error(job.error || "Retry failed");
    writeUnifiedJobLog(job);
    await refreshJobCenter();
  } catch (err) {
    setStatus(`Retry failed: ${err.message}`);
  }
}

async function openJobOutput(jobId) {
  try {
    const res = await fetch(apiPath("/api/jobs/open-output"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id: jobId }),
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Open output failed");
    setStatus(`Opened output folder: ${data.path}`);
  } catch (err) {
    setStatus(`Open output failed: ${err.message}`);
  }
}

function showAssetManager() {
  if (!assetManagerPanel) return;
  assetManagerPanel.hidden = false;
  refreshAssetManager();
}

function assetKindLabel(asset) {
  return asset.label || asset.kind || "Asset";
}

function assetMetaText(asset) {
  const size = asset.exists ? formatByteSize(asset.size) : "missing";
  if (asset.kind === "psnr_report") {
    const parts = ["psnr"];
    if (asset.average_psnr !== undefined && asset.average_psnr !== null && Number.isFinite(Number(asset.average_psnr))) {
      parts.push(`avg ${Number(asset.average_psnr).toFixed(2)} dB`);
    }
    if (asset.rendered_count || asset.requested_count) {
      parts.push(`${Number(asset.rendered_count || 0)}/${Number(asset.requested_count || 0)} views`);
    }
    if (asset.eval_width) parts.push(`width ${asset.eval_width}`);
    parts.push(size);
    return parts.join(" | ");
  }
  return `${asset.kind || "asset"} | ${size}${asset.mode ? ` | ${asset.mode}` : ""}`;
}

function renderAssetItem(asset, groupId) {
  const row = document.createElement("div");
  row.className = `asset-row ${asset.exists ? "exists" : "missing"}`;

  const info = document.createElement("div");
  info.className = "asset-info";
  const name = document.createElement("div");
  name.className = "asset-name";
  name.textContent = assetKindLabel(asset);
  const meta = document.createElement("div");
  meta.className = "asset-meta";
  meta.textContent = assetMetaText(asset);
  info.append(name, meta);
  if (asset.fallback_reason) {
    const note = document.createElement("div");
    note.className = "asset-warning";
    note.textContent = asset.fallback_reason;
    info.appendChild(note);
  }

  const path = document.createElement("div");
  path.className = "asset-path";
  path.textContent = asset.path || "";
  info.appendChild(path);

  const actions = document.createElement("div");
  actions.className = "asset-actions";
  if (asset.exists && asset.open_psnr_run) {
    actions.appendChild(makeJobButton("button.openAsset", () => openPsnrReport(asset.open_psnr_run)));
  }
  if (asset.exists && asset.url) {
    actions.appendChild(makeJobButton(asset.kind === "psnr_report" ? "button.downloadJson" : "button.downloadJob", () => {
      window.location.href = apiPath(asset.url);
    }));
  }
  if (asset.exists && asset.csv_url) {
    actions.appendChild(makeJobButton("button.downloadCsv", () => {
      window.location.href = apiPath(asset.csv_url);
    }));
  }
  if (!asset.exists && groupId === "splats" && asset.exportable && asset.format) {
    actions.appendChild(makeJobButton("button.exportAsset", () => downloadSplatFormat(asset.format)));
  }
  row.append(info, actions);
  return row;
}

function renderAssetGroup(group) {
  const section = document.createElement("section");
  section.className = "asset-group";
  const title = document.createElement("div");
  title.className = "asset-group-title";
  const items = Array.isArray(group.items) ? group.items : [];
  const existing = items.filter((item) => item.exists).length;
  title.textContent = `${group.title || group.id} (${existing}/${items.length})`;
  section.appendChild(title);
  if (!items.length) {
    const empty = document.createElement("div");
    empty.className = "asset-empty";
    empty.textContent = t("status.noAssets");
    section.appendChild(empty);
    return section;
  }
  for (const asset of items) {
    section.appendChild(renderAssetItem(asset, group.id));
  }
  return section;
}

function renderAssetJobs(jobs) {
  const section = document.createElement("section");
  section.className = "asset-group";
  const title = document.createElement("div");
  title.className = "asset-group-title";
  title.textContent = `Jobs (${jobs.length})`;
  section.appendChild(title);
  if (!jobs.length) {
    const empty = document.createElement("div");
    empty.className = "asset-empty";
    empty.textContent = t("status.noJobs");
    section.appendChild(empty);
    return section;
  }
  for (const job of jobs) {
    const row = document.createElement("div");
    row.className = `asset-row ${job.status || ""}`;
    const info = document.createElement("div");
    info.className = "asset-info";
    const name = document.createElement("div");
    name.className = "asset-name";
    name.textContent = jobKindLabel(job);
    const meta = document.createElement("div");
    meta.className = "asset-meta";
    meta.textContent = `${job.status || "unknown"} / ${job.stage || "-"} | ${formatJobTime(job.updated_at || job.created_at)}`;
    info.append(name, meta);
    if (job.error) {
      const error = document.createElement("div");
      error.className = "asset-error";
      error.textContent = job.error;
      info.appendChild(error);
    }
    const actions = document.createElement("div");
    actions.className = "asset-actions";
    actions.appendChild(makeJobButton("button.openLog", () => writeUnifiedJobLog(job)));
    row.append(info, actions);
    section.appendChild(row);
  }
  return section;
}

function renderAssetManager(data) {
  if (!assetManagerSummary || !assetManagerList) return;
  const source = data.source || {};
  const summaryParts = [
    `output/${data.scene}`,
    `${(data.backend || "3dgs").toUpperCase()} iteration ${data.iteration}`,
    `${Number(source.image_count || 0).toLocaleString()} images`,
    source.has_alignment ? "aligned" : "no alignment",
  ];
  if (data.psnr_latest?.average_psnr !== undefined && data.psnr_latest?.average_psnr !== null) {
    summaryParts.push(`PSNR ${Number(data.psnr_latest.average_psnr).toFixed(2)} dB`);
  }
  assetManagerSummary.textContent = summaryParts.join(" | ");
  assetManagerList.innerHTML = "";
  const actions = document.createElement("div");
  actions.className = "asset-manager-actions";
  actions.appendChild(makeJobButton("button.openOutput", () => openAssetDir("output")));
  actions.appendChild(makeJobButton("button.openOutput", () => openAssetDir("source")));
  actions.lastChild.textContent = currentLanguage === "zh" ? "打开源数据" : currentLanguage === "ja" ? "ソースを開く" : "Open Source";
  assetManagerList.appendChild(actions);
  for (const group of data.groups || []) {
    assetManagerList.appendChild(renderAssetGroup(group));
  }
  assetManagerList.appendChild(renderAssetJobs(data.jobs || []));
}

async function refreshAssetManager() {
  if (!currentScene || !assetManagerList) {
    setStatus("Load a scene first.");
    return;
  }
  try {
    const iteration = currentIteration || Number(iterationInput.value || 0);
    const res = await fetch(apiPath(`/api/assets?scene=${encodeURIComponent(currentScene)}&iteration=${encodeURIComponent(iteration)}`));
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Could not list assets");
    renderAssetManager(data);
    setStatus(`Assets refreshed for output/${data.scene}.`);
  } catch (err) {
    assetManagerList.innerHTML = "";
    const empty = document.createElement("div");
    empty.className = "asset-empty";
    empty.textContent = err.message;
    assetManagerList.appendChild(empty);
    setStatus(`Asset Manager failed: ${err.message}`);
  }
}

async function openAssetDir(target, scene = currentScene) {
  if (!scene) return;
  try {
    const res = await fetch(apiPath("/api/assets/open-dir"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ scene, target }),
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Open directory failed");
    setStatus(`Opened ${target} directory: ${data.path}`);
  } catch (err) {
    setStatus(`Open directory failed: ${err.message}`);
  }
}

async function openPsnrReport(run) {
  if (!currentScene || !run) return;
  try {
    const res = await fetch(apiPath("/api/assets/open-psnr"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ scene: currentScene, run }),
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Open PSNR report failed");
    setStatus(`Opened PSNR report: ${data.path}`);
  } catch (err) {
    setStatus(`Open PSNR report failed: ${err.message}`);
  }
}

function experimentSceneName() {
  return currentScene || sceneSelect.value;
}

function showExperimentManager() {
  if (!experimentManagerPanel) return;
  experimentManagerPanel.hidden = false;
  refreshExperimentManager();
}

function experimentValueText(value) {
  if (value === undefined || value === null || value === "") return "-";
  if (typeof value === "object") return JSON.stringify(value);
  return String(value);
}

function renderExperimentParamGroup(titleText, params, limit = 18) {
  const section = document.createElement("section");
  section.className = "asset-group";
  const title = document.createElement("div");
  title.className = "asset-group-title";
  title.textContent = titleText;
  section.appendChild(title);
  const entries = Object.entries(params || {}).slice(0, limit);
  if (!entries.length) {
    const empty = document.createElement("div");
    empty.className = "asset-empty";
    empty.textContent = "-";
    section.appendChild(empty);
    return section;
  }
  for (const [key, value] of entries) {
    const row = document.createElement("div");
    row.className = "asset-row exists";
    const info = document.createElement("div");
    info.className = "asset-info";
    const name = document.createElement("div");
    name.className = "asset-name";
    name.textContent = key;
    const meta = document.createElement("div");
    meta.className = "asset-meta";
    meta.textContent = experimentValueText(value);
    info.append(name, meta);
    row.appendChild(info);
    section.appendChild(row);
  }
  return section;
}

function renderCheckpointRow(checkpoint, data) {
  const row = document.createElement("div");
  row.className = `asset-row ${checkpoint.loadable ? "exists" : "missing"}`;
  const info = document.createElement("div");
  info.className = "asset-info";
  const name = document.createElement("div");
  name.className = "asset-name";
  const badges = [checkpoint.best ? "BEST" : "", checkpoint.pinned ? "PINNED" : ""].filter(Boolean);
  name.textContent = `${checkpoint.label || `Iteration ${checkpoint.iteration}`}${badges.length ? ` [${badges.join(", ")}]` : ""}`;
  const meta = document.createElement("div");
  meta.className = "asset-meta";
  const parts = [];
  if (checkpoint.checkpoint_size) parts.push(`checkpoint ${formatByteSize(checkpoint.checkpoint_size)}`);
  if (checkpoint.point_cloud_size) parts.push(`ply ${formatByteSize(checkpoint.point_cloud_size)}`);
  if (checkpoint.mtime) parts.push(formatJobTime(checkpoint.mtime));
  meta.textContent = parts.join(" | ") || (checkpoint.loadable ? "checkpoint" : "point cloud only");
  const path = document.createElement("div");
  path.className = "asset-path";
  path.textContent = checkpoint.checkpoint_path || checkpoint.point_cloud_path || "";
  info.append(name, meta, path);
  const actions = document.createElement("div");
  actions.className = "asset-actions";
  const options = data.training?.options || {};
  const defaultTarget = Math.max(Number(options.iterations || 0), Number(checkpoint.iteration || 0) + 1000, 30000);
  const resumeTarget = document.createElement("input");
  resumeTarget.className = "experiment-small-input";
  resumeTarget.type = "number";
  resumeTarget.min = String(Number(checkpoint.iteration || 0) + 1);
  resumeTarget.step = "1000";
  resumeTarget.value = String(defaultTarget);
  resumeTarget.title = currentLanguage === "zh" ? "续训目标迭代数" : currentLanguage === "ja" ? "再開後の目標反復数" : "Target iterations";
  const resumeOutput = document.createElement("input");
  resumeOutput.className = "experiment-name-input";
  resumeOutput.value = `${data.scene}_resume_${checkpoint.iteration}`;
  resumeOutput.title = currentLanguage === "zh" ? "续训输出实验名" : currentLanguage === "ja" ? "再開出力の実験名" : "Resume output experiment";
  actions.append(resumeTarget, resumeOutput);
  actions.appendChild(makeJobButton("button.resumeCheckpoint", () => {
    resumeExperimentCheckpoint(data, checkpoint, resumeOutput.value.trim(), Number(resumeTarget.value));
  }, !checkpoint.loadable));
  actions.appendChild(makeJobButton("button.markBest", () => markExperimentCheckpoint(data.scene, checkpoint.iteration, "best", true), checkpoint.best));
  actions.appendChild(makeJobButton(checkpoint.pinned ? "button.unpinCheckpoint" : "button.pinCheckpoint", () => {
    markExperimentCheckpoint(data.scene, checkpoint.iteration, "pinned", !checkpoint.pinned);
  }));
  actions.appendChild(makeJobButton("button.deleteCheckpoint", (event) => deleteExperimentCheckpoint(data.scene, checkpoint.iteration, event.currentTarget), !checkpoint.loadable));
  row.append(info, actions);
  return row;
}

function renderCheckpointGroup(data) {
  const section = document.createElement("section");
  section.className = "asset-group";
  const checkpoints = Array.isArray(data.checkpoints) ? data.checkpoints : [];
  const title = document.createElement("div");
  title.className = "asset-group-title";
  title.textContent = `Checkpoints (${checkpoints.length})`;
  section.appendChild(title);
  if (!checkpoints.length) {
    const empty = document.createElement("div");
    empty.className = "asset-empty";
    empty.textContent = t("status.noCheckpoints");
    section.appendChild(empty);
    return section;
  }
  for (const checkpoint of checkpoints) {
    section.appendChild(renderCheckpointRow(checkpoint, data));
  }
  return section;
}

function renderCurveSummary(data) {
  const section = document.createElement("details");
  section.className = "asset-group";
  const curve = Array.isArray(data.curve) ? data.curve : [];
  const curves = data.curves || {};
  const series = Array.isArray(curves.series) ? curves.series : [];
  const title = document.createElement("summary");
  title.className = "asset-group-title";
  const pointCount = series.reduce((sum, item) => sum + (Array.isArray(item.points) ? item.points.length : 0), 0);
  title.textContent = `Training Curves (${curves.source || "none"}, ${pointCount || curve.length} points)`;
  section.appendChild(title);
  if (!curve.length && !series.length) {
    const empty = document.createElement("div");
    empty.className = "asset-empty";
    empty.textContent = "No TensorBoard or parsed log curve points.";
    section.appendChild(empty);
    return section;
  }
  for (const item of series.slice(0, 8)) {
    const points = Array.isArray(item.points) ? item.points : [];
    const latest = points[points.length - 1];
    const row = document.createElement("div");
    row.className = "asset-row exists";
    const info = document.createElement("div");
    info.className = "asset-info";
    const name = document.createElement("div");
    name.className = "asset-name";
    name.textContent = item.tag || "curve";
    const meta = document.createElement("div");
    meta.className = "asset-meta";
    meta.textContent = latest
      ? `${points.length.toLocaleString()} points | latest iter ${latest.iteration}: ${Number(latest.value).toPrecision(6)}`
      : "0 points";
    info.append(name, meta);
    row.appendChild(info);
    section.appendChild(row);
  }
  if (curve.length) {
    const recent = document.createElement("div");
    recent.className = "asset-group-title";
    recent.textContent = "Recent Loss Points";
    section.appendChild(recent);
  }
  for (const point of curve.slice(-6).reverse()) {
    const row = document.createElement("div");
    row.className = "asset-row exists";
    const info = document.createElement("div");
    info.className = "asset-info";
    const name = document.createElement("div");
    name.className = "asset-name";
    name.textContent = `Iteration ${point.iteration}`;
    const meta = document.createElement("div");
    meta.className = "asset-meta";
    meta.textContent = `loss ${Number(point.loss).toPrecision(6)}${point.total ? ` / ${point.total}` : ""}`;
    info.append(name, meta);
    row.appendChild(info);
    section.appendChild(row);
  }
  return section;
}

function renderExperimentDiffRows(diff) {
  const wrap = document.createElement("div");
  wrap.className = "experiment-diff";
  const changes = Array.isArray(diff?.changes) ? diff.changes : [];
  if (!changes.length) {
    const empty = document.createElement("div");
    empty.className = "asset-empty";
    empty.textContent = t("status.noDiff");
    wrap.appendChild(empty);
    return wrap;
  }
  for (const change of changes.slice(0, 80)) {
    const row = document.createElement("div");
    row.className = "experiment-diff-row";
    const key = document.createElement("strong");
    key.textContent = change.key;
    const left = document.createElement("span");
    left.textContent = experimentValueText(change.left);
    const right = document.createElement("span");
    right.textContent = experimentValueText(change.right);
    row.append(key, left, right);
    wrap.appendChild(row);
  }
  return wrap;
}

function renderCompareGroup(data) {
  const section = document.createElement("section");
  section.className = "asset-group";
  const title = document.createElement("div");
  title.className = "asset-group-title";
  title.textContent = "Parameter Diff";
  section.appendChild(title);
  const form = document.createElement("div");
  form.className = "experiment-form";
  const select = document.createElement("select");
  for (const opt of Array.from(sceneSelect.options)) {
    if (opt.value === data.scene) continue;
    const item = document.createElement("option");
    item.value = opt.value;
    item.textContent = opt.textContent;
    select.appendChild(item);
  }
  const diffBody = document.createElement("div");
  diffBody.className = "experiment-diff";
  const button = makeJobButton("button.compareExperiment", async () => {
    if (!select.value) return;
    try {
      const res = await fetch(apiPath(`/api/experiments/diff?scene=${encodeURIComponent(data.scene)}&compare=${encodeURIComponent(select.value)}`));
      const diff = await res.json();
      if (!res.ok) throw new Error(diff.error || "Compare failed");
      diffBody.innerHTML = "";
      diffBody.appendChild(renderExperimentDiffRows(diff));
    } catch (err) {
      setStatus(`Compare failed: ${err.message}`);
    }
  }, !select.options.length);
  form.append(select, button);
  section.appendChild(form);
  const empty = document.createElement("div");
  empty.className = "asset-empty";
  empty.textContent = select.options.length ? "Choose another experiment to compare." : "No other experiment found.";
  diffBody.appendChild(empty);
  section.appendChild(diffBody);
  return section;
}

function renderExperimentActions(data) {
  const actions = document.createElement("div");
  actions.className = "asset-manager-actions";
  actions.appendChild(makeJobButton("button.openOutput", () => openAssetDir("output", data.scene)));
  const cloneInput = document.createElement("input");
  cloneInput.className = "experiment-name-input";
  cloneInput.value = `${data.scene}_copy`;
  cloneInput.title = currentLanguage === "zh" ? "复制输出实验名" : currentLanguage === "ja" ? "複製先の実験名" : "Clone output experiment";
  cloneInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter") cloneCurrentExperiment(data, cloneInput.value.trim());
  });
  actions.appendChild(cloneInput);
  actions.appendChild(makeJobButton("button.cloneExperiment", () => cloneCurrentExperiment(data, cloneInput.value.trim())));
  return actions;
}

function renderExperimentManager(data) {
  if (!experimentManagerSummary || !experimentManagerList) return;
  const training = data.training || {};
  const options = training.options || {};
  const summaryParts = [
    `output/${data.scene}`,
    `${(data.backend || "3dgs").toUpperCase()} iteration ${data.latest_iteration}`,
    training.quality ? `quality ${training.quality}` : "",
    options.iterations ? `target ${options.iterations}` : "",
    `${(data.checkpoints || []).length} checkpoints`,
  ].filter(Boolean);
  experimentManagerSummary.textContent = summaryParts.join(" | ");
  experimentManagerList.innerHTML = "";
  experimentManagerList.appendChild(renderExperimentActions(data));
  experimentManagerList.appendChild(renderCheckpointGroup(data));
  experimentManagerList.appendChild(renderCompareGroup(data));
  experimentManagerList.appendChild(renderExperimentParamGroup("Training Options", options));
  experimentManagerList.appendChild(renderExperimentParamGroup("cfg_args", data.cfg_args || {}));
  experimentManagerList.appendChild(renderCurveSummary(data));
  experimentManagerList.appendChild(renderAssetJobs(data.jobs || []));
}

async function refreshExperimentManager() {
  const scene = experimentSceneName();
  if (!scene || !experimentManagerList) {
    setStatus("Load a scene first.");
    return;
  }
  try {
    const res = await fetch(apiPath(`/api/experiments?scene=${encodeURIComponent(scene)}`));
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Could not load experiment");
    renderExperimentManager(data);
    setStatus(`Experiment refreshed for output/${data.scene}.`);
  } catch (err) {
    experimentManagerList.innerHTML = "";
    const empty = document.createElement("div");
    empty.className = "asset-empty";
    empty.textContent = err.message;
    experimentManagerList.appendChild(empty);
    setStatus(`Experiment Manager failed: ${err.message}`);
  }
}

async function cloneCurrentExperiment(data, outputScene) {
  if (!outputScene) return;
  try {
    const res = await fetch(apiPath("/api/experiments/clone"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ scene: data.scene, output_scene: outputScene, overwrite: false }),
    });
    const job = await res.json();
    if (!res.ok) throw new Error(job.error || "Clone failed");
    writeUnifiedJobLog(job);
    await refreshJobCenter();
    setStatus(`Experiment clone queued: output/${outputScene}.`);
  } catch (err) {
    setStatus(`Clone failed: ${err.message}`);
  }
}

async function markExperimentCheckpoint(scene, iteration, mark, enabled) {
  try {
    const res = await fetch(apiPath("/api/experiments/checkpoint-mark"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ scene, iteration, mark, enabled }),
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Checkpoint mark failed");
    renderExperimentManager(data);
    setStatus(`Checkpoint ${iteration} updated.`);
  } catch (err) {
    setStatus(`Checkpoint mark failed: ${err.message}`);
  }
}

async function deleteExperimentCheckpoint(scene, iteration, button) {
  if (button && button.dataset.confirmDelete !== "true") {
    button.dataset.confirmDelete = "true";
    button.textContent = currentLanguage === "zh" ? "确认删除" : currentLanguage === "ja" ? "削除確認" : "Confirm";
    setStatus(`Click Confirm to delete checkpoint ${iteration}. point_cloud is kept.`);
    setTimeout(() => {
      if (button.dataset.confirmDelete === "true") {
        button.dataset.confirmDelete = "false";
        button.textContent = t("button.deleteCheckpoint");
      }
    }, 5000);
    return;
  }
  try {
    const res = await fetch(apiPath("/api/experiments/checkpoint-delete"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ scene, iteration }),
    });
    const result = await res.json();
    if (!res.ok) throw new Error(result.error || "Checkpoint delete failed");
    renderExperimentManager(result.payload);
    setStatus(`Checkpoint ${iteration} deleted.`);
  } catch (err) {
    setStatus(`Checkpoint delete failed: ${err.message}`);
  }
}

async function resumeExperimentCheckpoint(data, checkpoint, outputScene, targetIterations) {
  if (!Number.isFinite(targetIterations) || targetIterations <= Number(checkpoint.iteration || 0)) {
    setStatus("Target iterations must be greater than the checkpoint iteration.");
    return;
  }
  if (!outputScene) return;
  try {
    setTrainingBusy(true);
    showTrainingLog();
    trainLog.textContent = "";
    const res = await fetch(apiPath("/api/experiments/resume"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        scene: data.scene,
        checkpoint_iteration: checkpoint.iteration,
        output_scene: outputScene,
        target_iterations: targetIterations,
      }),
    });
    const job = await res.json();
    if (!res.ok) throw new Error(job.error || "Resume failed");
    activeTrainJobId = job.id;
    writeTrainingLog(job);
    setStatus(`Resume training queued: output/${outputScene}.`);
    pollTrainingJob(job.id, outputScene);
    refreshJobCenter();
  } catch (err) {
    activeTrainJobId = null;
    setTrainingBusy(false);
    setStatus(`Resume failed: ${err.message}`);
    appendTrainingLog(`ERROR: ${err.message}`);
  }
}

function updateCancelButton() {
  cancelTrainButton.disabled = !(activeTrainJobId || activeMeshJobId);
}

function setTrainingBusy(busy) {
  importOnlyButton.disabled = busy;
  startTrainButton.disabled = busy;
  trainExistingButton.disabled = busy;
  if (runColmapButton) runColmapButton.disabled = busy;
  trainFilesInput.disabled = busy;
  trainFolderInput.disabled = busy;
  trainMasksInput.disabled = busy;
  splatImportFileInput.disabled = busy;
  clearTrainInputButton.disabled = busy || (selectedTrainingFiles().length === 0 && trainMaskFiles.length === 0);
  trainSceneInput.disabled = busy;
  trainBackendSelect.disabled = busy;
  trainQualitySelect.disabled = busy;
  if (openExperimentManagerButton) openExperimentManagerButton.disabled = busy || !currentScene;
  for (const control of document.querySelectorAll(".train-control")) {
    control.disabled = busy;
  }
  for (const control of document.querySelectorAll(".colmap-control")) {
    control.disabled = busy;
  }
  videoFpsInput.disabled = busy;
  document.getElementById("overwriteTrain").disabled = busy;
  updateCancelButton();
}

function setMeshBusy(busy) {
  const actions = meshActionsForMode(selectedMeshMode(), currentBackend, Boolean(currentScene), busy);
  updateMeshTextureToggleLabel();
  exportMeshButton.disabled = actions.exportMeshDisabled;
  loadMeshButton.disabled = actions.loadMeshDisabled;
  bakeTextureButton.disabled = actions.bakeTextureDisabled;
  loadTextureButton.disabled = actions.loadTextureDisabled;
  meshModeSelect.disabled = busy;
  meshResInput.disabled = busy;
  meshTextureResInput.disabled = busy;
  if (meshTextureQualitySelect) meshTextureQualitySelect.disabled = busy;
  if (meshTextureBackendSelect) meshTextureBackendSelect.disabled = busy;
  for (const control of document.querySelectorAll(".sugar-control input, .sugar-control select")) {
    control.disabled = busy;
  }
  for (const control of document.querySelectorAll(".gs2mesh-control input, .gs2mesh-control select")) {
    control.disabled = busy;
  }
  if (meshTextureToggle) meshTextureToggle.disabled = busy || !(meshData?.hasVertexColors || currentMeshTexture);
  if (meshTrimToggle) meshTrimToggle.disabled = busy || !meshData;
  downloadMeshButton.disabled = busy || (!lastMeshDownloadUrl && !meshDirty);
  downloadTextureButton.disabled = busy || !lastTextureDownloadUrl;
  downloadGlbButton.disabled = busy || !currentScene;
  if (openAssetManagerButton) openAssetManagerButton.disabled = busy || !currentScene;
  if (openExperimentManagerButton) openExperimentManagerButton.disabled = busy || !currentScene;
  downloadSpzButton.disabled = busy || !currentScene;
  downloadSogButton.disabled = busy || !currentScene;
  splatImportFileInput.disabled = busy;
  updateCancelButton();
}

function setPsnrBusy(busy) {
  if (runPsnrButton) runPsnrButton.disabled = busy || !currentScene;
  if (psnrBackendSelect) psnrBackendSelect.disabled = busy;
  if (psnrCountInput) psnrCountInput.disabled = busy;
  if (psnrEvalWidthInput) psnrEvalWidthInput.disabled = busy;
  if (openPsnrOutputButton) openPsnrOutputButton.disabled = !lastPsnrJobId;
}

function createImportJobId() {
  if (globalThis.crypto?.randomUUID) return globalThis.crypto.randomUUID();
  return `import_${Date.now().toString(36)}_${Math.random().toString(36).slice(2)}`;
}

function stageLabel(stage, status) {
  const stageLabels = {
    en: {
      queued: "Waiting",
      upload: "Uploading files",
      preparing: "Preparing dataset",
      archiving: "Archiving / linking source files",
      copying_images: "Copying photos",
      extracting_frames: "Extracting video frames",
      masks: "Applying masks",
      done: "Import complete",
      failed: "Import failed",
      fallback: "Working",
    },
    zh: {
      queued: "等待开始",
      upload: "上传文件",
      preparing: "准备数据集",
      archiving: "归档/硬链接源文件",
      copying_images: "复制照片",
      extracting_frames: "视频抽帧",
      masks: "处理 Mask",
      done: "导入完成",
      failed: "导入失败",
      fallback: "处理中",
    },
    ja: {
      queued: "待機中",
      upload: "ファイル送信",
      preparing: "データセット準備",
      archiving: "元ファイル保存/リンク",
      copying_images: "写真コピー",
      extracting_frames: "動画フレーム抽出",
      masks: "マスク処理",
      done: "インポート完了",
      failed: "インポート失敗",
      fallback: "処理中",
    },
  };
  const labels = stageLabels[currentLanguage] || stageLabels.en;
  if (status === "failed") return labels.failed;
  if (status === "done") return labels.done;
  return labels[stage] || stage || labels.fallback;
}

function importPercent(job, uploadPercent) {
  if (job?.status === "done") return 100;
  if (job?.status === "failed") return 100;
  if (!job || job.stage === "upload") return Math.max(1, Math.min(30, uploadPercent * 0.3));
  if (job.stage === "preparing") return 32;
  if (job.stage === "archiving") {
    const total = Math.max(1, Number(job.total_files || 0));
    return 32 + Math.min(13, (Number(job.processed_files || 0) / total) * 13);
  }
  if (job.stage === "copying_images") return 45;
  if (job.stage === "extracting_frames") {
    const total = Math.max(1, Number(job.total_videos || 0));
    return 45 + Math.min(50, (Number(job.processed_videos || 0) / total) * 50);
  }
  if (job.stage === "masks") return 97;
  return 5;
}

function renderImportProgress(job = null) {
  if (!importProgressPanel) return;
  const percent = Math.round(importPercent(job, lastImportUploadPercent));
  importProgressStage.textContent = stageLabel(job?.stage || "upload", job?.status || "running");
  importProgressPercent.textContent = `${percent}%`;
  importProgressFill.style.width = `${Math.max(0, Math.min(100, percent))}%`;
  const fileText = job ? `${Number(job.processed_files || 0).toLocaleString()} / ${Number(job.total_files || 0).toLocaleString()} files` : `upload ${Math.round(lastImportUploadPercent)}%`;
  const videoText = job && Number(job.total_videos || 0) > 0
    ? `${Number(job.processed_videos || 0).toLocaleString()} / ${Number(job.total_videos || 0).toLocaleString()} videos`
    : "no videos";
  const frameText = job ? `${Number(job.extracted_frames || 0).toLocaleString()} frames` : "0 frames";
  const workerText = job?.video_workers ? `${job.video_workers} workers` : "";
  const currentText = job?.current_file ? `Current: ${job.current_file}` : "";
  const errorText = job?.error ? `Error: ${job.error}` : "";
  importProgressDetails.textContent = [fileText, videoText, frameText, workerText, currentText, errorText].filter(Boolean).join(" | ");
  const recent = Array.isArray(job?.recent) ? job.recent : [];
  importProgressRecent.textContent = recent.length
    ? recent.map((item) => `${item.name}: ${Number(item.frames || 0).toLocaleString()} frames`).join("\n")
    : "";
}

function startImportProgress(jobId, sceneName, files) {
  activeImportJobId = jobId;
  lastImportUploadPercent = 0;
  if (importProgressPanel) importProgressPanel.hidden = false;
  renderImportProgress({
    id: jobId,
    scene: sceneName,
    status: "running",
    stage: "upload",
    total_files: files.length,
    processed_files: 0,
    total_videos: files.filter((file) => VIDEO_EXTS.has((file.name.split(".").pop() || "").toLowerCase())).length,
    processed_videos: 0,
    extracted_frames: 0,
  });
  pollImportProgress(jobId);
}

function stopImportProgressPolling() {
  if (importPollTimer) clearTimeout(importPollTimer);
  importPollTimer = null;
}

async function pollImportProgress(jobId) {
  stopImportProgressPolling();
  if (!jobId || jobId !== activeImportJobId) return;
  try {
    const res = await fetch(apiPath(`/api/import/status?id=${encodeURIComponent(jobId)}`));
    if (res.ok) {
      const job = await res.json();
      renderImportProgress(job);
      if (job.status === "done" || job.status === "failed") {
        activeImportJobId = null;
        return;
      }
    }
  } catch (_) {
    // Keep polling; during multipart upload the server may not have created the job yet.
  }
  importPollTimer = setTimeout(() => pollImportProgress(jobId), 500);
}

function updateUploadProgress(percent) {
  lastImportUploadPercent = Math.max(0, Math.min(100, Number(percent) || 0));
  renderImportProgress({
    id: activeImportJobId,
    status: "running",
    stage: "upload",
  });
}

function buildImportForm(sceneName, overwrite, files, importJobId = "") {
  const form = new FormData();
  form.append("scene", sceneName);
  form.append("overwrite", overwrite ? "true" : "false");
  form.append("fps", videoFpsInput.value || "2");
  if (importJobId) form.append("import_job_id", importJobId);
  form.append("fileMetadata", JSON.stringify(files.map((file) => ({
    name: file.name,
    relativePath: file.webkitRelativePath || file.name,
    type: file.type || "",
    size: file.size,
    lastModified: file.lastModified,
  }))));
  for (const file of files) form.append("files", file, file.webkitRelativePath || file.name);
  for (const file of trainMaskFiles) form.append("maskFiles", file, file.webkitRelativePath || file.name);
  return form;
}

function localPathForFile(file) {
  if (window.gsEditor?.getPathForFile) return window.gsEditor.getPathForFile(file);
  return file.path || "";
}

function fileMetadata(file) {
  return {
    name: file.name,
    relativePath: file.webkitRelativePath || file.name,
    type: file.type || "",
    size: file.size,
    lastModified: file.lastModified,
  };
}

function buildImportPathPayload(sceneName, overwrite, files, importJobId = "") {
  if (trainMaskFiles.length) return null;
  const pathFiles = files.map((file) => ({ ...fileMetadata(file), path: localPathForFile(file) }));
  if (!pathFiles.length || pathFiles.some((file) => !file.path)) return null;
  return {
    scene: sceneName,
    overwrite: Boolean(overwrite),
    fps: Number(videoFpsInput.value || 2),
    import_job_id: importJobId,
    files: pathFiles,
  };
}

function uploadDataset(form, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", apiPath("/api/import"));
    xhr.upload.onprogress = (event) => {
      if (!event.lengthComputable) return;
      onProgress(Math.round((event.loaded / event.total) * 100));
    };
    xhr.upload.onload = () => {
      onProgress(100);
      setStatus("Upload complete. Extracting frames and preparing images...");
      appendTrainingLog("Upload complete. Server is extracting frames / copying images...");
    };
    xhr.onerror = () => reject(new Error("Upload failed"));
    xhr.onload = () => {
      let data = {};
      try {
        data = JSON.parse(xhr.responseText || "{}");
      } catch (_) {
        reject(new Error(xhr.responseText || "Import failed"));
        return;
      }
      if (xhr.status >= 200 && xhr.status < 300) resolve(data);
      else reject(new Error(data.error || "Import failed"));
    };
    xhr.send(form);
  });
}

async function importDataset(sceneName, overwrite, files, onProgress) {
  const importJobId = createImportJobId();
  startImportProgress(importJobId, sceneName, files);
  const wrappedProgress = (percent) => {
    updateUploadProgress(percent);
    onProgress(percent);
  };
  const pathPayload = buildImportPathPayload(sceneName, overwrite, files, importJobId);
  if (pathPayload) {
    wrappedProgress(100);
    setStatus("Using local file paths. Extracting frames and preparing images...");
    appendTrainingLog("Local path import: server is linking/copying files and extracting frames...");
    const res = await fetch(apiPath("/api/import-paths"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(pathPayload),
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Local path import failed");
    return data;
  }
  return uploadDataset(buildImportForm(sceneName, overwrite, files, importJobId), wrappedProgress);
}

async function importSplatFile() {
  const file = splatImportFileInput.files?.[0];
  if (!file) return;
  const fallbackName = file.name.replace(/\.[^.]+$/, "").replace(/[^A-Za-z0-9_.-]+/g, "_") || "imported_splat";
  const sceneName = (outputInput.value || fallbackName).trim();
  try {
    validateSceneName(sceneName);
  } catch (err) {
    setStatus(err.message);
    splatImportFileInput.value = "";
    return;
  }
  const form = new FormData();
  form.append("scene", sceneName);
  form.append("overwrite", document.getElementById("overwriteOutput")?.checked ? "true" : "false");
  form.append("file", file, file.name);
  setTrainingBusy(true);
  try {
    setStatus(`Importing ${file.name} as output/${sceneName}...`);
    const res = await fetch(apiPath("/api/splat/import"), { method: "POST", body: form });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Splat import failed");
    await loadScenes();
    sceneSelect.value = data.scene;
    iterationInput.value = data.latest_iteration;
    setStatus(`Imported ${data.format.toUpperCase()} scene: output/${data.scene}.`);
  } catch (err) {
    setStatus(`Splat import failed: ${err.message}`);
  } finally {
    setTrainingBusy(false);
    splatImportFileInput.value = "";
  }
}

async function downloadSplatFormat(format) {
  if (!currentScene) {
    setStatus("Load a scene before exporting splats.");
    return;
  }
  const iteration = currentIteration || Number(iterationInput.value || 0);
  downloadSpzButton.disabled = true;
  downloadSogButton.disabled = true;
  try {
    const res = await fetch(apiPath("/api/splat/export/start"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ scene: currentScene, iteration, format }),
    });
    const job = await res.json();
    if (!res.ok) throw new Error(job.error || "Could not start splat export");
    setStatus(`${format.toUpperCase()} export queued for ${currentScene} iteration ${iteration}.`);
    writeUnifiedJobLog(job);
    showJobCenter();
    updateJobCenterPolling(true);
  } catch (err) {
    setStatus(`${format.toUpperCase()} export failed to start: ${err.message}`);
  } finally {
    setMeshBusy(false);
  }
}

async function checkTrainingEnvironment() {
  const backend = trainBackendSelect?.value || "3dgs";
  try {
    showTrainingLog();
    appendTrainingLog(`Checking ${backend.toUpperCase()} training environment...`);
    const res = await fetch(apiPath(`/api/train/check?backend=${encodeURIComponent(backend)}`));
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Environment check failed");
    const lines = [
      `Environment check: ${backend.toUpperCase()}`,
      `Root: ${data.root || ""}`,
      `Miniforge: ${data.miniforge_exists ? "OK" : "MISSING"} - ${data.miniforge}`,
      `Conda: ${data.conda_exists ? "OK" : "MISSING"} - ${data.conda}`,
      `Python: ${data.python}`,
      `Conda env: ${data.env_root_exists ? "OK" : "MISSING"} - ${data.env_root}`,
      `gaussian-splatting: ${data.gaussian_dir_exists ? "OK" : "MISSING"} - ${data.gaussian_dir}`,
      `Git: ${data.git_exists ? "OK" : "MISSING"} - ${data.git || ""}`,
      `VS Build Tools: ${data.vs_build_tools_exists ? "OK" : "MISSING"} - ${data.vs_build_tools || ""}`,
      ...(backend === "2dgs"
        ? [
            `2DGS repo: ${data.two_dgs_dir_exists ? "OK" : "MISSING"} - ${data.two_dgs_dir}`,
            `2DGS venv: ${data.two_dgs_python_exists ? "OK" : "MISSING"} - ${data.two_dgs_python}`,
            `2DGS train.py: ${data.two_dgs_train_exists ? "OK" : "MISSING"} - ${data.two_dgs_train}`,
          ]
        : []),
      `COLMAP: ${data.colmap_exists ? "OK" : "MISSING"} - ${data.colmap}`,
      `ffmpeg: ${data.ffmpeg_exists ? "OK" : "MISSING"} - ${data.ffmpeg}`,
      `OpenCV: ${data.opencv_ok ? `OK - ${data.opencv_detail || ""}` : `MISSING - ${data.opencv_error}`}`,
      `Video packages: ${data.video_packages_ok ? "OK" : `MISSING - ${data.video_packages_error}`}`,
      `3DGS runtime imports: ${data.runtime_imports_ok ? `OK - ${data.runtime_imports_detail || ""}` : `FAILED - ${data.runtime_imports_error}`}`,
      `Node helper: ${data.crop_node_helper_exists ? "OK" : "MISSING"} - ${data.crop_node_helper}`,
    ];
    appendTrainingLog(lines.join("\n"));
    const backendOk = backend === "2dgs"
      ? data.two_dgs_dir_exists && data.two_dgs_python_exists && data.two_dgs_train_exists
      : true;
    const commonOk = data.conda_exists && data.python_exists && data.env_root_exists && data.colmap_exists && data.opencv_ok && data.video_packages_ok && data.ffmpeg_exists && data.gaussian_dir_exists && data.runtime_imports_ok;
    setStatus(commonOk && backendOk ? `${backend.toUpperCase()} training environment OK.` : "Training environment has missing components. See log.");
  } catch (err) {
    setStatus(`Environment check failed: ${err.message}`);
  }
}

async function importOnly() {
  const sceneName = trainSceneInput.value.trim();
  if (!sceneName) {
    setStatus("Training scene name is required.");
    return;
  }
  try {
    validateSceneName(sceneName);
  } catch (err) {
    setStatus(err.message);
    return;
  }
  const files = selectedTrainingFiles();
  if (!files.length) {
    setStatus("Choose photos, a video, or a folder first.");
    return;
  }
  const overwrite = Boolean(document.getElementById("overwriteTrain")?.checked);
  setTrainingBusy(true);
  activeTrainJobId = null;
  showTrainingLog();
  trainLog.textContent = "";
  try {
    setStatus(`Importing ${files.length.toLocaleString()} file(s) into datasets/${sceneName}...`);
    appendTrainingLog(`Import only: uploading ${files.length.toLocaleString()} input file(s) into datasets/${sceneName}...`);
    const importData = await importDataset(sceneName, overwrite, files, (percent) => {
      setStatus(`Importing ${files.length.toLocaleString()} file(s): ${percent}%`);
    });
    appendTrainingLog(`Import complete: ${importData.image_count.toLocaleString()} image(s) ready in datasets/${sceneName}/images. Videos: ${importData.saved_videos}, extracted frames: ${importData.extracted_frames}${importData.video_workers ? `, video workers: ${importData.video_workers}` : ""}.`);
    setStatus(`Import complete. datasets/${sceneName}/images has ${importData.image_count.toLocaleString()} image(s).`);
  } catch (err) {
    appendTrainingLog(`ERROR: ${err.message}`);
    setStatus(`Import failed: ${err.message}`);
  } finally {
    setTrainingBusy(false);
  }
}

async function importAndStartTraining(uploadFirst) {
  const sceneName = trainSceneInput.value.trim();
  if (!sceneName) {
    setStatus("Training scene name is required.");
    return;
  }
  try {
    validateSceneName(sceneName);
  } catch (err) {
    setStatus(err.message);
    return;
  }
  const overwrite = Boolean(document.getElementById("overwriteTrain")?.checked);
  const backend = trainBackendSelect?.value || "3dgs";
  const sourceScene = uploadFirst ? sceneName : selectedAlignedSourceName(sceneName);
  const outputScene = uploadFirst
    ? trainingOutputSceneName(sceneName, backend)
    : existingTrainingOutputSceneName(sourceScene, sceneName, backend, outputBackendByName);
  if (!uploadFirst) {
    try {
      validateSceneName(sourceScene);
      validateSceneName(outputScene);
    } catch (err) {
      setStatus(err.message);
      return;
    }
  }
  const files = selectedTrainingFiles();
  setTrainingBusy(true);
  activeTrainJobId = null;
  showTrainingLog();
  trainLog.textContent = "";
  try {
    await checkTrainingEnvironment();
    if (uploadFirst) {
      if (!files.length) {
        setStatus("Choose photos, a video, or a folder first.");
        setTrainingBusy(false);
        return;
      }
      setStatus(`Uploading ${files.length.toLocaleString()} files into datasets/${sceneName}...`);
      appendTrainingLog(`Uploading ${files.length.toLocaleString()} input file(s) into datasets/${sceneName}...`);
      const importData = await importDataset(sceneName, overwrite, files, (percent) => {
        setStatus(`Importing ${files.length.toLocaleString()} file(s): ${percent}%`);
      });
      setStatus(`Imported ${importData.image_count.toLocaleString()} images. Starting COLMAP + ${backend.toUpperCase()} training to output/${outputScene}...`);
      appendTrainingLog(`Imported ${importData.image_count.toLocaleString()} image(s). Saved videos: ${importData.saved_videos}, extracted frames: ${importData.extracted_frames}${importData.video_workers ? `, video workers: ${importData.video_workers}` : ""}.`);
    } else {
      const sourceInfo = alignedDatasetInfo(sourceScene);
      const sourceKind = sourceInfo?.has_alignment ? "aligned data" : "existing images";
      setStatus(`Starting ${backend.toUpperCase()} training from datasets/${sourceScene} (${sourceKind}) to output/${outputScene}...`);
      appendTrainingLog(`Starting ${backend.toUpperCase()} training from datasets/${sourceScene} (${sourceKind}) to output/${outputScene}...`);
    }

    const startRes = await fetch(apiPath("/api/train/start"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        scene: sourceScene,
        output_scene: outputScene,
        backend,
        quality: trainQualitySelect.value,
        train_options: trainingOptions(),
        run_convert: uploadFirst,
        overwrite,
        allow_existing_output: !uploadFirst,
        colmap: colmapTrainingOptions(),
      }),
    });
    const job = await startRes.json();
    if (!startRes.ok) throw new Error(job.error || "Could not start training");
    activeTrainJobId = job.id;
    setTrainingBusy(true);
    writeTrainingLog(job);
    refreshJobCenter();
    pollTrainingJob(job.id, outputScene);
  } catch (err) {
    setStatus(`Training start failed: ${err.message}`);
    appendTrainingLog(`ERROR: ${err.message}`);
    activeTrainJobId = null;
    setTrainingBusy(false);
  }
}

async function startColmapAlignment() {
  const sceneName = trainSceneInput.value.trim();
  if (!sceneName) {
    setStatus("Training scene name is required.");
    return;
  }
  try {
    validateSceneName(sceneName);
  } catch (err) {
    setStatus(err.message);
    return;
  }
  activeMeshJobId = null;
  setTrainingBusy(true);
  showTrainingLog();
  trainLog.textContent = "";
  try {
    await checkTrainingEnvironment();
    setStatus(`Starting COLMAP alignment for datasets/${sceneName}...`);
    appendTrainingLog(`Starting COLMAP alignment for datasets/${sceneName}...`);
    const res = await fetch(apiPath("/api/colmap/start"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        scene: sceneName,
        options: colmapTrainingOptions(),
      }),
    });
    const job = await res.json();
    if (!res.ok) throw new Error(job.error || "Could not start COLMAP alignment");
    activeMeshJobId = job.id;
    updateCancelButton();
    writeMeshLog(job);
    refreshJobCenter();
    pollMeshJob(job.id);
  } catch (err) {
    setStatus(`COLMAP start failed: ${err.message}`);
    appendTrainingLog(`ERROR: ${err.message}`);
    activeMeshJobId = null;
    setTrainingBusy(false);
  }
}

async function cancelActiveTraining() {
  if (!activeTrainJobId && !activeMeshJobId) return;
  try {
    cancelTrainButton.disabled = true;
    const isMeshJob = !activeTrainJobId && Boolean(activeMeshJobId);
    const id = activeTrainJobId || activeMeshJobId;
    setStatus(isMeshJob ? "Cancelling mesh job..." : "Cancelling training...");
    const res = await fetch(apiPath(isMeshJob ? "/api/mesh/cancel" : "/api/train/cancel"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id }),
    });
    const job = await res.json();
    if (!res.ok) throw new Error(job.error || "Cancel failed");
    if (isMeshJob) writeMeshLog(job);
    else writeTrainingLog(job);
  } catch (err) {
    setStatus(`Cancel failed: ${err.message}`);
    updateCancelButton();
  }
}

function pollTrainingJob(jobId, outputScene) {
  if (trainPollTimer) clearInterval(trainPollTimer);
  const tick = async () => {
    try {
      const res = await fetch(apiPath(`/api/train/status?id=${encodeURIComponent(jobId)}`));
      const job = await res.json();
      if (!res.ok) throw new Error(job.error || "Training status failed");
      writeTrainingLog(job);
      setStatus(`${(job.backend || "3dgs").toUpperCase()} training ${job.output_scene}: ${job.status} / ${job.stage}`);
      if (job.status === "done") {
        clearInterval(trainPollTimer);
        trainPollTimer = null;
        await refreshSceneList(outputScene);
        const found = Array.from(sceneSelect.options).find((opt) => opt.value === outputScene);
        if (found) {
          sceneSelect.value = outputScene;
          iterationInput.value = found.dataset.iteration;
          outputInput.value = `${outputScene}_crop`;
        }
        setStatus(`Training complete. ${outputScene} is ready to Load.`);
        activeTrainJobId = null;
        setTrainingBusy(false);
        refreshJobCenter();
      }
      if (job.status === "failed" || job.status === "cancelled") {
        clearInterval(trainPollTimer);
        trainPollTimer = null;
        setStatus(job.status === "cancelled" ? "Training cancelled." : `Training failed: ${job.error}`);
        activeTrainJobId = null;
        setTrainingBusy(false);
        refreshJobCenter();
      }
      if (job.status === "cancelling") {
        setStatus("Cancelling training...");
      }
    } catch (err) {
      clearInterval(trainPollTimer);
      trainPollTimer = null;
      setStatus(`Training status failed: ${err.message}`);
      activeTrainJobId = null;
      setTrainingBusy(false);
    }
  };
  tick();
  trainPollTimer = setInterval(tick, 1500);
}

async function startMeshExport() {
  if (!currentScene || !currentIteration) {
    setStatus(selectedMeshModeUsesSugar() ? "Load a 3DGS scene first." : "Load a 2DGS scene first.");
    return;
  }
  if (!selectedMeshModeMatchesBackend()) {
    setStatus(selectedMeshModeUsesSugar() ? "SuGaR mesh export is only available for 3DGS scenes." : "Mesh export is only available for 2DGS scenes.");
    return;
  }
  const meshRes = Number(meshResInput.value || 512);
  if (!selectedMeshModeUsesSugar() && (!Number.isFinite(meshRes) || meshRes < 64 || meshRes > 4096)) {
    setStatus("Mesh resolution must be between 64 and 4096.");
    return;
  }
  activeMeshJobId = null;
  lastMeshDownloadUrl = null;
  setMeshBusy(true);
  showTrainingLog();
  trainLog.textContent = "";
  try {
    const backendLabel = selectedMeshModeUsesSugar() ? "SuGaR" : (selectedMeshModeUsesGs2Mesh() ? "GS2Mesh" : "2DGS");
    const options = {
      mode: selectedMeshMode(),
      mesh_res: meshRes,
      num_cluster: 50,
      depth_ratio: 0.0,
    };
    if (selectedMeshModeUsesSugar()) {
      Object.assign(options, sugarMeshOptions());
    } else if (selectedMeshModeUsesGs2Mesh()) {
      Object.assign(options, gs2meshMeshOptions());
    }
    setStatus(`Starting ${backendLabel} mesh export for ${currentScene}...`);
    appendTrainingLog(`Starting ${backendLabel} mesh export for output/${currentScene} iteration ${currentIteration}...`);
    if (selectedMeshModeUsesSugar()) {
      appendTrainingLog(`SuGaR options: ${JSON.stringify({
        preset: sugarQualitySelect?.value || "preview",
        poly: options.sugar_quality,
        refine: options.sugar_refinement_time,
        regularization: options.sugar_regularization,
        level: options.sugar_surface_level,
        uv: options.sugar_square_size,
        images: options.sugar_max_images || "all",
        imageMax: options.sugar_max_image_size,
        postprocess: options.sugar_postprocess,
      })}`);
    } else if (selectedMeshModeUsesGs2Mesh()) {
      appendTrainingLog(`GS2Mesh options: ${JSON.stringify({
        downsample: options.gs2mesh_downsample,
        baseline: options.gs2mesh_baseline_percentage,
        voxel: options.gs2mesh_tsdf_voxel,
        depth: [options.gs2mesh_tsdf_min_depth_baselines, options.gs2mesh_tsdf_max_depth_baselines],
        scene360: options.gs2mesh_scene_360,
      })}`);
    }
    const res = await fetch(apiPath("/api/mesh/start"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        scene: currentScene,
        iteration: currentIteration,
        options,
      }),
    });
    const job = await res.json();
    if (!res.ok) throw new Error(job.error || "Could not start mesh export");
    activeMeshJobId = job.id;
    updateCancelButton();
    writeMeshLog(job);
    refreshJobCenter();
    pollMeshJob(job.id);
  } catch (err) {
    setStatus(`Mesh export failed to start: ${err.message}`);
    appendTrainingLog(`ERROR: ${err.message}`);
    activeMeshJobId = null;
    setMeshBusy(false);
  }
}

async function startTextureBake() {
  if (!currentScene || !currentIteration) {
    setStatus("Load a scene first.");
    return;
  }
  if (selectedMeshModeUsesSugar()) {
    setStatus("SuGaR uses its own texture export path. Photo texture baking here supports 2DGS and GS2Mesh meshes.");
    return;
  }
  if (!meshModeSupportsTextureBake(selectedMeshMode(), currentBackend)) {
    setStatus("Photo texture baking supports 2DGS meshes and GS2Mesh meshes from loaded 3DGS scenes.");
    return;
  }
  const textureRes = Number(meshTextureResInput.value || 4096);
  if (!Number.isFinite(textureRes) || textureRes < 256 || textureRes > 8192) {
    setStatus("Texture resolution must be between 256 and 8192.");
    return;
  }
  const textureOptions = textureOptionsFromQuality(meshTextureQualitySelect?.value, textureRes);
  const textureBackend = meshTextureBackendSelect?.value || "openmvs";
  if (meshTextureQualitySelect?.value === "smooth" || meshTextureQualitySelect?.value === "ultra") {
    meshTextureResInput.value = String(textureOptions.textureRes);
  }
  activeMeshJobId = null;
  lastTextureDownloadUrl = null;
  lastGlbDownloadUrl = null;
  setMeshBusy(true);
  showTrainingLog();
  trainLog.textContent = "";
  try {
    const backendLabel = textureBackend === "colmap" ? "COLMAP" : "OpenMVS";
    const meshLabel = selectedMeshModeUsesGs2Mesh() ? "GS2Mesh" : "2DGS";
    setStatus(`Starting ${backendLabel} photo texture bake for ${meshLabel} ${currentScene}...`);
    appendTrainingLog(`Starting ${backendLabel} photo texture bake for ${meshLabel} output/${currentScene} iteration ${currentIteration}...`);
    const res = await fetch(apiPath("/api/mesh/texture/start"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        scene: currentScene,
        iteration: currentIteration,
        options: {
          mode: meshModeSelect.value,
          texture_res: textureOptions.textureRes,
          padding: 8,
          max_faces: textureOptions.maxFaces,
          max_images: 96,
          bake_source: "photo",
          backend: textureBackend,
          local_seam_leveling: textureOptions.localSeamLeveling,
          cost_smoothness_ratio: textureOptions.costSmoothnessRatio,
          texture_size_multiple: textureOptions.textureSizeMultiple,
          post: true,
        },
      }),
    });
    const job = await res.json();
    if (!res.ok) throw new Error(job.error || "Could not start texture bake");
    activeMeshJobId = job.id;
    updateCancelButton();
    writeMeshLog(job);
    refreshJobCenter();
    pollMeshJob(job.id);
  } catch (err) {
    setStatus(`Texture bake failed to start: ${err.message}`);
    appendTrainingLog(`ERROR: ${err.message}`);
    activeMeshJobId = null;
    setMeshBusy(false);
  }
}

function pollMeshJob(jobId) {
  if (meshPollTimer) clearInterval(meshPollTimer);
  const tick = async () => {
    try {
      const res = await fetch(apiPath(`/api/mesh/status?id=${encodeURIComponent(jobId)}`));
      const job = await res.json();
      if (!res.ok) throw new Error(job.error || "Mesh status failed");
      writeMeshLog(job);
      const isTexture = job.kind === "texture";
      const isGlb = job.kind === "glb";
      const isColmap = job.kind === "colmap";
      const meshBackendLabel = isColmap ? "COLMAP" : (job.mode === "sugar" ? "SuGaR" : (job.mode === "gs2mesh" ? "GS2Mesh" : "2DGS"));
      const jobLabel = isColmap ? "alignment" : (isGlb ? "GLB export" : (isTexture ? "texture bake" : "mesh export"));
      setStatus(`${meshBackendLabel} ${jobLabel} ${job.scene}: ${job.status} / ${job.stage}`);
      if (job.status === "done") {
        clearInterval(meshPollTimer);
        meshPollTimer = null;
        activeMeshJobId = null;
        if (isGlb) {
          lastGlbDownloadUrl = job.glb_download_url || job.download_url || null;
          if (job.texture_download_url) lastTextureDownloadUrl = job.texture_download_url;
        } else if (isTexture || job.texture_download_url) lastTextureDownloadUrl = job.texture_download_url;
        else lastMeshDownloadUrl = job.download_url;
        if (isColmap) setTrainingBusy(false);
        else setMeshBusy(false);
        setStatus(isColmap ? `COLMAP alignment complete. ${job.output_dir || ""}` : (isGlb ? `GLB export complete. ${job.texture?.glb || ""}` : (isTexture ? `Texture bake complete. ${job.texture?.zip || ""}` : `Mesh export complete. ${job.output_mesh}`)));
        if (isColmap) {
          await refreshSceneList();
          refreshJobCenter();
          return;
        }
        if ((isTexture || job.mode === "sugar") && job.texture_download_url) {
          try {
            await loadCurrentTexture();
          } catch (err) {
            setStatus(`Texture baked, but preview load failed: ${err.message}`);
          }
        }
        if (!isTexture && job.mode !== "sugar" && job.download_url) {
          try {
            await loadCurrentMesh();
          } catch (err) {
            setStatus(`Mesh exported, but preview load failed: ${err.message}`);
          }
        }
        refreshJobCenter();
      }
      if (job.status === "failed" || job.status === "cancelled") {
        clearInterval(meshPollTimer);
        meshPollTimer = null;
        activeMeshJobId = null;
        if (isColmap) setTrainingBusy(false);
        else setMeshBusy(false);
        const failedLabel = isColmap ? "COLMAP" : (isGlb ? "GLB" : "Mesh");
        setStatus(job.status === "cancelled" ? `${failedLabel} job cancelled.` : `${failedLabel} job failed: ${job.error}`);
        refreshJobCenter();
      }
    } catch (err) {
      clearInterval(meshPollTimer);
      meshPollTimer = null;
      activeMeshJobId = null;
      setTrainingBusy(false);
      setMeshBusy(false);
      setStatus(`Mesh status failed: ${err.message}`);
    }
  };
  tick();
  meshPollTimer = setInterval(tick, 1500);
}

async function startPsnrAnalysis() {
  if (!currentScene || !currentIteration) {
    setStatus("Load a scene before running PSNR analysis.");
    return;
  }
  const count = Math.max(1, Math.min(500, Math.round(Number(psnrCountInput?.value || 20))));
  const evalWidth = Math.max(0, Math.min(12000, Math.round(Number(psnrEvalWidthInput?.value || 0))));
  const backend = psnrBackendSelect?.value || currentBackend || "3dgs";
  if (psnrCountInput) psnrCountInput.value = String(count);
  if (psnrEvalWidthInput) psnrEvalWidthInput.value = String(evalWidth);
  trainLog.textContent = "";
  trainPanel.hidden = false;
  setPsnrBusy(true);
  try {
    setStatus(`Starting PSNR analysis for ${currentScene} (${count} views)...`);
    appendTrainingLog(`Starting PSNR analysis for output/${currentScene} iteration ${currentIteration}...`);
    appendTrainingLog(`PSNR eval width: ${evalWidth || "original"}${evalWidth ? "" : " (auto fallback to 1600 only if 3DGS rasterizer overflows)"}`);
    const res = await fetch(apiPath("/api/psnr/start"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        scene: currentScene,
        iteration: currentIteration,
        backend,
        count,
        eval_width: evalWidth,
      }),
    });
    const job = await res.json();
    if (!res.ok) throw new Error(job.error || "Could not start PSNR analysis");
    activePsnrJobId = job.id;
    lastPsnrJobId = job.id;
    pollPsnrJob(job.id);
  } catch (err) {
    activePsnrJobId = null;
    setPsnrBusy(false);
    appendTrainingLog(`ERROR: ${err.message}`);
    setStatus(`PSNR analysis failed: ${err.message}`);
  }
}

function pollPsnrJob(jobId) {
  if (psnrPollTimer) clearInterval(psnrPollTimer);
  const tick = async () => {
    try {
      const res = await fetch(apiPath(`/api/mesh/status?id=${encodeURIComponent(jobId)}`));
      const job = await res.json();
      if (!res.ok) throw new Error(job.error || "PSNR job not found");
      trainLog.textContent = (job.log || []).join("\n");
      trainLog.scrollTop = trainLog.scrollHeight;
      const avg = job.result?.average_psnr;
      const avgText = Number.isFinite(avg) ? `${avg.toFixed(3)} dB` : "";
      const widthText = job.eval_width ? `, width ${job.eval_width}` : "";
      setStatus(`PSNR ${job.scene}: ${job.status} / ${job.stage}${avgText ? `, avg ${avgText}` : ""}${widthText}`);
      if (job.status === "done") {
        clearInterval(psnrPollTimer);
        psnrPollTimer = null;
        activePsnrJobId = null;
        lastPsnrJobId = job.id;
        setPsnrBusy(false);
        const fallbackText = job.fallback_reason ? ` Fallback: ${job.fallback_reason}.` : "";
        setStatus(`PSNR complete: ${avgText || "done"}.${fallbackText} Output: ${job.output_dir || ""}`);
        return;
      }
      if (job.status === "failed" || job.status === "cancelled") {
        clearInterval(psnrPollTimer);
        psnrPollTimer = null;
        activePsnrJobId = null;
        setPsnrBusy(false);
        setStatus(`PSNR ${job.status}: ${job.error || ""}`);
      }
    } catch (err) {
      clearInterval(psnrPollTimer);
      psnrPollTimer = null;
      activePsnrJobId = null;
      setPsnrBusy(false);
      setStatus(`PSNR poll failed: ${err.message}`);
    }
  };
  tick();
  psnrPollTimer = setInterval(tick, 1500);
}

async function openLastPsnrOutput() {
  if (!lastPsnrJobId) return;
  try {
    const res = await fetch(apiPath("/api/jobs/open-output"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id: lastPsnrJobId }),
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Open PSNR output failed");
    setStatus(`Opened PSNR output: ${data.path}`);
  } catch (err) {
    setStatus(`Open PSNR output failed: ${err.message}`);
  }
}

function downloadLastMesh() {
  if (meshDirty && meshData) {
    if (meshData.isPreview) {
      setStatus("Preview mesh cannot be saved as a trim. Enable Mesh Trim and load the full mesh first.");
      return;
    }
    const blob = new Blob([meshToAsciiPly(meshData, meshDeletedFaces)], { type: "application/octet-stream" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `${currentScene || "mesh"}_${meshModeSelect.value}_trimmed.ply`;
    document.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    setStatus("Trimmed mesh downloaded.");
    return;
  }
  if (!lastMeshDownloadUrl) {
    setStatus("Export a mesh first.");
    return;
  }
  window.location.href = apiPath(lastMeshDownloadUrl);
}

function downloadLastTexture() {
  if (!lastTextureDownloadUrl) {
    setStatus("Bake a texture first.");
    return;
  }
  window.location.href = apiPath(lastTextureDownloadUrl);
}

function formatByteSize(bytes) {
  const value = Number(bytes || 0);
  if (!Number.isFinite(value) || value <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB"];
  let size = value;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return `${size.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
}

async function downloadLastGlb() {
  if (!lastGlbDownloadUrl) {
    if (!currentScene || !currentIteration) {
      setStatus("Load a scene first.");
      return;
    }
    try {
      setMeshBusy(true);
      setStatus(`Checking textured assets for ${currentScene} before GLB export...`);
      const assetsRes = await fetch(apiPath(`/api/mesh/assets?scene=${encodeURIComponent(currentScene)}&iteration=${currentIteration}`));
      const assets = await assetsRes.json();
      if (!assetsRes.ok) throw new Error(assets.error || "Could not list texture assets");
      const texture = assets.meshes?.[meshModeSelect.value]?.texture;
      if (!texture?.files?.obj?.exists || !texture?.files?.png?.exists) {
        throw new Error(`No baked ${meshModeSelect.value} texture found. Bake Texture first.`);
      }
      const glbFile = texture.files.glb;
      if (glbFile?.exists) {
        lastGlbDownloadUrl = glbFile.url;
        window.location.href = apiPath(lastGlbDownloadUrl);
        setMeshBusy(false);
        return;
      }
      showTrainingLog();
      appendTrainingLog(`Starting GLB export for ${meshModeSelect.value} output/${currentScene} iteration ${currentIteration}...`);
      const res = await fetch(apiPath("/api/mesh/glb/start"), {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          scene: currentScene,
          iteration: currentIteration,
          options: {
            mode: meshModeSelect.value,
            post: true,
          },
        }),
      });
      const job = await res.json();
      if (!res.ok) throw new Error(job.error || "Could not start GLB export");
      activeMeshJobId = job.id;
      updateCancelButton();
      writeMeshLog(job);
      refreshJobCenter();
      pollMeshJob(job.id);
    } catch (err) {
      setStatus(`GLB export failed to start: ${err.message}`);
      appendTrainingLog(`ERROR: ${err.message}`);
      activeMeshJobId = null;
      setMeshBusy(false);
    }
    return;
  }
  window.location.href = apiPath(lastGlbDownloadUrl);
}

async function loadCurrentTexture() {
  if (!currentScene || !currentIteration) {
    setStatus(meshModeUses3dgs(selectedMeshMode()) ? "Load a 3DGS scene first." : "Load a 2DGS scene first.");
    return;
  }
  if (!selectedMeshModeMatchesBackend()) {
    setStatus(meshModeUses3dgs(selectedMeshMode()) ? "Texture view is only available for loaded 3DGS scenes in this mesh mode." : "Texture view is only available for 2DGS scenes.");
    return;
  }
  setStatus(`Looking for baked texture for ${currentScene}...`);
  const res = await fetch(apiPath(`/api/mesh/assets?scene=${encodeURIComponent(currentScene)}&iteration=${currentIteration}`));
  const assets = await res.json();
  if (!res.ok) throw new Error(assets.error || "Could not list texture assets");
  const texture = assets.meshes?.[meshModeSelect.value]?.texture;
  if (!texture?.files?.obj?.exists || !texture?.files?.png?.exists) {
    setStatus(`No baked ${meshModeSelect.value} texture found. Bake Texture first.`);
    return;
  }
  lastTextureDownloadUrl = texture.files.zip?.url || null;
  lastGlbDownloadUrl = texture.files.glb?.exists ? texture.files.glb.url : null;
  const objSize = Number(texture.files.obj?.size || 0);
  if (objSize > TEXTURED_OBJ_INLINE_LOAD_LIMIT) {
    if (texture.chunk_source_exists && texture.chunks_url) {
      await loadChunkedTexturedMeshFromManifest(
        texture.chunks_url,
        texture.files.png.url,
        `${meshModeSelect.value} large textured mesh`,
        { originalSize: objSize }
      );
      return;
    }
    setMeshBusy(false);
    setStatus(`Texture preview skipped: textured OBJ is ${formatByteSize(objSize)} and no chunk source was found. Download Texture instead.`);
    return;
  }
  await loadTexturedMeshFromUrls(texture.files.obj.url, texture.files.png.url, `${meshModeSelect.value} textured mesh`);
  setMeshBusy(false);
}

async function loadCurrentMesh({ forceOriginal = false } = {}) {
  if (!currentScene || !currentIteration) {
    setStatus(selectedMeshModeUsesSugar() ? "Load a 3DGS scene first." : "Load a 2DGS scene first.");
    return;
  }
  if (!selectedMeshModeMatchesBackend()) {
    setStatus(selectedMeshModeUsesSugar() ? "SuGaR mesh view is only available for 3DGS scenes." : "Mesh view is only available for 2DGS scenes.");
    return;
  }
  if (shouldKeepUnsavedMeshTrim(meshDirty, meshData)) {
    rebuildMeshObject();
    setMeshBusy(false);
    setStatus("Mesh trim edits are still unsaved. Save trimmed mesh or Undo before reloading the original mesh.");
    return;
  }
  try {
    setStatus(`Looking for ${meshModeSelect.value} mesh for ${currentScene}...`);
    const res = await fetch(apiPath(`/api/mesh/assets?scene=${encodeURIComponent(currentScene)}&iteration=${currentIteration}`));
    const assets = await res.json();
    if (!res.ok) throw new Error(assets.error || "Could not list meshes");
    const modeAssets = assets.meshes?.[meshModeSelect.value];
    const mesh = modeAssets?.post?.exists ? modeAssets.post : modeAssets?.raw?.exists ? modeAssets.raw : null;
    if (!mesh) {
      setStatus(`No ${meshModeSelect.value} mesh found. Export Mesh first.`);
      return;
    }
    lastMeshDownloadUrl = mesh.url;
    lastTextureDownloadUrl = modeAssets?.texture?.files?.zip?.exists ? modeAssets.texture.files.zip.url : null;
    lastGlbDownloadUrl = modeAssets?.texture?.files?.glb?.exists ? modeAssets.texture.files.glb.url : null;
    const useChunks = !forceOriginal && mesh.chunks_url && mesh.size > MESH_PREVIEW_SIZE_THRESHOLD;
    if (useChunks) {
      try {
        await loadChunkedMeshFromManifest(mesh.chunks_url, `${meshModeSelect.value} large mesh`, {
          originalUrl: mesh.url,
          originalSize: mesh.size,
        });
      } catch (chunkErr) {
        const usePreview = !meshTrimToggle?.checked && mesh.preview_url;
        if (!usePreview) throw chunkErr;
        setStatus(`Chunked mesh load failed (${chunkErr.message}). Falling back to preview...`);
        await loadMeshFromUrl(mesh.preview_url, `${meshModeSelect.value} preview mesh`, {
          isPreview: true,
          originalUrl: mesh.url,
          originalSize: mesh.size,
        });
      }
    } else {
      await loadMeshFromUrl(mesh.url, `${meshModeSelect.value} mesh`, {
        originalUrl: mesh.url,
        originalSize: mesh.size,
      });
    }
    setMeshBusy(false);
  } catch (err) {
    if (forceOriginal && meshTrimToggle) meshTrimToggle.checked = false;
    setStatus(`Mesh load failed: ${err.message}`);
  }
}

async function loadTexturedMeshFromUrls(objUrl, textureUrl, label = "textured mesh") {
  setStatus(`Loading ${label}...`);
  const objRes = await fetch(apiPath(objUrl));
  if (!objRes.ok) {
    const err = await objRes.json().catch(() => ({}));
    throw new Error(err.error || "Textured OBJ file not found");
  }
  const parsed = parseObjTexturedMesh(await objRes.text());
  const texture = await new THREE.TextureLoader().loadAsync(apiPath(textureUrl));
  configureOpenMvsTexture(texture, renderer, THREE);

  disposeObject(meshObject);
  disposeObject(meshSelectionPoints);
  meshObject = null;
  meshSelectionPoints = null;
  meshData = {
    ...parsed,
    hasTextureMap: true,
  };
  meshSelected = new Set();
  meshDeletedFaces = new Set();
  meshUndoStack = [];
  meshDirty = false;
  currentMeshTexture = texture;
  currentMeshUrl = objUrl;
  if (meshTextureToggle) {
    meshTextureToggle.checked = true;
    meshTextureToggle.disabled = true;
  }

  rebuildMeshObject();
  if (showMeshToggle) showMeshToggle.checked = true;
  document.getElementById("showPoints").checked = false;
  document.getElementById("showSplats").checked = false;
  updateLayerVisibility();
  setMeshBusy(false);
  setStatus(`${label} loaded: ${parsed.vertexCount.toLocaleString()} vertices, ${parsed.faceCount.toLocaleString()} triangles with photo texture.`);
}

async function loadChunkedTexturedMeshFromManifest(manifestUrl, textureUrl, label = "large textured mesh", options = {}) {
  setStatus(`Preparing ${label} chunks...`);
  const manifestRes = await fetch(apiPath(manifestUrl));
  const manifest = await manifestRes.json();
  if (!manifestRes.ok) throw new Error(manifest.error || "Could not prepare textured mesh chunks");
  setStatus(`Loading ${label}: manifest ready, ${manifest.chunks.length} chunks...`);

  const texture = await new THREE.TextureLoader().loadAsync(apiPath(textureUrl || manifest.texture_url));
  configureOpenMvsTexture(texture, renderer, THREE);

  disposeObject(meshObject);
  disposeObject(meshSelectionPoints);
  meshObject = new THREE.Group();
  scene.add(meshObject);
  meshSelectionPoints = null;
  meshData = {
    isChunked: true,
    chunks: [],
    vertexCount: Number(manifest.vertex_count || 0),
    faceCount: Number(manifest.face_count || 0),
    hasTextureMap: true,
    originalSize: Number(options.originalSize || 0),
  };
  currentMeshTexture = texture;
  meshSelected = new Set();
  meshDeletedFaces = new Set();
  meshUndoStack = [];
  meshDirty = false;
  currentMeshUrl = manifestUrl;
  if (showMeshToggle) showMeshToggle.checked = true;
  if (meshTextureToggle) {
    meshTextureToggle.checked = true;
    meshTextureToggle.disabled = true;
  }
  if (meshTrimToggle?.checked) {
    meshTrimToggle.checked = false;
    setMode("navigate");
  }
  document.getElementById("showPoints").checked = false;
  document.getElementById("showSplats").checked = false;

  for (let i = 0; i < manifest.chunks.length; i++) {
    const chunk = manifest.chunks[i];
    setStatus(`Loading ${label}: fetching chunk ${i + 1}/${manifest.chunks.length}...`);
    const chunkRes = await fetch(apiPath(chunk.url));
    if (!chunkRes.ok) {
      const err = await chunkRes.json().catch(() => ({}));
      throw new Error(err.error || `Textured mesh chunk ${i + 1} failed`);
    }
    const parsed = parseMeshChunk(await chunkRes.arrayBuffer());
    const mesh = texturedMeshFromChunkData(parsed, texture);
    meshObject.add(mesh);
    setStatus(`Loading ${label}: chunk ${i + 1}/${manifest.chunks.length} (${Number(manifest.face_count || 0).toLocaleString()} triangles)...`);
    await new Promise((resolve) => requestAnimationFrame(resolve));
  }

  updateLayerVisibility();
  setMeshBusy(false);
  setStatus(`${label} loaded in ${manifest.chunks.length} chunks: ${Number(manifest.vertex_count || 0).toLocaleString()} source vertices, ${Number(manifest.face_count || 0).toLocaleString()} triangles with photo texture.`);
}

async function loadChunkedMeshFromManifest(manifestUrl, label = "large mesh", options = {}) {
  setStatus(`Preparing ${label} chunks...`);
  const manifestRes = await fetch(apiPath(manifestUrl));
  const manifest = await manifestRes.json();
  if (!manifestRes.ok) throw new Error(manifest.error || "Could not prepare mesh chunks");
  setStatus(`Loading ${label}: manifest ready, ${manifest.chunks.length} chunks...`);

  disposeObject(meshObject);
  disposeObject(meshSelectionPoints);
  meshObject = new THREE.Group();
  scene.add(meshObject);
  meshSelectionPoints = null;
  meshData = {
    isChunked: true,
    chunks: [],
    vertexCount: Number(manifest.vertex_count || 0),
    faceCount: Number(manifest.face_count || 0),
    hasVertexColors: Boolean(manifest.has_vertex_colors),
    originalUrl: options.originalUrl || manifestUrl,
    originalSize: Number(options.originalSize || 0),
  };
  currentMeshTexture = null;
  meshSelected = new Set();
  meshDeletedFaces = new Set();
  meshUndoStack = [];
  meshDirty = false;
  currentMeshUrl = manifestUrl;
  if (showMeshToggle) showMeshToggle.checked = true;
  if (meshTextureToggle) {
    meshTextureToggle.checked = false;
    meshTextureToggle.disabled = !meshData.hasVertexColors;
  }
  document.getElementById("showPoints").checked = false;
  document.getElementById("showSplats").checked = false;

  for (let i = 0; i < manifest.chunks.length; i++) {
    const chunk = manifest.chunks[i];
    setStatus(`Loading ${label}: fetching chunk ${i + 1}/${manifest.chunks.length}...`);
    const chunkRes = await fetch(apiPath(chunk.url));
    if (!chunkRes.ok) {
      const err = await chunkRes.json().catch(() => ({}));
      throw new Error(err.error || `Mesh chunk ${i + 1} failed`);
    }
    const parsed = parseMeshChunk(await chunkRes.arrayBuffer());
    meshData.chunks.push(parsed);
    meshObject.add(meshFromChunkData(parsed, false));
    if (i === 0 || (i + 1) % 4 === 0 || i + 1 === manifest.chunks.length) {
      setStatus(`Loading ${label}: ${i + 1}/${manifest.chunks.length} chunks...`);
      await new Promise((resolve) => requestAnimationFrame(resolve));
    }
  }
  updateLayerVisibility();
  setMeshBusy(false);
  setStatus(`${label} loaded in ${manifest.chunks.length.toLocaleString()} chunks: ${meshData.vertexCount.toLocaleString()} source vertices, ${meshData.faceCount.toLocaleString()} triangles. Trim is disabled for chunked view.`);
}

async function loadMeshFromUrl(url, label = "mesh", options = {}) {
  setStatus(`Loading ${label}...`);
  const res = await fetch(apiPath(url));
  if (!res.ok) {
    const err = await res.json().catch(() => ({}));
    throw new Error(err.error || "Mesh file not found");
  }
  const parsed = parsePlyMesh(await res.arrayBuffer());
  disposeObject(meshObject);
  disposeObject(meshSelectionPoints);
  meshObject = null;
  meshSelectionPoints = null;
  meshData = parsed;
  meshData.isPreview = Boolean(options.isPreview);
  meshData.originalUrl = options.originalUrl || url;
  meshData.originalSize = Number(options.originalSize || 0);
  currentMeshTexture = null;
  meshSelected = new Set();
  meshDeletedFaces = new Set();
  meshUndoStack = [];
  meshDirty = false;
  currentMeshUrl = url;
  if (showMeshToggle) showMeshToggle.checked = true;
  if (meshTextureToggle) {
    meshTextureToggle.checked = Boolean(parsed.hasVertexColors);
    meshTextureToggle.disabled = !parsed.hasVertexColors;
  }
  rebuildMeshObject();
  document.getElementById("showPoints").checked = false;
  document.getElementById("showSplats").checked = false;
  updateLayerVisibility();
  const count = parsed.indices?.length ? parsed.indices.length / 3 : parsed.faceCount;
  const textureText = parsed.hasVertexColors ? " with vertex colors" : "";
  const previewText = meshData.isPreview ? ` Preview only; trim requires loading the full mesh (${meshData.originalSize.toLocaleString()} bytes).` : "";
  setStatus(`${label} loaded${textureText}: ${parsed.vertexCount.toLocaleString()} vertices, ${count.toLocaleString()} triangles.${previewText}`);
  setMeshBusy(false);
}

function meshGeometryFromData(data, options = {}) {
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(data.positions, 3));
  if (data.uvs?.length === (data.positions.length / 3) * 2) {
    geometry.setAttribute("uv", new THREE.BufferAttribute(data.uvs, 2));
  }
  if (data.hasVertexColors && data.colors?.length === data.positions.length) {
    geometry.setAttribute("color", new THREE.BufferAttribute(data.colors, 3, true));
  }
  if (data.indices?.length) geometry.setIndex(new THREE.BufferAttribute(data.indices, 1));
  const hasNormals = data.normals?.some?.((value) => value !== 0);
  if (hasNormals) geometry.setAttribute("normal", new THREE.BufferAttribute(data.normals, 3));
  else if (options.computeNormals !== false) geometry.computeVertexNormals();
  geometry.computeBoundingBox();
  return geometry;
}

function meshMaterialForData(useVertexColors) {
  return new THREE.MeshPhongMaterial({
    color: useVertexColors ? 0xffffff : 0xd6d6d6,
    specular: 0x333333,
    shininess: useVertexColors ? 12 : 24,
    vertexColors: useVertexColors,
    flatShading: false,
    side: THREE.DoubleSide,
    depthWrite: true,
  });
}

function meshMaterialForTexture(texture) {
  return new THREE.MeshBasicMaterial({
    map: texture,
    color: 0xffffff,
    side: THREE.DoubleSide,
    depthWrite: true,
  });
}

function meshMaterialForChunkData(useVertexColors) {
  return new THREE.ShaderMaterial({
    defines: useVertexColors ? { USE_VERTEX_COLOR: 1 } : {},
    extensions: { derivatives: true },
    side: THREE.DoubleSide,
    depthWrite: true,
    vertexShader: `
      varying vec3 vViewPosition;
      #ifdef USE_VERTEX_COLOR
      attribute vec3 color;
      varying vec3 vColor;
      #endif
      void main() {
        vec4 mvPosition = modelViewMatrix * vec4(position, 1.0);
        vViewPosition = mvPosition.xyz;
        #ifdef USE_VERTEX_COLOR
        vColor = color;
        #endif
        gl_Position = projectionMatrix * mvPosition;
      }
    `,
    fragmentShader: `
      varying vec3 vViewPosition;
      #ifdef USE_VERTEX_COLOR
      varying vec3 vColor;
      #endif
      void main() {
        vec3 dx = dFdx(vViewPosition);
        vec3 dy = dFdy(vViewPosition);
        vec3 normal = normalize(cross(dx, dy));
        if (!gl_FrontFacing) normal = -normal;
        vec3 keyLight = normalize(vec3(0.32, 0.48, 0.82));
        vec3 fillLight = normalize(vec3(-0.60, -0.24, 0.72));
        float key = max(dot(normal, keyLight), 0.0);
        float fill = max(dot(normal, fillLight), 0.0) * 0.16;
        float shade = clamp(0.34 + key * 0.58 + fill, 0.0, 1.0);
        #ifdef USE_VERTEX_COLOR
        vec3 baseColor = vColor;
        #else
        vec3 baseColor = vec3(0.74);
        #endif
        gl_FragColor = vec4(baseColor * shade, 1.0);
      }
    `,
  });
}

function meshFromChunkData(chunk, useVertexColors) {
  const geometry = meshGeometryFromData(chunk, { computeNormals: false });
  return new THREE.Mesh(geometry, meshMaterialForChunkData(useVertexColors && chunk.hasVertexColors));
}

function texturedMeshFromChunkData(chunk, texture) {
  const geometry = meshGeometryFromData(chunk, { computeNormals: false });
  return new THREE.Mesh(geometry, meshMaterialForTexture(texture));
}

function rebuildMeshObject() {
  disposeObject(meshObject);
  meshObject = null;
  if (!meshData) return;
  if (meshData.isChunked) {
    const group = new THREE.Group();
    const imageTextured = meshData.hasTextureMap && currentMeshTexture;
    const useVertexColors = !imageTextured && meshTextureEnabled() && meshData.hasVertexColors;
    for (const chunk of meshData.chunks || []) {
      group.add(imageTextured ? texturedMeshFromChunkData(chunk, currentMeshTexture) : meshFromChunkData(chunk, useVertexColors));
    }
    scene.add(group);
    meshObject = group;
    updateLayerVisibility();
    return;
  }
  const visibleMesh = compactMeshByDeletedFaces(meshData, meshDeletedFaces);
  const geometry = meshGeometryFromData(visibleMesh);
  const group = new THREE.Group();
  const vertexTextured = meshTextureEnabled() && visibleMesh.hasVertexColors && visibleMesh.colors?.length === visibleMesh.positions.length;
  const imageTextured = meshTextureEnabled() && currentMeshTexture && visibleMesh.uvs?.length === (visibleMesh.positions.length / 3) * 2;
  const material = imageTextured
    ? new THREE.MeshBasicMaterial({
      map: currentMeshTexture,
      color: 0xffffff,
      side: THREE.DoubleSide,
      depthWrite: true,
    })
    : new THREE.MeshPhongMaterial({
      color: vertexTextured ? 0xffffff : 0xd6d6d6,
      specular: 0x333333,
      shininess: vertexTextured ? 12 : 22,
      vertexColors: vertexTextured,
      flatShading: true,
      side: THREE.DoubleSide,
      depthWrite: true,
    });
  group.add(new THREE.Mesh(geometry, material));
  group.add(new THREE.LineSegments(
    new THREE.WireframeGeometry(geometry),
    new THREE.LineBasicMaterial({ color: 0xffffff, transparent: true, opacity: (vertexTextured || imageTextured) ? 0.015 : 0.03 })
  ));
  scene.add(group);
  meshObject = group;
  buildMeshSelectionCloud();
  updateLayerVisibility();
}

async function loadCurrent() {
  if (!threeReady) {
    setStatus("3D viewport is not ready. Restart the editor, or use Import + Train first.");
    return;
  }
  resetInteractionState();
  setMode("navigate");
  currentScene = sceneSelect.value;
  currentIteration = Number(iterationInput.value);
  currentBackend = selectedSceneBackend();
  lastMeshDownloadUrl = null;
  lastTextureDownloadUrl = null;
  lastGlbDownloadUrl = null;
  currentMeshUrl = null;
  currentMeshTexture = null;
  disposeObject(meshObject);
  disposeObject(meshSelectionPoints);
  meshObject = null;
  meshSelectionPoints = null;
  meshData = null;
  meshSelected.clear();
  meshDeletedFaces.clear();
  meshUndoStack = [];
  meshDirty = false;
  if (meshTextureToggle) {
    meshTextureToggle.checked = false;
    meshTextureToggle.disabled = true;
  }
  setMeshBusy(false);
  setStatus(`Loading ${currentScene} [${activeRendererLabel()}] iteration ${currentIteration}...`);
  const res = await fetch(apiPath(`/api/points?scene=${encodeURIComponent(currentScene)}&iteration=${currentIteration}`));
  if (!res.ok) {
    const err = await res.json();
    throw new Error(err.error || "Failed to load points");
  }
  const buf = await res.arrayBuffer();
  const view = new DataView(buf);
  const headerLen = view.getUint32(0, true);
  const header = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 4, headerLen)));
  currentBackend = header.backend || currentBackend;
  if (psnrBackendSelect) psnrBackendSelect.value = currentBackend || "3dgs";
  setPsnrBusy(false);
  const n = header.count;
  const xyzOffset = 4 + headerLen;
  const rgbOffset = xyzOffset + n * 3 * 4;
  const opacityOffset = rgbOffset + n * 3;
  const scaleOffset = opacityOffset + n * 4;
  positions = new Float32Array(buf.slice(xyzOffset, rgbOffset));
  colors = new Uint8Array(buf.slice(rgbOffset, opacityOffset));
  opacities = new Float32Array(buf.slice(opacityOffset, scaleOffset));
  splatScales = new Float32Array(buf.slice(scaleOffset, scaleOffset + n * 4));
  indexMap = new Int32Array(n);
  for (let i = 0; i < n; i++) indexMap[i] = i;
  deletedOriginal.clear();
  selected.clear();
  undoStack = [];
  hasUnsavedEdits = false;
  editRevision++;
  realPreviewKey = null;
  realMask = null;
  realIndexMaskInstalled = false;
  document.getElementById("showRealSplats").checked = false;
  await loadCameras(currentScene);
  buildRenderLayers();
  realSplatReady = false;
  if (realSplatViewer?.splatMesh) realSplatViewer.splatMesh.visible = false;
  currentBounds = header.view_bounds || header.bounds;
  updateCenterGizmo(currentBounds);
  frameBounds(currentBounds);
  setMeshBusy(false);
  setStatus(`${currentScene} [${activeRendererLabel()}]: ${n.toLocaleString()} Gaussians loaded. Toggle Points/Splats/Cameras as needed.`);
}

async function onRealSplatsToggle() {
  const checked = document.getElementById("showRealSplats").checked;
  if (checked && !realSplatReady && currentScene && currentIteration) {
    setStatus(`Loading real ${activeRendererLabel()} renderer for ${currentScene}...`);
    const ok = await loadRealSplatScene(currentScene, currentIteration);
    if (ok) {
      applyRealEditMask();
      setStatus(`${currentScene}: real ${activeRendererLabel()} renderer ready (${lastRealSplatCount.toLocaleString()}/${lastRealPointCount.toLocaleString()}). Delete/Undo updates without reload.`);
    }
  }
  updateLayerVisibility();
}

async function previewRealNow() {
  if (!currentScene || !currentIteration) {
    setStatus("Load a scene first.");
    return;
  }
  const realToggle = document.getElementById("showRealSplats");
  if (realToggle) realToggle.checked = true;
  setStatus(`Reloading real ${activeRendererLabel()} renderer for ${currentScene}...`);
  const ok = await loadRealSplatScene(currentScene, currentIteration, true);
  if (ok) updateLayerVisibility();
}

function invalidateRealPreview(reason) {
  realSplatReady = false;
  realPreviewKey = null;
  realMask = null;
  const realToggle = document.getElementById("showRealSplats");
  if (realToggle) realToggle.checked = false;
  if (realSplatViewer?.splatMesh) realSplatViewer.splatMesh.visible = false;
  updateLayerVisibility();
  if (reason) setStatus(reason);
}

function refreshRealAfterEdit(reason) {
  if (realSplatReady && realSplatViewer?.splatMesh) {
    applyRealEditMask();
    setStatus(`${reason} Real ${activeRendererLabel()} render list updated without reload.`);
    updateLayerVisibility();
    return;
  }
  setStatus(`${reason} Live edit preview is updated. Enable Real to use realtime 3DGS masking.`);
  const pointsToggle = document.getElementById("showPoints");
  const softToggle = document.getElementById("showSplats");
  if (softToggle && !softToggle.checked) softToggle.checked = true;
  if (pointsToggle && !pointsToggle.checked && !softToggle?.checked) pointsToggle.checked = true;
  updateLayerVisibility();
}

async function loadCameras(sceneName) {
  const res = await fetch(apiPath(`/api/cameras?scene=${encodeURIComponent(sceneName)}`));
  if (!res.ok) return;
  const data = await res.json();
  buildCameraGroup(data.cameras || []);
}

function disposeObject(object) {
  if (!object) return;
  scene.remove(object);
  object.traverse?.((child) => {
    child.geometry?.dispose?.();
    if (Array.isArray(child.material)) {
      child.material.forEach((material) => {
        material.map?.dispose?.();
        material.dispose?.();
      });
    } else {
      child.material?.map?.dispose?.();
      child.material?.dispose?.();
    }
  });
  object.geometry?.dispose?.();
  if (Array.isArray(object.material)) {
    object.material.forEach((material) => {
      material.map?.dispose?.();
      material.dispose?.();
    });
  } else {
    object.material?.map?.dispose?.();
    object.material?.dispose?.();
  }
}

function buildRenderLayers() {
  disposeObject(pointCloud);
  disposeObject(splatCloud);
  disposePickingResources();
  pointCloud = buildPointCloud();
  splatCloud = buildSplatCloud();
  scene.add(pointCloud);
  scene.add(splatCloud);
  updateLayerVisibility();
  buildSelectionCloud();
}

function createCircleGeometry(plane) {
  const points = [];
  const segments = 160;
  for (let i = 0; i <= segments; i++) {
    const t = (i / segments) * Math.PI * 2;
    const c = Math.cos(t);
    const s = Math.sin(t);
    if (plane === "xy") points.push(c, s, 0);
    else if (plane === "yz") points.push(0, c, s);
    else points.push(c, 0, s);
  }
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.Float32BufferAttribute(points, 3));
  return geometry;
}

function createCenterGizmo() {
  const group = new THREE.Group();
  group.visible = false;
  group.renderOrder = 80;
  const sphere = new THREE.Mesh(
    new THREE.SphereGeometry(1, 48, 24),
    new THREE.MeshStandardMaterial({
      color: 0xffffff,
      transparent: true,
      opacity: 0.32,
      roughness: 0.82,
      metalness: 0.0,
      depthWrite: false,
      depthTest: false
    })
  );
  sphere.renderOrder = 80;
  group.add(sphere);
  const lineOptions = { transparent: true, opacity: 0.72, depthWrite: false, depthTest: false };
  const rings = [
    ["xy", 0xff6f6f],
    ["yz", 0x76e08a],
    ["xz", 0x7895ff]
  ];
  for (const ringSpec of rings) {
    const plane = ringSpec[0];
    const color = ringSpec[1];
    const ring = new THREE.Line(
      createCircleGeometry(plane),
      new THREE.LineBasicMaterial({
        color,
        transparent: lineOptions.transparent,
        opacity: lineOptions.opacity,
        depthWrite: lineOptions.depthWrite,
        depthTest: lineOptions.depthTest
      })
    );
    ring.renderOrder = 81;
    group.add(ring);
  }
  return group;
}

function updateCenterGizmo(bounds) {
  if (!centerGizmo) return;
  const metrics = centerGizmoMetrics(bounds);
  centerGizmo.userData.hasPivot = metrics.visible;
  centerGizmo.position.set(metrics.center[0], metrics.center[1], metrics.center[2]);
  centerGizmoRadius = metrics.radius;
  syncCenterGizmoToTarget();
  updateLayerVisibility();
}

function syncCenterGizmoToTarget() {
  if (!centerGizmo || !controls || !centerGizmo.userData.hasPivot) return;
  centerGizmo.position.copy(controls.target);
  centerGizmo.scale.setScalar(screenStableGizmoScale({
    distance: camera.position.distanceTo(controls.target),
    fovDeg: camera.fov,
    viewportHeight: canvas.clientHeight,
    pixelRadius: CENTER_GIZMO_SCREEN_RADIUS_PX,
    fallback: centerGizmoRadius
  }));
}

async function ensureRealSplatViewer() {
  if (realSplatViewer) return realSplatViewer;
  realSplatViewer = new GaussianSplats3D.Viewer({
    selfDrivenMode: false,
    renderer,
    camera,
    useBuiltInControls: false,
    threeScene: scene,
    ignoreDevicePixelRatio: false,
    gpuAcceleratedSort: false,
    enableSIMDInSort: false,
    sharedMemoryForWorkers: false,
    integerBasedSort: false,
    halfPrecisionCovariancesOnGPU: true,
    dynamicScene: false,
    renderMode: GaussianSplats3D.RenderMode.Always,
    sceneRevealMode: GaussianSplats3D.SceneRevealMode.Instant,
    logLevel: GaussianSplats3D.LogLevel.None,
    sphericalHarmonicsDegree: 0,
    optimizeSplatData: false,
  });
  return realSplatViewer;
}

async function loadRealSplatScene(sceneName, iteration, forceReload = false) {
  if (realSplatReady && !forceReload && realPreviewKey === `${sceneName}|${iteration}`) return true;
  const url = apiPath(`/api/ply?scene=${encodeURIComponent(sceneName)}&iteration=${iteration}`);
  return loadRealSplatSceneUrl(url, `${sceneName}|${iteration}`);
}

async function loadRealSplatSceneUrl(url, cacheKey = null) {
  realSplatLoading = true;
  realSplatReady = false;
  realMask = null;
  realIndexMaskInstalled = false;
  updateLayerVisibility();
  try {
    const viewer = await ensureRealSplatViewer();
    if (realSplatSceneIndex !== null) {
      await viewer.removeSplatScene(realSplatSceneIndex, false);
      realSplatSceneIndex = null;
    }
    await viewer.addSplatScene(url, {
      format: GaussianSplats3D.SceneFormat.Ply,
      splatAlphaRemovalThreshold: 0,
      showLoadingUI: false,
      progressiveLoad: false,
      halfPrecisionCovariancesOnGPU: true,
      optimizeSplatData: false,
    });
    realSplatSceneIndex = 0;
    realSplatReady = true;
    realPreviewKey = cacheKey;
    applyRealEditMask();
    const realCount = realSplatViewer?.splatMesh?.getSplatCount?.() || 0;
    const pointCount = positions ? positions.length / 3 : 0;
    lastRealSplatCount = realCount;
    lastRealPointCount = pointCount;
    window.__3dgsDebug = { realCount, pointCount, deleted: deletedOriginal.size };
    if (realCount !== pointCount) {
      setStatus(`Real index warning: points=${pointCount.toLocaleString()}, real=${realCount.toLocaleString()}. Realtime crop may be offset.`);
    }
    return true;
  } catch (err) {
    console.error(err);
    setStatus(`Real ${activeRendererLabel()} load failed: ${err.message}`);
    document.getElementById("showRealSplats").checked = false;
    return false;
  } finally {
    realSplatLoading = false;
    updateLayerVisibility();
  }
}

function installRealEditMask() {
  const splatMesh = realSplatViewer?.splatMesh;
  const material = splatMesh?.material;
  if (!splatMesh || !material) return false;
  const count = splatMesh.getSplatCount?.() || 0;
  if (!count) return false;

  const width = 4096;
  const height = Math.max(1, Math.ceil(count / width));
  const data = new Uint8Array(width * height);
  data.fill(255);
  const texture = new THREE.DataTexture(data, width, height, THREE.RedFormat, THREE.UnsignedByteType);
  texture.minFilter = THREE.NearestFilter;
  texture.magFilter = THREE.NearestFilter;
  texture.generateMipmaps = false;
  texture.unpackAlignment = 1;
  texture.needsUpdate = true;

  material.uniforms.editMaskTexture = { value: texture };
  material.uniforms.editMaskTextureSize = { value: new THREE.Vector2(width, height) };
  material.uniforms.editMaskEnabled = { value: 1 };
  if (!material.userData.editMaskInjected) {
    material.vertexShader = material.vertexShader.replace(
      "uniform int sceneCount;",
      `uniform int sceneCount;
        uniform sampler2D editMaskTexture;
        uniform vec2 editMaskTextureSize;
        uniform int editMaskEnabled;`
    );
    material.vertexShader = material.vertexShader.replace(
      "void main () {",
      `vec2 getEditMaskUV() {
            float x = mod(float(splatIndex), editMaskTextureSize.x);
            float y = floor(float(splatIndex) / editMaskTextureSize.x);
            return (vec2(x, y) + vec2(0.5)) / editMaskTextureSize;
        }

        void main () {`
    );
    material.vertexShader = material.vertexShader.replace(
      "uint oddOffset = splatIndex & uint(0x00000001);",
      `if (editMaskEnabled == 1 && texture(editMaskTexture, getEditMaskUV()).r < 0.5) {
                gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
                return;
            }

            uint oddOffset = splatIndex & uint(0x00000001);`
    );
    material.userData.editMaskInjected = true;
    material.needsUpdate = true;
  }

  realMask = { data, texture, width, height, count };
  return true;
}

function applyRealEditMask() {
  if (!realSplatReady || !realSplatViewer?.splatMesh) return false;
  installRealIndexMask();
  applyRealIndexMask();
  if (!realMask && !installRealEditMask()) return true;
  realMask.data.fill(255);
  for (const id of deletedOriginal) {
    if (id >= 0 && id < realMask.count) realMask.data[id] = 0;
  }
  realMask.texture.needsUpdate = true;
  const material = realSplatViewer.splatMesh.material;
  if (material?.uniforms?.editMaskEnabled) material.uniforms.editMaskEnabled.value = 1;
  return true;
}

function installRealIndexMask() {
  const splatTree = realSplatViewer?.splatMesh?.getSplatTree?.();
  if (!splatTree) return false;
  if (realIndexMaskInstalled) return true;
  for (const subTree of splatTree.subTrees || []) {
    for (const node of subTree.nodesWithIndexes || []) {
      if (!node.data?.indexes) continue;
      if (!node.data._codexOriginalIndexes) node.data._codexOriginalIndexes = node.data.indexes;
    }
  }
  realIndexMaskInstalled = true;
  return true;
}

function applyRealIndexMask() {
  const viewer = realSplatViewer;
  const splatTree = viewer?.splatMesh?.getSplatTree?.();
  if (!splatTree) return false;
  const hasDeletes = deletedOriginal.size > 0;
  for (const subTree of splatTree.subTrees || []) {
    for (const node of subTree.nodesWithIndexes || []) {
      const original = node.data?._codexOriginalIndexes || node.data?.indexes;
      if (!node.data || !original) continue;
      if (!hasDeletes) {
        node.data.indexes = original;
        continue;
      }
      const kept = [];
      for (let i = 0; i < original.length; i++) {
        const id = original[i];
        if (!deletedOriginal.has(id)) kept.push(id);
      }
      node.data.indexes = new Uint32Array(kept);
    }
  }
  viewer.splatRenderCount = Math.max(0, (viewer.splatMesh.getSplatCount?.() || 0) - deletedOriginal.size);
  applyImmediateRealRenderIndexes();
  viewer.runSplatSort?.(true, true);
  viewer.forceRenderNextFrame?.();
  window.__3dgsDebug = {
    realCount: viewer.splatMesh.getSplatCount?.() || 0,
    pointCount: positions ? positions.length / 3 : 0,
    deleted: deletedOriginal.size,
    renderCount: viewer.splatRenderCount,
  };
  return true;
}

function applyImmediateRealRenderIndexes() {
  const splatMesh = realSplatViewer?.splatMesh;
  if (!splatMesh?.updateRenderIndexes) return false;
  const count = splatMesh.getSplatCount?.() || 0;
  const renderCount = Math.max(0, count - deletedOriginal.size);
  const renderIndexes = new Uint32Array(renderCount);
  let w = 0;
  for (let i = 0; i < count; i++) {
    if (!deletedOriginal.has(i)) renderIndexes[w++] = i;
  }
  splatMesh.updateRenderIndexes(renderIndexes, w);
  realSplatViewer.splatRenderCount = w;
  return true;
}

function makeColorFloat() {
  const colorFloat = new Float32Array(colors.length);
  for (let i = 0; i < colors.length; i++) colorFloat[i] = colors[i] / 255;
  return colorFloat;
}

function buildPointCloud() {
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  geometry.setAttribute("color", new THREE.BufferAttribute(makeColorFloat(), 3));
  const pointSize = Number(document.getElementById("pointSize")?.value || 1);
  const material = new THREE.PointsMaterial({ size: pointSize, vertexColors: true, sizeAttenuation: false });
  return new THREE.Points(geometry, material);
}

function disposePickingResources({ target = false } = {}) {
  if (pickingCloud) {
    pickingScene?.remove(pickingCloud);
    pickingCloud.geometry?.dispose?.();
    pickingCloud.material?.dispose?.();
    pickingCloud = null;
  }
  pickingRevision = -1;
  pickingRenderCacheKey = "";
  if (target && pickingTarget) {
    pickingTarget.dispose();
    pickingTarget = null;
    pickingTargetKey = "";
  }
}

function makePickingColorFloat(count) {
  const pickingColors = new Float32Array(count * 3);
  for (let i = 0; i < count; i++) {
    const [r, g, b] = encodePickingId(i);
    pickingColors[i * 3] = r / 255;
    pickingColors[i * 3 + 1] = g / 255;
    pickingColors[i * 3 + 2] = b / 255;
  }
  return pickingColors;
}

function buildPickingCloud() {
  if (!positions || !positions.length || !pickingScene) return null;
  const count = positions.length / 3;
  if (count > 0xffffff - 1) {
    setStatus("Visible selection ID pass supports up to 16,777,214 Gaussians. Falling back to depth map.");
    return null;
  }
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  geometry.setAttribute("pickingColor", new THREE.BufferAttribute(makePickingColorFloat(count), 3));
  geometry.setAttribute("splatScale", new THREE.BufferAttribute(splatScales || new Float32Array(count).fill(0.01), 1));
  const material = new THREE.ShaderMaterial({
    depthTest: true,
    depthWrite: true,
    transparent: false,
    toneMapped: false,
    uniforms: {
      pointScale: { value: 650.0 },
      minSize: { value: 2.0 },
      maxSize: { value: 72.0 },
    },
    vertexShader: `
      attribute vec3 pickingColor;
      attribute float splatScale;
      varying vec3 vPickingColor;
      uniform float pointScale;
      uniform float minSize;
      uniform float maxSize;
      void main() {
        vPickingColor = pickingColor;
        vec4 mvPosition = modelViewMatrix * vec4(position, 1.0);
        gl_Position = projectionMatrix * mvPosition;
        float size = pointScale * max(splatScale, 0.0001) / max(-mvPosition.z, 0.001);
        gl_PointSize = clamp(size, minSize, maxSize);
      }
    `,
    fragmentShader: `
      varying vec3 vPickingColor;
      void main() {
        vec2 p = gl_PointCoord * 2.0 - 1.0;
        if (dot(p, p) > 1.0) discard;
        gl_FragColor = vec4(vPickingColor, 1.0);
      }
    `,
  });
  const object = new THREE.Points(geometry, material);
  object.frustumCulled = false;
  pickingScene.add(object);
  return object;
}

function ensurePickingCloud() {
  if (!visibleSelectionEnabled() || !positions || !renderer || !camera || !pickingScene) return false;
  if (pickingCloud && pickingRevision === editRevision) return true;
  disposePickingResources();
  pickingCloud = buildPickingCloud();
  if (!pickingCloud) return false;
  pickingRevision = editRevision;
  return true;
}

function ensurePickingTarget() {
  if (!renderer || !canvas) return null;
  const ratio = Math.max(1, renderer.getPixelRatio?.() || Math.min(window.devicePixelRatio, 2));
  const width = Math.max(1, Math.ceil(canvas.clientWidth * ratio));
  const height = Math.max(1, Math.ceil(canvas.clientHeight * ratio));
  const key = `${width}x${height}`;
  if (pickingTarget && pickingTargetKey === key) return { target: pickingTarget, width, height, pixelRatio: ratio };
  if (pickingTarget) pickingTarget.dispose();
  pickingTarget = new THREE.WebGLRenderTarget(width, height, {
    depthBuffer: true,
    stencilBuffer: false,
    minFilter: THREE.NearestFilter,
    magFilter: THREE.NearestFilter,
    format: THREE.RGBAFormat,
    type: THREE.UnsignedByteType,
  });
  pickingTarget.texture.generateMipmaps = false;
  pickingTargetKey = key;
  return { target: pickingTarget, width, height, pixelRatio: ratio };
}

function currentPickingRenderKey(targetInfo) {
  camera.updateMatrixWorld();
  camera.updateProjectionMatrix();
  return pickingRenderKey({
    editRevision,
    pointCount: positions ? positions.length / 3 : 0,
    width: targetInfo.width,
    height: targetInfo.height,
    pixelRatio: targetInfo.pixelRatio,
    viewMatrix: camera.matrixWorldInverse.elements,
    projectionMatrix: camera.projectionMatrix.elements,
  });
}

function renderPickingTarget() {
  if (!ensurePickingCloud()) return null;
  const targetInfo = ensurePickingTarget();
  if (!targetInfo) return null;
  const cacheKey = currentPickingRenderKey(targetInfo);
  if (pickingRenderCacheKey === cacheKey) {
    pickingStats.cacheHits++;
    window.__pickingDebug = pickingStats;
    return targetInfo;
  }
  const previousTarget = renderer.getRenderTarget();
  const previousClearColor = renderer.getClearColor(new THREE.Color());
  const previousClearAlpha = renderer.getClearAlpha();
  renderer.setRenderTarget(targetInfo.target);
  renderer.setClearColor(0x000000, 1);
  renderer.clear(true, true, true);
  renderer.render(pickingScene, camera);
  renderer.setRenderTarget(previousTarget);
  renderer.setClearColor(previousClearColor, previousClearAlpha);
  pickingRenderCacheKey = cacheKey;
  pickingStats.renders++;
  window.__pickingDebug = pickingStats;
  return targetInfo;
}

function readVisiblePickingIds(cssBounds, shape = null) {
  if (!cssBounds) return null;
  try {
    const targetInfo = renderPickingTarget();
    if (!targetInfo) return null;
    const readRect = deviceReadRect(cssBounds, targetInfo.pixelRatio, targetInfo.height);
    const buffer = new Uint8Array(readRect.width * readRect.height * 4);
    renderer.readRenderTargetPixels(targetInfo.target, readRect.x, readRect.y, readRect.width, readRect.height, buffer);
    pickingStats.readbacks++;
    window.__pickingDebug = pickingStats;
    return collectPickingIds(buffer, readRect, cssBounds, shape);
  } catch (err) {
    console.warn("GPU picking failed; falling back to CPU visibility map.", err);
    return null;
  }
}

function updatePointSize() {
  const pointSize = Number(document.getElementById("pointSize")?.value || 1);
  if (pointCloud?.material) pointCloud.material.size = pointSize;
  if (selectionPoints?.material) selectionPoints.material.size = Math.max(pointSize * 2.2, 4);
}

function buildSplatCloud() {
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  geometry.setAttribute("color", new THREE.BufferAttribute(makeColorFloat(), 3));
  geometry.setAttribute("opacity", new THREE.BufferAttribute(opacities, 1));
  geometry.setAttribute("splatScale", new THREE.BufferAttribute(splatScales, 1));
  const material = new THREE.ShaderMaterial({
    transparent: true,
    depthWrite: false,
    vertexColors: true,
    blending: THREE.NormalBlending,
    uniforms: {
      pointScale: { value: 650.0 },
      minSize: { value: 2.0 },
      maxSize: { value: 72.0 },
    },
    vertexShader: `
      attribute float opacity;
      attribute float splatScale;
      varying vec3 vColor;
      varying float vOpacity;
      uniform float pointScale;
      uniform float minSize;
      uniform float maxSize;
      void main() {
        vColor = color;
        vOpacity = opacity;
        vec4 mvPosition = modelViewMatrix * vec4(position, 1.0);
        gl_Position = projectionMatrix * mvPosition;
        float size = pointScale * max(splatScale, 0.0001) / max(-mvPosition.z, 0.001);
        gl_PointSize = clamp(size, minSize, maxSize);
      }
    `,
    fragmentShader: `
      varying vec3 vColor;
      varying float vOpacity;
      void main() {
        vec2 p = gl_PointCoord * 2.0 - 1.0;
        float r2 = dot(p, p);
        if (r2 > 1.0) discard;
        float alpha = exp(-4.0 * r2) * vOpacity;
        gl_FragColor = vec4(vColor, alpha);
      }
    `,
  });
  return new THREE.Points(geometry, material);
}

function buildCameraGroup(cameras) {
  disposeObject(cameraGroup);
  cameraGroup = new THREE.Group();
  if (!cameras.length) {
    scene.add(cameraGroup);
    updateLayerVisibility();
    return;
  }
  const pos = cameras.map((cam) => new THREE.Vector3(...cam.position));
  const bounds = new THREE.Box3().setFromPoints(pos);
  const size = bounds.getSize(new THREE.Vector3()).length();
  const frustumSize = Math.max(size * 0.015, 0.08);
  const linePositions = [];
  const pathPositions = [];
  for (let i = 0; i < cameras.length; i++) {
    const cam = cameras[i];
    const p = new THREE.Vector3(...cam.position);
    pathPositions.push(p.x, p.y, p.z);
    const r = cam.rotation || [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
    const right = new THREE.Vector3(r[0][0], r[1][0], r[2][0]).normalize();
    const up = new THREE.Vector3(r[0][1], r[1][1], r[2][1]).normalize();
    const forward = new THREE.Vector3(r[0][2], r[1][2], r[2][2]).normalize();
    const center = p.clone().add(forward.multiplyScalar(frustumSize * 1.6));
    const w = frustumSize;
    const h = frustumSize * (cam.height && cam.width ? cam.height / cam.width : 0.65);
    const corners = [
      center.clone().add(right.clone().multiplyScalar(-w)).add(up.clone().multiplyScalar(-h)),
      center.clone().add(right.clone().multiplyScalar(w)).add(up.clone().multiplyScalar(-h)),
      center.clone().add(right.clone().multiplyScalar(w)).add(up.clone().multiplyScalar(h)),
      center.clone().add(right.clone().multiplyScalar(-w)).add(up.clone().multiplyScalar(h)),
    ];
    for (const c of corners) linePositions.push(p.x, p.y, p.z, c.x, c.y, c.z);
    for (let j = 0; j < 4; j++) {
      const a = corners[j];
      const b = corners[(j + 1) % 4];
      linePositions.push(a.x, a.y, a.z, b.x, b.y, b.z);
    }
  }
  const frustumGeometry = new THREE.BufferGeometry();
  frustumGeometry.setAttribute("position", new THREE.Float32BufferAttribute(linePositions, 3));
  cameraGroup.add(new THREE.LineSegments(frustumGeometry, new THREE.LineBasicMaterial({ color: 0x54d17a })));
  const pathGeometry = new THREE.BufferGeometry();
  pathGeometry.setAttribute("position", new THREE.Float32BufferAttribute(pathPositions, 3));
  cameraGroup.add(new THREE.Line(pathGeometry, new THREE.LineBasicMaterial({ color: 0x6da0ff })));
  scene.add(cameraGroup);
  updateLayerVisibility();
}

function updateLayerVisibility() {
  const showPoints = document.getElementById("showPoints")?.checked ?? true;
  const showSplats = document.getElementById("showSplats")?.checked ?? false;
  const showRealSplats = document.getElementById("showRealSplats")?.checked ?? false;
  const showCameras = document.getElementById("showCameras")?.checked ?? false;
  const showPivot = document.getElementById("showPivot")?.checked ?? true;
  const showMesh = document.getElementById("showMesh")?.checked ?? true;
  if (pointCloud) pointCloud.visible = showPoints;
  if (splatCloud) splatCloud.visible = showSplats;
  if (cameraGroup) cameraGroup.visible = showCameras;
  if (centerGizmo) centerGizmo.visible = Boolean(centerGizmo.userData.hasPivot) && showPivot;
  if (meshObject) meshObject.visible = showMesh;
  if (meshSelectionPoints) meshSelectionPoints.visible = showMesh;
  if (realSplatViewer?.splatMesh) realSplatViewer.splatMesh.visible = showRealSplats && realSplatReady && !realSplatLoading;
}

function buildSelectionCloud() {
  if (selectionPoints) {
    disposeObject(selectionPoints);
    selectionPoints = null;
  }
  if (!selected.size || !positions) return;
  const selectedPositions = new Float32Array(selected.size * 3);
  let w = 0;
  for (const i of selected) {
    selectedPositions.set(positions.subarray(i * 3, i * 3 + 3), w * 3);
    w++;
  }
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(selectedPositions, 3));
  const material = new THREE.PointsMaterial({ color: 0xffd84d, size: 5.5, sizeAttenuation: false });
  selectionPoints = new THREE.Points(geometry, material);
  scene.add(selectionPoints);
}

function frameBounds(bounds) {
  const min = new THREE.Vector3(...bounds.min);
  const max = new THREE.Vector3(...bounds.max);
  const center = min.clone().add(max).multiplyScalar(0.5);
  const size = max.clone().sub(min).length();
  controls.target.copy(center);
  camera.position.copy(center).add(new THREE.Vector3(0, -size * 0.9, size * 0.35));
  camera.near = Math.max(0.001, size / 10000);
  camera.far = Math.max(1000, size * 10);
  camera.updateProjectionMatrix();
  controls.update();
  updateCenterGizmo(bounds);
}

function resize() {
  if (uiScaleMode === "auto") applyUiScale("auto", { persist: false, scheduleResize: false });
  syncToolbarHeight();
  if (!renderer || !camera) return;
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  renderer.setSize(w, h, false);
  overlay.width = Math.floor(w * window.devicePixelRatio);
  overlay.height = Math.floor(h * window.devicePixelRatio);
  overlay.style.width = `${w}px`;
  overlay.style.height = `${h}px`;
  ctx.setTransform(window.devicePixelRatio, 0, 0, window.devicePixelRatio, 0, 0);
  camera.aspect = w / h;
  camera.updateProjectionMatrix();
  if (pickingTarget && pickingTargetKey !== `${Math.max(1, Math.ceil(w * renderer.getPixelRatio()))}x${Math.max(1, Math.ceil(h * renderer.getPixelRatio()))}`) {
    disposePickingResources({ target: true });
  }
  if (controls && controls.handleResize) controls.handleResize();
}

function updateFpsMeter(frameTimeMs) {
  if (!fpsMeterEl) return;
  const metrics = fpsMeter.recordFrame(frameTimeMs);
  if (metrics.fps) {
    const text = `FPS ${metrics.fps.toFixed(1)}  ${metrics.frameTimeMs.toFixed(2)} ms`;
    if (text !== displayedFpsText) {
      displayedFpsText = text;
      fpsMeterEl.textContent = text;
    }
  }
}

function hasRenderableModel() {
  const realSplatsVisible = Boolean(
    realSplatReady &&
    realSplatViewer?.splatMesh?.visible &&
    document.getElementById("showRealSplats")?.checked
  );
  const gaussianPreviewVisible = Boolean(
    positions?.length &&
    (pointCloud?.visible || splatCloud?.visible)
  );
  return realSplatsVisible || gaussianPreviewVisible || Boolean(meshObject?.visible);
}

function resetFpsMeterDisplay() {
  fpsMeter.reset();
  gpuRenderTimer?.reset();
  displayedFpsText = "";
  if (fpsMeterEl) fpsMeterEl.textContent = "FPS --  -- ms";
}

function completeRenderMeasurement(renderStart, gpuFrameTimeMs, queryStarted, monitorFrame) {
  if (queryStarted) gpuRenderTimer.end();
  if (!monitorFrame) {
    resetFpsMeterDisplay();
    return;
  }
  const cpuFrameTimeMs = performance.now() - renderStart;
  if (gpuRenderTimer?.supported()) {
    if (gpuFrameTimeMs === null) return;
    updateFpsMeter(Math.max(cpuFrameTimeMs, gpuFrameTimeMs));
    if (fpsMeterEl) fpsMeterEl.title = "SIBR-style 60-frame average using asynchronous GPU render timing.";
    return;
  }
  updateFpsMeter(cpuFrameTimeMs);
  if (fpsMeterEl) fpsMeterEl.title = "SIBR-style 60-frame average using CPU render timing because GPU timing is unavailable.";
}

function animate() {
  requestAnimationFrame(animate);
  const monitorFrame = hasRenderableModel();
  const gpuFrameTimeMs = monitorFrame && gpuRenderTimer?.supported() ? gpuRenderTimer.poll() : null;
  const queryStarted = monitorFrame ? gpuRenderTimer?.begin() : false;
  const renderStart = performance.now();
  controls.update();
  syncCenterGizmoToTarget();
  drawOverlay();
  if (realSplatViewer) {
    realSplatViewer.update();
    if (document.getElementById("showRealSplats")?.checked && realSplatReady) {
      realSplatViewer.render();
      renderCenterGizmoOverlay();
      completeRenderMeasurement(renderStart, gpuFrameTimeMs, queryStarted, monitorFrame);
      return;
    }
  }
  renderer.render(scene, camera);
  renderCenterGizmoOverlay();
  completeRenderMeasurement(renderStart, gpuFrameTimeMs, queryStarted, monitorFrame);
}

function renderCenterGizmoOverlay() {
  if (!renderer || !camera || !centerGizmoScene || !centerGizmo || !centerGizmo.visible) return;
  const previousAutoClear = renderer.autoClear;
  renderer.autoClear = false;
  renderer.clearDepth();
  renderer.render(centerGizmoScene, camera);
  renderer.autoClear = previousAutoClear;
}

function focusOnDoubleClick(e) {
  if (e.button !== 0 || !camera || !controls) return;
  if (!currentScene || dragStart) return;
  e.preventDefault();
  e.stopImmediatePropagation();
  const point = pickFocusPoint(e);
  if (!point) {
    setStatus("No model point under double-click.");
    return;
  }
  if (pivotPickMode) setPivotPickMode(false, false);
  movePivotToPoint(point, "Rotation center moved");
}

function movePivotToPoint(point, label = "View center moved") {
  const offset = camera.position.clone().sub(controls.target);
  controls.target.copy(point);
  camera.position.copy(point).add(offset);
  controls.update();
  syncCenterGizmoToTarget();
  setStatus(`${label} to X ${point.x.toFixed(3)}, Y ${point.y.toFixed(3)}, Z ${point.z.toFixed(3)}.`);
}

function pickFocusPoint(e) {
  return pickFocusPointAtClient(e.clientX, e.clientY);
}

function pickFocusPointAtClient(clientX, clientY) {
  const pickEvent = { clientX, clientY };
  return pickMeshFocusPoint(pickEvent) || pickPointCloudFocusPoint(pickEvent);
}

function eventToNdc(e) {
  const rect = canvas.getBoundingClientRect();
  return new THREE.Vector2(
    ((e.clientX - rect.left) / rect.width) * 2 - 1,
    -((e.clientY - rect.top) / rect.height) * 2 + 1
  );
}

function pickMeshFocusPoint(e) {
  if (!meshObject || !meshObject.visible || !focusRaycaster) return null;
  focusRaycaster.setFromCamera(eventToNdc(e), camera);
  const hits = focusRaycaster.intersectObject(meshObject, true).filter((hit) => hit.object.visible);
  return hits.length ? hits[0].point.clone() : null;
}

function pickPointCloudFocusPoint(e) {
  if (!positions || !positions.length) return null;
  const rect = canvas.getBoundingClientRect();
  const x = e.clientX - rect.left;
  const y = e.clientY - rect.top;
  const count = positions.length / 3;
  const stride = Math.max(1, Math.ceil(count / 200000));
  const pointSize = Number(document.getElementById("pointSize")?.value || 1);
  const threshold = Math.max(26, pointSize * 8);
  const threshold2 = threshold * threshold;
  const projected = new THREE.Vector3();
  let bestIndex = -1;
  let bestDistance2 = Infinity;
  for (let i = 0; i < count; i += stride) {
    projected.set(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]);
    projected.project(camera);
    if (projected.z < -1 || projected.z > 1) continue;
    const sx = (projected.x * 0.5 + 0.5) * canvas.clientWidth;
    const sy = (-projected.y * 0.5 + 0.5) * canvas.clientHeight;
    const dx = sx - x;
    const dy = sy - y;
    const distance2 = dx * dx + dy * dy;
    if (distance2 < bestDistance2) {
      bestDistance2 = distance2;
      bestIndex = i;
    }
  }
  if (bestIndex < 0 || bestDistance2 > threshold2) return null;
  return new THREE.Vector3(
    positions[bestIndex * 3],
    positions[bestIndex * 3 + 1],
    positions[bestIndex * 3 + 2]
  );
}

function shouldStartSelection(e) {
  return shouldStartSelectionDrag({
    mode,
    hasPositions: Boolean(positions) || meshTrimEnabled(),
    button: e.button,
    isSpaceDown,
    altKey: e.altKey,
    ctrlKey: e.ctrlKey,
    metaKey: e.metaKey,
  });
}

function shouldStartViewPan(e) {
  if (!camera || !controls) return false;
  if (pivotPickMode || dragStart || viewPanDrag) return false;
  if (e.pointerType === "touch") return false;
  return e.button === 2 || e.button === 1;
}

function viewPanPointerDown(e) {
  if (!shouldStartViewPan(e)) return;
  e.preventDefault();
  e.stopImmediatePropagation();
  viewPanDrag = {
    pointerId: e.pointerId,
    lastX: e.clientX,
    lastY: e.clientY,
    controlsEnabled: controls.enabled,
  };
  controls.enabled = false;
  canvas.setPointerCapture(e.pointerId);
}

function viewPanPointerMove(e) {
  if (!viewPanDrag || e.pointerId !== viewPanDrag.pointerId) return;
  e.preventDefault();
  e.stopImmediatePropagation();
  const dx = e.clientX - viewPanDrag.lastX;
  const dy = e.clientY - viewPanDrag.lastY;
  viewPanDrag.lastX = e.clientX;
  viewPanDrag.lastY = e.clientY;
  panViewByPixels(dx, dy);
}

function viewPanPointerUp(e) {
  if (!viewPanDrag || e.pointerId !== viewPanDrag.pointerId) return;
  e.preventDefault();
  e.stopImmediatePropagation();
  finishViewPan(e.pointerId);
}

function viewPanPointerCancel(e) {
  if (!viewPanDrag || e.pointerId !== viewPanDrag.pointerId) return;
  e.preventDefault();
  e.stopImmediatePropagation();
  finishViewPan(e.pointerId);
}

function finishViewPan(pointerId = null) {
  if (!viewPanDrag) return;
  const restoreControls = viewPanDrag.controlsEnabled;
  viewPanDrag = null;
  controls.enabled = restoreControls;
  if (pointerId !== null && canvas.hasPointerCapture(pointerId)) canvas.releasePointerCapture(pointerId);
}

function panViewByPixels(deltaX, deltaY) {
  if (!camera || !controls || (!deltaX && !deltaY)) return;
  camera.updateMatrix();

  const panOffset = new THREE.Vector3();
  const axis = new THREE.Vector3();

  if (camera.isPerspectiveCamera) {
    const offset = camera.position.clone().sub(controls.target);
    const targetDistance = offset.length() * Math.tan((camera.fov / 2) * Math.PI / 180);
    const distanceX = 2 * deltaX * targetDistance / Math.max(1, canvas.clientHeight);
    const distanceY = 2 * deltaY * targetDistance / Math.max(1, canvas.clientHeight);
    axis.setFromMatrixColumn(camera.matrix, 0).multiplyScalar(-distanceX);
    panOffset.add(axis);
    axis.setFromMatrixColumn(camera.matrix, 1).multiplyScalar(distanceY);
    panOffset.add(axis);
  } else if (camera.isOrthographicCamera) {
    const distanceX = deltaX * (camera.right - camera.left) / camera.zoom / Math.max(1, canvas.clientWidth);
    const distanceY = deltaY * (camera.top - camera.bottom) / camera.zoom / Math.max(1, canvas.clientHeight);
    axis.setFromMatrixColumn(camera.matrix, 0).multiplyScalar(-distanceX);
    panOffset.add(axis);
    axis.setFromMatrixColumn(camera.matrix, 1).multiplyScalar(distanceY);
    panOffset.add(axis);
  } else {
    return;
  }

  camera.position.add(panOffset);
  controls.target.add(panOffset);
  controls.update();
  syncCenterGizmoToTarget();
}

function pointerDown(e) {
  if (pivotPickMode && e.button === 0) {
    e.preventDefault();
    e.stopImmediatePropagation();
    const point = pickFocusPoint(e);
    if (!point) {
      setStatus("No model point under cursor. Pick rotation center still active.");
      return;
    }
    movePivotToPoint(point, "Rotation center picked");
    setPivotPickMode(false, false);
    updateInteractionCursor();
    return;
  }
  if (autoPickPivotAtCenter && e.button === 0 && (mode === "navigate" || isSpaceDown)) {
    const rect = canvas.getBoundingClientRect();
    const point = pickFocusPointAtClient(rect.left + rect.width / 2, rect.top + rect.height / 2);
    if (point) movePivotToPoint(point, "Auto-picked rotation center");
  }
  if (!shouldStartSelection(e)) return;
  e.preventDefault();
  e.stopImmediatePropagation();
  controls.enabled = false;
  dragStart = mouse(e);
  dragCurrent = dragStart;
  brushSubtract = e.shiftKey;
  lasso = [dragStart];
  if (mode === "brush") {
    lastBrushBuild = 0;
    lastBrushPickingSample = null;
    brushSelectAt(dragStart, e.shiftKey, { forcePicking: true });
    buildSelectionCloud();
  }
  drawOverlay();
  updateInteractionCursor();
  canvas.setPointerCapture(e.pointerId);
}

function pointerMove(e) {
  if (!dragStart || mode === "navigate") return;
  e.preventDefault();
  e.stopImmediatePropagation();
  const p = mouse(e);
  dragCurrent = p;
  brushSubtract = e.shiftKey;
  if (mode === "brush") {
    brushSelectAt(p, e.shiftKey);
    const now = performance.now();
    if (now - lastBrushBuild > 90) {
      if (meshTrimEnabled()) buildMeshSelectionCloud();
      else buildSelectionCloud();
      lastBrushBuild = now;
    }
    setStatus(meshTrimEnabled()
      ? `${meshSelected.size.toLocaleString()} mesh vertices selected. Brush: drag selects, Shift+drag removes.`
      : `${selected.size.toLocaleString()} selected. Brush: drag selects, Shift+drag removes.`);
  }
  if (mode === "lasso") {
    lasso.push(p);
  }
  drawOverlay();
}

function pointerUp(e) {
  if (!dragStart || mode === "navigate") return;
  e.preventDefault();
  e.stopImmediatePropagation();
  const end = mouse(e);
  if (mode === "rect") selectRect(dragStart, end, e.shiftKey);
  if (mode === "lasso") selectLasso(lasso, e.shiftKey);
  if (mode === "brush") {
    brushSelectAt(end, e.shiftKey, { forcePicking: true });
    if (meshTrimEnabled()) {
      buildMeshSelectionCloud();
      setStatus(`${meshSelected.size.toLocaleString()} mesh vertices selected. Brush selection updated.`);
    } else {
      buildSelectionCloud();
      setStatus(`${selected.size.toLocaleString()} selected. Brush selection updated.`);
    }
  }
  dragStart = null;
  dragCurrent = null;
  lastBrushPickingSample = null;
  lasso = [];
  drawOverlay();
  controls.enabled = true;
  updateInteractionCursor();
  if (canvas.hasPointerCapture(e.pointerId)) canvas.releasePointerCapture(e.pointerId);
}

function pointerCancel(e) {
  if (!dragStart) return;
  e.preventDefault();
  e.stopImmediatePropagation();
  cancelSelectionDrag(e.pointerId);
}

function cancelSelectionDrag(pointerId = null) {
  dragStart = null;
  dragCurrent = null;
  lastBrushPickingSample = null;
  lasso = [];
  drawOverlay();
  controls.enabled = true;
  updateInteractionCursor();
  if (pointerId !== null && canvas.hasPointerCapture(pointerId)) canvas.releasePointerCapture(pointerId);
}

function mouse(e) {
  const rect = canvas.getBoundingClientRect();
  return { x: e.clientX - rect.left, y: e.clientY - rect.top };
}

function clearOverlay() {
  ctx.clearRect(0, 0, canvas.clientWidth, canvas.clientHeight);
}

function drawOverlay() {
  if (!ctx || !camera) return;
  clearOverlay();
  drawAxisGizmo();
  if (!dragStart || mode === "navigate") return;
  if (mode === "rect" && dragCurrent) {
    drawRect(dragStart, dragCurrent);
  } else if (mode === "lasso") {
    drawLasso(lasso);
  } else if (mode === "brush" && dragCurrent) {
    drawBrush(dragCurrent, brushSubtract);
  }
}

function drawAxisGizmo() {
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  if (w < 80 || h < 80) return;
  camera.updateMatrixWorld();
  const e = camera.matrixWorld.elements;
  const segments = axisGizmoSegments({
    width: w,
    height: h,
    length: Math.min(46, Math.max(34, Math.min(w, h) * 0.075)),
    margin: 22,
    cameraRight: [e[0], e[1], e[2]],
    cameraUp: [e[4], e[5], e[6]]
  });
  ctx.save();
  ctx.font = "12px Segoe UI, Arial, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  for (const segment of segments) {
    const dx = segment.end.x - segment.start.x;
    const dy = segment.end.y - segment.start.y;
    const len = Math.hypot(dx, dy);
    if (len < 4) continue;
    const ux = dx / len;
    const uy = dy / len;
    ctx.strokeStyle = "rgba(0, 0, 0, 0.62)";
    ctx.lineWidth = 4.5;
    ctx.beginPath();
    ctx.moveTo(segment.start.x, segment.start.y);
    ctx.lineTo(segment.end.x, segment.end.y);
    ctx.stroke();
    ctx.strokeStyle = segment.color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(segment.start.x, segment.start.y);
    ctx.lineTo(segment.end.x, segment.end.y);
    ctx.stroke();
    const head = 7;
    const side = 4.5;
    ctx.fillStyle = segment.color;
    ctx.beginPath();
    ctx.moveTo(segment.end.x, segment.end.y);
    ctx.lineTo(segment.end.x - ux * head - uy * side, segment.end.y - uy * head + ux * side);
    ctx.lineTo(segment.end.x - ux * head + uy * side, segment.end.y - uy * head - ux * side);
    ctx.closePath();
    ctx.fill();
    const labelX = segment.end.x + ux * 11;
    const labelY = segment.end.y + uy * 11;
    ctx.fillStyle = "#ffffff";
    ctx.strokeStyle = "rgba(0, 0, 0, 0.85)";
    ctx.lineWidth = 3;
    ctx.strokeText(segment.label, labelX, labelY);
    ctx.fillText(segment.label, labelX, labelY);
  }
  const origin = segments.length ? segments[0].start : null;
  if (origin) {
    ctx.fillStyle = "rgba(235, 239, 246, 0.9)";
    ctx.strokeStyle = "rgba(0, 0, 0, 0.72)";
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.arc(origin.x, origin.y, 3.5, 0, Math.PI * 2);
    ctx.fill();
    ctx.stroke();
  }
  ctx.restore();
}

function drawRect(a, b) {
  ctx.strokeStyle = "#6da0ff";
  ctx.lineWidth = 1.5;
  ctx.setLineDash([5, 4]);
  ctx.strokeRect(a.x, a.y, b.x - a.x, b.y - a.y);
  ctx.setLineDash([]);
}

function drawLasso(poly) {
  if (poly.length < 2) return;
  ctx.strokeStyle = "#6da0ff";
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  ctx.moveTo(poly[0].x, poly[0].y);
  for (const p of poly.slice(1)) ctx.lineTo(p.x, p.y);
  ctx.stroke();
}

function brushRadius() {
  return Number(document.getElementById("brushSize")?.value || 44);
}

function drawBrush(center, subtract) {
  const r = brushRadius();
  ctx.strokeStyle = subtract ? "#ff756d" : "#6da0ff";
  ctx.fillStyle = subtract ? "rgba(255, 117, 109, 0.12)" : "rgba(109, 160, 255, 0.12)";
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  ctx.arc(center.x, center.y, r, 0, Math.PI * 2);
  ctx.fill();
  ctx.stroke();
}

function projectPoint(i, out) {
  const v = new THREE.Vector3(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]);
  v.project(camera);
  out.x = (v.x * 0.5 + 0.5) * canvas.clientWidth;
  out.y = (-v.y * 0.5 + 0.5) * canvas.clientHeight;
  out.z = v.z;
}

function visibleSelectionEnabled() {
  return Boolean(visibleSelectionToggle?.checked);
}

function buildPointVisibilityMap() {
  if (!visibleSelectionEnabled() || !positions || !camera) return null;
  const width = Math.max(1, Math.floor(canvas.clientWidth));
  const height = Math.max(1, Math.floor(canvas.clientHeight));
  const depth = new Float32Array(width * height);
  depth.fill(Infinity);
  const p = { x: 0, y: 0, z: 0 };
  for (let i = 0; i < positions.length / 3; i++) {
    projectPoint(i, p);
    if (p.z < -1 || p.z > 1 || p.x < 0 || p.x >= width || p.y < 0 || p.y >= height) continue;
    const idx = Math.floor(p.y) * width + Math.floor(p.x);
    if (p.z < depth[idx]) depth[idx] = p.z;
  }
  return { width, height, depth, tolerance: 0.006 };
}

function isPointVisibleInMap(projected, visibilityMap) {
  if (!visibilityMap) return true;
  if (projected.x < 0 || projected.x >= visibilityMap.width || projected.y < 0 || projected.y >= visibilityMap.height) return false;
  const idx = Math.floor(projected.y) * visibilityMap.width + Math.floor(projected.x);
  return projected.z <= visibilityMap.depth[idx] + visibilityMap.tolerance;
}

function applyPickingIds(ids, subtract = false) {
  if (!ids) return false;
  for (const id of ids) {
    if (id < 0 || !positions || id >= positions.length / 3) continue;
    if (subtract) selected.delete(id);
    else selected.add(id);
  }
  return true;
}

function brushSelectAt(center, subtract, options = {}) {
  if (meshTrimEnabled()) {
    brushSelectMeshAt(center, subtract);
    return;
  }
  if (!positions) return;
  const r = brushRadius();
  if (visibleSelectionEnabled()) {
    if (!shouldSampleBrushPicking({
      last: lastBrushPickingSample,
      point: center,
      radius: r,
      subtract,
      force: Boolean(options.forcePicking),
    })) {
      pickingStats.brushSkips++;
      window.__pickingDebug = pickingStats;
      return;
    }
    const bounds = pickingBoundsForBrush(center, r, { width: canvas.clientWidth, height: canvas.clientHeight });
    const r2 = r * r;
    const ids = readVisiblePickingIds(bounds, (x, y) => {
      const dx = x - center.x;
      const dy = y - center.y;
      return dx * dx + dy * dy <= r2;
    });
    if (applyPickingIds(ids, subtract)) {
      lastBrushPickingSample = { x: center.x, y: center.y, subtract };
      return;
    }
  }
  const r2 = r * r;
  const p = { x: 0, y: 0, z: 0 };
  const visibilityMap = buildPointVisibilityMap();
  for (let i = 0; i < positions.length / 3; i++) {
    projectPoint(i, p);
    if (p.z < -1 || p.z > 1) continue;
    if (!isPointVisibleInMap(p, visibilityMap)) continue;
    const dx = p.x - center.x;
    const dy = p.y - center.y;
    if (dx * dx + dy * dy <= r2) {
      if (subtract) selected.delete(i);
      else selected.add(i);
    }
  }
}

function selectRect(a, b, append) {
  if (meshTrimEnabled()) {
    selectMeshRect(a, b, append);
    return;
  }
  if (!append) selected.clear();
  if (visibleSelectionEnabled()) {
    const bounds = pickingBoundsForRect(a, b, { width: canvas.clientWidth, height: canvas.clientHeight });
    const ids = readVisiblePickingIds(bounds);
    if (applyPickingIds(ids)) {
      setStatus(`${selected.size.toLocaleString()} selected. Hold Shift to add to selection.`);
      buildSelectionCloud();
      return;
    }
  }
  const minX = Math.min(a.x, b.x), maxX = Math.max(a.x, b.x);
  const minY = Math.min(a.y, b.y), maxY = Math.max(a.y, b.y);
  const p = { x: 0, y: 0, z: 0 };
  const visibilityMap = buildPointVisibilityMap();
  for (let i = 0; i < positions.length / 3; i++) {
    projectPoint(i, p);
    if (p.z >= -1 && p.z <= 1 && isPointVisibleInMap(p, visibilityMap) && p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY) selected.add(i);
  }
  setStatus(`${selected.size.toLocaleString()} selected. Hold Shift to add to selection.`);
  buildSelectionCloud();
}

function selectLasso(poly, append) {
  if (meshTrimEnabled()) {
    selectMeshLasso(poly, append);
    return;
  }
  if (poly.length < 3) return;
  if (!append) selected.clear();
  if (visibleSelectionEnabled()) {
    const bounds = pickingBoundsForPolygon(poly, { width: canvas.clientWidth, height: canvas.clientHeight });
    const ids = readVisiblePickingIds(bounds, (x, y) => pointInPickingPolygon(x, y, poly));
    if (applyPickingIds(ids)) {
      setStatus(`${selected.size.toLocaleString()} selected. Hold Shift to add to selection.`);
      buildSelectionCloud();
      return;
    }
  }
  const p = { x: 0, y: 0, z: 0 };
  const visibilityMap = buildPointVisibilityMap();
  for (let i = 0; i < positions.length / 3; i++) {
    projectPoint(i, p);
    if (p.z >= -1 && p.z <= 1 && isPointVisibleInMap(p, visibilityMap) && pointInPolygon(p, poly)) selected.add(i);
  }
  setStatus(`${selected.size.toLocaleString()} selected. Hold Shift to add to selection.`);
  buildSelectionCloud();
}

function pointInPolygon(point, poly) {
  let inside = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const xi = poly[i].x, yi = poly[i].y;
    const xj = poly[j].x, yj = poly[j].y;
    const intersect = ((yi > point.y) !== (yj > point.y)) &&
      (point.x < (xj - xi) * (point.y - yi) / (yj - yi + 1e-12) + xi);
    if (intersect) inside = !inside;
  }
  return inside;
}

function projectMeshVertex(i, out) {
  const v = new THREE.Vector3(meshData.positions[i * 3], meshData.positions[i * 3 + 1], meshData.positions[i * 3 + 2]);
  v.project(camera);
  out.x = (v.x * 0.5 + 0.5) * canvas.clientWidth;
  out.y = (-v.y * 0.5 + 0.5) * canvas.clientHeight;
  out.z = v.z;
}

function meshSelectableSet() {
  return selectableVertices(meshData, meshDeletedFaces);
}

function brushSelectMeshAt(center, subtract) {
  if (!meshData) return;
  const r = brushRadius();
  const r2 = r * r;
  const p = { x: 0, y: 0, z: 0 };
  for (const i of meshSelectableSet()) {
    projectMeshVertex(i, p);
    if (p.z < -1 || p.z > 1) continue;
    const dx = p.x - center.x;
    const dy = p.y - center.y;
    if (dx * dx + dy * dy <= r2) {
      if (subtract) meshSelected.delete(i);
      else meshSelected.add(i);
    }
  }
}

function selectMeshRect(a, b, append) {
  if (!meshData) return;
  if (!append) meshSelected.clear();
  const minX = Math.min(a.x, b.x), maxX = Math.max(a.x, b.x);
  const minY = Math.min(a.y, b.y), maxY = Math.max(a.y, b.y);
  const p = { x: 0, y: 0, z: 0 };
  for (const i of meshSelectableSet()) {
    projectMeshVertex(i, p);
    if (p.z >= -1 && p.z <= 1 && p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY) meshSelected.add(i);
  }
  setStatus(`${meshSelected.size.toLocaleString()} mesh vertices selected. Delete removes touched triangles.`);
  buildMeshSelectionCloud();
}

function selectMeshLasso(poly, append) {
  if (!meshData || poly.length < 3) return;
  if (!append) meshSelected.clear();
  const p = { x: 0, y: 0, z: 0 };
  for (const i of meshSelectableSet()) {
    projectMeshVertex(i, p);
    if (p.z >= -1 && p.z <= 1 && pointInPolygon(p, poly)) meshSelected.add(i);
  }
  setStatus(`${meshSelected.size.toLocaleString()} mesh vertices selected. Delete removes touched triangles.`);
  buildMeshSelectionCloud();
}

function buildMeshSelectionCloud() {
  disposeObject(meshSelectionPoints);
  meshSelectionPoints = null;
  if (!meshSelected.size || !meshData) return;
  const selectedPositions = new Float32Array(meshSelected.size * 3);
  let w = 0;
  for (const i of meshSelected) {
    selectedPositions.set(meshData.positions.subarray(i * 3, i * 3 + 3), w * 3);
    w++;
  }
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(selectedPositions, 3));
  const material = new THREE.PointsMaterial({ color: 0xffd84d, size: 7.0, sizeAttenuation: false });
  meshSelectionPoints = new THREE.Points(geometry, material);
  scene.add(meshSelectionPoints);
}

function clearSelection() {
  if (meshTrimEnabled()) {
    meshSelected.clear();
    buildMeshSelectionCloud();
    setStatus("Mesh selection cleared.");
    return;
  }
  selected.clear();
  buildSelectionCloud();
  setStatus("Selection cleared.");
}

function invertSelection() {
  if (meshTrimEnabled()) {
    const all = meshSelectableSet();
    const next = new Set();
    for (const i of all) {
      if (!meshSelected.has(i)) next.add(i);
    }
    meshSelected = next;
    buildMeshSelectionCloud();
    setStatus(`Mesh selection inverted: ${meshSelected.size.toLocaleString()} vertices selected.`);
    return;
  }
  if (!positions) {
    setStatus("Load a scene before inverting selection.");
    return;
  }
  const count = positions.length / 3;
  const previous = selected.size;
  const next = new Set();
  for (let i = 0; i < count; i++) {
    if (!selected.has(i)) next.add(i);
  }
  selected = next;
  buildSelectionCloud();
  setStatus(`Selection inverted: ${previous.toLocaleString()} -> ${selected.size.toLocaleString()} selected.`);
}

function deleteSelected() {
  if (meshTrimEnabled()) {
    deleteSelectedMeshFaces();
    return;
  }
  if (!selected.size) {
    setStatus("No selection to delete.");
    return;
  }
  const remove = new Uint8Array(positions.length / 3);
  for (const i of selected) remove[i] = 1;
  const nextCount = remove.length - selected.size;
  const nextPos = new Float32Array(nextCount * 3);
  const nextCol = new Uint8Array(nextCount * 3);
  const nextOpacity = new Float32Array(nextCount);
  const nextSplatScale = new Float32Array(nextCount);
  const nextMap = new Int32Array(nextCount);
  const removedOriginal = [];
  let w = 0;
  for (let i = 0; i < remove.length; i++) {
    if (remove[i]) {
      removedOriginal.push(indexMap[i]);
      deletedOriginal.add(indexMap[i]);
      continue;
    }
    nextPos.set(positions.subarray(i * 3, i * 3 + 3), w * 3);
    nextCol.set(colors.subarray(i * 3, i * 3 + 3), w * 3);
    nextOpacity[w] = opacities[i];
    nextSplatScale[w] = splatScales[i];
    nextMap[w] = indexMap[i];
    w++;
  }
  undoStack.push({ positions, colors, opacities, splatScales, indexMap, removedOriginal });
  positions = nextPos;
  colors = nextCol;
  opacities = nextOpacity;
  splatScales = nextSplatScale;
  indexMap = nextMap;
  hasUnsavedEdits = deletedOriginal.size > 0;
  editRevision++;
  selected.clear();
  buildRenderLayers();
  refreshRealAfterEdit(`Deleted ${removedOriginal.length.toLocaleString()}. Remaining ${nextCount.toLocaleString()}.`);
}

function deleteSelectedMeshFaces() {
  if (meshData?.isPreview) {
    setStatus("Preview mesh cannot be trimmed accurately. Enable Mesh Trim to load the full mesh first.");
    return;
  }
  if (!meshSelected.size || !meshData) {
    setStatus("No mesh selection to delete.");
    return;
  }
  const faces = facesTouchingVertices(meshData.indices, meshSelected);
  const removed = [];
  for (const face of faces) {
    if (!meshDeletedFaces.has(face)) {
      meshDeletedFaces.add(face);
      removed.push(face);
    }
  }
  if (!removed.length) {
    setStatus("Selected mesh area was already deleted.");
    return;
  }
  meshUndoStack.push(removed);
  meshSelected.clear();
  meshDirty = true;
  rebuildMeshObject();
  setMeshBusy(false);
  setStatus(`Deleted ${removed.length.toLocaleString()} mesh triangles. Download Mesh saves the trimmed mesh.`);
}

function undo() {
  if (meshTrimEnabled() && meshUndoStack.length) {
    const last = meshUndoStack.pop();
    for (const face of last) meshDeletedFaces.delete(face);
    meshDirty = meshDeletedFaces.size > 0;
    meshSelected.clear();
    rebuildMeshObject();
    setMeshBusy(false);
    setStatus(meshDirty ? "Mesh undo complete." : "Mesh undo complete. No mesh trim edits.");
    return;
  }
  const last = undoStack.pop();
  if (!last) {
    setStatus("Nothing to undo.");
    return;
  }
  for (const id of last.removedOriginal) deletedOriginal.delete(id);
  positions = last.positions;
  colors = last.colors;
  opacities = last.opacities;
  splatScales = last.splatScales;
  indexMap = last.indexMap;
  hasUnsavedEdits = deletedOriginal.size > 0;
  editRevision++;
  selected.clear();
  buildRenderLayers();
  refreshRealAfterEdit(hasUnsavedEdits ? "Undo complete." : "Undo complete. No unsaved crop edits.");
}

async function saveModel() {
  if (!currentScene || !currentIteration) return;
  if (meshDirty && meshData) {
    await saveTrimmedMesh();
    return;
  }
  if (!deletedOriginal.size) {
    setStatus("No deleted Gaussians to save. Select, Delete, then Save.");
    return;
  }
  const outputScene = outputInput.value.trim();
  if (!outputScene) {
    setStatus("Output scene is required.");
    return;
  }
  const body = {
    scene: currentScene,
    iteration: currentIteration,
    output_scene: outputScene,
    delete_indices: Array.from(deletedOriginal),
    overwrite: Boolean(document.getElementById("overwriteOutput")?.checked),
  };
  setStatus(`Saving ${outputScene}...`);
  const res = await fetch(apiPath("/api/save"), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const data = await res.json();
  if (!res.ok) {
    setStatus(`Save failed: ${data.error}`);
    return;
  }
  await refreshSceneList(data.output_scene);
  sceneSelect.value = data.output_scene;
  const opt = sceneSelect.selectedOptions[0];
  if (opt?.dataset?.iteration) iterationInput.value = opt.dataset.iteration;
  outputInput.value = `${data.output_scene}_crop`;
  hasUnsavedEdits = false;
  deletedOriginal.clear();
  selected.clear();
  await loadCurrent();
  setStatus(`Saved and loaded ${data.output_scene}. Removed ${data.removed.toLocaleString()}, kept ${data.kept.toLocaleString()}.`);
}

async function saveTrimmedMesh() {
  if (meshData?.isPreview) {
    setStatus("Preview mesh cannot be saved as a trim. Load the full mesh in Mesh Trim mode first.");
    return;
  }
  const outputScene = outputInput.value.trim();
  if (!outputScene) {
    setStatus("Output scene is required.");
    return;
  }
  const body = {
    scene: currentScene,
    iteration: currentIteration,
    output_scene: outputScene,
    mode: meshModeSelect.value,
    overwrite: Boolean(document.getElementById("overwriteOutput")?.checked),
    ply: meshToAsciiPly(meshData, meshDeletedFaces),
  };
  setStatus(`Saving trimmed mesh ${outputScene}...`);
  const res = await fetch(apiPath("/api/mesh/trimmed/save"), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const data = await res.json();
  if (!res.ok) {
    setStatus(`Save trimmed mesh failed: ${data.error}`);
    return;
  }
  await refreshSceneList(data.output_scene);
  sceneSelect.value = data.output_scene;
  const opt = sceneSelect.selectedOptions[0];
  if (opt?.dataset?.iteration) iterationInput.value = opt.dataset.iteration;
  currentScene = data.output_scene;
  currentIteration = Number(iterationInput.value || data.iteration);
  currentBackend = selectedSceneBackend();
  outputInput.value = `${data.output_scene}_crop`;
  meshDirty = false;
  meshDeletedFaces.clear();
  meshUndoStack = [];
  meshSelected.clear();
  lastMeshDownloadUrl = data.mesh_url;
  lastTextureDownloadUrl = null;
  lastGlbDownloadUrl = null;
  await loadMeshFromUrl(data.mesh_url, `${data.mode} trimmed mesh`);
  setStatus(`Saved trimmed mesh scene ${data.output_scene}. Mesh path: ${data.mesh_path}`);
}
