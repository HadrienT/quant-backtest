import re

import quantbacktest


def test_version_matches_build_info():
    info = quantbacktest.build_info()
    assert re.fullmatch(r"\d+\.\d+\.\d+", info["version"])
    assert quantbacktest.__version__ == info["version"]


def test_build_info_names_both_commits():
    info = quantbacktest.build_info()
    assert set(info) == {"version", "git_sha", "quantmodeling_sha"}
    assert re.fullmatch(r"[0-9a-f]{40}(-dirty)?|unknown", info["git_sha"])
    # The wheel is built with the hedging module: the pin is a commit.
    assert re.fullmatch(r"[0-9a-f]{40}", info["quantmodeling_sha"])
