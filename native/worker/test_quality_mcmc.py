"""Real Torch CPU tests at the independent MCMC tensor/optimizer seam."""
import unittest
import copy

try:
    import torch
except ImportError:
    torch = None
if torch is not None:
    from native.worker.quality_mcmc import relocation_parameters, covariance_noise, MCMCAdapter


def fixture(opacities=(.75, .001, .002), cap=8, seed=17, warm=False, optimizer_group_names=None, **kwargs):
    count = len(opacities)
    parameters = {
        'means': torch.nn.Parameter(torch.arange(count*3, dtype=torch.float64).reshape(count, 3)),
        'log_scales': torch.nn.Parameter(torch.zeros((count, 3), dtype=torch.float64)),
        'quaternions': torch.nn.Parameter(torch.tensor([[1., 0., 0., 0.]]*count, dtype=torch.float64).reshape(count, 4)),
        'opacity_logits': torch.nn.Parameter(torch.logit(torch.tensor(opacities, dtype=torch.float64))[:, None]),
        'features': torch.nn.Parameter(torch.arange(count*2, dtype=torch.float64).reshape(count, 2)),
    }
    mapping = optimizer_group_names or {name: name for name in parameters}
    optimizer = torch.optim.Adam([{'params': [value], 'name': mapping[name]} for name, value in parameters.items()], lr=.01)
    if warm:
        sum(value.square().sum() for value in parameters.values()).backward()
        optimizer.step()
        optimizer.zero_grad(set_to_none=True)
        for state in optimizer.state.values():
            state['exp_avg'].fill_(2.)
            state['exp_avg_sq'].fill_(3.)
    return MCMCAdapter(parameters, optimizer, seed=seed, cap=cap,
                       optimizer_group_names=optimizer_group_names, **kwargs)


@unittest.skipIf(torch is None, 'real Torch runtime required')
class MCMCMathTests(unittest.TestCase):
    def test_rotated_covariance_and_opacity_gate_follow_the_declared_convention(self):
        normal = torch.randn((1, 3), dtype=torch.float64, generator=torch.Generator().manual_seed(8))
        delta = covariance_noise(torch.log(torch.tensor([[2., 3., 4.]], dtype=torch.float64)),
            torch.tensor([[2**-.5, 0., 0., 2**-.5]], dtype=torch.float64), torch.zeros((1, 1), dtype=torch.float64),
            position_lr=1., noise_lr=2., generator=torch.Generator().manual_seed(8), opacity_threshold=.5)
        torch.testing.assert_close(delta, normal*torch.tensor([[9., 4., 16.]], dtype=torch.float64), rtol=1e-13, atol=1e-13)
        def norm(alpha):
            return covariance_noise(torch.zeros((1, 3), dtype=torch.float64),
                torch.tensor([[1., 0., 0., 0.]], dtype=torch.float64),
                torch.logit(torch.tensor([[alpha]], dtype=torch.float64)), position_lr=1., noise_lr=1.,
                generator=torch.Generator().manual_seed(8)).norm()
        self.assertGreater(float(norm(.001)), float(norm(.9))*1e20)

    def test_relocation_multiplicity_domain_has_peak_and_integral_checks(self):
        for copies, old_opacity in ((1, .01), (2, .5), (7, .9), (50, .9999)):
            alpha, scale = relocation_parameters(torch.tensor([old_opacity], dtype=torch.float64),
                torch.ones((1, 3), dtype=torch.float64), torch.tensor([copies]))
            x = torch.linspace(-12., 12., 24001, dtype=torch.float64)
            profile = alpha[0]*torch.exp(-x.square()/(2*scale[0, 0].square()))
            integrated = torch.trapz(-torch.expm1(copies*torch.log1p(-profile)), x)
            self.assertAlmostEqual(float(integrated), old_opacity*(2*torch.pi)**.5, places=8)

    def test_covariance_shaped_noise_uses_sigma_not_square_root_sigma(self):
        generator = torch.Generator().manual_seed(41)
        independent = torch.Generator().manual_seed(41)
        normal = torch.randn((1, 3), dtype=torch.float64, generator=independent)
        observed = covariance_noise(torch.log(torch.tensor([[2., 3., 4.]], dtype=torch.float64)),
            torch.tensor([[1., 0., 0., 0.]], dtype=torch.float64),
            torch.zeros((1, 1), dtype=torch.float64), position_lr=.2, noise_lr=10.,
            generator=generator, opacity_threshold=.5)
        # At opacity=threshold=.5 gate=.5, lr*.noise_lr=2: net coefficient=1.
        expected = normal * torch.tensor([[4., 9., 16.]], dtype=torch.float64)
        torch.testing.assert_close(observed, expected, rtol=1e-14, atol=1e-14)

    def test_relocation_preserves_peak_and_one_dimensional_integral(self):
        opacity = torch.tensor([.75], dtype=torch.float64)
        scales = torch.tensor([[2., 3., 4.]], dtype=torch.float64)
        adjusted_opacity, adjusted_scales = relocation_parameters(opacity, scales, torch.tensor([2]))
        self.assertAlmostEqual(float(adjusted_opacity[0]), .5, places=14)
        self.assertAlmostEqual(1. - (1. - float(adjusted_opacity[0]))**2, .75, places=14)
        x = torch.linspace(-30., 30., 10001, dtype=torch.float64)
        before = .75 * torch.exp(-x.square() / 8.)
        one_copy = adjusted_opacity[0] * torch.exp(-x.square() / (2*adjusted_scales[0, 0].square()))
        after = 1. - (1. - one_copy).square()
        self.assertAlmostEqual(float(torch.trapz(before, x)), float(torch.trapz(after, x)), places=10)
        self.assertTrue(torch.equal(adjusted_scales[0] / scales[0],
                                    (adjusted_scales[0, 0] / scales[0, 0]).expand(3)))


@unittest.skipIf(torch is None, 'real Torch runtime required')
class MCMCAdapterTests(unittest.TestCase):
    def test_complete_topology_preflight_rejects_growth_before_any_relocation_commit(self):
        adapter = fixture(cap=10, max_multiplicity=3)
        before = adapter.state_dict()
        # Relocating 2 dead slots to 1 parent needs N=3 (valid); seven later
        # additions into three live parents must need at least N=4 (invalid).
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_topology_preflight_failed: growth'):
            adapter.after_optimizer_step(position_lr=.01, refine=True, grow_count=7)
        after = adapter.state_dict()
        for name in adapter.parameters:
            self.assertTrue(torch.equal(before['parameters'][name], after['parameters'][name]))
        self.assertTrue(torch.equal(before['rng']['relocation'], after['rng']['relocation']))
        self.assertEqual(before['event_index'], after['event_index'])

    def test_explicit_group_mapping_survives_growth_and_checkpoint_without_renaming(self):
        mapping = {'means': 'xyz', 'log_scales': 'scaling', 'quaternions': 'rotation',
                   'opacity_logits': 'opacity', 'features': 'f_dc'}
        adapter = fixture(opacities=(.5, .6, .7), warm=True, optimizer_group_names=mapping)
        names = [group['name'] for group in adapter.optimizer.param_groups]
        adapter.grow(1)
        self.assertEqual([group['name'] for group in adapter.optimizer.param_groups], names)
        saved = adapter.state_dict()
        resumed = fixture(opacities=(.5, .6, .7), optimizer_group_names=mapping)
        resumed.load_state_dict(saved)
        self.assertEqual([group['name'] for group in resumed.optimizer.param_groups], names)
        self.assertEqual(resumed.optimizer_group_names, mapping)
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_checkpoint_identity_mismatch'):
            fixture(opacities=(.5, .6, .7)).load_state_dict(saved)

    def test_checkpoint_identity_and_rng_rejection_are_atomic(self):
        adapter = fixture(warm=True)
        before = adapter.state_dict()
        wrong_version = copy.deepcopy(before)
        wrong_version['version'] = True
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_checkpoint_identity_mismatch'):
            adapter.load_state_dict(wrong_version)
        wrong_rng = copy.deepcopy(before)
        wrong_rng['rng']['noise'] = torch.tensor([1], dtype=torch.uint8)
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_checkpoint_invalid'):
            adapter.load_state_dict(wrong_rng)
        after = adapter.state_dict()
        self.assertTrue(torch.equal(before['rng']['noise'], after['rng']['noise']))
        self.assertTrue(torch.equal(before['parameters']['means'], after['parameters']['means']))

    def test_pending_gradients_cannot_be_mislabeled_as_a_complete_checkpoint(self):
        adapter = fixture()
        adapter.parameters['means'].sum().backward()
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_topology_requires_cleared_gradients'):
            adapter.state_dict()

    def test_step_requires_actual_finite_gradients_and_matching_learning_rate(self):
        adapter = fixture()
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_step_requires_finite_gradients'):
            adapter.step(position_lr=.01)
        adapter.parameters['means'].sum().backward()
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_position_lr_mismatch'):
            adapter.step(position_lr=.02)

    def test_empty_all_dead_no_dead_and_budget_exhaustion_are_explicit(self):
        empty = fixture(opacities=(), cap=4)
        self.assertEqual(empty.relocate()['reason'], 'no_dead')
        self.assertEqual(empty.grow(2)['reason'], 'no_live')
        self.assertEqual(empty.inject_noise(.01)['maximum_norm'], 0.)
        dead = fixture(opacities=(.001, .002))
        self.assertEqual(dead.relocate()['reason'], 'no_live')
        self.assertEqual(dead.grow(2)['reason'], 'no_live')
        live = fixture(opacities=(.4, .8), cap=2)
        before = live.state_dict()['rng']['relocation']
        self.assertEqual(live.relocate()['reason'], 'no_dead')
        self.assertEqual(live.grow(10)['added'], 0)
        self.assertTrue(torch.equal(before, live.state_dict()['rng']['relocation']))

    def test_rejected_multiplicity_does_not_change_pool_or_consume_rng(self):
        adapter = fixture(max_multiplicity=2)
        before = adapter.state_dict()
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_relocation_domain_invalid'):
            adapter.relocate()
        after = adapter.state_dict()
        for name in adapter.parameters:
            self.assertTrue(torch.equal(before['parameters'][name], after['parameters'][name]))
        self.assertTrue(torch.equal(before['rng']['relocation'], after['rng']['relocation']))
        self.assertEqual(before['event_index'], after['event_index'])

    def test_strategy_rng_channels_are_independent_and_snapshot_has_no_aliases(self):
        adapter = fixture()
        before = adapter.state_dict()
        adapter.relocate()
        after_relocation = adapter.state_dict()
        self.assertTrue(torch.equal(before['rng']['noise'], after_relocation['rng']['noise']))
        adapter.inject_noise(.01)
        after_noise = adapter.state_dict()
        self.assertTrue(torch.equal(after_relocation['rng']['relocation'], after_noise['rng']['relocation']))
        self.assertFalse(torch.equal(before['parameters']['means'], after_noise['parameters']['means']))

    def test_growth_requested_without_refinement_is_not_silently_ignored(self):
        adapter = fixture()
        sum(parameter.square().sum() for parameter in adapter.parameters.values()).backward()
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_growth_requires_refine'):
            adapter.step(position_lr=.01, refine=False, grow_count=1)

    def test_corrupt_optimizer_checkpoint_is_rejected_without_changing_live_state(self):
        adapter = fixture(warm=True)
        saved = adapter.state_dict()
        corrupted = copy.deepcopy(saved)
        first = next(iter(corrupted['optimizer']['state'].values()))
        first['step'].fill_(float('nan'))
        before = {name: value.detach().clone() for name, value in adapter.parameters.items()}
        with self.assertRaisesRegex(ValueError, 'quality_mcmc_checkpoint_invalid'):
            adapter.load_state_dict(corrupted)
        for name in before:
            self.assertTrue(torch.equal(before[name], adapter.parameters[name]))
        self.assertTrue(torch.equal(saved['rng']['noise'], adapter.state_dict()['rng']['noise']))

    def test_complete_adapter_checkpoint_replays_topology_noise_and_adam_exactly(self):
        adapter = fixture(opacities=(.6, .7, .001), cap=6, noise_lr=.1, noise_threshold=.8)
        def advance(value):
            with torch.no_grad():
                value.parameters['opacity_logits'][-1] = torch.logit(torch.tensor(.001, dtype=torch.float64))
            loss = sum(parameter.square().sum() for parameter in value.parameters.values()) + value.regularization_loss()
            loss.backward()
            return value.step(position_lr=.01, refine=True, grow_count=1)
        advance(adapter)
        saved = adapter.state_dict()
        resumed = fixture(opacities=(.6, .7, .001), cap=6, noise_lr=.1, noise_threshold=.8)
        resumed.load_state_dict(saved)
        self.assertEqual(len(resumed.parameters['means']), 4)
        for _ in range(3):
            self.assertEqual(advance(adapter), advance(resumed))
            self.assertEqual(adapter.steps, resumed.steps)
            self.assertTrue(torch.equal(adapter.gaussian_ids, resumed.gaussian_ids))
            for name in adapter.parameters:
                self.assertTrue(torch.equal(adapter.parameters[name], resumed.parameters[name]))
                left = adapter.optimizer.state[adapter.parameters[name]]
                right = resumed.optimizer.state[resumed.parameters[name]]
                self.assertEqual(set(left), set(right))
                for key in left:
                    self.assertTrue(torch.equal(left[key], right[key]))

    def test_complete_step_performs_adam_drift_and_isolates_strategy_rng(self):
        global_rng = torch.get_rng_state().clone()
        adapter = fixture(opacities=(.001, .002, .003), noise_lr=.1)
        before = adapter.parameters['means'].detach().clone()
        loss = sum(value.square().sum() for value in adapter.parameters.values()) + adapter.regularization_loss()
        loss.backward()
        event = adapter.step(position_lr=.01, refine=False)
        self.assertEqual(adapter.steps, 1)
        self.assertTrue(event['optimizer_step'])
        self.assertGreater(event['noise']['maximum_norm'], 0.)
        self.assertFalse(torch.equal(adapter.parameters['means'], before))
        self.assertTrue(all(value.grad is None for value in adapter.parameters.values()))
        self.assertTrue(all(float(state['step']) == 1. for state in adapter.optimizer.state.values()))
        self.assertTrue(torch.equal(global_rng, torch.get_rng_state()))

    def test_regularizers_have_actual_opacity_and_scale_parameter_gradients(self):
        adapter = fixture(opacities=(.25, .75), opacity_reg=.2, scale_reg=.3)
        with torch.no_grad():
            adapter.parameters['log_scales'].copy_(torch.log(torch.tensor([[2., 3., 4.]]*2, dtype=torch.float64)))
        loss = adapter.regularization_loss()
        self.assertAlmostEqual(float(loss), 1., places=12)
        loss.backward()
        torch.testing.assert_close(adapter.parameters['opacity_logits'].grad,
                                   torch.full((2, 1), .01875, dtype=torch.float64))
        torch.testing.assert_close(adapter.parameters['log_scales'].grad,
                                   torch.tensor([[.1, .15, .2]]*2, dtype=torch.float64))

    def test_growth_obeys_cap_and_adam_uses_rebound_parameters(self):
        adapter = fixture(opacities=(.5, .6, .7), cap=4, warm=True)
        event = adapter.grow(10)
        self.assertEqual((event['requested'], event['added']), (10, 1))
        self.assertEqual(adapter.gaussian_ids.tolist(), [0, 1, 2, 3])
        parent = event['target_ids'][0]
        for group in adapter.optimizer.param_groups:
            parameter = adapter.parameters[group['name']]
            self.assertIs(group['params'][0], parameter)
            state = adapter.optimizer.state[parameter]
            self.assertEqual(tuple(state['exp_avg'].shape), tuple(parameter.shape))
            self.assertTrue(torch.equal(state['exp_avg'][parent], torch.zeros_like(state['exp_avg'][parent])))
            self.assertTrue(torch.equal(state['exp_avg'][-1], torch.zeros_like(state['exp_avg'][-1])))
        before = adapter.parameters['means'].detach().clone()
        sum(value.square().sum() for value in adapter.parameters.values()).backward()
        adapter.optimizer.step()
        adapter.optimizer.zero_grad(set_to_none=True)
        self.assertFalse(torch.equal(adapter.parameters['means'], before))
        self.assertEqual(adapter.grow(1)['added'], 0)

    def test_dead_relocation_adjusts_family_and_repairs_only_live_parent_moments(self):
        adapter = fixture(warm=True)
        before_peak = float(adapter.parameters['opacity_logits'][0].sigmoid())
        live_position = adapter.parameters['means'][0].detach().clone()
        event = adapter.relocate()
        self.assertEqual(event['relocated'], 2)
        self.assertEqual(event['target_ids'], [0, 0])
        self.assertTrue(torch.equal(adapter.parameters['means'], live_position.expand(3, 3)))
        new_opacity = adapter.parameters['opacity_logits'].sigmoid().detach().flatten()
        self.assertAlmostEqual(float(1.-torch.prod(1.-new_opacity)), before_peak, places=12)
        for parameter in adapter.parameters.values():
            state = adapter.optimizer.state[parameter]
            self.assertTrue(torch.equal(state['exp_avg'][0], torch.zeros_like(state['exp_avg'][0])))
            self.assertTrue(torch.equal(state['exp_avg'][1:], torch.full_like(state['exp_avg'][1:], 2.)))
            self.assertTrue(torch.equal(state['exp_avg_sq'][1:], torch.full_like(state['exp_avg_sq'][1:], 3.)))


if __name__ == '__main__':
    unittest.main()
