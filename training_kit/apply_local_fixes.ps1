$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Src = Join-Path $Root "gaussian-splatting"

function Replace-Once {
    param(
        [string]$Path,
        [string]$Old,
        [string]$New
    )
    $Text = Get-Content $Path -Raw
    if ($Text.Contains($New)) { return }
    if (-not $Text.Contains($Old)) {
        return
    }
    Set-Content -Path $Path -Value $Text.Replace($Old, $New) -NoNewline
}

function Remove-DuplicateLine {
    param(
        [string]$Path,
        [string]$Line
    )
    $Lines = Get-Content $Path
    $Seen = $false
    $Result = foreach ($Current in $Lines) {
        if ($Current -eq $Line) {
            if (-not $Seen) {
                $Seen = $true
                $Current
            }
        } else {
            $Current
        }
    }
    Set-Content -Path $Path -Value $Result
}

$Convert = Join-Path $Src "convert.py"
Replace-Once $Convert "use_gpu = 1 if not args.no_gpu else 0" "use_gpu = 1 if not args.no_gpu else 0`r`nimage_input_path = args.source_path + `"/input`"`r`nif not os.path.exists(image_input_path):`r`n    image_input_path = args.source_path + `"/images`""
Replace-Once $Convert "--image_path `" + args.source_path + `"/input \" "--image_path `" + image_input_path + `" \"
Replace-Once $Convert "--SiftExtraction.use_gpu `" + str(use_gpu)" "--FeatureExtraction.use_gpu `" + str(use_gpu)"
Replace-Once $Convert "--SiftMatching.use_gpu `" + str(use_gpu)" "--FeatureMatching.use_gpu `" + str(use_gpu)"
Replace-Once $Convert "--image_path `"  + args.source_path + `"/input \" "--image_path `"  + image_input_path + `" \"

$Dgr = Join-Path $Src "submodules\diff-gaussian-rasterization\setup.py"
Replace-Once $Dgr "import os`r`nos.path.dirname(os.path.abspath(__file__))" "import os`r`nos.path.dirname(os.path.abspath(__file__))`r`n`r`nnvcc_flags = [`"-I`" + os.path.join(os.path.dirname(os.path.abspath(__file__)), `"third_party/glm/`")]`r`nif os.name == 'nt':`r`n    nvcc_flags.append(`"-allow-unsupported-compiler`")`r`n    nvcc_flags.append(`"-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH`")"
Replace-Once $Dgr "extra_compile_args={`"nvcc`": [`"-I`" + os.path.join(os.path.dirname(os.path.abspath(__file__)), `"third_party/glm/`")]})" "extra_compile_args={`"nvcc`": nvcc_flags})"

$Knn = Join-Path $Src "submodules\simple-knn\setup.py"
Replace-Once $Knn "cxx_compiler_flags = []" "cxx_compiler_flags = []`r`nnvcc_flags = []"
Replace-Once $Knn "cxx_compiler_flags.append(`"/wd4624`")" "cxx_compiler_flags.append(`"/wd4624`")`r`n    cxx_compiler_flags.append(`"/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH`")`r`n    nvcc_flags.append(`"-allow-unsupported-compiler`")`r`n    nvcc_flags.append(`"-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH`")"
Replace-Once $Knn "extra_compile_args={`"nvcc`": [], `"cxx`": cxx_compiler_flags})" "extra_compile_args={`"nvcc`": nvcc_flags, `"cxx`": cxx_compiler_flags})"

$Ssim = Join-Path $Src "submodules\fused-ssim\setup.py"
Replace-Once $Ssim "from torch.utils.cpp_extension import CUDAExtension, BuildExtension" "from torch.utils.cpp_extension import CUDAExtension, BuildExtension`r`nimport os`r`n`r`nnvcc_flags = []`r`nif os.name == 'nt':`r`n    nvcc_flags.append(`"-allow-unsupported-compiler`")`r`n    nvcc_flags.append(`"-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH`")"
Replace-Once $Ssim "`"ext.cpp`"])" "`"ext.cpp`"],`r`n            extra_compile_args={`"nvcc`": nvcc_flags})"

$Gaussian = Join-Path $Src "scene\gaussian_model.py"
Replace-Once $Gaussian "        self._exposure = nn.Parameter(exposure.requires_grad_(True))`r`n`r`n    def training_setup" "        self._exposure = nn.Parameter(exposure.requires_grad_(True))`r`n`r`n    def setup_exposure(self, cam_infos):`r`n        self.exposure_mapping = {cam_info.image_name: idx for idx, cam_info in enumerate(cam_infos)}`r`n        self.pretrained_exposures = None`r`n        exposure = torch.eye(3, 4, device=`"cuda`")[None].repeat(len(cam_infos), 1, 1)`r`n        self._exposure = nn.Parameter(exposure.requires_grad_(True))`r`n`r`n    def training_setup"

$Scene = Join-Path $Src "scene\__init__.py"
Replace-Once $Scene "import json" "import json`r`nimport torch"
Replace-Once $Scene "                                                           `"point_cloud.ply`"), args.train_test_exp)`r`n        else:" "                                                           `"point_cloud.ply`"), args.train_test_exp)`r`n            self.gaussians.spatial_lr_scale = self.cameras_extent`r`n            self.gaussians.max_radii2D = torch.zeros((self.gaussians.get_xyz.shape[0]), device=`"cuda`")`r`n            self.gaussians.setup_exposure(scene_info.train_cameras)`r`n        else:"

$Train = Join-Path $Src "train.py"
Replace-Once $Train "def training(dataset, opt, pipe, testing_iterations, saving_iterations, checkpoint_iterations, checkpoint, debug_from):" "def training(dataset, opt, pipe, testing_iterations, saving_iterations, checkpoint_iterations, checkpoint, debug_from, load_iteration=None):"
Replace-Once $Train "    scene = Scene(dataset, gaussians)`r`n    gaussians.training_setup(opt)" "    scene = Scene(dataset, gaussians, load_iteration=load_iteration)`r`n    gaussians.training_setup(opt)`r`n    if load_iteration and not checkpoint:`r`n        first_iter = load_iteration"
Replace-Once $Train "    parser.add_argument(`"--start_checkpoint`", type=str, default = None)" "    parser.add_argument(`"--start_checkpoint`", type=str, default = None)`r`n    parser.add_argument(`"--load_iteration`", type=int, default=None)"
Replace-Once $Train "training(lp.extract(args), op.extract(args), pp.extract(args), args.test_iterations, args.save_iterations, args.checkpoint_iterations, args.start_checkpoint, args.debug_from)" "training(lp.extract(args), op.extract(args), pp.extract(args), args.test_iterations, args.save_iterations, args.checkpoint_iterations, args.start_checkpoint, args.debug_from, args.load_iteration)"
Remove-DuplicateLine $Train '    parser.add_argument("--load_iteration", type=int, default=None)'

Write-Host "Local 3DGS fixes verified/applied."
