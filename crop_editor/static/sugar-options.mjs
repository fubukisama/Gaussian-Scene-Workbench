export const SUGAR_QUALITY_PRESETS = {
  preview: {
    sugar_quality: "low",
    sugar_refinement_time: "short",
    sugar_regularization: "dn_consistency",
    sugar_surface_level: 0.3,
    sugar_square_size: 8,
    sugar_postprocess: false,
    sugar_max_images: 96,
    sugar_max_image_size: 960,
  },
  balanced: {
    sugar_quality: "low",
    sugar_refinement_time: "medium",
    sugar_regularization: "dn_consistency",
    sugar_surface_level: 0.3,
    sugar_square_size: 10,
    sugar_postprocess: false,
    sugar_max_images: 160,
    sugar_max_image_size: 1280,
  },
  high: {
    sugar_quality: "high",
    sugar_refinement_time: "medium",
    sugar_regularization: "dn_consistency",
    sugar_surface_level: 0.3,
    sugar_square_size: 10,
    sugar_postprocess: false,
    sugar_max_images: 256,
    sugar_max_image_size: 1600,
  },
  ultra: {
    sugar_quality: "high",
    sugar_refinement_time: "long",
    sugar_regularization: "dn_consistency",
    sugar_surface_level: 0.3,
    sugar_square_size: 12,
    sugar_postprocess: false,
    sugar_max_images: 0,
    sugar_max_image_size: 1920,
  },
};

export function sugarOptionsFromPreset(preset) {
  return { ...(SUGAR_QUALITY_PRESETS[preset] || SUGAR_QUALITY_PRESETS.preview) };
}
