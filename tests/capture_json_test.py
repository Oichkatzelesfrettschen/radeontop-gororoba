#!/usr/bin/env python3

# Copyright (C) 2012 Lauri Kasanen
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 3 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

import hashlib
import json
import subprocess
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} CAPTURE_TEST", file=sys.stderr)
        return 2

    result = subprocess.run(
        [sys.argv[1], "--emit-json-fixture"],
        check=True,
        capture_output=True,
        text=True,
    )
    lines = result.stdout.splitlines()
    require(len(lines) == 3, "fixture emits exactly three records")

    header_prefix = "# radeontop_capture_v1 "
    evidence_marker = ", evidence_v1 "
    end_prefix = "# radeontop_run_end_v1 "
    require(lines[0].startswith(header_prefix), "capture header prefix")
    require(evidence_marker in lines[1], "evidence marker")
    require(lines[2].startswith(end_prefix), "run-end prefix")

    header = json.loads(lines[0][len(header_prefix) :])
    evidence = json.loads(lines[1].split(evidence_marker, 1)[1])
    run_end = json.loads(lines[2][len(end_prefix) :])

    require(header["build"]["source_state"] == "clean", "source state")
    require(len(header["build"]["source_sha256"]) == 64, "source digest")
    require(len(header["build"]["manifest_sha256"]) == 64, "build digest")
    require(header["build"]["source_manifest_encoding"] == "byte-u00xx",
            "source manifest encoding")
    require(header["build"]["manifest_encoding"] == "byte-u00xx",
            "build manifest encoding")
    source_manifest = header["build"]["source_manifest"].encode("latin-1")
    build_manifest = header["build"]["manifest"].encode("latin-1")
    require(hashlib.sha256(source_manifest).hexdigest() ==
            header["build"]["source_sha256"], "source manifest digest")
    require(hashlib.sha256(build_manifest).hexdigest() ==
            header["build"]["manifest_sha256"], "build manifest digest")
    require(header["argv_encoding"] == "byte-u00xx", "argument encoding")
    require([ord(character) for character in header["argv"][3]] == [
        0x75, 0x74, 0x66, 0x38, 0x3A, 0xC3, 0xA9, 0x3A, 0xFF,
    ], "argument byte round trip")
    require(evidence["run_id"] == header["run_id"], "run identity link")
    require(evidence["signals"]["status"]["supported"] is True,
            "supported status signal")
    require(evidence["signals"]["uvd"]["supported"] is False,
            "unsupported UVD signal")
    require(evidence["clock_means_khz"]["sclk"] == 425000.0,
            "shader clock mean")
    require(evidence["clock_means_khz"]["mclk"] is None,
            "missing memory clock mean")
    require(evidence["endpoints"]["vram"] == {
        "supported": True,
        "valid": False,
        "bytes": None,
    }, "failed VRAM endpoint")
    require(evidence["endpoints"]["gtt"]["supported"] is False,
            "unsupported GTT endpoint")
    require(evidence["terminal"] == {"fatal": True, "read_result": 2},
            "terminal cause")
    require(run_end["run_id"] == header["run_id"], "run-end identity link")
    require(run_end["reason"] == "collector-fatal", "run-end reason")
    require(run_end["logical_complete"] is False, "fatal run completeness")
    require(run_end["logical_status"] == 1, "fatal logical status")
    require(run_end["collector"] == {
        "generation": 0,
        "fatal": True,
        "read_result": 2,
    }, "fatal-before-consumer terminal record")

    print("capture JSON: parsed schema and state distinctions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
