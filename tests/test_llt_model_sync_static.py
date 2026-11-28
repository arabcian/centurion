#!/usr/bin/env python3
"""The model tables match LenovoLegionToolkit 2.34.0 (Utils/Compatibility.cs).

Pins the last LLT state the tables were compared with, so the next comparison
starts from a known point: GUI known-model prefixes (gui/src/sysinfo.cpp,
knownModel) and the helpers' machine-type -> series table
(crates/centurion-helpers/src/machine.rs, MACHINE_TYPES).
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

# LLT 2.34.0 AllowedModelsPrefix, minus "ThinkBook" (Centurion refuses ThinkBooks
# in supportedMachine() before knownModel() is asked).
LLT_PREFIXES = {
    "8APU1",
    "17ACH", "17ARH", "17ITH", "17IMH", "17ITL",
    "16ACH", "16ADR", "16AFR", "16AHP", "16APH", "16ARH", "16ARP", "16ARX",
    "16IAH", "16IAX", "16IMH", "16IRH", "16IRX", "16ITH",
    "18IAX", "NX",
    "15ACH", "15AHP", "15ALC", "15IPH", "15AKP", "15APH", "15ARH", "15ARP", "15ASH",
    "15IAH", "15IAX", "15IHU", "15IMH", "15IRH", "15IRX", "15ITH",
    "14AHP", "14APH", "14IRP", "14AKP", "14IRH", "14IAP",
    "G5000", "R9000", "R7000", "Y9000", "Y7000",
    "17IR", "15IR", "15IC", "15IK",
}

# LLT 2.34.0 MachineTypeMap (LegionSeries names mapped to machine.rs Series).
LLT_MACHINE_TYPES = {
    **dict.fromkeys(["83F0", "83F1", "83M0", "83NX", "83N2", "83LY", "83DG", "83EW", "83EG", "83JJ",
                     "82RC", "82RB", "82TB", "83EF", "82RE", "82RD", "83Q7", "83Q6", "83RW", "83VK"], "Legion5"),
    **dict.fromkeys(["83DH", "83EX", "82Y5", "82Y9", "82YA", "83D6"], "LegionSlim5"),
    **dict.fromkeys(["83LT", "83F3", "83DF", "83F2", "83LU", "82WM", "83NN", "82WK", "82JQ"], "LegionPro5"),
    **dict.fromkeys(["83KY", "83FD", "82UH", "82TD", "82N6", "83V9"], "Legion7"),
    **dict.fromkeys(["83RU", "83F5", "83DE", "82WR", "82WQ", "82WS"], "LegionPro7"),
    **dict.fromkeys(["83G0", "83EY"], "Legion9"),
    "83E1": "LegionGo",
}


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


class LltModelSync(unittest.TestCase):
    def test_gui_known_model_prefixes(self):
        src = read("gui/src/sysinfo.cpp")
        body = src[src.index("bool knownModel()"):]
        body = body[body.index("prefixes[] = {"):body.index("};")]
        found = re.findall(r'"([^"]+)"', body)
        self.assertEqual(len(found), len(set(found)), "duplicate prefix")
        self.assertEqual(set(found), LLT_PREFIXES)

    def test_helper_machine_types(self):
        src = read("crates/centurion-helpers/src/machine.rs")
        body = src[src.index("const MACHINE_TYPES"):]
        body = body[:body.index("];")]
        pairs = re.findall(r'\("(\w{4})", Series::(\w+)\)', body)
        self.assertEqual(len(pairs), len({k for k, _ in pairs}), "duplicate machine type")
        self.assertEqual(dict(pairs), LLT_MACHINE_TYPES)

    def test_china_names_agree(self):
        rs = read("crates/centurion-helpers/src/machine.rs")
        cpp = read("gui/src/sysinfo.cpp")
        rs_cn = set(re.findall(r'"(\w+)"', re.search(r"CN_MODELS: &\[&str\] = &\[([^\]]+)\]", rs).group(1)))
        cpp_cn = set(re.search(r"\(\^\|\[\^A-Za-z0-9\]\)\(([^)]+)\)", cpp).group(1).split("|"))
        self.assertEqual(rs_cn, cpp_cn)
        self.assertTrue(rs_cn <= LLT_PREFIXES)

    def test_extra_system_fan(self):
        src = read("crates/centurion-helpers/src/legion_wmi.rs")
        fans = re.search(r"pub const EC_FANS: [^=]+= &\[([^;]+)\];", src).group(1)
        self.assertIn('("sys2", 0x0403_0005)', fans)
        self.assertIn('{"sys2", "System 2"}', read("gui/src/hometab.cpp"))


if __name__ == "__main__":
    unittest.main()
