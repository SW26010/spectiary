#!/usr/bin/env python3
"""ASDF fixtures and measurements for SpecForge sample-labeling schema 2.0.0."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any, Iterable
import zlib

import asdf
import numpy as np
import psutil


FORMAT_KIND = "specforge.sample_labeling"
SCHEMA_VERSION = "2.0.0"
REFERENCE_STANDARD_VERSION = "1.5.0"
UNLABELED = -1
CREATED_AT = "2026-08-30T08:00:00.000Z"
MODIFIED_AT = "2026-08-30T08:00:00.000Z"
FIXTURE_BUILD_SOURCE_REVISION = "0123456789abcdef0123456789abcdef01234567"
UUID_V4_PATTERN = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"
)


def _task_uuid(value: str) -> str:
    if UUID_V4_PATTERN.fullmatch(value):
        return value
    raw = bytearray(hashlib.sha256(value.encode("utf-8")).digest()[:16])
    raw[6] = (raw[6] & 0x0F) | 0x40
    raw[8] = (raw[8] & 0x3F) | 0x80
    text = raw.hex()
    return f"{text[:8]}-{text[8:12]}-{text[12:16]}-{text[16:20]}-{text[20:]}"


def _is_canonical_timestamp(value: str) -> bool:
    match = re.fullmatch(
        r"(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})\.(\d{3})Z",
        value,
    )
    if match is None:
        return False
    year, month, day, hour, minute, second, _millisecond = (
        int(part) for part in match.groups()
    )
    if month < 1 or month > 12 or hour > 23 or minute > 59 or second > 59:
        return False
    month_days = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    if year % 4 == 0 and (year % 100 != 0 or year % 400 == 0):
        month_days[1] = 29
    return 1 <= day <= month_days[month - 1]


def _is_non_whitespace_text(value: Any) -> bool:
    if not isinstance(value, str):
        return False
    try:
        value.encode("utf-8")
    except UnicodeEncodeError:
        return False
    return any(not character.isspace() for character in value)


def _is_lowercase_token(value: str) -> bool:
    return re.fullmatch(r"[a-z][a-z0-9_]*", value) is not None


def _is_annotation_fingerprint(value: str) -> bool:
    return re.fullmatch(r"sha256:[0-9a-f]{64}", value) is not None


def _is_portable_annotation_origin_name(value: Any) -> bool:
    if not _is_non_whitespace_text(value):
        return False
    if value in {".", ".."} or "/" in value or "\\" in value:
        return False
    return not (
        len(value) >= 2
        and value[1] == ":"
        and ("A" <= value[0] <= "Z" or "a" <= value[0] <= "z")
    )


def _values(value: Any) -> np.ndarray:
    return np.asarray(value, dtype=np.int32)


def _names(value: Any) -> list[str]:
    if isinstance(value, list):
        return [str(item) for item in value]
    return [str(item) for item in np.asarray(value).tolist()]


def _tree(
    *,
    source_kind: str,
    source_name: str,
    source_fingerprint: str,
    values: Iterable[int],
    names: list[str] | None,
    task_id: str,
    task_name: str,
    labels: list[dict[str, Any]],
    created_at: str = CREATED_AT,
    modified_at: str = MODIFIED_AT,
    origin: dict[str, Any] | None = None,
    description: str | None = None,
    authors: list[dict[str, Any]] | None = None,
    build_source_mode: str = "head",
    build_source_revision: str | None = FIXTURE_BUILD_SOURCE_REVISION,
    roster_representation: str = "string_ndarray",
    extra: dict[str, Any] | None = None,
) -> dict[str, Any]:
    array = _values(values)
    roster: dict[str, Any]
    if names is None:
        roster = {"identity_kind": "source_index"}
    else:
        if len(names) != len(array):
            raise ValueError("sample names and values must have equal length")
        if roster_representation == "sequence":
            encoded_names: Any = list(names)
        elif roster_representation == "string_ndarray":
            encoded_names = np.asarray(names, dtype=np.str_)
        else:
            raise ValueError(f"unknown roster representation: {roster_representation}")
        roster = {"identity_kind": "explicit_names", "names": encoded_names}

    tree: dict[str, Any] = {
        "format_kind": FORMAT_KIND,
        "schema_version": SCHEMA_VERSION,
        "specforge_build": {"source_mode": build_source_mode},
        "source_collection": {
            "identity": f"source:{source_fingerprint.removeprefix('sha256:')}",
            "source_kind": source_kind,
            "name": source_name,
            "fingerprint": source_fingerprint,
            "sample_count": int(array.size),
        },
        "sample_roster": roster,
        "annotation": {
            "kind": "categorical_integer",
            "alignment": {"mode": "by_index", "target": "sample_roster"},
            "values": array,
            "missing": {"semantic": "unlabeled", "value": UNLABELED},
        },
        "labeling_task": {
            "id": _task_uuid(task_id),
            "name": task_name,
            "created_at": created_at,
            "modified_at": modified_at,
            "origin": origin if origin is not None else {"kind": "manual"},
            "labels": labels,
        },
    }
    if build_source_revision is not None:
        tree["specforge_build"]["source_revision"] = build_source_revision
    if description is not None:
        tree["labeling_task"]["description"] = description
    if authors:
        tree["labeling_task"]["authors"] = authors
    if extra:
        tree.update(extra)
    return tree


def _write_reference(
    path: Path,
    tree: dict[str, Any],
    *,
    standard_version: str = REFERENCE_STANDARD_VERSION,
    compression: str | None = None,
    compression_level: int | None = None,
    include_block_index: bool = False,
    write_checksums: bool = False,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    af = asdf.AsdfFile(tree, version=standard_version)
    write_options: dict[str, Any] = {
        "version": standard_version,
        "include_block_index": include_block_index,
        "write_checksums": write_checksums,
    }
    if compression is not None:
        write_options["all_array_compression"] = compression
    if compression_level is not None:
        if compression is None:
            raise ValueError("compression_level requires compression")
        write_options["compression_kwargs"] = {"level": compression_level}
    af.write_to(path, **write_options)


def _author_summary(author: Any) -> dict[str, Any]:
    summary = {
        "name": str(author["name"]),
        "identifier": str(author["identifier"]) if "identifier" in author else None,
    }
    if "email" in author:
        email = author["email"]
        if not isinstance(email, str):
            raise TypeError("author email must be a string")
        summary["email"] = email
    return summary


def _semantic_summary(tree: Any) -> dict[str, Any]:
    build_source = tree["specforge_build"]
    if not isinstance(build_source, dict):
        raise TypeError("specforge_build must be a mapping")
    build_source_mode = build_source["source_mode"]
    if not isinstance(build_source_mode, str):
        raise TypeError("specforge_build.source_mode must be a string")
    build_source_revision = (
        build_source["source_revision"]
        if "source_revision" in build_source
        else None
    )
    source = tree["source_collection"]
    roster = tree["sample_roster"]
    annotation = tree["annotation"]
    alignment = annotation["alignment"]
    if not isinstance(alignment, dict):
        raise TypeError("annotation alignment must be a mapping")
    alignment_mode = alignment["mode"]
    alignment_target = alignment["target"]
    if not isinstance(alignment_mode, str) or not isinstance(
        alignment_target, str
    ):
        raise TypeError("annotation alignment fields must be strings")
    task = tree["labeling_task"]
    values = _values(annotation["values"])
    summary: dict[str, Any] = {
        "format_kind": str(tree["format_kind"]),
        "schema_version": str(tree["schema_version"]),
        "build_source_mode": build_source_mode,
        "build_source_revision": build_source_revision,
        "source_kind": str(source["source_kind"]),
        "source_name": str(source["name"]),
        "source_identity": str(source["identity"]),
        "source_fingerprint": str(source["fingerprint"]),
        "sample_count": int(source["sample_count"]),
        "roster_identity_kind": str(roster["identity_kind"]),
        "sample_names": _names(roster["names"]) if "names" in roster else [],
        "annotation_kind": str(annotation["kind"]),
        "alignment_mode": alignment_mode,
        "alignment_target": alignment_target,
        "missing_semantic": str(annotation["missing"]["semantic"]),
        "missing_value": int(annotation["missing"]["value"]),
        "task_id": str(task["id"]),
        "task_name": str(task["name"]),
        "created_at": str(task["created_at"]),
        "modified_at": str(task["modified_at"]),
        "origin_kind": str(task["origin"]["kind"]),
        "origin_annotation": (
            {
                "name": str(task["origin"]["annotation"]["name"]),
                "format": str(task["origin"]["annotation"]["format"]),
                "fingerprint": (
                    str(task["origin"]["annotation"]["fingerprint"])
                    if "fingerprint" in task["origin"]["annotation"]
                    else None
                ),
            }
            if "annotation" in task["origin"]
            else None
        ),
        "description": str(task["description"]) if "description" in task else None,
        "authors": [_author_summary(author) for author in task.get("authors", [])],
        "labels": [
            {
                "code": int(label["code"]),
                "name": str(label["name"]),
                "shortcut": str(label.get("shortcut", "")),
            }
            for label in task["labels"]
        ],
        "values": values.tolist(),
        "values_dtype": str(values.dtype),
        "values_shape": list(values.shape),
    }
    return summary


def _semantic_errors(tree: Any) -> list[str]:
    errors: list[str] = []
    build_source = tree.get("specforge_build") if isinstance(tree, dict) else None
    revision_present = (
        isinstance(build_source, dict) and "source_revision" in build_source
    )
    try:
        summary = _semantic_summary(tree)
    except Exception as exc:  # fixtures intentionally exercise malformed trees
        return [f"required structure: {exc}"]

    if summary["format_kind"] != FORMAT_KIND:
        errors.append("format_kind")
    if summary["schema_version"] != SCHEMA_VERSION:
        errors.append("schema_version")
    revision = summary["build_source_revision"]
    revision_type_valid = not revision_present or isinstance(revision, str)
    if not revision_type_valid:
        errors.append("build source revision")
    if summary["build_source_mode"] == "working_tree":
        if revision_present and revision_type_valid:
            errors.append("build source revision")
    elif summary["build_source_mode"] == "head":
        if revision_type_valid and (
            not revision_present
            or re.fullmatch(r"[0-9a-f]{40}", revision) is None
        ):
            errors.append("build source revision")
    else:
        errors.append("build source mode")
    if summary["values_dtype"] != "int32" or len(summary["values_shape"]) != 1:
        errors.append("values dtype/shape")
    if summary["sample_count"] != len(summary["values"]):
        errors.append("sample_count/value count")
    if summary["roster_identity_kind"] == "explicit_names":
        if len(summary["sample_names"]) != summary["sample_count"]:
            errors.append("roster/value count")
        if len(set(summary["sample_names"])) != len(summary["sample_names"]):
            errors.append("sample names unique")
    elif summary["roster_identity_kind"] != "source_index":
        errors.append("roster identity kind")
    if summary["annotation_kind"] != "categorical_integer":
        errors.append("annotation kind")
    if summary["alignment_mode"] != "by_index":
        errors.append("annotation alignment mode")
    if summary["alignment_target"] != "sample_roster":
        errors.append("annotation alignment target")
    if "name" in tree["annotation"]:
        errors.append("annotation.name is not in SpecForge sample-labeling schema 2.0.0")
    if not UUID_V4_PATTERN.fullmatch(summary["task_id"]):
        errors.append("task id")
    if not _is_non_whitespace_text(summary["task_name"]):
        errors.append("task name")
    if not _is_canonical_timestamp(summary["created_at"]):
        errors.append("created_at")
    if not _is_canonical_timestamp(summary["modified_at"]):
        errors.append("modified_at")
    if summary["created_at"] > summary["modified_at"]:
        errors.append("timestamp order")
    origin = tree["labeling_task"]["origin"]
    origin_kind = summary["origin_kind"]
    origin_annotation = summary["origin_annotation"]
    if not _is_lowercase_token(origin_kind):
        errors.append("origin kind")
    if (origin_kind == "manual" and "annotation" in origin) or (
        origin_kind == "annotation_promotion" and "annotation" not in origin
    ):
        errors.append("origin annotation")
    if origin_annotation is not None:
        if not _is_portable_annotation_origin_name(origin_annotation["name"]):
            errors.append("origin annotation name")
        if origin_annotation["format"] not in {"csv", "npy"}:
            errors.append("origin annotation format")
        fingerprint = origin_annotation["fingerprint"]
        if fingerprint is not None and not _is_annotation_fingerprint(fingerprint):
            errors.append("origin annotation fingerprint")
    description = summary["description"]
    if description is not None:
        try:
            description.encode("utf-8")
        except UnicodeEncodeError:
            errors.append("description")
    for author in summary["authors"]:
        if not _is_non_whitespace_text(author["name"]):
            errors.append("author name")
        if author["identifier"] is not None and not _is_non_whitespace_text(
            author["identifier"]
        ):
            errors.append("author identifier")
        if author.get("email") is not None and not _is_non_whitespace_text(
            author["email"]
        ):
            errors.append("author email")
    if len(summary["authors"]) > 10_000:
        errors.append("author count")
    if summary["missing_semantic"] != "unlabeled" or summary["missing_value"] != UNLABELED:
        errors.append("missing semantics")

    codes = [label["code"] for label in summary["labels"]]
    shortcuts = [label["shortcut"] for label in summary["labels"] if label["shortcut"]]
    if len(codes) != len(set(codes)):
        errors.append("label codes unique")
    if UNLABELED in codes:
        errors.append("label code collides with unlabeled")
    if len(shortcuts) != len(set(shortcuts)):
        errors.append("label shortcuts unique")
    defined = set(codes)
    if any(value != UNLABELED and value not in defined for value in summary["values"]):
        errors.append("values reference defined labels")
    return errors


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _fixture_cases() -> list[dict[str, Any]]:
    labels = [
        {"code": 0, "name": "Galaxy", "shortcut": "g"},
        {"code": 1, "name": "Quasar", "shortcut": "q"},
    ]
    unicode_labels = [
        {"code": 0, "name": "星系", "shortcut": "g"},
        {"code": 1, "name": "类星体", "shortcut": "q"},
    ]
    forward_unknown = _tree(
        source_kind="npy",
        source_name="future.npy",
        source_fingerprint="sha256:future",
        values=[0, 1],
        names=["future-a", "future-b"],
        task_id="future-task",
        task_name="Future fields",
        labels=[dict(label) for label in labels],
        extra={"future_vendor": {"new_flag": True, "new_text": "preserve or ignore"}},
    )
    forward_unknown["labeling_task"]["future_task"] = {
        "token": "task-survives"
    }
    forward_unknown["labeling_task"]["origin"]["future_origin"] = {
        "token": "origin-survives"
    }
    forward_unknown["labeling_task"]["labels"][0]["future_label"] = {
        "token": "label-survives"
    }
    forward_unknown["specforge_build"]["future_build"] = {
        "token": "build-survives"
    }
    return [
        {
            "name": "minimal",
            "valid": True,
            "tree": _tree(
                source_kind="npy",
                source_name="one.npy",
                source_fingerprint="sha256:minimal",
                values=[UNLABELED],
                names=None,
                task_id="minimal-task",
                task_name="Minimal",
                labels=[{"code": 0, "name": "Target", "shortcut": "t"}],
                build_source_mode="working_tree",
                build_source_revision=None,
            ),
        },
        {
            "name": "invalid_build_source_mode",
            "valid": False,
            "expected_error": "build source mode",
            "tree": _tree(
                source_kind="npy",
                source_name="invalid-build-mode.npy",
                source_fingerprint="sha256:invalid-build-mode",
                values=[UNLABELED],
                names=None,
                task_id="invalid-build-mode",
                task_name="Invalid build mode",
                labels=[{"code": 0, "name": "Target", "shortcut": "t"}],
                build_source_mode="archive",
                build_source_revision=None,
            ),
        },
        {
            "name": "invalid_head_missing_revision",
            "valid": False,
            "expected_error": "build source revision",
            "tree": _tree(
                source_kind="npy",
                source_name="head-missing-revision.npy",
                source_fingerprint="sha256:head-missing-revision",
                values=[UNLABELED],
                names=None,
                task_id="head-missing-revision",
                task_name="Head missing revision",
                labels=[{"code": 0, "name": "Target", "shortcut": "t"}],
                build_source_mode="head",
                build_source_revision=None,
            ),
        },
        {
            "name": "invalid_working_tree_revision",
            "valid": False,
            "expected_error": "build source revision",
            "tree": _tree(
                source_kind="npy",
                source_name="working-tree-revision.npy",
                source_fingerprint="sha256:working-tree-revision",
                values=[UNLABELED],
                names=None,
                task_id="working-tree-revision",
                task_name="Working tree revision",
                labels=[{"code": 0, "name": "Target", "shortcut": "t"}],
                build_source_mode="working_tree",
                build_source_revision=FIXTURE_BUILD_SOURCE_REVISION,
            ),
        },
        {
            "name": "invalid_working_tree_null_revision",
            "valid": False,
            "expected_error": "build source revision",
            "tree": _tree(
                source_kind="npy",
                source_name="working-tree-null-revision.npy",
                source_fingerprint="sha256:working-tree-null-revision",
                values=[UNLABELED],
                names=None,
                task_id="working-tree-null-revision",
                task_name="Working tree null revision",
                labels=[{"code": 0, "name": "Target", "shortcut": "t"}],
                build_source_mode="working_tree",
                build_source_revision=None,
            ),
            "mutate": lambda tree: tree["specforge_build"].__setitem__(
                "source_revision", None
            ),
        },
        {
            "name": "unicode",
            "valid": True,
            "tree": _tree(
                source_kind="folder",
                source_name="巡天样本",
                source_fingerprint="sha256:unicode",
                values=[0, UNLABELED, 1],
                names=["星系一.fits", "类星体β.fits", "échelle-γ.fits"],
                task_id="任务-α",
                task_name="天体分类",
                labels=unicode_labels,
                description="跨语言描述：星系分类 🧪",
                authors=[
                    {
                        "name": "Alice",
                        "identifier": "https://orcid.org/0000-0001-2345-6789",
                        "email": "alice@example.org",
                    },
                    {"name": "验证者"},
                ],
            ),
        },
        {
            "name": "folder_roster",
            "valid": True,
            "tree": _tree(
                source_kind="folder",
                source_name="DR17 subset",
                source_fingerprint="sha256:folder",
                values=[0, 1, UNLABELED],
                names=["a/ignored-by-contract.fits", "bravo.fits", "charlie.csv"],
                task_id="folder-task",
                task_name="Folder labels",
                labels=labels,
            ),
        },
        {
            "name": "npy_with_names",
            "valid": True,
            "tree": _tree(
                source_kind="npy",
                source_name="spectra.npy",
                source_fingerprint="sha256:npy-names",
                values=[1, 0, UNLABELED],
                names=["spec-0001", "spec-0002", "spec-0003"],
                task_id="npy-named-task",
                task_name="NPY named labels",
                labels=labels,
            ),
        },
        {
            "name": "npy_source_index",
            "valid": True,
            "tree": _tree(
                source_kind="npy",
                source_name="matrix.npy",
                source_fingerprint="sha256:npy-index",
                values=[UNLABELED, 0, 1, 0],
                names=None,
                task_id="npy-index-task",
                task_name="NPY index labels",
                labels=labels,
            ),
        },
        {
            "name": "mixed_unlabeled",
            "valid": True,
            "tree": _tree(
                source_kind="folder",
                source_name="mixed",
                source_fingerprint="sha256:mixed",
                values=[UNLABELED, 0, UNLABELED, 1, 0, UNLABELED],
                names=[f"sample-{index}.fits" for index in range(6)],
                task_id="mixed-task",
                task_name="Mixed labels",
                labels=labels,
            ),
        },
        {
            "name": "large_values",
            "valid": True,
            "tree": _tree(
                source_kind="npy",
                source_name="large.npy",
                source_fingerprint="sha256:large",
                values=np.where(np.arange(100_000) % 7 == 0, 1, UNLABELED).astype(np.int32),
                names=None,
                task_id="large-task",
                task_name="Large labels",
                labels=labels,
            ),
        },
        {
            "name": "forward_unknown",
            "valid": True,
            "tree": forward_unknown,
            "write_options": {
                "compression": "zlib",
                "compression_level": 6,
            },
        },
        {
            "name": "invalid_count",
            "valid": False,
            "expected_error": "sample_count/value count",
            "tree": _tree(
                source_kind="npy",
                source_name="bad-count.npy",
                source_fingerprint="sha256:bad-count",
                values=[0, 1],
                names=None,
                task_id="bad-count",
                task_name="Bad count",
                labels=labels,
            ),
            "mutate": lambda tree: tree["source_collection"].__setitem__("sample_count", 3),
        },
        {
            "name": "invalid_duplicate_label",
            "valid": False,
            "expected_error": "label codes unique",
            "tree": _tree(
                source_kind="npy",
                source_name="bad-label.npy",
                source_fingerprint="sha256:bad-label",
                values=[0, 1],
                names=None,
                task_id="bad-label",
                task_name="Bad label",
                labels=[{"code": 0, "name": "A"}, {"code": 0, "name": "B"}],
            ),
        },
        {
            "name": "invalid_sentinel_collision",
            "valid": False,
            "expected_error": "label code collides with unlabeled",
            "tree": _tree(
                source_kind="npy",
                source_name="bad-sentinel.npy",
                source_fingerprint="sha256:bad-sentinel",
                values=[UNLABELED],
                names=None,
                task_id="bad-sentinel",
                task_name="Bad sentinel",
                labels=[{"code": UNLABELED, "name": "Collision"}],
            ),
        },
        {
            "name": "invalid_undefined_value",
            "valid": False,
            "expected_error": "values reference defined labels",
            "tree": _tree(
                source_kind="npy",
                source_name="bad-value.npy",
                source_fingerprint="sha256:bad-value",
                values=[0, 42],
                names=None,
                task_id="bad-value",
                task_name="Bad value",
                labels=[{"code": 0, "name": "A"}],
            ),
        },
        {
            "name": "promoted_unicode_origin",
            "valid": True,
            "tree": _tree(
                source_kind="npy",
                source_name="promoted.npy",
                source_fingerprint="sha256:promoted-origin-unicode",
                values=[0],
                names=None,
                task_id="promoted-unicode-origin",
                task_name="Promoted Unicode labels",
                labels=[{"code": 0, "name": "星系"}],
                origin={
                    "kind": "annotation_promotion",
                    "annotation": {
                        "name": "初始标签-😀.csv",
                        "format": "csv",
                    },
                },
            ),
        },
        *[
            {
                "name": fixture_name,
                "valid": False,
                "expected_error": "origin annotation name",
                "tree": _tree(
                    source_kind="npy",
                    source_name="promoted.npy",
                    source_fingerprint="sha256:promoted-origin",
                    values=[0],
                    names=None,
                    task_id=fixture_name,
                    task_name="Promoted labels",
                    labels=[{"code": 0, "name": "A"}],
                    origin={
                        "kind": "annotation_promotion",
                        "annotation": {
                            "name": annotation_name,
                            "format": "csv",
                        },
                    },
                ),
            }
            for fixture_name, annotation_name in (
                ("invalid_origin_dot", "."),
                ("invalid_origin_dotdot", ".."),
                ("invalid_origin_absolute", "/home/user/labels.csv"),
                ("invalid_origin_forward_directory", "folder/labels.csv"),
                ("invalid_origin_backslash_directory", r"folder\labels.csv"),
                ("invalid_origin_drive_prefix", "C:labels.csv"),
            )
        ],
    ]


def generate_fixtures(output: Path) -> dict[str, Any]:
    output.mkdir(parents=True, exist_ok=True)
    manifest: dict[str, Any] = {
        "reference": {
            "asdf": asdf.__version__,
            "asdf_standard_version": REFERENCE_STANDARD_VERSION,
            "schema_version": SCHEMA_VERSION,
        },
        "fixtures": [],
    }
    for case in _fixture_cases():
        tree = case["tree"]
        if "mutate" in case:
            case["mutate"](tree)
        path = output / f"{case['name']}.asdf"
        _write_reference(path, tree, **case.get("write_options", {}))
        summary_path = output / f"{case['name']}.semantic.json"
        summary_path.write_text(
            json.dumps(_semantic_summary(tree), ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        record = {
            "name": case["name"],
            "path": path.name,
            "semantic_path": summary_path.name,
            "structurally_valid": True,
            "semantically_valid": case["valid"],
            "native_profile_supported": case["valid"],
            "expected_error": case.get("expected_error", ""),
            "sha256": _sha256(path),
        }
        manifest["fixtures"].append(record)

    profile_tree = _tree(
        source_kind="folder",
        source_name="wire-profile",
        source_fingerprint="sha256:wire-profile",
        values=[UNLABELED, 0, 1],
        names=["alpha.fits", "星系-β.fits", "gamma.fits"],
        task_id="wire-profile-task",
        task_name="Wire profile",
        labels=[{"code": 0, "name": "A"}, {"code": 1, "name": "B"}],
    )
    for name, write_options in (
        ("profile_zlib", {"compression": "zlib", "compression_level": 6}),
        ("profile_checksum", {"write_checksums": True}),
    ):
        path = output / f"{name}.asdf"
        _write_reference(path, profile_tree, **write_options)
        summary_path = output / f"{name}.semantic.json"
        summary_path.write_text(
            json.dumps(_semantic_summary(profile_tree), ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        manifest["fixtures"].append(
            {
                "name": name,
                "path": path.name,
                "semantic_path": summary_path.name,
                "structurally_valid": True,
                "semantically_valid": True,
                "native_profile_supported": name == "profile_zlib",
                "expected_error": "" if name == "profile_zlib" else "unsupported wire profile",
                "sha256": _sha256(path),
            }
        )

    malformed = {
        "malformed_bad_magic": b"NOT-ASDF\n",
        "malformed_yaml": b"#ASDF 1.0.0\n%YAML 1.1\n--- !core/asdf-1.1.0\nkey: [\n...\n",
    }
    minimal = (output / "minimal.asdf").read_bytes()
    malformed["malformed_truncated_block"] = minimal[:-3]
    corrupt_zlib = bytearray((output / "profile_zlib.asdf").read_bytes())
    _, compressed_blocks = _scan_asdf_blocks(output / "profile_zlib.asdf")
    compressed_block_start = compressed_blocks[0][0]
    compressed_header_size = int.from_bytes(
        corrupt_zlib[compressed_block_start + 4 : compressed_block_start + 6],
        "big",
    )
    compressed_payload_start = compressed_block_start + 6 + compressed_header_size
    corrupt_zlib[compressed_payload_start] ^= 0xFF
    malformed["malformed_zlib_payload"] = bytes(corrupt_zlib)
    for name, content in malformed.items():
        path = output / f"{name}.asdf"
        path.write_bytes(content)
        manifest["fixtures"].append(
            {
                "name": name,
                "path": path.name,
                "semantic_path": "",
                "structurally_valid": False,
                "semantically_valid": False,
                "native_profile_supported": False,
                "expected_error": "structural",
                "sha256": _sha256(path),
            }
        )

    manifest_path = output / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest


def validate_fixtures(fixtures: Path) -> dict[str, Any]:
    manifest = json.loads((fixtures / "manifest.json").read_text(encoding="utf-8"))
    results: list[dict[str, Any]] = []
    for record in manifest["fixtures"]:
        path = fixtures / record["path"]
        actual_hash = _sha256(path)
        if actual_hash != record["sha256"]:
            raise RuntimeError(f"fixture hash mismatch: {path}")
        if not record["structurally_valid"]:
            try:
                with asdf.open(path) as opened:
                    # ASDF opens array blocks lazily, so semantic projection is
                    # also the structural hydration probe for truncated blocks.
                    _semantic_summary(opened.tree)
            except Exception as exc:
                results.append({"name": record["name"], "status": "expected-structural-error", "detail": str(exc)})
                continue
            raise RuntimeError(f"malformed fixture unexpectedly opened: {path}")

        with asdf.open(path, lazy_load=False, memmap=False) as opened:
            errors = _semantic_errors(opened.tree)
            summary = _semantic_summary(opened.tree)
        expected_summary = json.loads((fixtures / record["semantic_path"]).read_text(encoding="utf-8"))
        if summary != expected_summary:
            raise RuntimeError(f"semantic equality failed: {path}")
        if record["semantically_valid"] and errors:
            raise RuntimeError(f"valid fixture rejected: {path}: {errors}")
        if not record["semantically_valid"] and record["expected_error"] not in errors:
            raise RuntimeError(f"invalid fixture missed expected error: {path}: {errors}")
        results.append({"name": record["name"], "status": "valid" if not errors else "expected-semantic-error", "errors": errors})
    return {"asdf": asdf.__version__, "fixture_count": len(results), "results": results}


def _metadata_text(path: Path) -> str:
    content = path.read_bytes()
    marker = content.find(b"...\n")
    if marker < 0:
        raise RuntimeError(f"ASDF YAML terminator missing: {path}")
    return content[: marker + 4].decode("utf-8")


def missing_value_experiment(output: Path) -> dict[str, Any]:
    output.mkdir(parents=True, exist_ok=True)
    base = output / "mask-sentinel.asdf"
    rewrite = output / "mask-sentinel-rewrite.asdf"
    values = np.asarray([UNLABELED, 0, 1, UNLABELED], dtype=np.int32)
    _write_reference(base, {"values": values})
    content = base.read_bytes()
    needle = b"  datatype: int32\n"
    if needle not in content:
        raise RuntimeError("could not locate ndarray datatype in reference file")
    base.write_bytes(content.replace(needle, needle + b"  mask: -1\n", 1))
    with asdf.open(base, memmap=False) as opened:
        array = opened["values"]
        recognized_mask = np.asarray(array.mask, dtype=bool).tolist()
        reopened_values = np.asarray(array, dtype=np.int32).tolist()
        opened.write_to(rewrite, include_block_index=False, write_checksums=False)
    rewrite_text = _metadata_text(rewrite)
    with asdf.open(rewrite, memmap=False) as reopened:
        rewritten = reopened["values"]
        rewritten_has_mask = hasattr(rewritten, "mask")

    explicit = output / "explicit-missing.asdf"
    explicit_rewrite = output / "explicit-missing-rewrite.asdf"
    tree = _tree(
        source_kind="npy",
        source_name="missing.npy",
        source_fingerprint="sha256:missing",
        values=values,
        names=None,
        task_id="missing-task",
        task_name="Missing experiment",
        labels=[{"code": 0, "name": "A"}, {"code": 1, "name": "B"}],
    )
    _write_reference(explicit, tree)
    with asdf.open(explicit, lazy_load=False, memmap=False) as opened:
        explicit_summary = _semantic_summary(opened.tree)
        _write_reference(explicit_rewrite, dict(opened.tree))
    with asdf.open(explicit_rewrite, lazy_load=False, memmap=False) as reopened:
        explicit_rewrite_summary = _semantic_summary(reopened.tree)

    return {
        "asdf": asdf.__version__,
        "scalar_mask": {
            "recognized_mask": recognized_mask,
            "values": reopened_values,
            "expected_mask": [True, False, False, True],
            "rewrite_contains_scalar_mask": "mask: -1" in rewrite_text,
            "rewrite_has_any_mask": rewritten_has_mask,
            "semantic_preserved": rewritten_has_mask,
        },
        "explicit_missing_field": {
            "semantic_preserved": explicit_summary == explicit_rewrite_summary,
            "missing_value": explicit_rewrite_summary["missing_value"],
            "values": explicit_rewrite_summary["values"],
        },
        "recommendation": "explicit_missing_field",
    }


def wire_version_experiment(output: Path) -> dict[str, Any]:
    output.mkdir(parents=True, exist_ok=True)
    tree = _tree(
        source_kind="folder",
        source_name="版本",
        source_fingerprint="sha256:versions",
        values=[UNLABELED, 0],
        names=["α.fits", "星系.fits"],
        task_id="version-task",
        task_name="版本测试",
        labels=[{"code": 0, "name": "星系", "shortcut": "g"}],
    )
    records: list[dict[str, Any]] = []
    for version in [str(item) for item in asdf.versioning.supported_versions]:
        path = output / f"standard-{version}.asdf"
        try:
            _write_reference(path, tree, standard_version=version)
            with asdf.open(path, lazy_load=False, memmap=False) as opened:
                errors = _semantic_errors(opened.tree)
                equal = _semantic_summary(opened.tree) == _semantic_summary(tree)
            metadata = _metadata_text(path)
            ndarray_line = next(line.strip() for line in metadata.splitlines() if "values: !core/ndarray" in line)
            root_line = next(line.strip() for line in metadata.splitlines() if line.startswith("---"))
            records.append({
                "standard_version": version,
                "roundtrip": equal and not errors,
                "root_tag": root_line,
                "ndarray_tag": ndarray_line.split("!", 1)[1],
            })
        except Exception as exc:
            records.append({"standard_version": version, "roundtrip": False, "error": str(exc)})
    return {
        "asdf": asdf.__version__,
        "supported": records,
        "selected_baseline": REFERENCE_STANDARD_VERSION,
        "selection_note": "ASDF Standard 1.5.0 is the maintained stable baseline; 1.6.0 adds no wire construct required by SpecForge sample-labeling schema 2.0.0.",
    }


def _case_names(profile: str, count: int) -> list[str] | None:
    if profile == "none":
        return None
    if profile == "ascii":
        return [f"sample-{index:07d}.fits" for index in range(count)]
    if profile == "variable_unicode":
        return [f"观测-{index:07d}-{'长' * (index % 11)}-αβ.fits" for index in range(count)]
    raise ValueError(profile)


def _case_values(profile: str, count: int) -> np.ndarray:
    indexes = np.arange(count, dtype=np.int64)
    if profile == "mostly_unlabeled":
        return np.where(indexes % 20 == 0, indexes % 2, UNLABELED).astype(np.int32)
    if profile == "mostly_labeled":
        return np.where(indexes % 20 == 0, UNLABELED, indexes % 2).astype(np.int32)
    raise ValueError(profile)


def _fsync(path: Path) -> None:
    descriptor = os.open(path, os.O_RDWR)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


ASDF_BLOCK_MAGIC = b"\xd3BLK"
ASDF_BLOCK_HEADER_SIZE = 48
ASDF_BLOCK_INDEX_HEADER = b"#ASDF BLOCK INDEX\n%YAML 1.1\n---\n"
COPY_CHUNK_SIZE = 1024 * 1024


def _measurement(action: Any) -> dict[str, float]:
    wall_start = time.perf_counter()
    cpu_start = time.process_time()
    action()
    return {
        "wall_seconds": time.perf_counter() - wall_start,
        "cpu_seconds": time.process_time() - cpu_start,
    }


def _atomic_write_bytes(temporary: Path, target: Path, payload: bytes) -> None:
    with temporary.open("wb") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, target)


def _scan_asdf_blocks(path: Path) -> tuple[bytes, list[tuple[int, int]]]:
    """Return the YAML prefix and byte ranges of internal ASDF blocks."""
    with path.open("rb") as stream:
        prefix_parts: list[bytes] = []
        while True:
            line = stream.readline()
            if not line:
                raise RuntimeError(f"ASDF YAML terminator missing: {path}")
            prefix_parts.append(line)
            if line.rstrip(b"\r\n") == b"...":
                break

        blocks: list[tuple[int, int]] = []
        while True:
            start = stream.tell()
            magic = stream.read(4)
            if not magic:
                break
            if magic == ASDF_BLOCK_INDEX_HEADER[:4]:
                stream.seek(start)
                index = stream.read()
                if not index.startswith(
                    ASDF_BLOCK_INDEX_HEADER
                ) or not index.endswith(b"...\n"):
                    raise RuntimeError(f"invalid ASDF block index: {path}")
                entries = index[len(ASDF_BLOCK_INDEX_HEADER) : -4].splitlines()
                try:
                    offsets = [
                        int(entry.removeprefix(b"- "))
                        for entry in entries
                        if entry
                    ]
                except ValueError as error:
                    raise RuntimeError(f"invalid ASDF block index offset: {path}") from error
                if any(not entry.startswith(b"- ") for entry in entries) or offsets != [
                    offset for offset, _ in blocks
                ]:
                    raise RuntimeError(f"stale ASDF block index: {path}")
                break
            if magic != ASDF_BLOCK_MAGIC:
                raise RuntimeError(f"unexpected data after ASDF YAML tree: {path}")
            encoded_header_size = stream.read(2)
            if len(encoded_header_size) != 2:
                raise RuntimeError(f"truncated ASDF block header size: {path}")
            header_size = int.from_bytes(encoded_header_size, "big")
            header = stream.read(header_size)
            if header_size < ASDF_BLOCK_HEADER_SIZE or len(header) != header_size:
                raise RuntimeError(f"invalid ASDF block header: {path}")
            allocated = int.from_bytes(header[8:16], "big")
            stream.seek(allocated, os.SEEK_CUR)
            if stream.tell() > path.stat().st_size:
                raise RuntimeError(f"truncated ASDF block payload: {path}")
            blocks.append((start, 6 + header_size + allocated))
    return b"".join(prefix_parts), blocks


def _asdf_block_compressions(path: Path) -> list[str]:
    content = path.read_bytes()
    _, blocks = _scan_asdf_blocks(path)
    result: list[str] = []
    for offset, _ in blocks:
        code = content[offset + 10 : offset + 14]
        result.append("none" if code == bytes(4) else code.decode("ascii"))
    return result


def _asdf_raw_block(path: Path, block_index: int) -> bytes:
    content = path.read_bytes()
    _, blocks = _scan_asdf_blocks(path)
    offset, size = blocks[block_index]
    return content[offset : offset + size]


def _encoded_asdf_block(payload: bytes, compression: str | None, compression_level: int | None) -> bytes:
    if compression is None:
        encoded = payload
        compression_code = b"\0\0\0\0"
    elif compression == "zlib":
        encoded = zlib.compress(payload, level=compression_level if compression_level is not None else -1)
        compression_code = b"zlib"
    else:
        raise ValueError(f"unsupported second-round compression: {compression}")

    header = bytearray()
    header.extend((0).to_bytes(4, "big"))
    header.extend(compression_code)
    header.extend(len(encoded).to_bytes(8, "big"))
    header.extend(len(encoded).to_bytes(8, "big"))
    header.extend(len(payload).to_bytes(8, "big"))
    header.extend(bytes(16))
    if len(header) != ASDF_BLOCK_HEADER_SIZE:
        raise AssertionError("ASDF block header size mismatch")
    return ASDF_BLOCK_MAGIC + ASDF_BLOCK_HEADER_SIZE.to_bytes(2, "big") + bytes(header) + encoded


def _copy_file_range(source: Any, destination: Any, offset: int, length: int) -> None:
    source.seek(offset)
    remaining = length
    while remaining:
        chunk = source.read(min(remaining, COPY_CHUNK_SIZE))
        if not chunk:
            raise RuntimeError("source ASDF block ended while copying")
        destination.write(chunk)
        remaining -= len(chunk)


def _atomic_reuse_asdf_roster_block(
    source: Path,
    temporary: Path,
    target: Path,
    prefix: bytes,
    roster_block: tuple[int, int],
    values: np.ndarray,
    compression: str | None,
    compression_level: int | None,
) -> None:
    values_payload = np.asarray(values, dtype="<i4").tobytes(order="C")
    values_block = _encoded_asdf_block(values_payload, compression, compression_level)
    with source.open("rb") as old_stream, temporary.open("wb") as new_stream:
        new_stream.write(prefix)
        _copy_file_range(old_stream, new_stream, roster_block[0], roster_block[1])
        new_stream.write(values_block)
        new_stream.flush()
        os.fsync(new_stream.fileno())
    os.replace(temporary, target)


def _json_document(names: list[str], values: list[int]) -> dict[str, Any]:
    # JSON needs application fields for information carried natively by ASDF's
    # ndarray tag. Keeping them in the candidate makes this a semantic, not
    # merely byte-count, comparison.
    return {
        "format_kind": FORMAT_KIND,
        "schema_version": SCHEMA_VERSION,
        "specforge_build": {
            "source_mode": "head",
            "source_revision": FIXTURE_BUILD_SOURCE_REVISION,
        },
        "source_collection": {
            "identity": "source:benchmark",
            "source_kind": "folder",
            "name": "benchmark",
            "fingerprint": "sha256:benchmark",
            "sample_count": len(values),
        },
        "sample_roster": {
            "identity_kind": "explicit_names",
            "names": names,
        },
        "annotation": {
            "kind": "categorical_integer",
            "name": "Benchmark",
            "alignment": {"mode": "by_index", "target": "sample_roster"},
            "values": values,
            "values_dtype": "int32",
            "values_shape": [len(values)],
            "missing": {"semantic": "unlabeled", "value": UNLABELED},
        },
        "labeling_task": {
            "id": "benchmark-task",
            "name": "Benchmark",
            "labels": [{"code": 0, "name": "A"}, {"code": 1, "name": "B"}],
        },
    }


def _encode_json_document(document: dict[str, Any], compression: str | None, compression_level: int | None) -> bytes:
    raw = json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if compression is None:
        return raw
    if compression != "zlib":
        raise ValueError(f"unsupported JSON compression: {compression}")
    return zlib.compress(raw, level=compression_level if compression_level is not None else -1)


def _changed_values(values: Any, change_count: int) -> tuple[Any, dict[str, float]]:
    changed = values.copy()
    count = len(changed)
    actual_change_count = min(change_count, count)
    indexes = np.linspace(0, count - 1, actual_change_count, dtype=np.int64).tolist()

    def apply_changes() -> None:
        for index in indexes:
            changed[index] = 0 if changed[index] == UNLABELED else UNLABELED

    return changed, _measurement(apply_changes)


def _second_round_hydration(
    path: Path,
    format_name: str,
    compression: str | None,
    count: int,
    expected_first: int,
) -> None:
    if format_name == "json":
        payload = path.read_bytes()
        if compression == "zlib":
            payload = zlib.decompress(payload)
        document = json.loads(payload.decode("utf-8"))
        build_source = document.get("specforge_build")
        if (
            not isinstance(build_source, dict)
            or build_source.get("source_mode") != "head"
            or build_source.get("source_revision")
            != FIXTURE_BUILD_SOURCE_REVISION
        ):
            raise RuntimeError(
                "JSON benchmark hydration rejected build source identity"
            )
        annotation = document["annotation"]
        roster = document["sample_roster"]
        alignment = annotation.get("alignment")
        if (
            not isinstance(alignment, dict)
            or alignment.get("mode") != "by_index"
            or alignment.get("target") != "sample_roster"
        ):
            raise RuntimeError(
                "JSON benchmark hydration rejected annotation alignment"
            )
        if (
            len(annotation["values"]) != count
            or len(roster["names"]) != count
            or annotation["values_dtype"] != "int32"
            or annotation["values_shape"] != [count]
            or annotation["values"][0] != expected_first
        ):
            raise RuntimeError("JSON benchmark hydration lost semantic equality")
        return

    with asdf.open(path, lazy_load=False, memmap=False) as opened:
        values = _values(opened["annotation"]["values"])
        names = _names(opened["sample_roster"]["names"])
        if values.size != count or len(names) != count or int(values[0]) != expected_first:
            raise RuntimeError("ASDF benchmark hydration lost semantic equality")


def _second_round_case(spec: dict[str, Any]) -> dict[str, Any]:
    directory = Path(spec["directory"])
    directory.mkdir(parents=True, exist_ok=True)
    count = int(spec["count"])
    format_name = str(spec["format"])
    compression = spec.get("compression") or None
    compression_level = spec.get("compression_level")
    names = _case_names(spec["roster_profile"], count)
    if names is None:
        raise ValueError("second-round canonical comparison requires an explicit roster")
    base_array = _case_values(spec["value_profile"], count)
    base_values: Any = base_array.tolist() if format_name == "json" else base_array
    extension = ".json.zlib" if format_name == "json" and compression else ".json" if format_name == "json" else ".asdf"
    initial = directory / f"initial{extension}"
    initial_temporary = directory / f"initial.tmp{extension}"

    if format_name == "json":
        document = _json_document(names, base_values)
        initial_measurement = _measurement(
            lambda: _atomic_write_bytes(
                initial_temporary,
                initial,
                _encode_json_document(document, compression, compression_level),
            )
        )
    elif format_name == "asdf":
        tree = _tree(
            source_kind="folder",
            source_name="benchmark",
            source_fingerprint="sha256:benchmark",
            values=base_values,
            names=names,
            task_id="benchmark-task",
            task_name="Benchmark",
            labels=[{"code": 0, "name": "A"}, {"code": 1, "name": "B"}],
            roster_representation="string_ndarray",
        )

        def write_initial_asdf() -> None:
            _write_reference(
                initial_temporary,
                tree,
                compression=compression,
                compression_level=compression_level,
            )
            _fsync(initial_temporary)
            os.replace(initial_temporary, initial)

        initial_measurement = _measurement(write_initial_asdf)
    else:
        raise ValueError(f"unsupported second-round format: {format_name}")

    hydration_measurement = _measurement(
        lambda: _second_round_hydration(
            initial,
            format_name,
            compression,
            count,
            int(base_values[0]),
        )
    )

    result: dict[str, Any] = {
        **{key: value for key, value in spec.items() if key != "directory"},
        "initial_write": initial_measurement,
        "hydration": hydration_measurement,
        "initial_file_size_bytes": initial.stat().st_size,
        "foreground_definition": "in-memory label mutations only; persistence is measured separately as background-eligible work",
    }

    changed_sets: dict[str, Any] = {}
    for label, change_count in (("one", 1), ("batch_1000", 1000)):
        changed, foreground = _changed_values(base_values, change_count)
        changed_sets[label] = changed
        target = directory / f"{label}.full{extension}"
        temporary = directory / f"{label}.full.tmp{extension}"
        shutil.copyfile(initial, target)

        if format_name == "json":
            changed_document = _json_document(names, changed)
            persistence = _measurement(
                lambda document=changed_document, temp=temporary, destination=target: _atomic_write_bytes(
                    temp,
                    destination,
                    _encode_json_document(document, compression, compression_level),
                )
            )
        else:
            changed_tree = _tree(
                source_kind="folder",
                source_name="benchmark",
                source_fingerprint="sha256:benchmark",
                values=changed,
                names=names,
                task_id="benchmark-task",
                task_name="Benchmark",
                labels=[{"code": 0, "name": "A"}, {"code": 1, "name": "B"}],
                roster_representation="string_ndarray",
            )

            def write_full_asdf(
                document: dict[str, Any] = changed_tree,
                temp: Path = temporary,
                destination: Path = target,
            ) -> None:
                _write_reference(
                    temp,
                    document,
                    compression=compression,
                    compression_level=compression_level,
                )
                _fsync(temp)
                os.replace(temp, destination)

            persistence = _measurement(write_full_asdf)

        expected_first = int(changed[0])
        _second_round_hydration(target, format_name, compression, count, expected_first)
        result[label] = {
            "changed_labels": min(change_count, count),
            "foreground_edit": foreground,
            "full_persistence": persistence,
            "file_size_bytes": target.stat().st_size,
        }

    if format_name == "asdf":
        prefix, blocks = _scan_asdf_blocks(initial)
        if len(blocks) != 2:
            raise RuntimeError(f"expected roster and values ASDF blocks, found {len(blocks)}")
        roster_block = blocks[0]
        for label in ("one", "batch_1000"):
            changed = changed_sets[label]
            target = directory / f"{label}.reuse.asdf"
            temporary = directory / f"{label}.reuse.tmp.asdf"
            shutil.copyfile(initial, target)
            persistence = _measurement(
                lambda changed_values=changed, temp=temporary, destination=target: _atomic_reuse_asdf_roster_block(
                    initial,
                    temp,
                    destination,
                    prefix,
                    roster_block,
                    changed_values,
                    compression,
                    compression_level,
                )
            )
            _second_round_hydration(target, format_name, compression, count, int(changed[0]))
            rewritten_prefix, rewritten_blocks = _scan_asdf_blocks(target)
            if rewritten_prefix != prefix or len(rewritten_blocks) != 2:
                raise RuntimeError("reused ASDF rewrite changed metadata or block count")
            with initial.open("rb") as old_stream, target.open("rb") as new_stream:
                old_stream.seek(roster_block[0])
                new_stream.seek(rewritten_blocks[0][0])
                roster_identical = old_stream.read(roster_block[1]) == new_stream.read(rewritten_blocks[0][1])
            if not roster_identical:
                raise RuntimeError("ASDF roster block was not copied verbatim")
            result[label]["reuse_persistence"] = persistence
            result[label]["reuse_file_size_bytes"] = target.stat().st_size
            result[label]["roster_block_reused_bytes"] = roster_block[1]
            result[label]["roster_block_verbatim"] = True

    for path in directory.iterdir():
        if path.is_file():
            path.unlink()
    return result


def _benchmark_case(spec: dict[str, Any]) -> dict[str, Any]:
    directory = Path(spec["directory"])
    directory.mkdir(parents=True, exist_ok=True)
    count = int(spec["count"])
    names = _case_names(spec["roster_profile"], count)
    values = _case_values(spec["value_profile"], count)
    tree = _tree(
        source_kind="npy" if names is None else "folder",
        source_name="benchmark",
        source_fingerprint="sha256:benchmark",
        values=values,
        names=names,
        task_id="benchmark-task",
        task_name="Benchmark",
        labels=[{"code": 0, "name": "A"}, {"code": 1, "name": "B"}],
        roster_representation=spec["roster_representation"],
    )
    target = directory / "case.asdf"
    temporary = directory / "case.tmp.asdf"
    rewrite = directory / "case.rewrite.asdf"
    compression = spec.get("compression") or None
    start = time.perf_counter()
    _write_reference(temporary, tree, compression=compression)
    _fsync(temporary)
    os.replace(temporary, target)
    initial_write_seconds = time.perf_counter() - start
    file_size = target.stat().st_size
    metadata_size = len(_metadata_text(target).encode("utf-8"))

    start = time.perf_counter()
    with asdf.open(target, lazy_load=False, memmap=False) as opened:
        loaded_values = _values(opened["annotation"]["values"])
        loaded_names = _names(opened["sample_roster"]["names"]) if names is not None else []
        if loaded_values.size != count or (names is not None and len(loaded_names) != count):
            raise RuntimeError("benchmark hydration did not preserve sample alignment")
    hydration_seconds = time.perf_counter() - start

    values[0] = 0 if values[0] == UNLABELED else UNLABELED
    start = time.perf_counter()
    _write_reference(rewrite, tree, compression=compression)
    _fsync(rewrite)
    os.replace(rewrite, target)
    atomic_rewrite_seconds = time.perf_counter() - start
    target.unlink(missing_ok=True)
    return {
        **{key: value for key, value in spec.items() if key != "directory"},
        "initial_write_seconds": initial_write_seconds,
        "atomic_rewrite_seconds": atomic_rewrite_seconds,
        "hydration_seconds": hydration_seconds,
        "file_size_bytes": file_size,
        "metadata_size_bytes": metadata_size,
    }


def _run_case_subprocess(spec: dict[str, Any]) -> dict[str, Any]:
    command = [sys.executable, str(Path(__file__).resolve()), "_benchmark_case", json.dumps(spec, ensure_ascii=False)]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
    monitored = psutil.Process(process.pid)
    peak_rss = 0
    while process.poll() is None:
        try:
            peak_rss = max(peak_rss, monitored.memory_info().rss)
        except psutil.Error:
            pass
        time.sleep(0.01)
    stdout, stderr = process.communicate()
    if process.returncode != 0:
        raise RuntimeError(f"benchmark child failed ({process.returncode}): {stderr}")
    result = json.loads(stdout)
    result["peak_rss_bytes"] = peak_rss
    return result


def benchmark(output: Path, scales: list[int]) -> dict[str, Any]:
    output.mkdir(parents=True, exist_ok=True)
    scratch = output / "scratch"
    cases: list[dict[str, Any]] = []
    for count in scales:
        for roster_profile in ("ascii", "variable_unicode"):
            for roster_representation in ("sequence", "string_ndarray"):
                cases.append({
                    "kind": "roster",
                    "count": count,
                    "roster_profile": roster_profile,
                    "roster_representation": roster_representation,
                    "value_profile": "mostly_labeled",
                    "compression": None,
                })
        for roster_profile in ("none", "ascii", "variable_unicode"):
            for value_profile in ("mostly_labeled", "mostly_unlabeled"):
                case = {
                    "kind": "autosave",
                    "count": count,
                    "roster_profile": roster_profile,
                    "roster_representation": "sequence" if roster_profile == "none" else "string_ndarray",
                    "value_profile": value_profile,
                    "compression": None,
                }
                if case not in cases:
                    cases.append(case)
    for count in scales:
        for value_profile in ("mostly_labeled", "mostly_unlabeled"):
            for compression in (None, "zlib"):
                cases.append({
                    "kind": "compression",
                    "count": count,
                    "roster_profile": "none",
                    "roster_representation": "sequence",
                    "value_profile": value_profile,
                    "compression": compression,
                })
        for roster_profile in ("ascii", "variable_unicode"):
            for value_profile in ("mostly_labeled", "mostly_unlabeled"):
                cases.append({
                    "kind": "compression_with_roster",
                    "count": count,
                    "roster_profile": roster_profile,
                    "roster_representation": "string_ndarray",
                    "value_profile": value_profile,
                    "compression": "zlib",
                })

    results: list[dict[str, Any]] = []
    result_path = output / "benchmark.json"
    for index, case in enumerate(cases):
        spec = {**case, "directory": str(scratch / f"case-{index:03d}")}
        results.append(_run_case_subprocess(spec))
        result_path.write_text(
            json.dumps({"asdf": asdf.__version__, "results": results}, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
    return {"asdf": asdf.__version__, "results": results}


def _run_second_round_case_subprocess(spec: dict[str, Any]) -> dict[str, Any]:
    command = [
        sys.executable,
        str(Path(__file__).resolve()),
        "_second_round_case",
        json.dumps(spec, ensure_ascii=False),
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
    monitored = psutil.Process(process.pid)
    peak_rss = 0
    while process.poll() is None:
        try:
            peak_rss = max(peak_rss, monitored.memory_info().rss)
        except psutil.Error:
            pass
        time.sleep(0.01)
    stdout, stderr = process.communicate()
    if process.returncode != 0:
        raise RuntimeError(f"second-round benchmark child failed ({process.returncode}): {stderr}")
    result = json.loads(stdout)
    result["peak_rss_bytes"] = peak_rss
    return result


def second_round_benchmark(output: Path, scales: list[int]) -> dict[str, Any]:
    output.mkdir(parents=True, exist_ok=True)
    scratch = output / "second-round-scratch"
    candidates = [
        {"candidate": "A", "format": "json", "compression": None, "compression_level": None},
        *[
            {"candidate": "B", "format": "json", "compression": "zlib", "compression_level": level}
            for level in (1, 6, 9)
        ],
        {"candidate": "C", "format": "asdf", "compression": None, "compression_level": None},
        *[
            {"candidate": "D", "format": "asdf", "compression": "zlib", "compression_level": level}
            for level in (1, 6, 9)
        ],
    ]
    cases: list[dict[str, Any]] = []
    for count in scales:
        for roster_profile in ("ascii", "variable_unicode"):
            for value_profile in ("mostly_labeled", "mostly_unlabeled"):
                for candidate in candidates:
                    cases.append(
                        {
                            "kind": "canonical_format_second_round",
                            "count": count,
                            "roster_profile": roster_profile,
                            "value_profile": value_profile,
                            **candidate,
                        }
                    )

    results: list[dict[str, Any]] = []
    result_path = output / "second-round-benchmark.json"
    for index, case in enumerate(cases):
        spec = {**case, "directory": str(scratch / f"case-{index:03d}")}
        results.append(_run_second_round_case_subprocess(spec))
        result_path.write_text(
            json.dumps(
                {
                    "reference": {
                        "asdf": asdf.__version__,
                        "json_encoding": "UTF-8 compact JSON",
                        "compression": "zlib levels 1, 6, and 9",
                    },
                    "case_count": len(cases),
                    "completed_case_count": len(results),
                    "results": results,
                },
                ensure_ascii=False,
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
    return {"asdf": asdf.__version__, "case_count": len(results), "results": results}


def _yaml_special_character_matrix() -> str:
    return (
        "yaml-indicators:-?:,[]{}#&*!|>'\"%@`/\\ space "
        + "".join(chr(codepoint) for codepoint in range(0x20))
        + "".join(chr(codepoint) for codepoint in range(0x7F, 0xA0))
        + "\u00a0\u2028\u2029\ufeff\ufffe\uffff"
    )


def _production_explicit_semantic_summary(values: list[int]) -> dict[str, Any]:
    return {
        "format_kind": "specforge.sample_labeling",
        "schema_version": "2.0.0",
        "build_source_mode": "head",
        "build_source_revision": FIXTURE_BUILD_SOURCE_REVISION,
        "source_kind": "folder",
        "source_name": "巡天样本",
        "source_identity": "sha256-v1:production-source",
        "source_fingerprint": "sha256-v1:production-fingerprint",
        "sample_count": 3,
        "roster_identity_kind": "explicit_names",
        "sample_names": ["alpha.fits", "星系-β.fits", "échelle-γ.fits"],
        "annotation_kind": "categorical_integer",
        "alignment_mode": "by_index",
        "alignment_target": "sample_roster",
        "missing_semantic": "unlabeled",
        "missing_value": -1,
        "task_id": "00000000-0000-4000-8000-000000000001",
        "task_name": "天体分类",
        "created_at": CREATED_AT,
        "modified_at": MODIFIED_AT,
        "origin_kind": "manual",
        "origin_annotation": None,
        "description": None,
        "authors": [],
        "labels": [
            {"code": 0, "name": "Galaxy", "shortcut": "g"},
            {"code": 1, "name": "Quasar", "shortcut": "q"},
        ],
        "values": values,
        "values_dtype": "int32",
        "values_shape": [3],
    }


def _production_source_index_semantic_summary(
    values: list[int], source_name: str = "巡天样本"
) -> dict[str, Any]:
    summary = _production_explicit_semantic_summary(values)
    summary["source_name"] = source_name
    summary["roster_identity_kind"] = "source_index"
    summary["sample_names"] = []
    return summary


def _production_author_email_semantic_summary() -> dict[str, Any]:
    summary = _production_explicit_semantic_summary([-1, 0, 1])
    summary["authors"] = [
        {
            "name": "Alice",
            "identifier": "https://orcid.org/0000-0001-2345-6789",
            "email": "alice@example.org",
        },
        {"name": "验证者", "identifier": None},
    ]
    return summary


def _build_source_fields(summary: dict[str, Any]) -> dict[str, Any]:
    return {
        "build_source_mode": summary["build_source_mode"],
        "build_source_revision": summary["build_source_revision"],
    }


def _with_build_source(
    summary: dict[str, Any], build_source: dict[str, Any]
) -> dict[str, Any]:
    summary.update(build_source)
    return summary


def _verify_author_email_type_rejections() -> None:
    for type_name, value in (
        ("null", None),
        ("integer", 123),
        ("over-range integer", 184467440737095516160),
        ("boolean", True),
    ):
        try:
            _author_summary({"name": "Author", "email": value})
        except TypeError as exc:
            if str(exc) == "author email must be a string":
                continue
            raise RuntimeError(
                f"author-email oracle returned the wrong {type_name} error: {exc}"
            ) from exc
        raise RuntimeError(
            f"author-email oracle accepted a present {type_name} value"
        )


def _verify_native_author_email_type_rejections(
    native: Path, fixture: Path, output: Path
) -> None:
    source = fixture.read_bytes()
    email_token = b"alice@example.org"
    if source.count(email_token) != 1:
        raise RuntimeError(
            "native author-email rejection fixture must contain exactly one email token"
        )
    for type_name, scalar in (
        ("null", b"null"),
        ("integer", b"123"),
        ("over-range-integer", b"184467440737095516160"),
        ("boolean", b"true"),
    ):
        path = output / f"native-author-email-{type_name}.asdf"
        path.write_bytes(source.replace(email_token, scalar, 1))
        completed = subprocess.run(
            [str(native), "read", str(path)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        if completed.returncode != 2 or "not a string: email" not in completed.stderr:
            raise RuntimeError(
                "native reader did not reject a present "
                f"{type_name} author email as a controlled string-type error: "
                f"returncode={completed.returncode}, stderr={completed.stderr!r}"
            )


def interoperability(fixtures: Path, native: Path, production_native: Path) -> dict[str, Any]:
    manifest = json.loads((fixtures / "manifest.json").read_text(encoding="utf-8"))
    if asdf.__version__ != manifest["reference"]["asdf"]:
        raise RuntimeError(
            f"interoperability oracle must use ASDF {manifest['reference']['asdf']}, got {asdf.__version__}"
        )
    records: list[dict[str, Any]] = []
    _verify_author_email_type_rejections()
    records.append(
        {
            "fixture": "author-email-non-string-types",
            "status": "oracle-controlled-rejection/null+integer+boolean",
            "returncode": 0,
        }
    )
    for fixture in manifest["fixtures"]:
        command = [str(native), "read", str(fixtures / fixture["path"])]
        completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", check=False)
        if (
            fixture["structurally_valid"]
            and fixture["semantically_valid"]
            and fixture.get("native_profile_supported", True)
        ):
            if completed.returncode != 0:
                raise RuntimeError(f"native rejected {fixture['name']}: {completed.stderr}")
            native_summary = json.loads(completed.stdout)
            expected = json.loads((fixtures / fixture["semantic_path"]).read_text(encoding="utf-8"))
            if native_summary != expected:
                raise RuntimeError(f"native semantic mismatch: {fixture['name']}")
            status = "python-writer/native-reader-equal"
        else:
            if completed.returncode != 2:
                raise RuntimeError(
                    f"native malformed/invalid result was not controlled error 2: {fixture['name']} -> {completed.returncode}"
                )
            status = "native-controlled-rejection"
        records.append({"fixture": fixture["name"], "status": status, "returncode": completed.returncode})

    with tempfile.TemporaryDirectory(prefix="specforge-asdf-native-") as temporary:
        _verify_native_author_email_type_rejections(
            native, fixtures / "unicode.asdf", Path(temporary)
        )
        records.append(
            {
                "fixture": "native-author-email-non-string-types",
                "status": "native-controlled-rejection/null+integer+over-range-integer+boolean",
                "returncode": 0,
            }
        )
        native_path = Path(temporary) / "native.asdf"
        completed = subprocess.run([str(native), "write-fixture", str(native_path)], capture_output=True, text=True, encoding="utf-8", check=False)
        if completed.returncode != 0:
            raise RuntimeError(f"native writer failed: {completed.stderr}")
        with asdf.open(native_path, lazy_load=False, memmap=False) as opened:
            errors = _semantic_errors(opened.tree)
            summary = _semantic_summary(opened.tree)
        if errors:
            raise RuntimeError(f"Python rejected native writer output: {errors}")
        if summary["sample_names"] != ["alpha.fits", "星系-β.fits", "gamma.fits"]:
            raise RuntimeError("native writer Unicode roster mismatch")
        native_compressions = _asdf_block_compressions(native_path)
        if native_compressions != ["zlib", "zlib"]:
            raise RuntimeError(f"native writer did not emit the compressed ASDF wire profile: {native_compressions}")
        records.append(
            {
                "fixture": "native-writer",
                "status": "native-writer/python-reader-equal",
                "returncode": 0,
                "compressions": native_compressions,
            }
        )

        production_yaml_specials_path = Path(temporary) / "production-yaml-specials.asdf"
        completed = subprocess.run(
            [
                str(production_native),
                "write-yaml-specials-oracle",
                str(production_yaml_specials_path),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(f"production writer YAML-special case failed: {completed.stderr}")
        with asdf.open(
            production_yaml_specials_path, lazy_load=False, memmap=False
        ) as opened:
            production_yaml_specials_errors = _semantic_errors(opened.tree)
            production_yaml_specials_summary = _semantic_summary(opened.tree)
        expected_yaml_specials = _production_source_index_semantic_summary(
            [-1, 0, 1], _yaml_special_character_matrix()
        )
        production_build_source = _build_source_fields(
            production_yaml_specials_summary
        )
        expected_yaml_specials.update(production_build_source)
        if (
            production_yaml_specials_errors
            or production_yaml_specials_summary != expected_yaml_specials
        ):
            raise RuntimeError(
                "official ASDF oracle did not preserve the complete "
                "production YAML-special source-index semantics"
            )
        records.append(
            {
                "fixture": "production-writer-yaml-specials",
                "status": "production-writer/asdf-5.3.1-reader-all-yaml-specials-equal",
                "returncode": 0,
            }
        )

        production_author_email_path = (
            Path(temporary) / "production-author-email.asdf"
        )
        completed = subprocess.run(
            [
                str(production_native),
                "write-author-email-oracle",
                str(production_author_email_path),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                f"production author-email writer failed: {completed.stderr}"
            )
        with asdf.open(
            production_author_email_path, lazy_load=False, memmap=False
        ) as opened:
            production_author_email_errors = _semantic_errors(opened.tree)
            production_author_email_summary = _semantic_summary(opened.tree)
        if (
            production_author_email_errors
            or production_author_email_summary
            != _with_build_source(
                _production_author_email_semantic_summary(),
                production_build_source,
            )
        ):
            raise RuntimeError(
                "official ASDF oracle rejected or changed optional author email"
            )
        records.append(
            {
                "fixture": "production-author-email",
                "status": "production-writer/asdf-5.3.1-reader-author-email-equal",
                "returncode": 0,
            }
        )

        production_explicit_path = Path(temporary) / "production-explicit.asdf"
        production_rewrite_path = Path(temporary) / "production-explicit-rewrite.asdf"
        completed = subprocess.run(
            [
                str(production_native),
                "write-explicit-roster-oracle",
                str(production_explicit_path),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                f"production explicit-roster writer failed: {completed.stderr}"
            )
        with asdf.open(
            production_explicit_path, lazy_load=False, memmap=False
        ) as opened:
            production_explicit_errors = _semantic_errors(opened.tree)
            production_explicit_summary = _semantic_summary(opened.tree)
        expected_production_explicit = _production_explicit_semantic_summary(
            [-1, 0, 1]
        )
        expected_production_explicit.update(production_build_source)
        if (
            production_explicit_errors
            or production_explicit_summary != expected_production_explicit
        ):
            raise RuntimeError(
                "official ASDF oracle rejected or changed the production "
                "two-block writer semantics"
            )
        production_explicit_compressions = _asdf_block_compressions(
            production_explicit_path
        )
        if production_explicit_compressions != ["zlib", "zlib"]:
            raise RuntimeError(
                "production explicit-roster writer did not emit two zlib blocks"
            )

        completed = subprocess.run(
            [
                str(production_native),
                "rewrite-explicit-roster-oracle",
                str(production_explicit_path),
                str(production_rewrite_path),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                f"production explicit-roster rewrite failed: {completed.stderr}"
            )
        with asdf.open(
            production_rewrite_path, lazy_load=False, memmap=False
        ) as opened:
            production_rewrite_errors = _semantic_errors(opened.tree)
            production_rewrite_summary = _semantic_summary(opened.tree)
        expected_production_rewrite = _production_explicit_semantic_summary(
            [0, 0, 1]
        )
        expected_production_rewrite.update(production_build_source)
        if (
            production_rewrite_errors
            or production_rewrite_summary != expected_production_rewrite
        ):
            raise RuntimeError(
                "official ASDF oracle rejected or changed the production "
                "two-block rewrite semantics"
            )
        if _asdf_raw_block(
            production_explicit_path, 0
        ) != _asdf_raw_block(production_rewrite_path, 0):
            raise RuntimeError(
                "production two-block rewrite did not preserve the roster block verbatim"
            )
        production_rewrite_compressions = _asdf_block_compressions(
            production_rewrite_path
        )
        if production_rewrite_compressions != ["zlib", "zlib"]:
            raise RuntimeError(
                "production explicit-roster rewrite did not retain two zlib blocks"
            )
        records.append(
            {
                "fixture": "production-explicit-writer-rewrite",
                "status": "production-writer+rewrite/asdf-5.3.1-semantic-equal+roster-verbatim",
                "returncode": 0,
                "compressions": production_rewrite_compressions,
            }
        )

        production_source_index_path = Path(temporary) / "production-source-index.asdf"
        production_source_index_rewrite_path = (
            Path(temporary) / "production-source-index-rewrite.asdf"
        )
        completed = subprocess.run(
            [
                str(production_native),
                "write-source-index-oracle",
                str(production_source_index_path),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                f"production source-index writer failed: {completed.stderr}"
            )
        with asdf.open(
            production_source_index_path, lazy_load=False, memmap=False
        ) as opened:
            production_source_index_errors = _semantic_errors(opened.tree)
            production_source_index_summary = _semantic_summary(opened.tree)
        if (
            production_source_index_errors
            or production_source_index_summary
            != _with_build_source(
                _production_source_index_semantic_summary([-1, 0, 1]),
                production_build_source,
            )
        ):
            raise RuntimeError(
                "official ASDF oracle rejected or changed the production "
                "source-index writer semantics"
            )
        if _asdf_block_compressions(production_source_index_path) != ["zlib"]:
            raise RuntimeError(
                "production source-index writer did not emit one zlib block"
            )

        production_duplicate_key_path = (
            Path(temporary) / "production-duplicate-key.asdf"
        )
        duplicate_bytes = production_source_index_path.read_bytes()
        duplicate_needle = b"  sample_count: 3\n"
        duplicate_replacement = (
            b"  sample_count: 3\n  sample_count: 4\n"
        )
        if duplicate_bytes.count(duplicate_needle) != 1:
            raise RuntimeError(
                "production duplicate-key oracle patch target was not unique"
            )
        production_duplicate_key_path.write_bytes(
            duplicate_bytes.replace(
                duplicate_needle, duplicate_replacement, 1
            )
        )
        with asdf.open(
            production_duplicate_key_path, lazy_load=False, memmap=False
        ) as opened:
            oracle_duplicate_sample_count = int(
                opened.tree["source_collection"]["sample_count"]
            )
        if oracle_duplicate_sample_count != 4:
            raise RuntimeError(
                "official ASDF oracle no longer resolves the duplicate key "
                "to the last value"
            )
        duplicate_rewrite_path = (
            Path(temporary) / "production-duplicate-key-rewrite.asdf"
        )
        completed = subprocess.run(
            [
                str(production_native),
                "rewrite-production-oracle",
                str(production_duplicate_key_path),
                str(duplicate_rewrite_path),
                "0",
                "1",
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        if completed.returncode != 1 or duplicate_rewrite_path.exists():
            raise RuntimeError(
                "production reader/rewrite did not reject ambiguous duplicate "
                "YAML mapping keys before creating output"
            )
        records.append(
            {
                "fixture": "production-duplicate-key-reader-rewrite",
                "status": "asdf-5.3.1-last-value/production-controlled-rejection",
                "returncode": completed.returncode,
                "oracle_sample_count": oracle_duplicate_sample_count,
            }
        )

        completed = subprocess.run(
            [
                str(production_native),
                "rewrite-production-oracle",
                str(production_source_index_path),
                str(production_source_index_rewrite_path),
                "0",
                "1",
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                f"production source-index rewrite failed: {completed.stderr}"
            )
        with asdf.open(
            production_source_index_rewrite_path, lazy_load=False, memmap=False
        ) as opened:
            production_source_index_rewrite_errors = _semantic_errors(opened.tree)
            production_source_index_rewrite_summary = _semantic_summary(opened.tree)
        if (
            production_source_index_rewrite_errors
            or production_source_index_rewrite_summary
            != _with_build_source(
                _production_source_index_semantic_summary([1, 0, 1]),
                production_build_source,
            )
        ):
            raise RuntimeError(
                "official ASDF oracle rejected or changed the production "
                "source-index rewrite semantics"
            )
        if _asdf_block_compressions(production_source_index_rewrite_path) != [
            "zlib"
        ]:
            raise RuntimeError(
                "production source-index rewrite did not retain one zlib block"
            )
        records.append(
            {
                "fixture": "production-source-index-writer-rewrite",
                "status": "production-source-index-writer+rewrite/asdf-5.3.1-semantic-equal",
                "returncode": 0,
                "compressions": ["zlib"],
            }
        )

        forward_record = next(
            fixture
            for fixture in manifest["fixtures"]
            if fixture["name"] == "forward_unknown"
        )
        forward_fixture = fixtures / forward_record["path"]
        forward_origin = Path(temporary) / "forward-typed-scalars.asdf"
        forward_bytes = forward_fixture.read_bytes()
        forward_needle = (
            b"future_vendor: {new_flag: true, new_text: preserve or ignore}\n"
        )
        forward_replacement = (
            b"future_vendor: {new_flag: true, new_text: preserve or ignore, "
            b"string_boolean: \"true\", string_integer: \"1\", "
            b"real_integer: 1}\n"
        )
        if forward_bytes.count(forward_needle) != 1:
            raise RuntimeError(
                "forward scalar-type oracle patch target was not unique"
            )
        forward_origin.write_bytes(
            forward_bytes.replace(forward_needle, forward_replacement, 1)
        )
        forward_rewrite = Path(temporary) / "production-forward-metadata-rewrite.asdf"
        completed = subprocess.run(
            [
                str(production_native),
                "rewrite-metadata-oracle",
                str(forward_origin),
                str(forward_rewrite),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                "production forward-metadata rewrite failed: "
                f"{completed.stderr}"
            )
        with asdf.open(forward_rewrite, lazy_load=False, memmap=False) as opened:
            forward_errors = _semantic_errors(opened.tree)
            forward_summary = _semantic_summary(opened.tree)
            future_vendor = opened.tree.get("future_vendor")
            future_vendor_preserved = (
                future_vendor is not None
                and type(future_vendor.get("new_flag")) is bool
                and future_vendor.get("new_flag") is True
                and future_vendor.get("new_text") == "preserve or ignore"
                and type(future_vendor.get("string_boolean")) is str
                and future_vendor.get("string_boolean") == "true"
                and type(future_vendor.get("string_integer")) is str
                and future_vendor.get("string_integer") == "1"
                and type(future_vendor.get("real_integer")) is int
                and future_vendor.get("real_integer") == 1
                and type(opened.tree["labeling_task"]["labels"][0]["shortcut"])
                is str
                and opened.tree["labeling_task"]["labels"][0]["shortcut"] == "1"
            )
        expected_forward = json.loads(
            (fixtures / forward_record["semantic_path"]).read_text(
                encoding="utf-8"
            )
        )
        expected_forward["task_name"] = "Forward metadata edited"
        expected_forward["labels"][0]["name"] = "Edited Galaxy"
        expected_forward["labels"][0]["shortcut"] = "1"
        expected_forward.update(production_build_source)
        if (
            forward_errors
            or forward_summary != expected_forward
            or not future_vendor_preserved
        ):
            raise RuntimeError(
                "official ASDF oracle rejected the metadata rewrite, changed "
                "known semantics, or lost forward-compatible metadata"
            )
        forward_compressions = _asdf_block_compressions(forward_rewrite)
        expected_forward_compressions = (
            ["zlib", "zlib"]
            if expected_forward["roster_identity_kind"] == "explicit_names"
            else ["zlib"]
        )
        if forward_compressions != expected_forward_compressions:
            raise RuntimeError(
                "production forward-metadata rewrite did not retain the canonical zlib blocks"
            )
        records.append(
            {
                "fixture": "production-forward-metadata-rewrite",
                "status": "python-forward-fields/production-full-rewrite/asdf-5.3.1-preserved",
                "returncode": 0,
                "compressions": forward_compressions,
            }
        )

        for fixture_name in ("profile_zlib", "npy_source_index"):
            fixture_record = next(
                fixture
                for fixture in manifest["fixtures"]
                if fixture["name"] == fixture_name
            )
            python_origin = fixtures / fixture_record["path"]
            production_origin_rewrite = (
                Path(temporary) / f"production-rewrite-{fixture_name}.asdf"
            )
            completed = subprocess.run(
                [
                    str(production_native),
                    "rewrite-production-oracle",
                    str(python_origin),
                    str(production_origin_rewrite),
                    "0",
                    "0",
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                check=False,
            )
            if completed.returncode != 0:
                raise RuntimeError(
                    f"Python-origin production rewrite failed for {fixture_name}: "
                    f"{completed.stderr}"
                )
            with asdf.open(
                production_origin_rewrite, lazy_load=False, memmap=False
            ) as opened:
                production_origin_errors = _semantic_errors(opened.tree)
                production_origin_summary = _semantic_summary(opened.tree)
            expected_origin = json.loads(
                (fixtures / fixture_record["semantic_path"]).read_text(
                    encoding="utf-8"
                )
            )
            expected_origin["values"][0] = 0
            expected_origin.update(production_build_source)
            if production_origin_errors or production_origin_summary != expected_origin:
                raise RuntimeError(
                    "official ASDF oracle rejected or changed the Python-origin "
                    f"production rewrite semantics for {fixture_name}"
                )
            expected_compressions = (
                ["zlib", "zlib"]
                if fixture_name == "profile_zlib"
                else ["zlib"]
            )
            origin_compressions = _asdf_block_compressions(
                production_origin_rewrite
            )
            if origin_compressions != expected_compressions:
                raise RuntimeError(
                    f"Python-origin production rewrite profile changed for {fixture_name}: "
                    f"{origin_compressions}"
                )
            if fixture_name == "profile_zlib" and _asdf_raw_block(
                python_origin, 0
            ) != _asdf_raw_block(production_origin_rewrite, 0):
                raise RuntimeError(
                    "Python-origin production rewrite did not preserve the roster "
                    "block verbatim"
                )
            records.append(
                {
                    "fixture": f"python-origin-production-rewrite-{fixture_name}",
                    "status": "python-writer/production-rewrite/asdf-5.3.1-semantic-equal",
                    "returncode": 0,
                    "compressions": origin_compressions,
                }
            )

        rewrite_source = fixtures / "profile_zlib.asdf"
        rewrite_path = Path(temporary) / "native-rewrite.asdf"
        completed = subprocess.run(
            [str(native), "rewrite-value", str(rewrite_source), str(rewrite_path), "0", "0"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(f"native compressed-block rewrite failed: {completed.stderr}")
        with asdf.open(rewrite_path, lazy_load=False, memmap=False) as opened:
            rewrite_errors = _semantic_errors(opened.tree)
            rewrite_summary = _semantic_summary(opened.tree)
        expected_rewrite = json.loads((fixtures / "profile_zlib.semantic.json").read_text(encoding="utf-8"))
        expected_rewrite["values"][0] = 0
        if rewrite_errors or rewrite_summary != expected_rewrite:
            raise RuntimeError(
                f"Python rejected native compressed-block rewrite: errors={rewrite_errors} equal={rewrite_summary == expected_rewrite}"
            )
        if _asdf_raw_block(rewrite_source, 0) != _asdf_raw_block(rewrite_path, 0):
            raise RuntimeError("native rewrite did not preserve the compressed roster block verbatim")
        rewrite_compressions = _asdf_block_compressions(rewrite_path)
        if rewrite_compressions != ["zlib", "zlib"]:
            raise RuntimeError(f"native rewrite did not preserve the compressed ASDF wire profile: {rewrite_compressions}")
        completed = subprocess.run(
            [str(native), "read", str(rewrite_path)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        if completed.returncode != 0 or json.loads(completed.stdout) != expected_rewrite:
            raise RuntimeError("native reader rejected native compressed-block rewrite")
        records.append(
            {
                "fixture": "native-rewrite",
                "status": "python-writer/native-rewrite/python-reader-equal+roster-verbatim",
                "returncode": 0,
                "compressions": rewrite_compressions,
            }
        )
        for case_name, index, replacement in (
            ("native-rewrite-index-range", "99", "0"),
            ("native-rewrite-undefined-label", "0", "42"),
        ):
            rejected_path = Path(temporary) / f"{case_name}.asdf"
            completed = subprocess.run(
                [str(native), "rewrite-value", str(rewrite_source), str(rejected_path), index, replacement],
                capture_output=True,
                text=True,
                encoding="utf-8",
                check=False,
            )
            if completed.returncode != 2:
                raise RuntimeError(f"native rewrite did not reject {case_name}: {completed.returncode}")
            records.append({"fixture": case_name, "status": "native-controlled-rejection", "returncode": 2})
    return {
        "asdf": asdf.__version__,
        "native": str(native),
        "production_native": str(production_native),
        "records": records,
    }


def _write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    generate = subparsers.add_parser("generate")
    generate.add_argument("--output", type=Path, required=True)
    validate = subparsers.add_parser("validate")
    validate.add_argument("--fixtures", type=Path, required=True)
    experiments = subparsers.add_parser("experiments")
    experiments.add_argument("--output", type=Path, required=True)
    experiments.add_argument("--scales", type=int, nargs="+", default=[10_000, 100_000, 1_000_000])
    second_round = subparsers.add_parser("second-round")
    second_round.add_argument("--output", type=Path, required=True)
    second_round.add_argument("--scales", type=int, nargs="+", default=[10_000, 100_000, 1_000_000])
    interop = subparsers.add_parser("interoperability")
    interop.add_argument("--fixtures", type=Path, required=True)
    interop.add_argument("--native", type=Path, required=True)
    interop.add_argument("--production-native", type=Path, required=True)
    interop.add_argument("--output", type=Path, required=True)
    internal = subparsers.add_parser("_benchmark_case")
    internal.add_argument("spec")
    second_round_internal = subparsers.add_parser("_second_round_case")
    second_round_internal.add_argument("spec")
    args = parser.parse_args()

    if args.command == "generate":
        print(json.dumps(generate_fixtures(args.output), ensure_ascii=False, indent=2))
    elif args.command == "validate":
        print(json.dumps(validate_fixtures(args.fixtures), ensure_ascii=False, indent=2))
    elif args.command == "experiments":
        args.output.mkdir(parents=True, exist_ok=True)
        missing = missing_value_experiment(args.output / "missing")
        wire = wire_version_experiment(args.output / "versions")
        bench = benchmark(args.output, args.scales)
        _write_json(args.output / "missing-value.json", missing)
        _write_json(args.output / "wire-versions.json", wire)
        print(json.dumps({"missing": missing, "wire": wire, "benchmark_cases": len(bench["results"])}, ensure_ascii=False, indent=2))
    elif args.command == "second-round":
        result = second_round_benchmark(args.output, args.scales)
        print(json.dumps({"asdf": result["asdf"], "benchmark_cases": result["case_count"]}, ensure_ascii=False, indent=2))
    elif args.command == "interoperability":
        result = interoperability(args.fixtures, args.native, args.production_native)
        _write_json(args.output, result)
        print(json.dumps(result, ensure_ascii=False, indent=2))
    elif args.command == "_benchmark_case":
        print(json.dumps(_benchmark_case(json.loads(args.spec)), ensure_ascii=False))
    elif args.command == "_second_round_case":
        print(json.dumps(_second_round_case(json.loads(args.spec)), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
