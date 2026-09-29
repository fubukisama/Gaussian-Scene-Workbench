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
    "bounded": dict(native_entry=False, checkpoint=False, live_preview="none",
                    limitation="2DGS TSDF meshing remains a legacy backend job, not a native typed job.",
                    acceptance="Native source/options/progress/cancel/output-validation and project-reopen tests."),
    "unbounded": dict(native_entry=False, checkpoint=False, live_preview="none",
                      limitation="Contracted-space TSDF has no native iteration-state resume adapter.",
                      acceptance="Native staged mesh progress, safe cancellation and output validation."),
    "sugar": dict(native_entry=False, checkpoint=False, live_preview="none",
                  limitation="Separate refinement optimizer and mesh/texture stages have no native state adapter.",
                  acceptance="Per-stage state, input identities, native preview, material output and reopen tests."),
    "gs2mesh": dict(native_entry=False, checkpoint=False, live_preview="none",
                    limitation="Depth estimation/fusion stages do not use the Gaussian optimizer checkpoint.",
                    acceptance="Validated stage artifacts, typed native job and retry from completed safe stages."),
    "openmvs": dict(native_entry=False, checkpoint=False, live_preview="none",
                    limitation="External dense reconstruction/meshing/texturing executables require stage-level recovery.",
                    acceptance="Native staged job with point/mesh/texture previews and cancellation/reopen tests."),
}


def supports_checkpoint(backend):
    return bool(PIPELINES.get(backend, {}).get("checkpoint", False))
