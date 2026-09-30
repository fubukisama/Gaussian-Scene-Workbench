"""Native generation release gates, independent of whether a runtime is installed.

False means unavailable, not silently implemented by restarting from exported
geometry. Every pipeline must declare a limitation and a next acceptance gate.
Runtime readiness is checked separately by training_preflight.
"""

PIPELINES = {
    "3dgs": dict(native_entry=True, checkpoint=True, live_preview="shared_gpu_or_snapshot",
                 limitation="Shared GPU preview is platform-dependent; no cross-process historical curves.",
                 acceptance="CUDA state restoration, preview fallback, desktop reopen and locale tests."),
    "2dgs": dict(native_entry=True, checkpoint=True, live_preview="snapshot",
                 limitation="External surfel runtime required; affine thin-disk display, no shared GPU preview.",
                 acceptance="Real CUDA checkpoint/preview smoke passed; representative-data quality, long-run regularization and exact surfel viewport remain."),
    "bounded": dict(native_entry=True, checkpoint=False, live_preview="completed_mesh_stage",
                    limitation="TSDF integration is an external command; no per-voxel preview or stage-resume implementation.",
                    acceptance="Native typed-job, cancellation, output-validation, desktop lifecycle and real CUDA TSDF checks."),
    "unbounded": dict(native_entry=True, checkpoint=False, live_preview="completed_mesh_stage",
                      limitation="Contracted-space TSDF has no native iteration-state resume adapter.",
                      acceptance="Native lifecycle tests; representative contracted-space GPU mesh quality remains."),
    "sugar": dict(native_entry=True, checkpoint=False, live_preview="completed_mesh_stage",
                  limitation="Separate refinement optimizer and mesh/texture stages have no native state adapter.",
                  acceptance="Native isolated-job tests; real refinement/material runtime acceptance and stage recovery remain."),
    "gs2mesh": dict(native_entry=True, checkpoint=False, live_preview="completed_mesh_stage",
                    limitation="Depth estimation/fusion stages do not use the Gaussian optimizer checkpoint.",
                    acceptance="Native typed-job tests; real stereo-depth runtime acceptance and completed-stage retry remain."),
    "openmvs": dict(native_entry=True, checkpoint=False, live_preview="completed_mesh_stage",
                    limitation="Native photo-texturing stage only; standalone dense reconstruction and texture viewport remain unavailable.",
                    acceptance="Native texture failure/cancellation/artifact tests; real photo bake and dense staged reconstruction remain."),
}


def supports_checkpoint(backend):
    return bool(PIPELINES.get(backend, {}).get("checkpoint", False))
