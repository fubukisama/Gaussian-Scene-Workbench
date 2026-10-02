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

# All generation entry points share source admission; imported geometry without
# an SfM dataset cannot be certified by counts alone (never fabricate a pass).
for _pipeline in PIPELINES.values():
    _pipeline["reconstruction_gate"] = "shared_colmap_viability"
    _pipeline["reconstruction_gate_limit"] = "Minimum viability, not held-out image or metric geometry accuracy; external/synthetic inputs require their own validation."


def supports_checkpoint(backend):
    return bool(PIPELINES.get(backend, {}).get("checkpoint", False))
