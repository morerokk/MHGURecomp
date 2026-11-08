"""Draw-buffer cursors must not consume persistent ImGui object slots."""
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

source = Path(__file__).resolve().parents[1] / 'tools/imgui_thunks.py'
spec = importlib.util.spec_from_file_location('imgui_thunks', source)
thunks = importlib.util.module_from_spec(spec)
with patch.object(sys, 'argv', [str(source)]):
    spec.loader.exec_module(thunks)


class DrawCursorTest(unittest.TestCase):
    def test_vertex_cursor_is_data_like_index_cursor(self):
        fields = thunks.fields_table('ImDrawList')[2]
        vertex = next(f for f in fields if f[:2] == (48, 64))
        index = next(f for f in fields if f[:2] == (52, 72))
        self.assertEqual(vertex[3:], ('data', None))
        self.assertEqual(vertex[3:], index[3:])

    def test_shared_draw_state_keeps_its_object_mirror(self):
        fields = thunks.fields_table('ImDrawList')[2]
        shared = next(f for f in fields if f[:2] == (44, 56))
        self.assertEqual(shared[3:], ('obj', 'ImDrawListSharedData'))


if __name__ == '__main__':
    unittest.main()
