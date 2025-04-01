# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Blender Online Asset Repository Index Generator.

Run: python -m index-generate <blender_executable>

"""

import logging
import sys
from argparse import ArgumentParser
from pathlib import Path

import pydantic

from . import asset_finder, pagination
from . import blender_asset_library_openapi as api_models

API_VERSION = 1
SCHEMA_VERSION = "1.0.0"

logger = logging.getLogger()


class CLIArguments(pydantic.BaseModel):
    """Parsed commandline arguments."""

    repository: Path
    outdir: Path
    limit: int


def main(args: list[str]) -> None:
    """Generate the index for the passed-on-the-CLI asset library path."""

    # Parse CLI arguments.
    arguments = _parse_arguments(args)
    _validate_inputs(arguments)

    # Set up logging.
    logging.basicConfig(level=logging.DEBUG)

    # Find all .blend files.
    filepaths: list[Path] = []
    logger.info("Traversing %s", arguments.repository)
    for filepath in arguments.repository.rglob("*.blend"):
        filepaths.append(filepath)

    files_total = len(filepaths)
    logger.info(f"* {files_total} .blend files found.")

    limit = _total_files_to_process(arguments, files_total)

    # Find the assets in the blend files.
    logger.info("Parsing the files...")
    assets: list[api_models.Asset] = []
    for i, filepath in enumerate(filepaths[:limit]):
        logger.info(f"* {i + 1}/{limit}: {filepath.relative_to(arguments.repository)}")
        assets_in_file = asset_finder.list_assets(filepath)
        assets.extend(assets_in_file)

    # Write the output.
    asset_index_pages = pagination.paginate_asset_list(assets, arguments.limit)
    _write_json_files(arguments, asset_index_pages)


def _write_json_files(
    arguments: CLIArguments,
    asset_index_pages: list[api_models.AssetLibraryIndexPage],
) -> None:
    outdir = arguments.outdir
    outdir.mkdir(exist_ok=True, parents=True)

    def _save_json(model: pydantic.BaseModel, filename: str) -> None:
        as_json = model.model_dump_json(indent=2, exclude_defaults=True)

        json_path = outdir / filename
        json_path.parent.mkdir(exist_ok=True, parents=True)

        logger.info("Writing %s", json_path)
        with json_path.open("wt") as json_file:
            json_file.write(as_json)

    # Metadata file /asset-library-meta.json:
    metadata = api_models.AssetLibraryMeta(
        api_version=API_VERSION,
        name="Your Asset Library",  # TODO: get from CLI/config? or load from pre-existing JSON?
        contact=api_models.Contact(
            name="Your Local Admin",  # TODO: get from CLI/config? or load from pre-existing JSON?
            url="https://awesomesauce.blender.org/",  # TODO: get from CLI/config? or load from pre-existing JSON?
            email="example@example.org",
        ),
    )
    _save_json(metadata, "asset-library-meta.json")

    total_asset_count = sum(len(page.assets) for page in asset_index_pages)

    # Library Index file /v1/asset-index.json:
    index = api_models.AssetLibraryIndex(
        schema_version=SCHEMA_VERSION,
        asset_size_bytes=0,  # TODO: collect this info.
        asset_count=total_asset_count,
        page_count=1,  # TODO: support pagination.
        catalogs=[],  # TODO: collect catalogs.
    )
    _save_json(index, f"v{API_VERSION}/asset-index.json")

    # Library Index Page /v1/assets-{page}.json
    for page_index, page in enumerate(asset_index_pages):
        _save_json(page, f"v{API_VERSION}/assets-{page_index:05}.json")


def _parse_arguments(args: list[str]) -> CLIArguments:
    """Parse command-line arguments."""
    parser = ArgumentParser(
        prog="blender -c asset_index",
        add_help=False,
        description="Create an index file listing all the assets.",
    )

    parser.add_argument(
        "--out",
        "-o",
        type=Path,
        help="""Folder where to save the JSON files, defaults to the asset repository folder""",
    )

    parser.add_argument(
        "repository",
        type=Path,
        help="""Asset repository folder""",
    )

    parser.add_argument(
        "--limit",
        "-l",
        type=int,
        default=None,
        help="Limit the number of files to process",
    )

    arguments_raw = parser.parse_args(args)

    repository = arguments_raw.repository.absolute()
    arguments = CLIArguments(
        repository=repository,
        outdir=arguments_raw.out or repository,
        limit=arguments_raw.limit or 0,
    )
    return arguments


def _validate_inputs(arguments: CLIArguments) -> None:
    """Make sure the passed arguments are valid."""

    repository = arguments.repository
    if not repository.is_dir():
        print(f"Error: Repository specified is not a folder: {repository}")
        sys.exit(1)

    outdir = arguments.outdir
    if outdir.exists() and not outdir.is_dir():
        print(f"Error: Output path exists but is not a folder: {outdir}")
        sys.exit(2)


def _total_files_to_process(arguments: CLIArguments, files_total: int) -> int:
    if not arguments.limit:
        return files_total

    return min(arguments.limit, files_total)
