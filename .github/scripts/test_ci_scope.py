import unittest
from ci_scope import classify

class ScopeTest(unittest.TestCase):
    def test_docs_only(self):
        self.assertFalse(any(classify(["README.md", "docs/guide.md"]).values()))
    def test_ui_keeps_platform_build_and_memory_checks(self):
        scope = classify(["src/ui/TimelineActivity.cpp"])
        self.assertTrue(scope["build"] and scope["ui"] and scope["memory"])
        self.assertFalse(scope["tsan"] or scope["fuzz"] or scope["valgrind"])
    def test_threading_keeps_tsan(self):
        self.assertTrue(classify(["src/engine/Sequencer.cpp"])["tsan"])
    def test_codec_keeps_fuzzing(self):
        self.assertTrue(classify(["src/engine/Hap.cpp"])["fuzz"])
    def test_build_rules_validate_everything_except_redundant_valgrind(self):
        scope = classify(["CMakeLists.txt"])
        self.assertTrue(all(v for k,v in scope.items() if k != "valgrind"))
        self.assertFalse(scope["valgrind"])
    def test_nightly_keeps_safety_without_repackaging_three_platforms(self):
        scope = classify([], "schedule")
        self.assertFalse(scope["build"])
        self.assertTrue(all(v for k,v in scope.items() if k != "build"))
    def test_release_runs_everything(self):
        self.assertTrue(all(classify([], ref_type="tag").values()))
    def test_manual_full_runs_everything(self):
        self.assertTrue(all(classify([], "workflow_dispatch", full=True).values()))

if __name__ == "__main__":
    unittest.main()
