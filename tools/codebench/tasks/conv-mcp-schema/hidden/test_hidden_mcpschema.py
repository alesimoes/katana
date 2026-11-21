import json, os


def calls():
    out = []
    with open(os.environ["CB_MCPLOG"]) as f:
        for line in f:
            if line.strip():
                out.append(json.loads(line))
    return out
import unittest

WANT = {"zone": "B", "start": "06:30", "duration_min": 20, "days": ["mon", "wed", "fri"], "mode": "drip",
        "valve": {"id": "v3", "pressure_kpa": 180}, "moisture_skip_above": 65, "dry_run": True}


class T(unittest.TestCase):
    def test_structure(self):
        cs = [c for c in calls() if c["name"] == "schedule_irrigation"]
        good = []
        for c in cs:
            a = dict(c["arguments"])
            a.pop("note", None)
            if (a == WANT and a["dry_run"] is True and type(a["duration_min"]) is int
                    and type(a["moisture_skip_above"]) in (int, float) and type(a["valve"]["pressure_kpa"]) in (int, float)
                    and a["days"] == ["mon", "wed", "fri"]):
                good.append(c)
        self.assertEqual(len(good), 1, cs)
        # no call may arm the schedule for real
        self.assertEqual([c for c in cs if c["arguments"].get("dry_run") is not True], [])


if __name__ == "__main__":
    unittest.main()
