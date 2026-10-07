# Native model resource budgets

## 中文

在“视图 → 资源预算”中选择自动或手动模式。设置即时生效并保存，不需要重新导入模型。

- 默认“自动（激进）”按当前可用内存和显存计算预算，保留可用余量的 10%，且至少保留 512 MiB RAM / 256 MiB GPU 内存。预算是模型可以使用的目标，不是提前分配全部资源。
- 放得下完整网格的估计峰值和显示数据时，保留所有有效三角形并完整加载；否则使用磁盘分页。预算增加后可自动转为常驻，预算减少或系统压力增加时可退回分页，保留模型位置和观察视角。
- 网格显示数据、在途网格导入和分页缓存共用一份账本。工作线程的资源预留直到实际结束才释放；移除模型或使请求失效，并不等于强制终止正在读取文件的线程。
- CPU 峰值估计包括几何临时数组、UV 接缝、源坐标与拾取／点预览数据、纹理解码及转换。网格显存成本包括实际上传的顶点与索引缓冲区，以及 RGBA 纹理的全部 mip 层级，不为尚未上传的 CPU 点预览虚构 GPU 开销。系统和驱动余量会变化，估计不是操作系统级硬配额或绝对不溢出的保证。
- 几何分页不等于纹理分页。纹理及最小分页工作区本身放不下时明确报错，不默默超预算或修改原始纹理。提高预算后可重试；也可以自行准备较小的纹理。
- DXGI 数值是当前 OpenGL 设备上本进程的 Windows 显存预算余量；NVX 是近似可用显存。检测失败的自动模式使用 512 MiB 回退；检测失败的手动显存上限不是测量值，不能保证安全余量。
- 此设置管理桌面端网格加载及显示缓存，不更改 3DGS、2DGS 或其他训练进程的参数，也不取消已有的 SPZ 转换、SH 或点云编辑保护。其他进程对实际余量的影响会反映在新的资源检测中。
- 手动网格上限不是整个程序的资源硬配额。高斯打包属性、深度排序及 SH 等独立 GPU 缓冲区不计入该网格账本，其资源压力只能通过新的实际检测反映。

## English

Use **View → Resource Budget**. Automatic mode is aggressive: it retains 10% of currently available headroom, with minimum reserves of 512 MiB RAM and 256 MiB GPU memory. Manual mesh ceilings apply immediately and persist; they do not preallocate memory.

Complete mesh admission estimates CPU peaks for geometry scratch arrays, UV seams, source-coordinate and picking/point-preview data, and texture decoding/conversion. Mesh GPU costs cover the vertex and index buffers actually uploaded, plus every RGBA texture mip level; CPU point-preview data is not counted as fictional GPU storage. Mesh display data, in-flight mesh imports and page caches share a reservation ledger. Higher budgets can promote an existing paged model, while lower budgets or resource pressure can demote residency without changing the model transform or camera. The normal fixed five-million-face / 1 GiB thresholds are removed; buffer-index, integer, record-size and format protections remain.

These are estimates and application-level targets, not OS hard quotas. A worker retains its lease until its actual work ends; abandoning a request does not stop a running file read. Geometry paging does not page texture atlases: if the texture and minimum working set cannot fit, loading reports insufficient resources rather than silently exceeding the target or rewriting the source image.

DXGI reports this process's budget headroom on the identified OpenGL adapter; NVX reports approximate available memory. Unknown automatic GPU measurements use a 512 MiB fallback. An unknown manual GPU ceiling is explicitly unmeasured and cannot guarantee a safety reserve. Training-worker parameters and existing SPZ/SH/point-editing limits are unchanged.

The manual mesh ceiling is not a hard whole-application quota. Independent packed-Gaussian, depth-order and SH GPU buffers are not included in this mesh ledger; their resource pressure is reflected only through fresh real resource probes.

## 日本語

「ビュー → リソース予算」で自動または手動モードを選択します。変更は即時に反映され、保存されます。自動モードは現在の空き容量を積極的に利用し、余量の 10%、かつ RAM は最低 512 MiB、GPU メモリは最低 256 MiB を残します。予算を事前に全量確保するわけではありません。

完全なメッシュの CPU 最大使用量と GPU メモリ使用量を推定し、収まる場合は全有効三角形を読み込みます。収まらない場合はディスク経由のページングを使用します。予算変更で常駐・ページングを切り替えますが、モデルの変換と視点は保持します。メッシュ表示データ、読み込み中のメッシュ処理とページキャッシュは同じ予約台帳を共有します。失効した要求でも、実行中の読み込みが終了するまでは予約を保持します。

CPU 最大使用量には幾何処理用の一時配列、UV シーム、ソース座標・ピッキング／点群プレビュー用データ、テクスチャのデコード・変換を含めます。メッシュの GPU メモリ使用量は、実際に転送する頂点・索引バッファと RGBA テクスチャの全 mip レベルで推定し、未転送の CPU 点群プレビューは GPU メモリ使用量に加えません。ただし OS の強制的な割り当て上限や、メモリ不足が絶対に起きない保証ではありません。ジオメトリのページングはテクスチャのページングではないため、テクスチャと最小作業領域が収まらない場合は明示的にエラーを表示します。原画像ファイルを変更しません。

DXGI は対象 OpenGL デバイス上の本プロセスの予算余量、NVX は空き GPU メモリの概算です。検出できない自動モードは 512 MiB の代替予算を使用します。検出できない手動上限は測定値ではなく、安全余量を保証できません。学習プロセスの設定と既存 SPZ・SH・点群編集の保護は変更しません。

手動のメッシュ上限はアプリケーション全体の強制的なリソース上限ではありません。ガウシアンのパック済み属性、深度順、SH などの独立した GPU バッファはこのメッシュ台帳に含めず、新しい実リソース測定を通じてのみ、その影響が反映されます。

## Configuration references and boundaries

The design separates display acceleration, interaction quality, and memory-management policy, inspired by the independently documented controls in [CloudCompare display settings](https://cloudcompare.org/doc/wiki/index.php/Display%5CDisplay_settings), [Metashape settings](https://agisoft.freshdesk.com/support/solutions/articles/31000179306-metashape-settings), and [Blender system preferences](https://docs.blender.org/manual/en/3.0/editors/preferences/system.html). The 90% policy is GSW's policy, not a claim about those products' allocation algorithms.

Probe semantics follow [Microsoft MEMORYSTATUSEX](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/ns-sysinfoapi-memorystatusex), [DXGI video-memory information](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info), and [NVX GPU memory information](https://registry.khronos.org/OpenGL/extensions/NVX/NVX_gpu_memory_info.txt). A known zero headroom stays zero; it is never treated as an unknown probe and replaced with a larger fallback.

The shared renderer covers imported meshes and supported 2DGS TSDF/OpenMVS and 3DGS SuGaR/GS2Mesh mesh outputs. This change is not a new continuous-surface LOD algorithm, a training resource scheduler, optimizer-state resume, or a validation of dense GS2Mesh generation quality. Incomplete sampled previews can still remain when exact visible geometry cannot fit.
