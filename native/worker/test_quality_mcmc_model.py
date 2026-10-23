"""Mock-free CPU protocol carrier; NOT the CUDA Graphdeco model acceptance."""
import copy
import unittest

try:
    import torch
except ImportError:
    torch = None
if torch is not None:
    from native.worker.quality_mcmc_model import GSWMCMCBridge


class CPUGaussianModelFacade:
    """Real Parameters/Adam with GSW attr/group protocol, no renderer claims."""
    LAYOUT = (('_xyz', 'xyz'), ('_features_dc', 'f_dc'), ('_features_rest', 'f_rest'),
              ('_opacity', 'opacity'), ('_scaling', 'scaling'), ('_rotation', 'rotation'))

    def __init__(self, count=3):
        self._xyz = torch.nn.Parameter(torch.arange(count*3, dtype=torch.float64).reshape(count, 3))
        self._features_dc = torch.nn.Parameter(torch.ones((count, 1, 3), dtype=torch.float64))
        self._features_rest = torch.nn.Parameter(torch.ones((count, 3, 3), dtype=torch.float64))
        self._opacity = torch.nn.Parameter(torch.logit(torch.tensor([.75]+[.001]*(count-1), dtype=torch.float64))[:, None])
        self._scaling = torch.nn.Parameter(torch.zeros((count, 3), dtype=torch.float64))
        self._rotation = torch.nn.Parameter(torch.tensor([[1., 0., 0., 0.]]*count, dtype=torch.float64))
        self.optimizer_type = 'default'
        self._make_optimizer()
        self.max_radii2D = torch.ones(count, dtype=torch.float64)
        self.xyz_gradient_accum = torch.ones((count, 1), dtype=torch.float64)
        self.denom = torch.ones((count, 1), dtype=torch.float64)
        self.tmp_radii = None
        self.active_sh_degree, self.spatial_lr_scale = 1, 2.
        self._exposure = torch.nn.Parameter(torch.ones((1, 3, 4), dtype=torch.float64))

    def _make_optimizer(self):
        self.optimizer = torch.optim.Adam([{'params': [getattr(self, attr)], 'name': group}
                                           for attr, group in self.LAYOUT], lr=.01)

    def update_learning_rate(self, value):
        next(group for group in self.optimizer.param_groups if group['name'] == 'xyz')['lr'] = value
        return value

    def capture(self):
        return copy.deepcopy({'parameters': {attr: getattr(self, attr).detach().clone() for attr, _ in self.LAYOUT},
            'optimizer': self.optimizer.state_dict(), 'active_sh_degree': self.active_sh_degree,
            'spatial_lr_scale': self.spatial_lr_scale,
            'statistics': {name: getattr(self, name).clone() for name in ('max_radii2D', 'xyz_gradient_accum', 'denom')}})

    def restore(self, data, training_args=None):
        for attr, tensor in data['parameters'].items():
            setattr(self, attr, torch.nn.Parameter(tensor.detach().clone()))
        self._make_optimizer()
        self.optimizer.load_state_dict(copy.deepcopy(data['optimizer']))
        self.active_sh_degree, self.spatial_lr_scale = data['active_sh_degree'], data['spatial_lr_scale']
        for name, tensor in data['statistics'].items():
            setattr(self, name, tensor.clone())


def adam_update(model, bridge):
    loss = sum(getattr(model, attr).square().sum() for attr, _ in model.LAYOUT) + bridge.regularization_loss()
    loss.backward()
    model.optimizer.step()


@unittest.skipIf(torch is None, 'real Torch runtime required')
class GSWMCMCBridgeTests(unittest.TestCase):
    def test_lean_native_checkpoint_rebinds_without_duplicate_parameter_snapshot(self):
        model = CPUGaussianModelFacade()
        bridge = GSWMCMCBridge(model, seed=17, cap=5, noise_lr=.1)
        adam_update(model, bridge)
        bridge.after_optimizer_step(position_lr=.01, refine=True, grow_count=1)
        native = model.capture()
        sidecar = bridge.native_checkpoint_state()
        self.assertNotIn('parameters', sidecar['strategy'])
        self.assertNotIn('optimizer', sidecar['strategy'])
        adam_update(model, bridge)
        bridge.after_optimizer_step(position_lr=.01)
        expected = model.capture()
        model.restore(native)
        bridge.restore_native_checkpoint(sidecar)
        adam_update(model, bridge)
        bridge.after_optimizer_step(position_lr=.01)
        for attr in expected['parameters']:
            self.assertTrue(torch.equal(getattr(model, attr), expected['parameters'][attr]))
        model._xyz.data[0, 0] += 1
        with self.assertRaisesRegex(ValueError, 'native_binding_mismatch'):
            bridge.restore_native_checkpoint(sidecar)

    def test_callback_requires_exactly_one_real_external_adam_step(self):
        model = CPUGaussianModelFacade()
        bridge = GSWMCMCBridge(model, seed=17, cap=5, noise_lr=.1)
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_model_requires_one_external_adam_step'):
            bridge.after_optimizer_step(position_lr=.01)
        adam_update(model, bridge)
        bridge.after_optimizer_step(position_lr=.01)
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_model_requires_one_external_adam_step'):
            bridge.after_optimizer_step(position_lr=.01)

    def test_active_capacity_storage_is_rejected_without_disabling_or_detaching_it(self):
        model = CPUGaussianModelFacade()
        manager = object()
        model._gsw_capacity_storage = manager
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_capacity_storage_adapter_required'):
            GSWMCMCBridge(model, seed=17, cap=5)
        self.assertIs(model._gsw_capacity_storage, manager)
        self.assertEqual([group['name'] for group in model.optimizer.param_groups],
                         ['xyz', 'f_dc', 'f_rest', 'opacity', 'scaling', 'rotation'])

    def test_multiplicity_refusal_is_pre_topology_and_recoverable_from_previous_capture(self):
        model = CPUGaussianModelFacade()
        bridge = GSWMCMCBridge(model, seed=17, cap=10, max_multiplicity=3)
        saved = bridge.capture()
        adam_update(model, bridge)
        after_adam = {attr: getattr(model, attr).detach().clone() for attr, _ in model.LAYOUT}
        rng = bridge.adapter.relocation_generator.get_state().clone()
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_topology_preflight_failed: growth'):
            bridge.after_optimizer_step(position_lr=.01, refine=True, grow_count=7)
        for attr in after_adam:
            self.assertTrue(torch.equal(getattr(model, attr), after_adam[attr]))
        self.assertTrue(torch.equal(rng, bridge.adapter.relocation_generator.get_state()))
        self.assertEqual(model.max_radii2D.shape, (3,))
        self.assertTrue(torch.equal(model.max_radii2D, torch.ones(3, dtype=torch.float64)))
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_model_incomplete_optimizer_step'):
            bridge.capture()
        model.restore(saved['model'])
        bridge.rebind_after_model_restore(saved['bridge'])
        self.assertEqual(bridge.adapter.steps, 0)
        self.assertTrue(torch.equal(model._xyz, saved['model']['parameters']['_xyz']))

    def test_wrong_native_capture_is_not_silently_overwritten_by_a_sidecar(self):
        source = CPUGaussianModelFacade()
        source_bridge = GSWMCMCBridge(source, seed=17, cap=5)
        adam_update(source, source_bridge)
        source_bridge.after_optimizer_step(position_lr=.01)
        saved = source_bridge.state_dict()
        model = CPUGaussianModelFacade()
        bridge = GSWMCMCBridge(model, seed=17, cap=5)
        xyz, optimizer = model._xyz, model.optimizer
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_model_checkpoint_invalid'):
            bridge.rebind_after_model_restore(saved)
        self.assertIs(model._xyz, xyz)
        self.assertIs(model.optimizer, optimizer)

    def test_temporary_radius_dtype_and_all_statistic_lengths_follow_growth(self):
        model = CPUGaussianModelFacade()
        model.tmp_radii = torch.arange(3, dtype=torch.int32)
        bridge = GSWMCMCBridge(model, seed=17, cap=5)
        adam_update(model, bridge)
        bridge.after_optimizer_step(position_lr=.01, refine=True, grow_count=1)
        self.assertEqual(model.tmp_radii.dtype, torch.int32)
        self.assertEqual(model.tmp_radii.shape, (4,))
        self.assertTrue(torch.equal(model.tmp_radii, torch.zeros(4, dtype=torch.int32)))
        self.assertTrue(torch.equal(model.xyz_gradient_accum, torch.zeros((4, 1), dtype=torch.float64)))
        self.assertTrue(torch.equal(model.denom, torch.zeros((4, 1), dtype=torch.float64)))

    def test_native_restore_rebinds_strategy_and_replays_the_next_external_adam_step(self):
        model = CPUGaussianModelFacade()
        bridge = GSWMCMCBridge(model, seed=17, cap=6, noise_lr=.1, noise_threshold=.8)
        adam_update(model, bridge)
        bridge.after_optimizer_step(position_lr=.01, refine=True, grow_count=1)
        checkpoint = bridge.capture()
        restored_model = CPUGaussianModelFacade()
        restored = GSWMCMCBridge(restored_model, seed=17, cap=6, noise_lr=.1, noise_threshold=.8)
        restored_model.restore(checkpoint['model'])
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_model_rebind_required'):
            restored.regularization_loss()
        restored.rebind_after_model_restore(checkpoint['bridge'])
        self.assertEqual(restored_model.active_sh_degree, model.active_sh_degree)
        self.assertEqual(restored_model.spatial_lr_scale, model.spatial_lr_scale)
        adam_update(model, bridge)
        adam_update(restored_model, restored)
        left = bridge.after_optimizer_step(position_lr=.01, refine=True, grow_count=1)
        right = restored.after_optimizer_step(position_lr=.01, refine=True, grow_count=1)
        self.assertEqual(left, right)
        for attr, _ in model.LAYOUT:
            self.assertTrue(torch.equal(getattr(model, attr), getattr(restored_model, attr)))
        self.assertTrue(torch.equal(bridge.adapter.gaussian_ids, restored.adapter.gaussian_ids))

    def test_native_group_names_and_references_survive_without_double_adam(self):
        model = CPUGaussianModelFacade()
        bridge = GSWMCMCBridge(model, seed=17, cap=5, noise_lr=.1)
        exposure = model._exposure
        names = [group['name'] for group in model.optimizer.param_groups]
        adam_update(model, bridge)
        event = bridge.after_optimizer_step(position_lr=.01, refine=True, grow_count=1)
        self.assertFalse(event['optimizer_step'])
        self.assertTrue(event['external_optimizer_step'])
        self.assertEqual([group['name'] for group in model.optimizer.param_groups], names)
        self.assertEqual(len(model._xyz), 4)
        for attr, name in model.LAYOUT:
            parameter = getattr(model, attr)
            group = next(group for group in model.optimizer.param_groups if group['name'] == name)
            self.assertIs(group['params'][0], parameter)
            self.assertEqual(float(model.optimizer.state[parameter]['step']), 1.)
        self.assertIs(model._exposure, exposure)
        self.assertEqual(model.max_radii2D.shape, (4,))
        self.assertEqual(model.xyz_gradient_accum.shape, (4, 1))
        self.assertEqual(model.denom.shape, (4, 1))
        self.assertIsNone(model.tmp_radii)
        self.assertEqual(model.update_learning_rate(.002), .002)


if __name__ == '__main__':
    unittest.main()
