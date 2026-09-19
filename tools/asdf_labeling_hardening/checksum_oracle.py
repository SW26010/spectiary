#!/usr/bin/env python3
"""Reproducible ASDF block-checksum interoperability oracle for ADR 0007."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess
import sys
import tempfile
from typing import Any

import asdf
import numpy as np


ASDF_VERSION = "5.3.1"
FORMAT_KIND = "spectiary.sample_labeling"
SCHEMA_VERSION = "2.0.0"
FIXTURE_BUILD_SOURCE_REVISION = "0123456789abcdef0123456789abcdef01234567"
STANDARD_VERSION = "1.5.0"
ZERO_CHECKSUM = bytes(16)


@dataclass(frozen=True)
class Block:
    offset: int
    header_size: int
    fields_offset: int
    payload_offset: int
    allocated_size: int
    used_size: int
    data_size: int
    compression: bytes
    checksum: bytes

    def used_bytes(self, content: bytes) -> bytes:
        return content[self.payload_offset : self.payload_offset + self.used_size]

    def report(self, content: bytes) -> dict[str, Any]:
        used = self.used_bytes(content)
        digest = hashlib.md5(used, usedforsecurity=False).digest()
        checksum_kind = "zero" if self.checksum == ZERO_CHECKSUM else "md5"
        return {
            "offset": self.offset,
            "header_size": self.header_size,
            "compression": self.compression.rstrip(b"\0").decode("ascii") or "none",
            "allocated_size": self.allocated_size,
            "used_size": self.used_size,
            "data_size": self.data_size,
            "checksum_kind": checksum_kind,
            "checksum_hex": self.checksum.hex(),
            "md5_of_on_disk_used_bytes": digest.hex(),
            "checksum_matches_on_disk_used_bytes": (
                None if checksum_kind == "zero" else self.checksum == digest
            ),
        }


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _production_tree(*, include_unknown: bool = False) -> dict[str, Any]:
    tree: dict[str, Any] = {
        "spectiary_build": {
            "source_mode": "head",
            "source_revision": FIXTURE_BUILD_SOURCE_REVISION,
        },
        "format_kind": FORMAT_KIND,
        "schema_version": SCHEMA_VERSION,
        "source_collection": {
            "identity": "sha256-v1:production-source",
            "source_kind": "folder",
            "name": "巡天样本",
            "fingerprint": "sha256-v1:production-fingerprint",
            "sample_count": 3,
        },
        "sample_roster": {
            "identity_kind": "explicit_names",
            "names": np.asarray(
                ["alpha.fits", "星系-β.fits", "échelle-γ.fits"],
                dtype=np.str_,
            ),
        },
        "annotation": {
            "kind": "categorical_integer",
            "alignment": {"mode": "by_index", "target": "sample_roster"},
            "values": np.asarray([-1, 0, 1], dtype=np.int32),
            "missing": {"semantic": "unlabeled", "value": -1},
        },
        "labeling_task": {
            "id": "00000000-0000-4000-8000-000000000001",
            "name": "天体分类",
            "created_at": "2026-08-30T08:00:00.000Z",
            "modified_at": "2026-08-30T08:00:00.000Z",
            "origin": {"kind": "manual"},
            "labels": [
                {"code": 0, "name": "Galaxy", "shortcut": "g"},
                {"code": 1, "name": "Quasar", "shortcut": "q"},
            ],
        },
    }
    if include_unknown:
        tree["future_vendor"] = {
            "retained_policy_probe": "must-not-be-silently-discarded"
        }
    return tree


def _string_list(value: Any) -> list[str]:
    if isinstance(value, list):
        return [str(item) for item in value]
    return [str(item) for item in np.asarray(value).tolist()]


def _semantic_summary(tree: Any) -> dict[str, Any]:
    build_source = tree["spectiary_build"]
    source = tree["source_collection"]
    roster = tree["sample_roster"]
    annotation = tree["annotation"]
    task = tree["labeling_task"]
    origin = task["origin"]
    origin_annotation = origin.get("annotation")
    return {
        "format_kind": str(tree["format_kind"]),
        "schema_version": str(tree["schema_version"]),
        "build_source_mode": str(build_source["source_mode"]),
        "build_source_revision": (
            str(build_source["source_revision"])
            if "source_revision" in build_source
            else None
        ),
        "source_kind": str(source["source_kind"]),
        "source_name": str(source["name"]),
        "source_identity": str(source["identity"]),
        "source_fingerprint": str(source["fingerprint"]),
        "sample_count": int(source["sample_count"]),
        "roster_identity_kind": str(roster["identity_kind"]),
        "sample_names": _string_list(roster["names"]),
        "annotation_kind": str(annotation["kind"]),
        "alignment_mode": str(annotation["alignment"]["mode"]),
        "alignment_target": str(annotation["alignment"]["target"]),
        "missing_semantic": str(annotation["missing"]["semantic"]),
        "missing_value": int(annotation["missing"]["value"]),
        "task_id": str(task["id"]),
        "task_name": str(task["name"]),
        "created_at": str(task["created_at"]),
        "modified_at": str(task["modified_at"]),
        "origin_kind": str(origin["kind"]),
        "origin_annotation": (
            None
            if origin_annotation is None
            else {
                "name": str(origin_annotation["name"]),
                "format": str(origin_annotation["format"]),
                "fingerprint": (
                    str(origin_annotation["fingerprint"])
                    if "fingerprint" in origin_annotation
                    else None
                ),
            }
        ),
        "description": str(task["description"]) if "description" in task else None,
        "authors": [
            {
                "name": str(author["name"]),
                "identifier": (
                    str(author["identifier"]) if "identifier" in author else None
                ),
            }
            for author in task.get("authors", [])
        ],
        "labels": [
            {
                "code": int(label["code"]),
                "name": str(label["name"]),
                "shortcut": str(label.get("shortcut", "")),
            }
            for label in task["labels"]
        ],
        "values": [int(value) for value in np.asarray(annotation["values"]).tolist()],
        "values_dtype": "int32",
        "values_shape": [int(np.asarray(annotation["values"]).size)],
    }


def _write_python(
    path: Path,
    *,
    checksums: bool,
    compression: str | None,
    include_unknown: bool = False,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        path.unlink()
    options: dict[str, Any] = {
        "version": STANDARD_VERSION,
        "include_block_index": False,
        "write_checksums": checksums,
    }
    if compression is not None:
        options["all_array_compression"] = compression
        options["compression_kwargs"] = {"level": 6}
    asdf.AsdfFile(
        _production_tree(include_unknown=include_unknown),
        version=STANDARD_VERSION,
    ).write_to(path, **options)


def _parse_blocks(path: Path) -> tuple[bytes, list[Block]]:
    content = path.read_bytes()
    terminator = content.find(b"\n...\n")
    _require(terminator >= 0, f"{path.name}: missing YAML terminator")
    offset = terminator + len(b"\n...\n")
    blocks: list[Block] = []
    while offset < len(content):
        while offset < len(content) and content[offset] in b"\x00\t\r\n ":
            offset += 1
        if offset == len(content) or content.startswith(b"#ASDF BLOCK INDEX", offset):
            break
        _require(
            content.startswith(b"\xd3BLK", offset),
            f"{path.name}: expected internal block magic at {offset}",
        )
        _require(offset + 54 <= len(content), f"{path.name}: truncated block header")
        header_size = int.from_bytes(content[offset + 4 : offset + 6], "big")
        fields = offset + 6
        _require(header_size >= 48, f"{path.name}: short block header")
        _require(fields + header_size <= len(content), f"{path.name}: block header OOB")
        allocated = int.from_bytes(content[fields + 8 : fields + 16], "big")
        used = int.from_bytes(content[fields + 16 : fields + 24], "big")
        data_size = int.from_bytes(content[fields + 24 : fields + 32], "big")
        payload = fields + header_size
        _require(used <= allocated, f"{path.name}: used_size exceeds allocated_size")
        _require(
            allocated <= len(content) - payload,
            f"{path.name}: allocated payload exceeds file",
        )
        blocks.append(
            Block(
                offset=offset,
                header_size=header_size,
                fields_offset=fields,
                payload_offset=payload,
                allocated_size=allocated,
                used_size=used,
                data_size=data_size,
                compression=content[fields + 4 : fields + 8],
                checksum=content[fields + 32 : fields + 48],
            )
        )
        offset = payload + allocated
    _require(blocks, f"{path.name}: expected at least one internal block")
    return content, blocks


def _file_profile(path: Path) -> dict[str, Any]:
    content, blocks = _parse_blocks(path)
    return {
        "file": path.name,
        "sha256": _sha256(path),
        "size_bytes": len(content),
        "blocks": [block.report(content) for block in blocks],
    }


def _normalize_error(message: str, work_dir: Path) -> str:
    return message.replace(str(work_dir), "<work-dir>").replace(
        str(work_dir).replace("\\", "/"), "<work-dir>"
    )


def _python_read(path: Path, *, validate_checksums: bool, work_dir: Path) -> dict[str, Any]:
    try:
        with asdf.open(
            path,
            validate_checksums=validate_checksums,
            lazy_load=False,
            memmap=False,
        ) as opened:
            summary = _semantic_summary(opened.tree)
        return {"succeeded": True, "error_kind": "None", "message": "", "summary": summary}
    except Exception as error:  # The matrix intentionally probes malformed inputs.
        return {
            "succeeded": False,
            "error_kind": type(error).__name__,
            "message": _normalize_error(str(error), work_dir),
            "summary": None,
        }


def _run_native(native: Path, mode: str, path: Path) -> dict[str, Any]:
    completed = subprocess.run(
        [str(native), mode, str(path)],
        check=False,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        timeout=60,
    )
    record: dict[str, Any] = {
        "command": [native.name, mode, path.name],
        "return_code": completed.returncode,
        "stderr": completed.stderr.strip(),
    }
    if completed.stdout.strip():
        try:
            record["result"] = json.loads(completed.stdout)
        except json.JSONDecodeError:
            record["stdout"] = completed.stdout.strip()
    return record


def _require_native_command(record: dict[str, Any], context: str) -> dict[str, Any]:
    _require(record["return_code"] == 0, f"{context}: native command failed")
    _require("result" in record, f"{context}: native command did not emit JSON")
    return record["result"]


def _require_native_unsupported(record: dict[str, Any], context: str) -> None:
    result = _require_native_command(record, context)
    _require(not result["succeeded"], f"{context}: native reader unexpectedly accepted")
    _require(
        result["error_kind"] == "UnsupportedProfile",
        f"{context}: expected UnsupportedProfile, got {result['error_kind']}",
    )


def _require_python_checksum_mismatch(result: dict[str, Any], context: str) -> None:
    _require(not result["succeeded"], f"{context}: Python unexpectedly accepted")
    _require(
        result["error_kind"] == "ValueError"
        and "does not match given checksum" in result["message"],
        f"{context}: expected an explicit Python checksum mismatch",
    )


def _flip_zlib_trailer(source: Path, destination: Path) -> dict[str, Any]:
    content, blocks = _parse_blocks(source)
    _require(len(blocks) == 2, "zlib corruption probe expects roster and values blocks")
    block = blocks[1]
    _require(block.compression == b"zlib", "zlib corruption probe requires zlib")
    _require(block.used_size > 0, "zlib corruption probe requires used bytes")
    mutated = bytearray(content)
    byte_offset = block.payload_offset + block.used_size - 1
    mutated[byte_offset] ^= 0x01
    destination.write_bytes(mutated)
    return {"block_index": 1, "byte_offset": byte_offset, "xor": 1}


def _replace_first_value(source: Path, destination: Path) -> dict[str, Any]:
    content, blocks = _parse_blocks(source)
    _require(len(blocks) == 2, "value mutation probe expects roster and values blocks")
    block = blocks[1]
    _require(block.compression == bytes(4), "value mutation probe requires uncompressed data")
    _require(block.used_size >= 4, "value mutation probe requires one int32")
    mutated = bytearray(content)
    old_value = struct.unpack_from("<i", mutated, block.payload_offset)[0]
    _require(old_value == -1, "value mutation probe expected canonical missing value")
    struct.pack_into("<i", mutated, block.payload_offset, 0)
    destination.write_bytes(mutated)
    return {
        "block_index": 1,
        "byte_offset": block.payload_offset,
        "old_int32": old_value,
        "new_int32": 0,
        "decoded_stream_remains_legal": True,
    }


def _flip_checksum_field(source: Path, destination: Path) -> dict[str, Any]:
    content, blocks = _parse_blocks(source)
    _require(len(blocks) == 2, "checksum mutation probe expects two blocks")
    mutated = bytearray(content)
    byte_offset = blocks[1].fields_offset + 32
    mutated[byte_offset] ^= 0x01
    destination.write_bytes(mutated)
    return {"block_index": 1, "byte_offset": byte_offset, "xor": 1}


def _run_matrix(native: Path, work_dir: Path) -> dict[str, Any]:
    expected = _semantic_summary(_production_tree())

    native_zero_zlib = work_dir / "native-zero-checksum-zlib.asdf"
    native_write = _run_native(native, "write-explicit-roster-oracle", native_zero_zlib)
    _require(native_write["return_code"] == 0, "native production writer failed")
    native_profile = _file_profile(native_zero_zlib)
    _require(
        all(block["compression"] == "zlib" for block in native_profile["blocks"]),
        "native production writer must emit zlib blocks",
    )
    _require(
        all(block["checksum_kind"] == "zero" for block in native_profile["blocks"]),
        "native production writer must emit zero checksums",
    )
    native_read = _run_native(native, "read-production-checksum-oracle", native_zero_zlib)
    native_read_result = _require_native_command(native_read, "native writer round trip")
    _require(native_read_result["succeeded"], "native writer output should reopen natively")
    _require(
        native_read_result["semantic_matches_production_document"],
        "native writer semantic summary differs from the production document",
    )
    native_python_off = _python_read(
        native_zero_zlib, validate_checksums=False, work_dir=work_dir
    )
    native_python_on = _python_read(
        native_zero_zlib, validate_checksums=True, work_dir=work_dir
    )
    native_expected = dict(expected)
    native_expected["build_source_mode"] = native_python_off["summary"][
        "build_source_mode"
    ]
    native_expected["build_source_revision"] = native_python_off["summary"][
        "build_source_revision"
    ]
    for validation, result in (("disabled", native_python_off), ("enabled", native_python_on)):
        _require(result["succeeded"], f"Python checksum validation {validation} failed")
        _require(
            result["summary"] == native_expected,
            f"Python summary differs with validation {validation}",
        )

    python_cases: dict[str, Any] = {}
    for name, checksums, compression in (
        ("zero-checksum-zlib", False, "zlib"),
        ("nonzero-checksum-uncompressed", True, None),
        ("nonzero-checksum-zlib", True, "zlib"),
    ):
        path = work_dir / f"python-{name}.asdf"
        _write_python(path, checksums=checksums, compression=compression)
        profile = _file_profile(path)
        expected_kind = "md5" if checksums else "zero"
        _require(
            all(block["checksum_kind"] == expected_kind for block in profile["blocks"]),
            f"{name}: Python checksum field did not match the requested profile",
        )
        if checksums:
            _require(
                all(
                    block["checksum_matches_on_disk_used_bytes"] is True
                    for block in profile["blocks"]
                ),
                f"{name}: checksum must cover on-disk used bytes",
            )
        native_case = _run_native(native, "read-production-checksum-oracle", path)
        if checksums:
            _require_native_unsupported(native_case, name)
        else:
            result = _require_native_command(native_case, name)
            _require(result["succeeded"], f"{name}: native reader rejected zero checksum")
            _require(
                result["durable_base_available"],
                f"{name}: production zlib profile did not produce a durable base",
            )
            _require(
                result["semantic_matches_production_document"],
                f"{name}: native semantic mismatch",
            )
        python_off = _python_read(path, validate_checksums=False, work_dir=work_dir)
        python_on = _python_read(path, validate_checksums=True, work_dir=work_dir)
        _require(
            python_off["succeeded"] and python_on["succeeded"],
            f"{name}: Python did not reopen its valid checksum profile",
        )
        _require(
            python_off["summary"] == expected and python_on["summary"] == expected,
            f"{name}: Python writer round-trip semantic mismatch",
        )
        python_cases[name] = {
            "profile": profile,
            "native_read": native_case,
            "python_read": {
                "validation_disabled": python_off,
                "validation_enabled": python_on,
            },
        }

    zero_zlib_corrupt = work_dir / "corrupt-zero-checksum-zlib-payload.asdf"
    zero_zlib_mutation = _flip_zlib_trailer(native_zero_zlib, zero_zlib_corrupt)
    zero_zlib_native = _run_native(
        native, "read-production-checksum-oracle", zero_zlib_corrupt
    )
    zero_zlib_native_result = _require_native_command(
        zero_zlib_native, "zero-checksum zlib payload corruption"
    )
    _require(
        not zero_zlib_native_result["succeeded"]
        and zero_zlib_native_result["error_kind"] == "MalformedDocument",
        "zero-checksum corrupt zlib must be controlled malformed input",
    )
    zero_zlib_off = _python_read(
        zero_zlib_corrupt, validate_checksums=False, work_dir=work_dir
    )
    zero_zlib_on = _python_read(
        zero_zlib_corrupt, validate_checksums=True, work_dir=work_dir
    )
    _require(
        not zero_zlib_off["succeeded"] and not zero_zlib_on["succeeded"],
        "corrupt zero-checksum zlib must fail decompression in Python",
    )

    checksummed_uncompressed = work_dir / "python-nonzero-checksum-uncompressed.asdf"
    nonzero_payload_corrupt = work_dir / "corrupt-nonzero-checksum-payload.asdf"
    nonzero_payload_mutation = _replace_first_value(
        checksummed_uncompressed, nonzero_payload_corrupt
    )
    nonzero_payload_off = _python_read(
        nonzero_payload_corrupt, validate_checksums=False, work_dir=work_dir
    )
    nonzero_payload_on = _python_read(
        nonzero_payload_corrupt, validate_checksums=True, work_dir=work_dir
    )
    _require(
        nonzero_payload_off["succeeded"],
        "disabled checksum validation should expose the still-decodable mutation",
    )
    _require_python_checksum_mismatch(
        nonzero_payload_on,
        "enabled validation of changed checksummed used bytes",
    )
    nonzero_payload_native = _run_native(
        native, "read-production-checksum-oracle", nonzero_payload_corrupt
    )
    _require_native_unsupported(
        nonzero_payload_native, "nonzero-checksum payload corruption"
    )

    checksum_field_corrupt = work_dir / "corrupt-checksum-field.asdf"
    checksum_field_mutation = _flip_checksum_field(
        checksummed_uncompressed, checksum_field_corrupt
    )
    checksum_field_off = _python_read(
        checksum_field_corrupt, validate_checksums=False, work_dir=work_dir
    )
    checksum_field_on = _python_read(
        checksum_field_corrupt, validate_checksums=True, work_dir=work_dir
    )
    _require(checksum_field_off["succeeded"], "disabled validation should ignore checksum field")
    _require_python_checksum_mismatch(
        checksum_field_on,
        "enabled validation of a corrupted checksum field",
    )
    checksum_field_native = _run_native(
        native, "read-production-checksum-oracle", checksum_field_corrupt
    )
    _require_native_unsupported(checksum_field_native, "checksum field corruption")

    zero_uncompressed = work_dir / "python-zero-checksum-uncompressed.asdf"
    _write_python(zero_uncompressed, checksums=False, compression=None)
    legal_used_mutation = work_dir / "zero-checksum-legal-used-byte-mutation.asdf"
    legal_mutation = _replace_first_value(zero_uncompressed, legal_used_mutation)
    legal_python_off = _python_read(
        legal_used_mutation, validate_checksums=False, work_dir=work_dir
    )
    legal_python_on = _python_read(
        legal_used_mutation, validate_checksums=True, work_dir=work_dir
    )
    _require(
        legal_python_off["succeeded"] and legal_python_on["succeeded"],
        "zero checksum must not claim to detect a legal decoded mutation",
    )
    _require(
        legal_python_on["summary"]["values"] == [0, 0, 1],
        "legal mutation should be observable in decoded semantics",
    )
    legal_native = _run_native(native, "read-production-checksum-oracle", legal_used_mutation)
    legal_native_result = _require_native_command(legal_native, "legal used-byte mutation")
    _require(
        legal_native_result["succeeded"],
        "native reader should accept legal zero-checksum data",
    )
    _require(
        legal_native_result["values"] == [0, 0, 1]
        and not legal_native_result["semantic_matches_production_document"],
        "native reader should expose the legal used-byte mutation",
    )

    overwrite_path = work_dir / "checksummed-existing-with-unknown-metadata.asdf"
    _write_python(
        overwrite_path,
        checksums=True,
        compression="zlib",
        include_unknown=True,
    )
    before_bytes = overwrite_path.read_bytes()
    before_sha256 = _sha256(overwrite_path)
    unknown_token = b"retained_policy_probe"
    _require(unknown_token in before_bytes, "unknown metadata probe missing before overwrite")
    overwrite = _run_native(native, "overwrite-production-checksum-oracle", overwrite_path)
    overwrite_result = _require_native_command(overwrite, "checksummed store overwrite")
    _require(
        not overwrite_result["succeeded"]
        and overwrite_result["error_kind"] == "CodecFailure"
        and overwrite_result["codec_error_kind"] == "UnsupportedProfile",
        "store overwrite must preserve the controlled checksum rejection",
    )
    after_bytes = overwrite_path.read_bytes()
    after_sha256 = _sha256(overwrite_path)
    _require(before_bytes == after_bytes, "store overwrite changed checksummed target bytes")
    _require(before_sha256 == after_sha256, "store overwrite changed checksummed target digest")
    _require(unknown_token in after_bytes, "store overwrite discarded unknown metadata")

    return {
        "expected_semantic_summary": expected,
        "native_writer_to_python": {
            "profile": native_profile,
            "native_write": native_write,
            "native_read": native_read,
            "python_read": {
                "validation_disabled": native_python_off,
                "validation_enabled": native_python_on,
            },
            "native_and_python_semantics_equal": (
                native_read_result["semantic_matches_production_document"]
                and native_python_off["summary"]
                == native_python_on["summary"]
                == native_expected
            ),
        },
        "python_writer_to_native": python_cases,
        "corruption": {
            "zero_checksum_zlib_payload": {
                "mutation": zero_zlib_mutation,
                "profile": _file_profile(zero_zlib_corrupt),
                "native_read": zero_zlib_native,
                "python_read": {
                    "validation_disabled": zero_zlib_off,
                    "validation_enabled": zero_zlib_on,
                },
                "classification": "malformed compressed stream; no checksum claim",
            },
            "nonzero_checksum_payload": {
                "mutation": nonzero_payload_mutation,
                "profile": _file_profile(nonzero_payload_corrupt),
                "native_read": nonzero_payload_native,
                "python_read": {
                    "validation_disabled": nonzero_payload_off,
                    "validation_enabled": nonzero_payload_on,
                },
                "classification": "wire-profile checksum mismatch when validation is enabled",
            },
            "checksum_field": {
                "mutation": checksum_field_mutation,
                "profile": _file_profile(checksum_field_corrupt),
                "native_read": checksum_field_native,
                "python_read": {
                    "validation_disabled": checksum_field_off,
                    "validation_enabled": checksum_field_on,
                },
                "classification": "wire-profile checksum mismatch when validation is enabled",
            },
            "zero_checksum_used_bytes_still_decode": {
                "mutation": legal_mutation,
                "profile": _file_profile(legal_used_mutation),
                "native_read": legal_native,
                "python_read": {
                    "validation_disabled": legal_python_off,
                    "validation_enabled": legal_python_on,
                },
                "classification": "accepted changed semantics; zero means no checksum verification",
            },
        },
        "store_overwrite_checksums_and_unknown_metadata": {
            "native_write": overwrite,
            "target_bytes_unchanged": before_bytes == after_bytes,
            "target_sha256_before": before_sha256,
            "target_sha256_after": after_sha256,
            "unknown_mapping_token_present_before": unknown_token in before_bytes,
            "unknown_mapping_token_present_after": unknown_token in after_bytes,
        },
    }


def _write_report(report: dict[str, Any], output: Path | None) -> None:
    text = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text, encoding="utf-8", newline="\n")
    sys.stdout.buffer.write(text.encode("utf-8"))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--native",
        required=True,
        type=Path,
        help="Path to spectiary_sample_labeling_asdf_codec_tests.exe",
    )
    parser.add_argument("--output", type=Path, help="Optional JSON evidence path")
    parser.add_argument(
        "--work-dir",
        type=Path,
        help="Optional directory in which matrix files are retained",
    )
    return parser


def main() -> int:
    arguments = _parser().parse_args()
    native = arguments.native.resolve()
    if not native.is_file():
        raise SystemExit(f"native oracle executable not found: {native}")
    if asdf.__version__ != ASDF_VERSION:
        raise SystemExit(
            f"checksum oracle requires asdf=={ASDF_VERSION}, got {asdf.__version__}"
        )

    report: dict[str, Any] = {
        "format_kind": "spectiary.asdf_labeling.checksum_policy_evidence",
        "oracle_version": 1,
        "reference_versions": {
            "python": platform.python_version(),
            "asdf": asdf.__version__,
            "numpy": np.__version__,
            "asdf_standard": STANDARD_VERSION,
        },
        "host": {"os": platform.system(), "architecture": platform.machine()},
        "native_executable": native.name,
        "policy_under_test": {
            "writer": "emit all-zero checksum",
            "reader": "controlled rejection of nonzero checksum",
        },
    }

    temporary: tempfile.TemporaryDirectory[str] | None = None
    if arguments.work_dir is None:
        temporary = tempfile.TemporaryDirectory(prefix="spectiary-asdf-checksum-")
        work_dir = Path(temporary.name)
    else:
        work_dir = arguments.work_dir.resolve()
        work_dir.mkdir(parents=True, exist_ok=True)

    try:
        report["matrix"] = _run_matrix(native, work_dir)
        report["success"] = True
        report["error"] = None
        return_code = 0
    except Exception as error:  # Preserve partial high-level diagnostics for CI evidence.
        report["success"] = False
        report["error"] = {
            "kind": type(error).__name__,
            "message": _normalize_error(str(error), work_dir),
        }
        return_code = 1
    finally:
        _write_report(report, arguments.output)
        if temporary is not None:
            temporary.cleanup()
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
