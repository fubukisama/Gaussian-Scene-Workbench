# gsplat SH evaluator / 球谐求值 / 球面調和関数の評価

Source: https://github.com/nerfstudio-project/gsplat/blob/512d366b67073d77ca099ede742683c165dfc23b/gsplat/cuda/csrc/SphericalHarmonicsCUDA.cu

License: Apache-2.0; see LICENSE and the retained source notices. No upstream NOTICE file was present at this revision.

English: `evaluate_sh.glsl` adapts the actual `sh_coeffs_to_color_fast` forward evaluator (degrees 0–4), not the CUDA/PyTorch runtime. Changes: GLSL vector RGB evaluation, buffer texture fetches, zero-length direction guard, display bias and lower clamp. No upstream binaries, network access or new runtime dependencies.

中文：直接改编上游 `sh_coeffs_to_color_fast` 前向求值源码（0–4 阶），不引入 CUDA/PyTorch 运行时。修改包括 GLSL RGB 向量计算、缓冲纹理读取、零方向保护、显示偏置和下限截断。未引入上游二进制、网络访问或额外运行时依赖。

日本語：上流の `sh_coeffs_to_color_fast` 順方向評価ソース（0–4 次）を直接移植し、CUDA/PyTorch ランタイムは導入しません。GLSL の RGB ベクトル評価、バッファテクスチャ参照、ゼロ方向保護、表示バイアスと下限クランプを追加しました。上流バイナリ、ネットワークアクセス、追加のランタイム依存はありません。
