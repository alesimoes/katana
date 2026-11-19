import unittest
from merge import merge_intervals
class T(unittest.TestCase):
    def test_overlap(self):
        self.assertEqual(merge_intervals([[1, 3], [2, 6], [8, 10], [15, 18]]), [[1, 6], [8, 10], [15, 18]])
    def test_touching(self):
        self.assertEqual(merge_intervals([[1, 4], [4, 5]]), [[1, 5]])
    def test_unsorted_and_input_untouched(self):
        data = [[8, 10], [1, 2], [2, 3]]
        self.assertEqual(merge_intervals(data), [[1, 3], [8, 10]])
        self.assertEqual(data, [[8, 10], [1, 2], [2, 3]])
    def test_empty_and_contained(self):
        self.assertEqual(merge_intervals([]), [])
        self.assertEqual(merge_intervals([[1, 10], [2, 3]]), [[1, 10]])
if __name__ == "__main__":
    unittest.main()
