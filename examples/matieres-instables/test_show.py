import tempfile
import unittest
from pathlib import Path
from build_show import build

class ShowTest(unittest.TestCase):
    def test_score_timing_and_ownership(self):
        with tempfile.TemporaryDirectory() as directory:
            project,cues=build(Path(directory))
            self.assertEqual(len(project['timelines']),18)
            self.assertEqual(len(project['interactions']),12)
            self.assertEqual(project['layers'][3]['params']['play_mode'],'oneshot')
            self.assertEqual(len(cues),11)
            self.assertEqual([cue['at'] for cue in cues],[0,16,32,48,64,80,85.8,88,98.3,102.8,108])
            self.assertAlmostEqual(sum(cue['duration'] for cue in cues),116.088,places=2)
            self.assertTrue((Path(directory)/'shaders/MatterInstable.fs').is_file())
            for snapshot in project['snapshots']:
                for state in snapshot['layers'][:3]:
                    self.assertFalse(any(key in state['params'] for key in ['param/energy','param/bass','param/brilliance','param/attack']))
                self.assertEqual(len(snapshot['interactions']),12)
            self.assertEqual(project['snapshots'][-1]['composition']['volume'],0)
    def test_one_timeline_variant(self):
        with tempfile.TemporaryDirectory() as directory:
            project,_=build(directory,timeline_count=1)
            self.assertEqual(len(project['timelines']),1)
            self.assertEqual(len(project['timelines'][0]['tracks']),18)

if __name__=='__main__':unittest.main()
