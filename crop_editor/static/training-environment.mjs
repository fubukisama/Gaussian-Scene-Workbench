export function trainingEnvironmentReady(data = {}, backend = "3dgs", { requireVideo = false } = {}) {
  const backendReady = backend === "2dgs"
    ? Boolean(data.two_dgs_dir_exists && data.two_dgs_python_exists && data.two_dgs_train_exists)
    : true;
  const commonReady = Boolean(
    data.conda_exists &&
      data.python_exists &&
      data.env_root_exists &&
      data.colmap_exists &&
      data.opencv_ok &&
      data.gaussian_dir_exists &&
      data.runtime_ready,
  );
  const videoReady = !requireVideo || Boolean(data.ffmpeg_exists || data.video_packages_ok);
  return commonReady && backendReady && videoReady;
}
