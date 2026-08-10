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


def parse_fixture(capture_test: str, option: str) -> tuple[dict, list[dict], dict]:
    result = subprocess.run(
        [capture_test, option],
        check=True,
        capture_output=True,
        encoding="ascii",
        text=True,
    )
    lines = result.stdout.splitlines()
    require(len(lines) >= 2, "fixture emits a header and run-end record")

    header_prefix = "# radeontop_capture_v1 "
    evidence_marker = ", evidence_v2 "
    end_prefix = "# radeontop_run_end_v2 "
    require(lines[0].startswith(header_prefix), "capture header prefix")
    require(lines[-1].startswith(end_prefix), "run-end prefix")

    header = json.loads(lines[0][len(header_prefix) :])
    evidence_records = []
    for line in lines[1:-1]:
        require(line.count(evidence_marker) == 1, "one evidence marker per sample")
        evidence_records.append(json.loads(line.split(evidence_marker, 1)[1]))
    run_end = json.loads(lines[-1][len(end_prefix) :])

    require(
        all(record["run_id"] == header["run_id"] for record in evidence_records),
        "evidence run identity link",
    )
    generations = [record["generation"] for record in evidence_records]
    require(
        generations == list(range(1, len(evidence_records) + 1)),
        "contiguous evidence generations",
    )
    require(run_end["run_id"] == header["run_id"], "run-end identity link")
    require(run_end["records"] == len(evidence_records), "footer record count")
    require(
        run_end["last_generation"] == (generations[-1] if generations else 0),
        "footer last generation",
    )
    collector = run_end["collector"]
    require(collector is not None, "terminal fixture collector state")
    require(
        collector["latest_generation"] >= run_end["last_generation"],
        "collector generation covers consumed evidence",
    )
    terminal = collector["terminal"]
    if terminal is not None:
        require(
            terminal["after_generation"] == collector["latest_generation"],
            "terminal binds to collector generation",
        )
    require(
        all("terminal" not in record for record in evidence_records),
        "measurement records omit terminal state",
    )

    return header, evidence_records, run_end


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} CAPTURE_TEST", file=sys.stderr)
        return 2

    device_header, device_evidence, device_run_end = parse_fixture(
        sys.argv[1], "--emit-device-read-before-first-json-fixture"
    )
    publication_header, publication_evidence, publication_run_end = parse_fixture(
        sys.argv[1], "--emit-publication-clock-before-first-json-fixture"
    )
    wait_header, wait_evidence, wait_run_end = parse_fixture(
        sys.argv[1], "--emit-clock-wait-after-generation-one-json-fixture"
    )
    gap_header, gap_evidence, gap_run_end = parse_fixture(
        sys.argv[1], "--emit-generation-gap-with-terminal-json-fixture"
    )
    require(len(wait_evidence) == 1, "wait-clock fixture carries generation one")
    evidence = wait_evidence[0]

    require(device_header["build"]["source_state"] == "clean", "source state")
    require(
        len(device_header["build"]["source_commit"]) in (40, 64), "Git object ID width"
    )
    require(len(device_header["build"]["source_sha256"]) == 64, "source digest")
    require(len(device_header["build"]["manifest_sha256"]) == 64, "build digest")
    require(
        device_header["build"]["source_manifest_encoding"] == "byte-u00xx",
        "source manifest encoding",
    )
    require(
        device_header["build"]["manifest_encoding"] == "byte-u00xx",
        "build manifest encoding",
    )
    source_manifest = device_header["build"]["source_manifest"].encode("latin-1")
    build_manifest = device_header["build"]["manifest"].encode("latin-1")
    require(
        hashlib.sha256(source_manifest).hexdigest()
        == device_header["build"]["source_sha256"],
        "source manifest digest",
    )
    require(
        hashlib.sha256(build_manifest).hexdigest()
        == device_header["build"]["manifest_sha256"],
        "build manifest digest",
    )
    require(device_header["argv_encoding"] == "byte-u00xx", "argument encoding")
    require(
        [ord(character) for character in device_header["argv"][3]]
        == [
            0x75,
            0x74,
            0x66,
            0x38,
            0x3A,
            0xC3,
            0xA9,
            0x3A,
            0xFF,
        ],
        "argument byte round trip",
    )
    require(
        wait_header["run_id"] == device_header["run_id"], "wait fixture run identity"
    )
    require(
        publication_header["run_id"] == device_header["run_id"],
        "publication fixture run identity",
    )
    require(gap_header["run_id"] == device_header["run_id"], "gap run identity")
    require(
        evidence["signals"]["status"]["supported"] is True, "supported status signal"
    )
    require(evidence["signals"]["uvd"]["supported"] is False, "unsupported UVD signal")
    require(evidence["clock_means_khz"]["sclk"] == 425000.0, "shader clock mean")
    require(evidence["clock_means_khz"]["mclk"] is None, "missing memory clock mean")
    require(
        evidence["endpoints"]["vram"]
        == {
            "supported": True,
            "valid": False,
            "bytes": None,
        },
        "failed VRAM endpoint",
    )
    require(
        evidence["endpoints"]["gtt"]["supported"] is False, "unsupported GTT endpoint"
    )
    require("terminal" not in evidence, "measurement omits terminal state")
    require(device_evidence == [], "device failure precedes all evidence")
    require(
        device_run_end["reason"] == "collector-device-error", "device run-end reason"
    )
    require(
        device_run_end["logical_complete"] is False, "device fatal run completeness"
    )
    require(device_run_end["logical_status"] == 1, "device logical status")
    require(
        device_run_end["collector"]
        == {
            "latest_generation": 0,
            "terminal": {
                "after_generation": 0,
                "cause": "device-read",
                "read_result": 2,
            },
        },
        "fatal-before-consumer terminal record",
    )
    require(
        publication_evidence == [], "publication clock failure precedes all evidence"
    )
    require(
        publication_run_end["reason"] == "collector-clock-error",
        "publication-clock run-end reason",
    )
    require(
        publication_run_end["collector"]
        == {
            "latest_generation": 0,
            "terminal": {
                "after_generation": 0,
                "cause": "clock-publication-monotonic",
                "read_result": None,
            },
        },
        "publication-clock terminal record",
    )
    require(
        wait_run_end["reason"] == "collector-clock-error", "wait-clock run-end reason"
    )
    require(wait_run_end["records"] == 1, "wait-clock record count")
    require(wait_run_end["last_generation"] == 1, "wait-clock last generation")
    require(
        wait_run_end["collector"]
        == {
            "latest_generation": 1,
            "terminal": {
                "after_generation": 1,
                "cause": "clock-wait",
                "read_result": None,
            },
        },
        "wait-clock terminal record",
    )
    require(gap_evidence == [], "generation gap precedes retained evidence")
    require(gap_run_end["reason"] == "generation-gap", "generation-gap reason")
    require(gap_run_end["last_generation"] == 0, "generation-gap consumed state")
    require(
        gap_run_end["collector"]
        == {
            "latest_generation": 3,
            "terminal": {
                "after_generation": 3,
                "cause": "clock-wait",
                "read_result": None,
            },
        },
        "generation-gap lock-consistent terminal record",
    )

    print("capture JSON: parsed schema and state distinctions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
