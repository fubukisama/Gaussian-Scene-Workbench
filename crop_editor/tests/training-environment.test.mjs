import test from "node:test";
import assert from "node:assert/strict";
import { trainingEnvironmentReady } from "../static/training-environment.mjs";

function completeEnvironment(overrides = {}) {
  return {
    conda_exists: true,
    python_exists: true,
    env_root_exists: true,
    colmap_exists: true,
    opencv_ok: true,
    video_packages_ok: false,
    ffmpeg_exists: false,
    gaussian_dir_exists: true,
    runtime_ready: true,
    two_dgs_dir_exists: true,
    two_dgs_python_exists: true,
    two_dgs_train_exists: true,
    ...overrides,
  };
}

test("existing Metashape image training does not require video tools", () => {
  assert.equal(trainingEnvironmentReady(completeEnvironment(), "2dgs", { requireVideo: false }), true);
});

test("video upload still requires an available extraction path", () => {
  assert.equal(trainingEnvironmentReady(completeEnvironment(), "2dgs", { requireVideo: true }), false);
  assert.equal(
    trainingEnvironmentReady(completeEnvironment({ ffmpeg_exists: true }), "2dgs", { requireVideo: true }),
    true,
  );
});

test("2DGS readiness still requires its repository and CUDA runtime", () => {
  assert.equal(
    trainingEnvironmentReady(completeEnvironment({ two_dgs_train_exists: false }), "2dgs"),
    false,
  );
  assert.equal(trainingEnvironmentReady(completeEnvironment({ runtime_ready: false }), "2dgs"), false);
});
