# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Blender Online Asset Repository Index Generator."""

import argparse
import logging
import typing
import sys
from pathlib import Path

import pydantic

from . import asset_finder, pagination
from . import blender_asset_library_openapi as api_models

API_VERSION = 1
SCHEMA_VERSION = "1.0.0"

DEFAULT_METADATA = api_models.AssetLibraryMeta(
    api_version=API_VERSION,
    name="Your Asset Library",
    contact=api_models.Contact(
        name="Your Name",
        url="https://example.org/",
        email="example@example.org",
    ),
)

logger = logging.getLogger(__name__)


class CLIArguments(pydantic.BaseModel):
    """Parsed commandline arguments."""

    repository: Path
    limit: int
    page_size: int


def cli_main(arguments_raw: argparse.Namespace) -> None:
    """Generate the index for the passed-on-the-CLI asset library path."""

    # Parse CLI arguments.
    arguments = _parse_cli_args(arguments_raw)

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
        assets_in_file = asset_finder.list_assets(filepath, arguments.repository)
        assets.extend(assets_in_file)

    # Write the output.
    asset_index_pages = pagination.paginate_asset_list(assets, arguments.page_size)
    _write_json_files(arguments, asset_index_pages)


def _write_json_files(
    arguments: CLIArguments,
    asset_index_pages: list[api_models.AssetLibraryIndexPage],
) -> None:
    outdir = arguments.repository

    def _save_json(model: pydantic.BaseModel, json_path: str | Path) -> None:
        as_json = model.model_dump_json(indent=2, exclude_defaults=True)

        if isinstance(json_path, str):
            json_path = outdir / json_path
        json_path.parent.mkdir(exist_ok=True, parents=True)

        logger.info("Writing %s", json_path)
        with json_path.open("wt") as json_file:
            json_file.write(as_json)

    # Metadata file /asset-library-meta.json. This gets loaded if it exists.
    meta_json_path = outdir / "asset-library-meta.json"
    try:
        metadata = _toplevel_metadata(meta_json_path)
    except pydantic.ValidationError as ex:
        msg = "Metadata file {} could not be parsed as JSON: {}"
        logger.error(msg.format(meta_json_path, ex))
    else:
        _save_json(metadata, meta_json_path)

    # Remove old pages, in case the number of assets per page was increased and
    # so less page files are needed.
    existing_pages = (outdir / f"v{API_VERSION}").glob("assets-*.json")
    for filepath in existing_pages:
        filepath.unlink()

    # Library Index Page /v1/assets-{page}.json
    page_urls = []
    for page_index, page in enumerate(asset_index_pages):
        page_relpath = f"v{API_VERSION}/assets-{page_index:05}.json"
        page_urls.append(page_relpath)

        _save_json(page, page_relpath)

    # Library Index file /v1/asset-index.json:
    total_asset_count = sum(len(page.assets) for page in asset_index_pages)
    index = api_models.AssetLibraryIndex(
        schema_version=SCHEMA_VERSION,
        asset_size_bytes=0,  # TODO: collect this info.
        asset_count=total_asset_count,
        page_urls=page_urls,
        catalogs=[],  # TODO: collect catalogs.
    )
    _save_json(index, f"v{API_VERSION}/asset-index.json")


def _toplevel_metadata(json_path: Path) -> api_models.AssetLibraryMeta:
    """Construct the top-level metadata.

    Returns the metadata, or raises a pydantic.ValidationError if it is not
    valid JSON.

    Writing is considered safe, except when the file exists but does not contain
    valid JSON. In that case, it's better to warn about this and keep the file
    as-is, so that the user can either delete or fix it.
    """
    try:
        json_data = json_path.read_bytes()
    except IOError:
        # Ignore any read errors, as this likely means the file simply doesn't exist.
        return DEFAULT_METADATA

    metadata = api_models.AssetLibraryMeta.model_validate_json(json_data)

    # Update the metadata to declare the API version for which we're going to
    # write the data.
    metadata.api_version = API_VERSION

    return metadata


# Ignore the type of the `subparsers` argument, because there doesn't seem
# to be a way to make both static mypy and the runtime Python happy at the
# same time.
def add_cli_parser(subparsers: argparse._SubParsersAction) -> None:  # type: ignore[type-arg]
    """Add argparser for this subcommand."""

    parser = subparsers.add_parser("generate", help="Generate files necessary to serve an asset library")
    parser.set_defaults(func=cli_main)

    parser.add_argument(
        "repository",
        type=Path,
        help="""Asset repository folder""",
    )

    parser.add_argument(
        "--limit",
        "-l",
        metavar="NUM_BLEND_FILES",
        type=int,
        default=None,
        help="Limit the number of files to process",
    )

    parser.add_argument(
        "--page",
        "-p",
        metavar="ASSETS_PER_PAGE",
        type=int,
        default=1000,
        help="Number of assets per JSON file, set to 0 to disable pagination",
    )


def _parse_cli_args(arguments_raw: argparse.Namespace) -> CLIArguments:
    """Make sure the passed arguments are valid."""

    repository = arguments_raw.repository.absolute()
    if not repository.is_dir():
        print(f"Error: Repository specified is not a folder: {repository}")
        sys.exit(1)

    arguments = CLIArguments(
        repository=repository,
        limit=arguments_raw.limit or 0,
        page_size=arguments_raw.page or 0,
    )

    return arguments


def _total_files_to_process(arguments: CLIArguments, files_total: int) -> int:
    if not arguments.limit:
        return files_total

    return min(arguments.limit, files_total)
