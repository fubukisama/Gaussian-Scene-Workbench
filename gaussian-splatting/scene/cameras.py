#
# Copyright (C) 2023, Inria
# GRAPHDECO research group, https://team.inria.fr/graphdeco
# All rights reserved.
#
# This software is free for non-commercial, research and evaluation use 
# under the terms of the LICENSE.md file.
#
# For inquiries contact  george.drettakis@inria.fr
#

import torch
from torch import nn
import numpy as np
from utils.graphics_utils import getWorld2View2, getProjectionMatrix
from utils.general_utils import PILtoTorch
import cv2
import os

class Camera(nn.Module):
    def __init__(self, resolution, colmap_id, R, T, FoVx, FoVy, depth_params, image, invdepthmap,
                 image_name, uid,
                 trans=np.array([0.0, 0.0, 0.0]), scale=1.0, data_device = "cuda",
                 train_test_exp = False, is_test_dataset = False, is_test_view = False
                 ):
        super(Camera, self).__init__()

        self.uid = uid
        self.colmap_id = colmap_id
        self.R = R
        self.T = T
        self.FoVx = FoVx
        self.FoVy = FoVy
        self.image_name = image_name

        try:
            self.data_device = torch.device(data_device)
        except Exception as e:
            print(e)
            print(f"[Warning] Custom device {data_device} failed, fallback to default cuda device" )
            self.data_device = torch.device("cuda")

        native_images = os.environ.get("GSW_TRAINING_IMAGE_PIPELINE") == "bounded"
        self._gsw_image_payload = None
        storage_device = torch.device("cpu") if native_images else self.data_device
        if native_images:
            from native.worker.training_image_pipeline import HostCameraImage, ImagePipelineConfig
            ImagePipelineConfig.from_environment()  # validate the finite runtime-only policy
            half = ("left" if is_test_dataset else "right") if train_test_exp and is_test_view else None
            self._gsw_image_payload = HostCameraImage.from_pil(image, resolution, torch, masked_half=half)

        if self._gsw_image_payload is None:
            # Legacy/full-resume and higher-bit-depth images retain the old
            # PILtoTorch float contract rather than being down-quantized.
            resized_image_rgb = PILtoTorch(image, resolution)
            gt_image = resized_image_rgb[:3, ...]
            self.alpha_mask = None
            if resized_image_rgb.shape[0] == 4:
                self.alpha_mask = resized_image_rgb[3:4, ...].to(storage_device)
            else:
                self.alpha_mask = torch.ones_like(resized_image_rgb[0:1, ...].to(storage_device))

            if train_test_exp and is_test_view:
                if is_test_dataset:
                    self.alpha_mask[..., :self.alpha_mask.shape[-1] // 2] = 0
                else:
                    self.alpha_mask[..., self.alpha_mask.shape[-1] // 2:] = 0
            self.original_image = gt_image.clamp(0.0, 1.0).to(storage_device)
            self.image_width = self.original_image.shape[2]
            self.image_height = self.original_image.shape[1]
        else:
            self.image_width = self._gsw_image_payload.rgb.shape[2]
            self.image_height = self._gsw_image_payload.rgb.shape[1]

        self.invdepthmap = None
        self.depth_reliable = False
        if invdepthmap is not None:
            self.depth_mask = (torch.ones((1, self.image_height, self.image_width), dtype=torch.float32)
                               if native_images else torch.ones_like(self.alpha_mask))
            self.invdepthmap = cv2.resize(invdepthmap, resolution)
            self.invdepthmap[self.invdepthmap < 0] = 0
            self.depth_reliable = True

            if depth_params is not None:
                if depth_params["scale"] < 0.2 * depth_params["med_scale"] or depth_params["scale"] > 5 * depth_params["med_scale"]:
                    self.depth_reliable = False
                    self.depth_mask *= 0
                
                if depth_params["scale"] > 0:
                    self.invdepthmap = self.invdepthmap * depth_params["scale"] + depth_params["offset"]

            if self.invdepthmap.ndim != 2:
                self.invdepthmap = self.invdepthmap[..., 0]
            self.invdepthmap = torch.from_numpy(self.invdepthmap[None]).to(storage_device)

        if native_images:
            from native.worker.training_image_pipeline import HostCameraImage
            if self._gsw_image_payload is None:
                self._gsw_image_payload = HostCameraImage.from_tensors(
                    self.original_image, self.alpha_mask, self.invdepthmap,
                    getattr(self, "depth_mask", None), torch_module=torch)
                self._gsw_legacy_original_image = None
                self._gsw_legacy_alpha_mask = None
            else:
                payload = self._gsw_image_payload
                self._gsw_image_payload = HostCameraImage(payload.rgb, payload.alpha_mask,
                    self.invdepthmap, getattr(self, "depth_mask", None), payload.bit_depth)

        self.zfar = 100.0
        self.znear = 0.01

        self.trans = trans
        self.scale = scale

        self.world_view_transform = torch.tensor(getWorld2View2(R, T, trans, scale)).transpose(0, 1).cuda()
        self.projection_matrix = getProjectionMatrix(znear=self.znear, zfar=self.zfar, fovX=self.FoVx, fovY=self.FoVy).transpose(0,1).cuda()
        self.full_proj_transform = (self.world_view_transform.unsqueeze(0).bmm(self.projection_matrix.unsqueeze(0))).squeeze(0)
        self.camera_center = self.world_view_transform.inverse()[3, :3]

    @property
    def original_image(self):
        payload = getattr(self, "_gsw_image_payload", None)
        return (payload.rgb_float(torch) if payload is not None
                else self.__dict__.get("_gsw_legacy_original_image"))

    @original_image.setter
    def original_image(self, value):
        self._gsw_legacy_original_image = value

    @property
    def alpha_mask(self):
        payload = getattr(self, "_gsw_image_payload", None)
        return (payload.alpha_float(torch) if payload is not None
                else self.__dict__.get("_gsw_legacy_alpha_mask"))

    @alpha_mask.setter
    def alpha_mask(self, value):
        self._gsw_legacy_alpha_mask = value
        
class MiniCam:
    def __init__(self, width, height, fovy, fovx, znear, zfar, world_view_transform, full_proj_transform):
        self.image_width = width
        self.image_height = height    
        self.FoVy = fovy
        self.FoVx = fovx
        self.znear = znear
        self.zfar = zfar
        self.world_view_transform = world_view_transform
        self.full_proj_transform = full_proj_transform
        view_inv = torch.inverse(self.world_view_transform)
        self.camera_center = view_inv[3][:3]

