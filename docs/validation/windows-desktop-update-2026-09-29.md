# Windows desktop update verification — 2026-09-29

## Installed build

- Desktop branch: `agent/native-desktop-0.3`.
- Application source revision: `e2f600f501bac95813686f1957729df09d094a5d`.
- Build: GitHub Actions run `36381682345`, Windows / Qt 6.8.3 / Release.
- Executable SHA-256: `D819CCDE886BE280BBD67361FF7D86C57F2889BD982CF6D2C7DCF8B04A896C02`.
- Local package: `native/dist/Gaussian-Scene-Workbench-0.3.1-native-preview-win-x64-e2f600f`.
- Native desktop shortcut updated; previous r13 package and shortcut backup retained. Legacy HTML shortcut unchanged.

## Verification

The hosted runner compiled successfully but passed only 33 of 41 tests. Its
bundled software OpenGL implementation rejected GLSL 3.30. The diagnostic
artifact was therefore retested on the workstation using hardware OpenGL;
the CI run remains failed and is not represented as a passing release gate.

Passed locally: application launch, adaptive layout, exit confirmation,
infinite grid, orthographic navigation, all three interface languages,
multi-scene import, Gaussian interaction, observation navigation, processing
preview, training pause/resume, SPZ interchange, regular-mesh reference axes,
dense point-cloud grid, textured paged mesh, and spherical harmonics degrees
0–4. Translation validation and its 14 tests passed. Backend tests: 61 passed,
1 skipped.

The pause/resume smoke test initially crashed because the diagnostic package
omitted `gsw_process_output_fixture.exe`. Building that unchanged helper from
`native/tests/helpers/ProcessOutputFixture.cpp` and placing it beside the app
made the test pass, including a second run. This is a simulated worker test,
not an end-to-end training workload. The SH test used the locally installed
Qt 6.8.3 QtTest runtime, which is not included in the diagnostic artifact.

## Known failure retained

`native_paged_mesh_scene` still fails reproducibly on hardware OpenGL when
both `GSW_MESH_RESIDENT_VERTEX_LIMIT` and `GSW_MESH_RESIDENT_FACE_LIMIT` are set
to `4`. The box fixture reports failed axis occlusion and precise model picking
(exit code 5). The equivalent normal-cache mesh test and the textured paging
test pass. This issue has **not** been fixed or dismissed as a CI limitation.

The user explicitly requested a forced local application update. The package
was installed with this limitation disclosed and the previous version kept
for rollback. Windows application-control policy was not changed or bypassed.
