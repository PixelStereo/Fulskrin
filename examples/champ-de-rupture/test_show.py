import tempfile
import unittest
from pathlib import Path
from build_show import build
class ScoreTest(unittest.TestCase):
    def test_cue_ownership_and_spectral_routes(self):
        with tempfile.TemporaryDirectory() as d:
            show,cues=build(d)
            self.assertEqual(len(show['timelines']),18)
            self.assertEqual(len(cues),13)
            self.assertEqual([c['at'] for c in cues],[0,12,28,44,60,76,91,104,116,128,144,160,172])
            self.assertAlmostEqual(sum(c['duration'] for c in cues),180)
            animated={t['tracks'][0]['param'] for t in show['timelines']}
            driven={i['param'] for i in show['interactions']}
            self.assertFalse(animated & driven,'audio and timeline ownership must not fight')
            self.assertEqual(len(show['interactions']),15)
            for i in show['interactions']:
                self.assertEqual(i['source'],'audio')
                self.assertLess(i['audio_low'],i['audio_high'])
            for s in show['snapshots']:
                for layer in s['layers'][:3]:
                    self.assertFalse((animated|driven) & layer['params'].keys(),'cues preserve live motion and spectral modulation')
            self.assertEqual(show['snapshots'][-1]['composition']['volume'],0)
            self.assertTrue((Path(d)/'shaders/ChampRupture.fs').is_file())
    def test_compact_transport(self):
        with tempfile.TemporaryDirectory() as d:
            show,_=build(d,timeline_count=1)
            self.assertEqual(len(show['timelines']),1)
            self.assertEqual(len(show['timelines'][0]['tracks']),18)
if __name__=='__main__':unittest.main()
