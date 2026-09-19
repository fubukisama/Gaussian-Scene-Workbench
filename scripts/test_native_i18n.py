"""Regression tests for the build-time localization gate."""
import json
from pathlib import Path
import tempfile
import unittest

import native_i18n as i18n


class CatalogTests(unittest.TestCase):
    def test_continuous_training_preview(self):
        catalog = i18n.load_catalog()
        for source in ('相机解算与稀疏点云', '初始化高斯', '高斯训练 · 优化与密度控制',
                       '处理完成 · 显示最终结果', '准备影像与训练数据',
                       '处理已中断 · 保留最近可用画面', ' · 迭代 %1 / %2',
                       '等待首个三维结果 · 保留当前画面', '定时快照回退',
                       '点云 → 初始化高斯 → 训练优化 → 最终结果 · 可自由观察',
                       '预览不修改源数据', '预览更新失败 · 继续显示上一结果'):
            self.assertEqual(set(catalog[source]), {'zh_CN', 'en_US', 'ja_JP'})
            self.assertEqual(i18n.validate({source: catalog[source]}, {}), [])

    def test_multi_item_lists_and_delete_semantics(self):
        catalog = i18n.load_catalog()
        for source in ('全选', '取消全选', '已选 %1 项', '删除所选记录',
                       '卸载所选模型', '移除所选任务记录',
                       '将处理所选的 %1 项。\n%2',
                       '恢复操作每次只能选择一项；多选可批量删除。'):
            self.assertEqual(set(catalog[source]), {'zh_CN', 'en_US', 'ja_JP'})
            self.assertEqual(i18n.validate({source: catalog[source]}, {}), [])
        for text in catalog['Ctrl 点选多选 · Shift 连选 · Ctrl+A 全选 · Delete 移除所选'].values():
            for shortcut in ('Ctrl', 'Shift', 'Ctrl+A', 'Delete'):
                self.assertIn(shortcut, text)

    def test_gaussian_resident_rendering_tooltip(self):
        catalog = i18n.load_catalog()
        source = '使用缩放、旋转与透明度显示高斯；支持时将属性常驻显存，仅更新深度顺序，不降低精度'
        self.assertEqual(set(catalog[source]), {'zh_CN', 'en_US', 'ja_JP'})
        self.assertEqual(len(set(catalog[source].values())), 3)
        self.assertEqual(i18n.validate({source: catalog[source]}, {}), [])

    def test_window_controls_and_desktop(self):
        catalog = i18n.load_catalog()
        for source in ('桌面', '全屏', '退出全屏', '最大化窗口', '还原窗口',
                       '转到桌面，保留当前文件名和文件类型'):
            self.assertEqual(set(catalog[source]), {'zh_CN', 'en_US', 'ja_JP'})
            self.assertEqual(len(set(catalog[source].values())), 3)
        for text in catalog['F11 切换全屏；Esc 退出全屏；双击系统标题栏最大化或还原'].values():
            self.assertIn('F11', text)
            self.assertIn('Esc', text)
        dock_help = catalog['拖动标题栏移动面板；双击停靠或浮动；Ctrl 拖动保持浮动']
        self.assertEqual(set(dock_help), {'zh_CN', 'en_US', 'ja_JP'})
        for text in dock_help.values():
            self.assertIn('Ctrl', text)

    def test_production_catalog(self):
        self.assertEqual(i18n.validate(i18n.load_catalog(), i18n.sources()), [])

    def test_missing_locale_and_placeholder(self):
        errors = i18n.validate({'Value %1': {'zh_CN': '值 %1', 'en_US': 'Value %2'}}, {})
        self.assertTrue(any('ja_JP' in error for error in errors))
        self.assertTrue(any('Placeholder mismatch' in error for error in errors))

    def test_missing_source_and_literal(self):
        errors = i18n.validate({}, {'New UI': [('test.cpp', 1, True)]})
        self.assertEqual(len(errors), 2)

    def test_functional_syntax(self):
        for source, bad in [('PLY (*.ply)', 'PLY (*.obj)'), ('<b>Title</b>', 'Title')]:
            entries = {source: dict.fromkeys(('zh_CN', 'en_US', 'ja_JP'), bad)}
            self.assertEqual(len(i18n.validate(entries, {})), 3)

    def test_cpp_extraction(self):
        text = 'QCoreApplication::translate("Workbench", "First " "second %1")'
        self.assertEqual(i18n.decode_literals(i18n.CALL.search(text)[1]), 'First second %1')
        self.assertTrue(i18n.requires_translation('New user-facing message'))
        self.assertFalse(i18n.requires_translation('Gaussian Scene Workbench/temporary'))

    def test_duplicate_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'duplicate.json'
            path.write_text('{"key": {}, "key": {}}', encoding='utf-8')
            with self.assertRaises(ValueError):
                i18n.load_catalog(path)

    def test_live_source_keys_are_audited(self):
        text = 'AppLanguage::source("Value " "%1")'
        self.assertEqual(i18n.decode_literals(i18n.CALL.search(text)[1]), 'Value %1')
        self.assertIn('Value %1', i18n.validate({}, {'Value %1': [('ui.cpp', 1, False)]})[0])

    def test_edit_lock_translations_and_shortcut(self):
        catalog = i18n.load_catalog()
        for source in ('锁定编辑工具', '工具已锁定', '已找到并聚焦模型；编辑工具保持锁定',
                       '观察轨迹球', '未命中模型表面，旋转中心保持不变'):
            self.assertEqual(set(catalog[source]), {'zh_CN', 'en_US', 'ja_JP'})
            self.assertEqual(len(set(catalog[source].values())), 3)
        tooltip = next(key for key in catalog if key.startswith('锁定/解锁编辑工具'))
        for text in catalog[tooltip].values():
            self.assertIn('Ctrl+Shift+L', text)

    def test_observation_navigation_controls(self):
        catalog = i18n.load_catalog()
        tooltip = next(key for key in catalog if key.startswith('观察导航：'))
        self.assertEqual(set(catalog[tooltip]), {'zh_CN', 'en_US', 'ja_JP'})
        self.assertEqual(len(set(catalog[tooltip].values())), 3)
        for text in catalog[tooltip].values():
            for token in ('Ctrl+', 'Shift+'):
                self.assertIn(token, text)

    def test_model_export_formats_and_placeholders(self):
        catalog = i18n.load_catalog()
        for source in ('导出模型...', '导出坐标', '场景坐标（应用位移、旋转、缩放）',
                       '模型已导出：%1', 'GLB（glTF 2.0）'):
            self.assertEqual(set(catalog[source]), {'zh_CN', 'en_US', 'ja_JP'})
        for text in catalog['模型已导出：%1'].values():
            self.assertIn('%1', text)
        tooltip = next(key for key in catalog if key.startswith('导出模型文件（PLY'))
        for text in catalog[tooltip].values():
            for token in ('PLY', 'GLB', 'OBJ', 'STL', 'XYZ', 'CSV', 'Ctrl+E'):
                self.assertIn(token, text)


if __name__ == '__main__':
    unittest.main()
