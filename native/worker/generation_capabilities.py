"""Native generation release gates, independent of whether a runtime is installed.

False means unavailable, not silently implemented by restarting from exported
geometry. Every pipeline must declare a limitation and a next acceptance gate.
Runtime readiness is checked separately by training_preflight.
"""

PIPELINES = {
    "3dgs": dict(native_entry=True, checkpoint=True, live_preview="shared_gpu_or_snapshot", density_control="native_guarded",
                 limitation="Shared GPU preview is platform-dependent; no cross-process historical curves.",
                 acceptance="CUDA state restoration, preview fallback, desktop reopen and locale tests."),
    "2dgs": dict(native_entry=True, checkpoint=True, live_preview="snapshot", density_control="native_guarded",
                 limitation="External surfel runtime required; no shared GPU preview; center-sorted OpenGL compositing is not the upstream CUDA tile renderer.",
                 acceptance="Real CUDA checkpoint/preview and nine-view reconstruction replay; perspective/orthographic surfel GPU regression. Held-out coverage and long-run regularization remain."),
    "bounded": dict(native_entry=True, checkpoint=False, live_preview="completed_mesh_stage",
                    density_control="2dgs_input_only",
                    material_preview="validated_diffuse_atlas",
                    limitation="TSDF integration is an external command; no per-voxel preview or stage-resume implementation.",
                    acceptance="Native typed-job, cancellation, output-validation, desktop lifecycle and real CUDA TSDF checks."),
    "unbounded": dict(native_entry=True, checkpoint=False, live_preview="completed_mesh_stage",
                      density_control="2dgs_input_only",
                      material_preview="validated_diffuse_atlas",
                      limitation="Contracted-space TSDF has no native iteration-state resume adapter.",
                      acceptance="Native lifecycle tests; representative contracted-space GPU mesh quality remains."),
    "sugar": dict(native_entry=True, checkpoint=False, live_preview="completed_mesh_stage",
                  density_control="3dgs_input_only",
                  material_preview="validated_diffuse_atlas",
                  limitation="Separate refinement optimizer and mesh/texture stages have no native state adapter.",
                  acceptance="Native isolated-job tests; real refinement/material runtime acceptance and stage recovery remain."),
    "gs2mesh": dict(native_entry=True, checkpoint=False, live_preview="completed_mesh_stage",
                    density_control="3dgs_input_only",
                    material_preview="validated_diffuse_atlas",
                    limitation="Depth estimation/fusion stages do not use the Gaussian optimizer checkpoint.",
                    acceptance="Native typed-job tests; real stereo-depth runtime acceptance and completed-stage retry remain."),
    "openmvs": dict(native_entry=True, checkpoint=False, live_preview="completed_mesh_stage",
                    density_control="not_applicable",
                    material_preview="validated_diffuse_atlas",
                    limitation="Native photo-texturing stage with validated diffuse-atlas preview; standalone dense reconstruction, general PBR and over-budget multi-atlas previews remain unavailable.",
                    acceptance="Native material mapping, preview, partial failure, locale/reopen tests; representative photo bake and dense staged reconstruction remain."),
}

# Source admission follows the explicit pose strategy for native GS training.
# Downstream external readers remain legacy-only until their own K/depth/unit/
# split adapters are implemented and accepted; a working GS preview or runtime
# installation cannot promote those pipelines to canonical-camera support.
_CANONICAL_DOWNSTREAM_REASONS = {
    "bounded": "external_bounded_tsdf_scene_reader_not_canonical_camera_contract",
    "unbounded": "external_contracted_tsdf_scene_reader_not_canonical_camera_contract",
    "sugar": "external_sugar_refinement_reader_not_canonical_camera_contract",
    "gs2mesh": "external_stereo_depth_fusion_reader_not_canonical_camera_contract",
    "openmvs": "external_photo_texturing_camera_mapping_not_canonical_camera_contract",
}
for _name, _pipeline in PIPELINES.items():
    if _name in ("3dgs", "2dgs"):
        _pipeline.update(canonical_camera_input=True, reconstruction_gate="explicit_pose_policy",
                         reconstruction_gates={"solve": "shared_colmap_viability", "locked": "canonical_locked_v1"},
                         reconstruction_gate_limit="Locked input uses validated full-K cameras, image identity, split and reversible scale chain; solve keeps the legacy SfM viability gate. Neither gate certifies geometry accuracy.")
    else:
        _pipeline.update(canonical_camera_input=False, reconstruction_gate="shared_colmap_viability",
                         canonical_camera_error="camera_dataset_mesh_backend_unavailable",
                         canonical_camera_reason=_CANONICAL_DOWNSTREAM_REASONS[_name],
                         canonical_camera_acceptance="Implement the downstream per-view full-K camera adapter, depth/normal conventions, training/source scale transforms and held-out split isolation; verify generated geometry/materials with real backend and native lifecycle tests.",
                         reconstruction_gate_limit="Legacy SfM viability remains enabled. Canonical training outputs are rejected before external scene readers, environment setup, meshing or texture commands; restoring PLY coordinates alone does not supply the missing camera/depth/split adapter.")


def supports_checkpoint(backend):
    return bool(PIPELINES.get(backend, {}).get("checkpoint", False))
