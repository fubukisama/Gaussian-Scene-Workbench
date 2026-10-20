# TUM RGB-D first-three-pose format fixture

This fixture is the first three pose rows and original comment header of the
official TUM RGB-D benchmark `freiburg1_xyz` ground-truth trajectory, downloaded
on 2026-10-20. The rows and their decimal spellings are unchanged; only this
small prefix is retained. This is pose-format and convention evidence only.
Tests create synthetic placeholder images and explicit synthetic intrinsics;
they do not establish actual image projection or reconstructed-model accuracy.

- Official download: https://webshare.cvg.cit.tum.de/g/rgbd/dataset/freiburg1/rgbd_dataset_freiburg1_xyz-groundtruth.txt
- Original full trajectory: 201100 bytes, SHA256 `aac0319a6ef4e1cdf61e779d2152b95aa7e9f7b1749d6d18717b43ddabffede2`.
- Selected LF prefix SHA256: `4459371978e44d2cda4ed714912edc1efeb0dd979f20d396fae80582322027bd`;
  pose tokens are unchanged if a platform's text checkout changes line endings.
- Format: https://cvg.cit.tum.de/data/datasets/rgbd-dataset/file_formats
- Dataset and license: https://cvg.cit.tum.de/data/datasets/rgbd-dataset
- License: Creative Commons Attribution 4.0 International, https://creativecommons.org/licenses/by/4.0/
- Attribution: Jürgen Sturm, Nikolas Engelhard, Felix Endres, Wolfram Burgard,
  and Daniel Cremers, *A Benchmark for the Evaluation of RGB-D SLAM Systems*,
  IEEE/RSJ International Conference on Intelligent Robots and Systems, 2012.

The dataset's license is retained for these data rows and is not replaced by
the software's GPL license. No TUM images, depth maps or source code are bundled.
